/*
 *  linux/kernel/console.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *    console.c
 *
 * This module implements the console io functions
 *    'void con_init(void)'
 *    'void con_write(struct tty_queue * queue)'
 * Hopefully this will be a rather complete VT102 implementation.
 *
 * Beeping thanks to John T Kohl.
 *
 * Virtual Consoles, Screen Blanking, Screen Dumping, Color, Graphics
 *   Chars, and VT100 enhancements by Peter MacDonald.
 */

/*
 *  NOTE!!! We sometimes disable and enable interrupts for a short while
 * (to put a word in video IO), but this will work even for keyboard
 * interrupts. We know interrupts aren't enabled when getting a keyboard
 * interrupt, as we use trap-gates. Hopefully all is well.
 */

/*
 * Code to check for different video-cards mostly by Galen Hunt,
 * <g-hunt@ee.utah.edu>
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/tty.h>

#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>

#include <errno.h>
#include <string.h>

#define DEF_TERMIOS                                                                                \
    (struct termios)                                                                               \
    {                                                                                              \
        .c_iflag = ICRNL,                                              /* 输入模式标志 */          \
            .c_oflag = OPOST | ONLCR,                                  /* 输出模式标志 */          \
            .c_cflag = 0,                                              /* 控制模式标志 */          \
            .c_lflag = IXON | ISIG | ICANON | ECHO | ECHOCTL | ECHOKE, /* local */                 \
            .c_line = 0,                                               /* 线路规程 */              \
            .c_cc = INIT_C_CC                                          /* 控制字符数组 */          \
    }

/*
 * These are set up by the setup-routine at boot-time:
 */
#define ORIG_X (*(unsigned char *)0x90000)                             /* 初始光标列号 */
#define ORIG_Y (*(unsigned char *)0x90001)                             /* 初始光标行号 */
#define ORIG_VIDEO_PAGE (*(unsigned short *)0x90004)                   /* 初始显示页面 */
#define ORIG_VIDEO_MODE ((*(unsigned short *)0x90006) & 0xff)          /* 显示模式 */
#define ORIG_VIDEO_COLS (((*(unsigned short *)0x90006) & 0xff00) >> 8) /* 屏幕列数 */
#define ORIG_VIDEO_LINES ((*(unsigned short *)0x9000e) & 0xff)         /* 屏幕行数 */
#define ORIG_VIDEO_EGA_AX (*(unsigned short *)0x90008)                 /* [? */
#define ORIG_VIDEO_EGA_BX (*(unsigned short *)0x9000a)                 /* 显示内存大小和色彩模式 */
#define ORIG_VIDEO_EGA_CX (*(unsigned short *)0x9000c)                 /* 显示卡特性参数 */

/* 定义显示器单色/彩色显示模式类型符号常数 */
#define VIDEO_TYPE_MDA 0x10  /* 单色文本    Monochrome Text Display */
#define VIDEO_TYPE_CGA 0x11  /* CGA显示器   CGA Display */
#define VIDEO_TYPE_EGAM 0x20 /* EGA/VGA单色 EGA/VGA in Monochrome Mode */
#define VIDEO_TYPE_EGAC 0x21 /* EGA/VGA彩色 EGA/VGA in Color Mode */

#define NPAR 16 /* 转义字符序列中最大参数个数 */

int NR_CONSOLES = 0; /* 系统实际支持的控制台数量 */

extern void keyboard_interrupt(void); /* 键盘中断处理程序 */

/* 以下这些静态变量是本文件函数中使用的一些全局变量 */
static unsigned char video_type;        /* 使用的显示类型, Type of display being used    */
static unsigned long video_num_columns; /* 屏幕文本列数, Number of text columns    */
static unsigned long video_mem_base;    /* 物理显示内存基地址, Base of video memory        */
static unsigned long video_mem_term;    /* 物理显示内存末端地址, End of video memory        */
static unsigned long video_size_row;    /* 屏幕每行使用的字节数, Bytes per row        */
static unsigned long video_num_lines;   /* 屏幕文本行数, Number of test lines        */
static unsigned char video_page;        /* 初试显示页面, Initial video page        */
static unsigned short video_port_reg;   /* 显示控制选择寄存器端口, Video register select port    */
static unsigned short video_port_val;   /* 显示控制数据寄存器端口, Video register value port    */

static int can_do_colour = 0; /* 标志: 可使用彩色功能 */

/* 虚拟控制台结构
 *
 * 包含一个虚拟控制台的当前所有信息. 其中:
 *  - vc_origin 和 vc_scr_end 是当前正在处理的虚拟控制台执行快速滚
 *    屏操作时使用的起始行和末行对应的显示内存位置
 * - vc_video_mem_start 和 vc_video_mem_end 是当前虚拟控制台使用
 *   的显示内存区域部分
 *
 * vc == Virtual Console */
static struct {
    unsigned short vc_video_erase_char; /* 擦除字符属性及字符(0x0720) */
    unsigned char vc_attr;              /* 字符属性 */
    unsigned char vc_def_attr;          /* 默认字符属性 */
    int vc_bold_attr;                   /* 粗体字符属性 */
    unsigned long vc_ques;              /* 问号字符 */
    unsigned long vc_state;             /* 处理转义或控制序列的当前状态 */
    unsigned long vc_restate;           /* 处理转义或控制序列的下一状态 */
    unsigned long vc_checkin;
    unsigned long vc_origin;             /* Used for EGA/VGA fast scroll */
    unsigned long vc_scr_end;            /* Used for EGA/VGA fast scroll */
    unsigned long vc_pos;                /* 当前光标位置 */
    unsigned long vc_x, vc_y;            /* 当前光标列、行值 */
    unsigned long vc_top, vc_bottom;     /* 滚动时顶行行号, 底行行号 */
    unsigned long vc_npar, vc_par[NPAR]; /* 转义序列参数个数和参数数组 */
    unsigned long vc_video_mem_start;    /* Start of video RAM */
    unsigned long vc_video_mem_end;      /* End of video RAM (sort of) */
    unsigned int vc_saved_x;             /* 保存的光标列号 */
    unsigned int vc_saved_y;             /* 保存的光标行号 */
    unsigned int vc_iscolor;             /* 彩色显示标志 */
    char *vc_translate;                  /* 使用的字符集 */
} vc_cons[MAX_CONSOLES];

/* 为了便于引用, 以下定义当前正在处理控制台信息的符号. 含义同上
 * 其中 currcons 是使用 vc_cons 结构的函数参数中的当前虚拟终端号 */
#define origin (vc_cons[currcons].vc_origin)   /* 快速滚屏操作起始内存位置(可视屏幕的开始位置) */
#define scr_end (vc_cons[currcons].vc_scr_end) /* 快速滚屏操作末端内存位置(可视屏幕的结束位置) */
#define pos (vc_cons[currcons].vc_pos)         /* 光标内存位置游标 */
#define top (vc_cons[currcons].vc_top)         /* 可视屏幕开始行号 */
#define bottom (vc_cons[currcons].vc_bottom)   /* 可视屏幕结束行号 */
#define x (vc_cons[currcons].vc_x)             /* 当前光标列值 */
#define y (vc_cons[currcons].vc_y)             /* 当前光标行值 */
#define state (vc_cons[currcons].vc_state)
#define restate (vc_cons[currcons].vc_restate)
#define checkin (vc_cons[currcons].vc_checkin)
#define npar (vc_cons[currcons].vc_npar) /* 转义序列参数个数 */
#define par (vc_cons[currcons].vc_par)   /* 转义序列参数数组 */
#define ques (vc_cons[currcons].vc_ques) /* 转移序列里面发现了 `?` */
#define attr (vc_cons[currcons].vc_attr) /* 显示字符属性 */
#define saved_x (vc_cons[currcons].vc_saved_x)
#define saved_y (vc_cons[currcons].vc_saved_y)
#define translate (vc_cons[currcons].vc_translate)
#define video_mem_start (vc_cons[currcons].vc_video_mem_start) /* 使用显存的起始位置 */
#define video_mem_end (vc_cons[currcons].vc_video_mem_end)     /* 使用显存的末端位置 */
#define def_attr (vc_cons[currcons].vc_def_attr)               /* 默认字符属性 */
#define video_erase_char (vc_cons[currcons].vc_video_erase_char)
#define iscolor (vc_cons[currcons].vc_iscolor) /* 是否彩色显示 */

int blankinterval = 0; // 设定的屏幕黑屏间隔时间
int blankcount = 0;    // 黑屏时间计数

static void sysbeep(void); // 系统蜂鸣函数

/*
 * this is what the terminal answers to a ESC-Z or csi0c
 * query (= vt100 response).
 *
 * 下面是终端回应 ESC-Z 或 csi0c 请求的应答(=vt100响应)
 *
 * csi - 控制序列引导码(Control Sequence Introducer)
 * 主机通过发送不带参数或参数是 0 的设备属性(DA)控制序列(`ESC [c` 或 `ESC [0c`),
 * 要求终端应答一个设备属性控制序列(`ESC Z`的作用与此相同), 终端则发送以下序列来
 * 响应主机. 序列 `ESC [?1;2c` 表示终端是具有高级视频功能的 VT100 兼容终端
 */
#define RESPONSE "\033[?1;2c"

// 定义使用的字符集
// 上半部分为普通 7 比特 ASCII 代码, 即 US 字符集
// 下半部分对应 VT100 终端设备中的线条字符, 即显示图表线条的字符集
static char *translations[] = {
    /* normal 7-bit ascii */
    " !\"#$%&'()*+,-./0123456789:;<=>?"
    "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
    "`abcdefghijklmnopqrstuvwxyz{|}~ ",
    /* vt100 graphics */
    " !\"#$%&'()*+,-./0123456789:;<=>?"
    "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^ "
    "\004\261\007\007\007\007\370\361\007\007\275\267\326\323\327\304"
    "\304\304\304\304\307\266\320\322\272\363\362\343\\007\234\007 ",
};

#define NORM_TRANS (translations[0])
#define GRAF_TRANS (translations[1])

/**
 * @brief 更新光标位置
 *
 * 当前光标位置变量 (x, y), 并修正光标在显示内存中的对应位置 pos
 *
 * 注意, 函数中的所有变量实际上是vc_cons[currcons]结构中的相应字段. 以下函数相同.
 *
 * NOTE! gotoxy thinks x==video_num_columns is ok
 * 注意！gotoxy函数认为 x==video_num_columns 是正确的
 *
 * @param currcons 虚拟终端号
 * @param new_x 光标所在列号
 * @param new_y 光标所在行号
 */
static inline void gotoxy(int currcons, int new_x, unsigned int new_y)
{
    /* 检查光标的位置, 如果超出屏幕范围, 就直接退出 */
    if (new_x > video_num_columns || new_y >= video_num_lines) {
        return;
    }

    x = new_x;
    y = new_y;

    /* 1 列用 2 个字节表示, 故有 `(x << 1)` */
    pos = origin + y * video_size_row + (x << 1);
}

/**
 * @brief 设置滚屏起始显示内存地址
 *
 * @param currcons 虚拟终端号
 */
static inline void set_origin(int currcons)
{
    // 首先判断显示卡类型
    // 对于 EGA/VGA 卡, 我们可以指定屏内行范围(区域)进行滚屏操作, 而 MDA 单色显示卡
    // 只能进行整屏滚屏操作. 因此只有 EGA/VGA 卡才需要设置滚屏起始行显示内存地址(起始行
    // 是 origin 对应的行). 即显示类型如果不是 EGA/VGA 彩色模式, 也不是 EGA/VGA单色
    // 模式, 那么就直接返回. 另外, 我们只对前台控制台进行操作, 因此当前控制台
    // currcons 必须是前台控制台时, 我们才需要设置其滚屏起始行对应的内存起点位置.

    if (video_type != VIDEO_TYPE_EGAC && video_type != VIDEO_TYPE_EGAM) {
        return;
    }

    if (currcons != fg_console) {
        return;
    }

    /* R12 输出的 `>> (8+1)` 表示先移动 8 位, 然后除以 2 即先得到高字节, 然后需要把
     * 高字节除以 2 得到真实的位置, 后面的 `>> 1` 同理 */

    /* 屏幕显示的开始内存地址写在 R12, R13 */

    cli();
    outb_p(12, video_port_reg); /* 选择数据寄存器 r12, 输出滚屏起始位置高字节 */
    outb_p(0xff & ((origin - video_mem_base) >> (8 + 1)), video_port_val);
    outb_p(13, video_port_reg); /* 选择数据寄存器 r13, 输出滚屏起始位置低字节 */
    outb_p(0xff & ((origin - video_mem_base) >> 1), video_port_val);
    sti();
}

/**
 * @brief 向上滚动屏幕一行
 *
 * 这块不需要更新 top/bottom, 可以观察 `insert_line` 函数里面对 top/bottom 的使用方法
 *
 * @param currcons 控制台号
 */
static void scrup(int currcons)
{
    /* 滚屏区域顶行号大于等于区域底行号, 则不满足进行滚行操作的条件
     * top + video_num_lines = bottom */
    if (bottom <= top) {
        return;
    }

    /* 对于 EGA/VGA 卡, 我们可以指定屏内行范围(区域)进行滚屏操作
     * 而 MDA 单色显示卡只能进行整屏滚屏操作 */
    if (video_type == VIDEO_TYPE_EGAC || video_type == VIDEO_TYPE_EGAM) {
        /* 如果显示类型是 EGA, 则还分为整屏窗口移动和区域内窗口移动
         * if 条件为真是整屏移动的情况 */
        if (!top && bottom == video_num_lines) {
            origin += video_size_row;  /* 屏幕可视位置, 在显存里面移动到下一行开始位置 */
            pos += video_size_row;     /* 光标也移动到下一行位置 */
            scr_end += video_size_row; /* 屏幕可视结束位置, 也往后移动 */

            /* 如果最后位置超过了可用的显存区域, 重新从显存的开始区域显示内容 */
            if (scr_end > video_mem_end) {
                __asm__("cld\n\t"
                        "rep movsl\n\t"
                        "movl _video_num_columns,%1\n\t"
                        "rep stosw"
                        :
                        : "a"(video_erase_char),                               /* 擦除字符 */
                          "c"((video_num_lines - 1) * video_num_columns >> 1), /* 一屏少一行 */
                          "D"(video_mem_start), /* 拷贝到显存开始位置 */
                          "S"(origin)           /* 从原来的可视区域开始位置拷贝 */
                        : "cx", "di", "si");
                /* origin - video_mem_start 实际上是这次移动的跨度, 在纸上画画就容易理解 */
                scr_end -= origin - video_mem_start;
                pos -= origin - video_mem_start;
                origin = video_mem_start;
            } else {
                /* 把显存对应的位置, 写擦除字符即可
                 * 注意原来的显示内容, 其实都还保留着 */
                __asm__("cld\n\t"
                        "rep stosw" /* al -> ES:EDI */
                        :
                        : "a"(video_erase_char),        /* 擦除字符 */
                          "c"(video_num_columns),       /* 擦除长度 */
                          "D"(scr_end - video_size_row) /* 擦除目标 */
                        : "cx", "di");
            }
            set_origin(currcons); /* 更新屏幕 */
        } else {
            __asm__("cld\n\t"
                    "rep movsl\n\t"
                    "movl _video_num_columns, %%ecx\n\t"
                    "rep stosw"
                    :
                    : "a"(video_erase_char),                            /**/
                      "c"((bottom - top - 1) * video_num_columns >> 1), /**/
                      "D"(origin + video_size_row * top),               /**/
                      "S"(origin + video_size_row * (top + 1))
                    : "cx", "di", "si");
        }
    } else {
        /* Not EGA/VGA */
        __asm__(
            "cld\n\t"                            /* 正向拷贝 */
            "rep movsl\n\t"                      /* DS:ESI -> ES:EDI */
            "movl _video_num_columns, %%ecx\n\t" /* 全局变量的符号可直接嵌入指令, 无需通过输入/输出操作数传递 */
            "rep stosw"                          /* al -> ES:EDI */
            :
            : "a"(video_erase_char),                            /* al=擦除字符 */
              "c"((bottom - top - 1) * video_num_columns >> 1), /* 要拷贝的内存大小(除2) */
              "D"(origin + video_size_row * top),               /* 拷贝到这个位置(第 0 行) */
              "S"(origin + video_size_row * (top + 1))          /* 从这里开始拷贝(第 1 行) */
            : "cx", "di", "si");
    }
}

/**
 * @brief 向下滚动一行屏幕
 *
 * @param currcons 控制台号
 */
static void scrdown(int currcons)
{
    if (bottom <= top) {
        return;
    }

    /* 由于窗口向上移动最多移动到当前控制台占用显示区域内存的起始位置, 因此不会发生
     * 屏幕窗口末端所对应的显示内存指针 scr_end 超出实际显示内存末端的情况,
     * 所以这里只需要处理普通的内存数据移动情况
     *
     * TODO: 滚出来的第一行, 有可能不是显存内容吧? */

    /* 这里if和else语句块中的操作完全一样 */

    if (video_type == VIDEO_TYPE_EGAC || video_type == VIDEO_TYPE_EGAM) {
        __asm__(
            "std\n\t" /* 倒着拷贝, 以防出现数据覆盖的情况 */
            "rep movsl\n\t"
            "addl $2, %%edi\n\t" /* %edi has been decremented by 4 */
            "movl _video_num_columns, %%ecx\n\t"
            "rep stosw"
            :
            : "a"(video_erase_char),                            /*擦除*/
              "c"((bottom - top - 1) * video_num_columns >> 1), /* 一屏少一行 */
              "D"(origin + video_size_row * bottom - 4),        /* 到: 窗口右下角最后一个长字 */
              "S"(origin + video_size_row * (bottom - 1) - 4)   /* 从: 窗口倒数第2行最后一个长字 */
            : "ax", "cx", "di", "si");
    } else {
        /* Not EGA/VGA */
        __asm__("std\n\t"
                "rep movsl\n\t"
                "addl $2, %%edi\n\t" /* %edi has been decremented by 4 */
                "movl _video_num_columns,%%ecx\n\t"
                "rep stosw"
                :
                : "a"(video_erase_char), /**/
                  "c"((bottom - top - 1) * video_num_columns >> 1),
                  "D"(origin + video_size_row * bottom - 4),
                  "S"(origin + video_size_row * (bottom - 1) - 4)
                : "ax", "cx", "di", "si");
    }
}

/**
 * @brief 处理换行
 *
 * 光标在同列位置下移一行
 *
 * @param currcons 控制台号
 */
static void lf(int currcons)
{
    /* 先判断换行之后, 是不是超出了屏幕 */
    if (y + 1 < bottom) {
        /* 如没有超出屏幕, 当前行号加一, 光标位置加一行 */
        y++;
        pos += video_size_row;
        return;
    }

    /* 如超出屏幕, 执行滚屏操作(将屏幕窗口内容上移一行) */
    scrup(currcons);
}

/**
 * @brief 光标在同列上移一行
 *
 * 函数名称 ri (reverse index 反向索引) 是指控制字符 RI 或转义序列 `ESC M`
 *
 * @param currcons
 */
static void ri(int currcons)
{
    /* 移动完成之后没有超出可视屏幕范围, 直接操作即可 */
    if (y > top) {
        y--;
        pos -= video_size_row;
        return;
    }

    /* 向下滚屏(将屏幕窗口内容下移一行) */
    scrdown(currcons);
}

/**
 * @brief 光标回到 0 列
 *
 * @param currcons 控制台号
 */
static void cr(int currcons)
{
    /* x 是当前光标所在列对应的列数, 考虑到字符属性, x*2 就是占用的具体内存数量
     * 因此这里给 pos 先减去具体列数, 然后给 x 赋值 0 */
    pos -= x << 1;
    x = 0;
}

/**
 * @brief 擦除光标前一字符(用空格替代)
 *
 * 如果光标已经在 0 列, 此函数没有动作(也即不能支持跨行的擦除)
 *
 * @param currcons
 */
static void del(int currcons)
{
    if (x) {
        pos -= 2;
        x--;

        /* 使用擦除字符填充要擦除的位置 */
        *(unsigned short *)pos = video_erase_char;
    }
}

/**
 * @brief 删除屏幕上与光标位置相关的部分
 *
 * 本函数根据指定的控制序列具体参数值, 执行与光标位置相关的删除操作,
 * 并且在擦除字符或行时光标位置不变
 *
 * ANSI 控制序列: `ESC [ Ps J`
 *
 * Ps 含义:
 *  - 0: 删除光标处到屏幕底端
 *  - 1: 删除屏幕开始到光标处
 *  - 2: 整屏删除
 *
 * 函数名称里面的 CSI 表示 `控制序列引导码(Control Sequence Introducer)`
 *
 * @param currcons 控制台号
 * @param vpar 对应上面控制序列中 Ps 的值
 */
static void csi_J(int currcons, int vpar)
{
    long count __asm__("cx"); /* 指定为寄存器变量 */
    long start __asm__("di");

    switch (vpar) {
    case 0: /* erase from cursor to end of display */
        count = (scr_end - pos) >> 1;
        start = pos;
        break;
    case 1: /* erase from start to cursor */
        count = (pos - origin) >> 1;
        start = origin;
        break;
    case 2: /* erase whole display */
        count = video_num_columns * video_num_lines;
        start = origin;
        break;
    default:
        return;
    }

    __asm__("cld\n\t"
            "rep stosw\n\t"
            :
            : "c"(count), "D"(start), "a"(video_erase_char)
            : "cx", "di");
}

/**
 * @brief 删除一行上与光标位置相关的部分
 *
 * 本函数根据参数擦除光标所在行的部分或所有字符, 在擦除字符或行时光标位置不变
 *
 * ANSI 转义字符序列: `ESC [ Ps K`
 * Ps 含义:
 *  - 0: 删除到行尾
 *  - 1: 从开始删除
 *  - 2: 整行都删除
 *
 * @param currcons
 * @param vpar 对应上面控制序列中Ps的值
 */
static void csi_K(int currcons, int vpar)
{
    long count __asm__("cx");
    long start __asm__("di");

    switch (vpar) {
    case 0: /* erase from cursor to end of line */
        if (x >= video_num_columns) {
            return;
        }

        count = video_num_columns - x;
        start = pos;
        break;
    case 1: /* erase from start of line to cursor */
        start = pos - (x << 1);
        count = (x < video_num_columns) ? x : video_num_columns;
        break;
    case 2: /* erase whole line */
        start = pos - (x << 1);
        count = video_num_columns;
        break;
    default:
        return;
    }

    __asm__("cld\n\t"
            "rep stosw\n\t"
            :
            : "c"(count), "D"(start), "a"(video_erase_char)
            : "cx", "di");
}

/**
 * @brief 设置显示字符属性
 *
 * ANSI 转义序列: `ESC [ Ps;Ps m`
 * Ps 含义:
 *  - 0: 默认属性
 *  - 1: 粗体并增亮
 *  - 4: 下划线
 *  - 5: 闪烁
 *  - 7: 反显
 *  - 22: 非粗体
 *  - 24: 无下划线
 *  - 25: 无闪烁
 *  - 27: 正显
 *  - 30~38: 设置前景色彩
 *  - 39: 默认前景色(White)
 *  - 40~48: 设置背景色彩
 *  - 49: 默认背景色(Black)
 *
 * 该控制序列根据参数设置字符显示属性. 以后所有发送到终端的字符都将使用这里指定的属
 * 性, 直到再次执行本控制序列重新设置字符显示的属性
 *
 * @param currcons 控制台号
 */
void csi_m(int currcons)
{
    int i;

    /* 一个控制序列中可以带有多个不同参数. 参数存储在数组 par[] 中
     * 根据接收到的参数个数 npar, 循环处理各个参数 Ps */
    for (i = 0; i <= npar; i++) {
        switch (par[i]) {
        case 0:
            /* 把当前虚拟控制台随后显示的字符属性设置为默认属性 def_attr.
             * 初始化时 def_attr 已被设置成黑底白字(0x07) */
            attr = def_attr;
            break; /* default */
        case 1:
            /* 把当前虚拟控制台随后显示的字符属性设置为粗体或增亮显示
             * 如果是彩色显示, 则把字符属性或上 0x08 让字符高亮度显示
             * 如果是单色显示, 则把字符属性或上 0x0f 让字符带下划线显示 */
            attr = (iscolor ? (attr | 0x08) : (attr | 0x0f));
            break;                        /* bold */
        /*case 4: attr=attr|0x01;break;*/ /* underline */
        case 4:                           /* bold */

            /* 对彩色和单色显示进行不同的处理 */
            if (!iscolor) {
                attr |= 0x01; /* 单色让字符带下划线显示 */
            } else {          /* check if forground == background */
                /* 如果是彩色显示, 那么若原来 vc_bold_attr 不等于 -1 时就复位其背景色
                 * 否则的话就把前景色取反. 若取反后前景色与背景色相同, 就把前景色增1而取另一种颜色 */
                if (vc_cons[currcons].vc_bold_attr != -1) {
                    attr = (vc_cons[currcons].vc_bold_attr & 0x0f) | (0xf0 & (attr));
                } else {
                    short newattr = (attr & 0xf0) | (0xf & (~attr));
                    attr = ((newattr & 0xf) == ((attr >> 4) & 0xf)
                                ? (attr & 0xf0) | (((attr & 0xf) + 1) % 0xf)
                                : newattr);
                }
            }
            break;

            // 如果Ps = 5, 则把当前虚拟控制台随后显示的字符设置为闪烁, 即把属性字节比特位7置1.
            // 如果Ps = 7, 则把当前虚拟控制台随后显示的字符设置为反显, 即把前景和背景色交换.
            // 如果Ps = 22, 则取消随后字符的高亮度显示 (取消粗体显示) .
            // 如果Ps = 24, 则对于单色显示是取消随后字符的下划线显示, 对于彩色显示则是取消绿色.
            // 如果Ps = 25, 则取消随后字符的闪烁显示.
            // 如果Ps = 27, 则取消随后字符的反显.
            // 如果Ps = 39, 则复位随后字符的前景色为默认前景色 (白色) .
            // 如果Ps = 49, 则复位随后字符的背景色为默认背景色 (黑色) .
        case 5:
            attr = attr | 0x80;
            break; /* blinking */
        case 7:
            attr = (attr << 4) | (attr >> 4);
            break; /* negative */
        case 22:
            attr = attr & 0xf7;
            break; /* not bold */
        case 24:
            attr = attr & 0xfe;
            break; /* not underline */
        case 25:
            attr = attr & 0x7f;
            break; /* not blinking */
        case 27:
            attr = def_attr;
            break; /* positive image */
        case 39:
            attr = (attr & 0xf0) | (def_attr & 0x0f);
            break;
        case 49:
            attr = (attr & 0x0f) | (def_attr & 0xf0);
            break;
        default:
            // 当Ps (par[i]) 为其他值时, 则是设置指定的前景色或背景色. 如果Ps = 30..37, 则是设置
            // 前景色; 如果Ps=40..47, 则是设置背景色. 有关颜色值请参见程序后说明.
            if (!can_do_colour) {
                break;
            }
            iscolor = 1;
            if ((par[i] >= 30) && (par[i] <= 38)) {
                attr = (attr & 0xf0) | (par[i] - 30);
            } else { /* Background color */
                if ((par[i] >= 40) && (par[i] <= 48)) {
                    attr = (attr & 0x0f) | ((par[i] - 40) << 4);
                } else {
                    break;
                }
            }
        }
    }
}

/**
 * @brief 设置显示控制器中光标显示内存位置
 *
 * @param currcons
 */
static inline void set_cursor(int currcons)
{
    /* 既然我们需要设置显示光标, 说明有键盘操作, 因此需要恢复进行黑屏操作的延时计数值 */
    blankcount = blankinterval;

    /* 显示光标的控制台必须是前台控制台, 因此若当前处理的台号 currcons 不是前台控制台就立刻返回. */
    if (currcons != fg_console) {
        return;
    }

    /* 光标位置写到 R14, R15 里面 */

    cli();
    outb_p(14, video_port_reg);
    outb_p(0xff & ((pos - video_mem_base) >> 9), video_port_val);
    outb_p(15, video_port_reg);
    outb_p(0xff & ((pos - video_mem_base) >> 1), video_port_val);
    sti();
}

/**
 * @brief 隐藏光标
 *
 * 把光标设置到当前虚拟控制台窗口的末端, 起到隐藏光标的作用
 *
 * @param currcons 当前控制台
 */
static inline void hide_cursor(int currcons)
{
    outb_p(14, video_port_reg);
    outb_p(0xff & ((scr_end - video_mem_base) >> 9), video_port_val);
    outb_p(15, video_port_reg);
    outb_p(0xff & ((scr_end - video_mem_base) >> 1), video_port_val);
}

/**
 * @brief 发送对 VT100 的响应序列
 *
 * 即为响应主机请求终端, 向主机发送设备属性(DA).
 * 主机通过发送不带参数或参数是 0 的DA控制序列(`ESC [ 0c` 或 `ESC Z`)要求终端发送一个
 * 设备属性(DA)控制序列, 终端则发送定义好的 `RESPONSE` 作为应答序列 (即 `ESC [?1;2c`)
 * 来响应主机的序列, 该序列告诉主机本终端是具有高级视频功能的 VT100 兼容终端
 *
 * @param currcons 控制台号
 * @param tty 控制台关联的 tty 设备
 */
static void respond(int currcons, struct tty_struct *tty)
{
    char *p = RESPONSE;

    cli();

    while (*p) {
        PUTCH(*p, tty->read_q); /* 将应答序列放入读队列 */
        p++;
    }

    sti();
    copy_to_cooked(tty); /* 转换成规范模式放到辅助队列 */
}

/**
 * @brief 在光标处插入一空格字符
 *
 * 把光标开始处的所有字符右移一格, 并将擦除字符插入在光标所在处
 *
 * TODO: 这岂不把当前行最后一个字符弄丢了?
 *
 * @param currcons 控制台号
 */
static void insert_char(int currcons)
{
    int i = x;
    unsigned short tmp, old = video_erase_char;
    unsigned short *p = (unsigned short *)pos;

    while (i++ < video_num_columns) {
        tmp = *p;
        *p = old;
        old = tmp;
        p++;
    }
}

/**
 * @brief 在光标处插入一行
 *
 * 将屏幕窗口从光标所在行到窗口底的内容向下卷动一行, 并光标将处在新的空行上
 *
 * @param currcons 控制台号
 */
static void insert_line(int currcons)
{
    int oldtop, oldbottom;

    oldtop = top;
    oldbottom = bottom;
    top = y;
    bottom = video_num_lines;
    scrdown(currcons);
    top = oldtop;
    bottom = oldbottom;
}

/**
 * @brief 删除光标处一字符
 *
 * 删除光标处的一个字符, 光标右边的所有字符左移一格
 *
 * @param currcons 控制台号
 */
static void delete_char(int currcons)
{
    int i;
    unsigned short *p = (unsigned short *)pos;

    // 如果光标的当前列位置 x 超出屏幕最右列, 则返回
    if (x >= video_num_columns) {
        return;
    }

    /* 从光标右一个字符开始到行末所有字符左移一格 */
    i = x;
    while (++i < video_num_columns) {
        *p = *(p + 1);
        p++;
    }

    *p = video_erase_char; /* 最后末尾位置填一个擦除字符 */
}

/**
 * @brief 删除光标所在行
 *
 * 删除光标所在的一行, 并从光标所在行开始屏幕内容上卷一行
 *
 * @param currcons 控制台号
 */
static void delete_line(int currcons)
{
    int oldtop, oldbottom;

    oldtop = top;
    oldbottom = bottom;
    top = y;
    bottom = video_num_lines;
    scrup(currcons);
    top = oldtop;
    bottom = oldbottom;
}

/**
 * @brief 在光标处插入 nr 个字符
 *
 * ANSI 转义字符序列: `ESC [ Pn @` 用于在当前光标处插入 1 个或多个空格字符
 * Pn 是插入的字符数, 默认是1
 * 操作后光标将仍然处于第 1 个插入的空格字符处, 在光标与右边界的字符将右移, 超过右边界的字符将被丢失
 *
 * @param currcons 控制台号
 * @param nr 转义字符序列中的参数 Pn
 */
static void csi_at(int currcons, unsigned int nr)
{
    /* 理论上 nr 最大也就是 video_num_columns
     * 再多的插入都是没有肉眼可见的效果的 */
    if (nr > video_num_columns) {
        nr = video_num_columns;
    } else if (!nr) {
        nr = 1;
    }

    while (nr--) {
        insert_char(currcons);
    }
}

/**
 * @brief 在光标位置处插入 nr 行
 *
 * ANSI 转义字符序列 `ESC [ Pn L` 在光标处插入 1 行或多行空行
 * 操作完成后光标位置不变
 *
 * 当空行被插入时, 光标以下滚动区域内的行向下移动, 滚动出显示页的行就丢失
 *
 * @param currcons 控制台号
 * @param nr 转义字符序列中的参数 Pn
 */
static void csi_L(int currcons, unsigned int nr)
{
    if (nr > video_num_lines) {
        nr = video_num_lines;
    } else if (!nr) {
        nr = 1;
    }

    while (nr--) {
        insert_line(currcons);
    }
}

/**
 * @brief 删除光标处的 nr 个字符
 *
 * ANSI 转义序列: `ESC [ Pn P` 从光标处删除 Pn 个字符
 *
 * 当一个字符被删除时, 光标右所有字符都左移. 这会在右边界处产生一个空字符.
 * 其属性应该与最后一个左移字符相同, 但这里作了简化处理, 仅使用字符的默认
 * 属性(黑底白字空格 0x0720)来设置空字符
 *
 * @param currcons 控制台号
 * @param nr 转义字符序列中的参数 Pn
 */
static void csi_P(int currcons, unsigned int nr)
{
    if (nr > video_num_columns) {
        nr = video_num_columns;
    } else if (!nr) {
        nr = 1;
    }

    while (nr--) {
        delete_char(currcons);
    }
}

/**
 * @brief 删除光标处的 nr 行
 * ANSI 转义序列: `ESC [ Pn M` 在滚动区域内, 从光标所在行开始删除 1 行或多行
 * 当行被删除时, 滚动区域内的被删行以下的行会向上移动, 并且会在最底行添加 1 空行
 * 若 Pn 大于显示页上剩余行数, 则本序列仅删除这些剩余行, 并对滚动区域外不起作用
 *
 * @param currcons 控制台号
 * @param nr 转义字符序列中的参数 Pn
 */
static void csi_M(int currcons, unsigned int nr)
{
    if (nr > video_num_lines) {
        nr = video_num_lines;
    } else if (!nr) {
        nr = 1;
    }

    while (nr--) {
        delete_line(currcons);
    }
}

/**
 * @brief 保存当前光标位置
 *
 * @param currcons
 */
static void save_cur(int currcons)
{
    saved_x = x;
    saved_y = y;
}

/**
 * @brief 恢复保存的光标位置
 *
 * @param currcons
 */
static void restore_cur(int currcons)
{
    gotoxy(currcons, saved_x, saved_y);
}

/* 这个枚举定义用于下面 con_write 函数中处理转义序列或控制序列的解析, ESnormal 是初
 * 始进入状态, 也是转义或控制序列处理完毕时的状态
 *
 * 转移序列的更多资料参考这里: https://teratermproject.github.io/manual/4/en/about/ctrlseq.html
 *
 * ESnormal -  表示处于初始正常状态. 此时若接收到的是普通显示字符, 则把字符直接显示
 *             在屏幕上; 若接收到的是控制字符 (例如回车字符) , 则对光标位置进行设置.
 *             当刚处理完一个转义或控制序列, 程序也会返回到本状态.
 * ESesc    -  表示接收到转义序列引导字符 ESC (0x1b = 033 = 27); 如果在此状态下接收
 *             到一个'['字符, 则说明转义序列引导码, 于是跳转到ESsquare去处理. 否则
 *             就把接收到的字符作为转义序列来处理. 对于选择字符集转义序列'ESC (' 和
 *             'ESC )', 我们使用单独的状态ESsetgraph来处理; 对于设备控制字符串序列
 *             'ESC P', 我们使用单独的状态ESsetterm来处理.
 * ESsquare -  表示已经接收到一个控制序列引导码('ESC [') , 表示接收到的是一个控制序
 *             列. 于是本状态执行参数数组 par[] 清零初始化工作. 如果此时接收到的又是一
 *             个'['字符, 则表示收到了'ESC [['序列. 该序列是键盘功能键发出的序列, 于
 *             是跳转到 Esfunckey 去处理. 否则我们需要准备接收控制序列的参数, 于是置
 *             状态Esgetpars并直接进入该状态去接收并保存序列的参数字符.
 * ESgetpars - 该状态表示我们此时要接收控制序列的参数值. 参数用十进制数表示, 我们把
 *             接收到的数字字符转换成数值并保存到 par[] 数组中. 如果收到一个分号 ';',
 *             则还是维持在本状态, 并把接收到的参数值保存在数据 par[] 下一项中. 若不是
 *             数字字符或分号, 说明已取得所有参数, 那么就转移到状态 ESgotpars 去处理.
 * ESgotpars - 表示我们已经接收到一个完整的控制序列. 此时我们可以根据本状态接收到的结
 *             尾字符对相应控制序列进行处理. 不过在处理之前, 如果我们在ESsquare 状态
 *             收到过 '?', 说明这个序列是终端设备私有序列. 本内核不对支持对这种序列的
 *             处理, 于是我们直接恢复到 ESnormal 状态. 否则就去执行相应控制序列. 待序
 *             列处理完后就把状态恢复到 ESnormal.
 * ESfunckey - 表示我们接收到了键盘上功能键发出的一个序列. 我们不用显示. 于是恢复到正
 *             常状态ESnormal.
 * ESsetterm - 表示处于设备控制字符串序列状态 (DCS) . 此时若收到字符 'S', 则恢复初始
 *             的显示字符属性. 若收到的字符是'L'或'l', 则开启或关闭折行显示方式.
 * ESsetgraph -表示收到设置字符集转移序列'ESC (' 或 'ESC )'. 它们分别用于指定G0和G1
 *             所用的字符集. 此时若收到字符 '0', 则选择图形字符集作为G0和G1, 若收到
 *             的字符是 'B', 这选择普通ASCII字符集作为G0和G1的字符集.
 */
enum {
    ESnormal,  /* 正常状态 */
    ESesc,     /* 收到转义序列引导字符 `ESC` */
    ESsquare,  /* 收到一个控制序列引导码 `ESC [` */
    ESgetpars, /* 准备接收控制序列的参数值 */
    ESgotpars, /* 已经接收到一个完整的控制序列 */
    ESfunckey, /* 接收到了键盘上功能键发出的一个序列 */
    ESsetterm, /* 处于设备控制字符串序列状态(DCS) */
    ESsetgraph /* 收到设置字符集转移序列 `ESC (` */
};

/**
 * @brief 控制台写函数
 *
 * 从终端对应的 tty 写缓冲队列中取字符, 针对每个字符进行分析. 若是控制字符或转义或控制
 * 序列, 则进行光标定位、字符删除等的控制处理; 对于普通字符就直接在光标处显示
 *
 * @param tty 当前控制台使用的 tty 结构指针
 */
void con_write(struct tty_struct *tty)
{
    int nr;
    char c;
    int currcons;

    currcons = tty - tty_table; /* tty 对应的是那个控制台 */
    if ((currcons >= MAX_CONSOLES) || (currcons < 0)) {
        panic("con_write: illegal tty");
    }

    /* 缓冲区中字符数量 */
    nr = CHARS(tty->write_q);
    while (nr--) {
        if (tty->stopped) {
            /* 控制台由于接收到键盘或程序发出的暂停命令(如按键Ctrl-S)而处于
             * 停止状态, 就停止处理写队列中的字符 */
            break;
        }

        GETCH(tty->write_q, c);

        /* 24 对应的是 ^X, CAN, 也即 Cancel 字符
         * 26 对应的是 ^Z, SUB, 也即 Substitute 字符 */
        if (c == 24 || c == 26) {
            state = ESnormal;
        }

        // 如果取出的是控制字符 CAN 或 SUB, 那么若是在转义或控制序列期间收到的, 则序列不会执行而立刻终
        // 止, 同时显示随后的字符. 注意, con_write()函数只处理取队列字符数时写队列中当前含有
        // 的字符. 这有可能在一个序列被放到写队列期间读取字符数, 因此本函数前一次退出时state
        // 有可能正处于处理转义或控制序列的其他状态上.

        switch (state) {
        case ESnormal:
            if (c > 31 && c < 127) {
                /* 可打印字符 */
                if (x >= video_num_columns) {
                    x -= video_num_columns;
                    pos -= video_size_row;
                    lf(currcons);
                }

                __asm__("movb %2, %%ah\n\t"
                        "movw %%ax, %1\n\t"
                        :
                        : "a"(translate[c - 32]), /* 转成对应的 ASCII 字符 */
                          "m"(*(short *)pos),     /* 输出的位置 */
                          "m"(attr)               /* 字符属性 */
                        : "ax");
                pos += 2;
                x++;
            } else if (c == 27) {
                /* 转移控制字符(Escape) */
                state = ESesc;
            } else if (c == 10 || c == 11 || c == 12) {
                /* 换行(Line Feed), 垂直制表(Line Tabulation), 换页(Form Feed) */
                lf(currcons);
            } else if (c == 13) {
                /* 回车(Carriage Return) */
                cr(currcons);
            } else if (c == ERASE_CHAR(tty)) {
                /* c 是擦除字符, 光标左边字符擦除(用空格字符替代), 并将光标移到被擦除位置
                 * 擦除字符是可以配置的, INIT_C_CC 默认的配置是 0177, 也就是 127 号
                 * ascii 字符, 含义是 Delete */
                del(currcons);
            } else if (c == 8) {
                /* 退格键(Backspace) */
                if (x) {
                    x--;
                    pos -= 2;
                }
            } else if (c == 9) {
                /* 制表符(Character Tabulation) */
                c = 8 - (x & 7); /* 对齐到 8 倍数列位置 */
                x += c;
                pos += c << 1;
                /* 光标列数超出屏幕最大列数, 则将光标移到下一行上
                 * NOTICE: 这里 x 不是从下一行的第一列开始 */
                if (x > video_num_columns) {
                    x -= video_num_columns;
                    pos -= video_size_row;
                    lf(currcons);
                }
                c = 9;
            } else if (c == 7) {
                /* 报警/警铃(Bell/Alert) */
                sysbeep();
            } else if (c == 14) {
                /* Shift Out, 选择字符集 G1 显示字符集 */
                translate = GRAF_TRANS;
            } else if (c == 15) {
                /* Shift In, 选择字符集 G0 显示字符集 */
                translate = NORM_TRANS;
            }
            break;
        case ESesc:
            /* 收到 Escape(27) 字符, 状态变为 ESesc */
            state = ESnormal; /* 大概率下一个状态是 ESnormal, 如果不是在 case 分支里面单独设置 */
            switch (c) {
            case '[':
                state = ESsquare; /* `ESC [`, 标识是 `控制序列导入器(CSI)` 序列 */
                break;
            case 'E': /* Next Line, 光标下移 1 行回到 0 列 */
                gotoxy(currcons, 0, y + 1);
                break;
            case 'M': /* Reverse Index, 光标上移 1 行 */
                ri(currcons);
                break;
            case 'D': /* Index, 光标下移一行 */
                lf(currcons);
                break;
            case 'Z': /* Reports a Terminal ID */
                respond(currcons, tty);
                break;
            case '7': /* 保存光标位置信息 */
                save_cur(currcons);
                break;
            case '8': /* 恢复光标位置信息 */
                restore_cur(currcons);
                break;
            case '(':
            case ')': /* Designates(指定) character set */
                state = ESsetgraph;
                break;
            case 'P': /* Device Control String */
                state = ESsetterm;
                break;
            case '#': /* 修改整行属性 */
                state = -1;
                break;
            case 'c': /* Terminal Full Reset */
                tty->termios = DEF_TERMIOS;
                state = restate = ESnormal;
                checkin = 0;
                top = 0;
                bottom = video_num_lines;
                break;
                /* case '>':   Numeric keypad */
                /* case '=':   Appl. keypad */
            }
            break;
        case ESsquare: /* 处理 CSI 序列 */
            /* 清空 par 数组 */
            for (npar = 0; npar < NPAR; npar++) {
                par[npar] = 0;
            }

            npar = 0;
            state = ESgetpars;

            /* 如果此时接收到的字符还是'[', 那么表明收到了键盘功能键发出的序列,
             * 设置下一状态为 ESfunckey */
            if (c == '[') {
                state = ESfunckey;
                break;
            }

            /* 接收到的字符是 '?', 说明这个序列是终端设备私有序列, 后面会有一个功能字符
             * 于是去读下一字符, 再到状态 ESgetpars 去处理代码处. */
            if (ques = (c == '?')) {
                break;
            }
        case ESgetpars:
            if (c == ';' && npar < NPAR - 1) {
                /* 收到一个分号 ';', 则还是维持在本状态, 准备接受下一个参数 */
                npar++;
                break;
            } else if (c >= '0' && c <= '9') {
                /* 具体的参数值 */
                par[npar] = 10 * par[npar] + c - '0';
                break;
            } else {
                /* 不是数字字符或分号, 说明已取得所有参数, 转移到状态 ESgotpars 去处理 */
                state = ESgotpars;
            }
        case ESgotpars:
            /* 下一个状态大概率是 ESnormal */
            state = ESnormal;

            /* 如果是终端设备私有序列, 这里不做任何处理 */
            if (ques) {
                ques = 0;
                break;
            }

            switch (c) {
            case 'G':
            case '`': /* Moves cursor to the Ps-th column of the active line */
                if (par[0]) {
                    par[0]--;
                }
                gotoxy(currcons, par[0], y);
                break;
            case 'A': /* Moves cursor up Ps lines in the same column */
                if (!par[0]) {
                    par[0]++;
                }
                gotoxy(currcons, x, y - par[0]);
                break;
            case 'B':
            case 'e': /* Moves cursor down Ps lines in the same column. */
                if (!par[0]) {
                    par[0]++;
                }
                gotoxy(currcons, x, y + par[0]);
                break;
            case 'C':
            case 'a': /* Moves cursor to the right Ps columns */
                if (!par[0]) {
                    par[0]++;
                }
                gotoxy(currcons, x + par[0], y);
                break;
            case 'D': /* Moves cursor to the left Ps columns */
                if (!par[0]) {
                    par[0]++;
                }
                gotoxy(currcons, x - par[0], y);
                break;
            case 'E': /* Moves cursor to the first column of Ps-th following line */
                if (!par[0]) {
                    par[0]++;
                }
                gotoxy(currcons, 0, y + par[0]);
                break;
            case 'F': /* Moves cursor to the first column of Ps-th preceding line */
                if (!par[0]) {
                    par[0]++;
                }
                gotoxy(currcons, 0, y - par[0]);
                break;
            case 'd':
                /* Move to the corresponding vertical position (line Ps) of the current column */
                if (par[0]) {
                    par[0]--;
                }
                gotoxy(currcons, x, par[0]);
                break;
            case 'H':
            case 'f': /* Moves cursor to the Ps1-th line and to the Ps2-th column */
                if (par[0]) {
                    par[0]--;
                }
                if (par[1]) {
                    par[1]--;
                }
                gotoxy(currcons, par[1], par[0]);
                break;
            case 'J': /* Erase in display */
                csi_J(currcons, par[0]);
                break;
            case 'K': /* Erase in line */
                csi_K(currcons, par[0]);
                break;
            case 'L': /* Inserts Ps lines, starting at the cursor */
                csi_L(currcons, par[0]);
                break;
            case 'M':
                /* Deletes Ps lines in the scrolling region, starting with the line that has the cursor */
                csi_M(currcons, par[0]);
                break;
            case 'P': /* Deletes Ps characters from the cursor position to the right */
                csi_P(currcons, par[0]);
                break;
            case '@': /* Insert Ps space (SP) characters starting at the cursor position */
                csi_at(currcons, par[0]);
                break;
            case 'm': /* Select character attributes */
                csi_m(currcons);
                break;
            case 'r': /* 用两个参数设置滚屏的起始行号和终止行号 */
                if (par[0]) {
                    par[0]--;
                }
                if (!par[1]) {
                    par[1] = video_num_lines;
                }
                if (par[0] < par[1] && par[1] <= video_num_lines) {
                    top = par[0];
                    bottom = par[1];
                }
                break;
            case 's': /* Save cursor position */
                save_cur(currcons);
                break;
            case 'u': /* Restore cursor position */
                restore_cur(currcons);
                break;
            case 'l': /* blank interval */
            case 'b': /* bold attribute */
                /* 'l' 或 'b' 分别表示设置屏幕黑屏间隔时间和设置粗体字符显示
                 * 此时参数数组中 par[1] 和 par[2] 是特征值, 它们分别必须为:
                 *   par[1] = par[0] + 13
                 *   par[2] = par[0] + 17
                 * 在这个条件下:
                 *  - 如果 c 是字符 'l', 那么 par[0] 中是开始黑屏时说延迟的分钟数
                 *  - 如果 c 是字符 'b', 那么 par[0] 中是设置的粗体字符属性值 */
                if (!((npar >= 2) && ((par[1] - 13) == par[0]) && ((par[2] - 17) == par[0]))) {
                    break;
                }

                if ((c == 'l') && (par[0] >= 0) && (par[0] <= 60)) {
                    blankinterval = HZ * 60 * par[0];
                    blankcount = blankinterval;
                }

                if (c == 'b') {
                    vc_cons[currcons].vc_bold_attr = par[0];
                }
            }
            break;
        case ESfunckey:
            /* 接收到了键盘上功能键发出的一个序列, 不用显示
             * 直接恢复到正常状态 ESnormal */
            state = ESnormal;
            break;
        case ESsetterm: /* Setterm functions. */
            /* 状态ESsetterm表示 处于 `设备控制字符串序列(Device Control String, DCS)` 状态
             * 此时若收到字符 'S', 则恢复初始的显示字符属性
             * 若收到的字符是 'L' 或 'l', 则开启或关闭折行显示方式. */
            state = ESnormal;
            if (c == 'S') {
                def_attr = attr;
                video_erase_char = (video_erase_char & 0x0ff) | (def_attr << 8);
            } else if (c == 'L') {
                ; /*linewrap on*/
            } else if (c == 'l') {
                ; /*linewrap off*/
            }
            break;
        case ESsetgraph:
            /* 收到设置字符集转移序列'ESC (' 或 'ESC )'. 它们分别用于指定 G0 和 G1 所用的字符集 */
            state = ESnormal;
            if (c == '0') {
                /* 收到字符 '0', 则选择图形字符集作为 G0 和 G1 */
                translate = GRAF_TRANS;
            } else if (c == 'B') {
                /* 收到字符 'B', 则选择普通 ASCII 字符集作为 G0 和 G1 的字符集 */
                translate = NORM_TRANS;
            }
            break;
        default:
            state = ESnormal;
        }
    }

    /* 根据上面设置的光标位置, 设置显示控制器中光标位置 */
    set_cursor(currcons);
}

/**
 * @brief 初始化控制台中断
 *
 * void con_init(void);
 *
 * This routine initalizes console interrupts, and does nothing
 * else. If you want the screen to clear, call tty_write with
 * the appropriate escape-sequece.
 *
 * Reads the information preserved by setup.s to determine the current display
 * type and sets everything accordingly.
 *
 * 这个子程序初始化控制台中断, 其他什么都不做
 * 如果你想让屏幕干净的话, 就使用适当的转义字符序列调用 tty_write 函数
 *
 * 读取 setup.s 程序保存的信息, 用以确定当前显示器类型, 并且设置所有相关参数
 */
void con_init(void)
{
    register unsigned char a;
    char *display_desc = "????";
    char *display_ptr;
    int currcons = 0; /* 当前虚拟控制台号 */
    long base, term;
    long video_memory;

    video_num_columns = ORIG_VIDEO_COLS;    /* 显示器显示字符列数 */
    video_size_row = video_num_columns * 2; /* 每行字符需使用的字节数 */
    video_num_lines = ORIG_VIDEO_LINES;     /* 显示器显示字符行数 */
    video_page = ORIG_VIDEO_PAGE;           /* 当前显示页面 */
    video_erase_char = 0x0720;              /* 擦除字符(0x20是空格字符, 0x07属性) */
    blankcount = blankinterval;             /* 默认的黑屏间隔时间(嘀嗒数) */

    /* 然后根据显示模式是单色还是彩色分别设置所使用的显示内存起始位置
     * 以及显示寄存器索引端口号和显示寄存器数据端口号
     * 如果获得的 BIOS 显示方式等于 7, 则表示是单色显示卡 */
    if (ORIG_VIDEO_MODE == 7) {   /* Is this a monochrome display? */
        video_mem_base = 0xb0000; /* 设置单显映像内存起始地址 */
        video_port_reg = 0x3b4;   /* 设置单显索引寄存器端口 */
        video_port_val = 0x3b5;   /* 设置单显数据寄存器端口 */

        /* 接着我们根据 BIOS 中断 int 0x10 功能 0x12 获得的显示模式信息, 判断显示卡是单色显示卡
         * 还是彩色显示卡. 若使用上述中断功能所得到的 BX 寄存器返回值不等于 0x10, 则说明是 EGA
         * 卡. 因此初始显示类型为 EGA 单色. 虽然 EGA 卡上有较多显示内存, 但在单色方式下最多只
         * 能利用地址范围在 0xb0000 ~ 0xb8000 之间的显示内存. 然后置显示器描述字符串为 'EGAm'.
         * 并会在系统初始化期间显示器描述字符串将显示在屏幕的右上角.
         * 注意, 这里使用了 bx 在调用中断 int 0x10 前后是否被改变的方法来判断卡的类型. 若 BL 在
         * 中断调用后值被改变, 表示显示卡支持 Ah=12h 功能调用, 是 EGA 或后推出来的 VGA 等类型的
         * 显示卡. 若中断调用返回值未变, 表示显示卡不支持这个功能, 则说明是一般单色显示卡 */
        if ((ORIG_VIDEO_EGA_BX & 0xff) != 0x10) {
            video_type = VIDEO_TYPE_EGAM; /* 设置显示类型(EGA单色) */
            video_mem_term = 0xb8000;     /* 设置显示内存末端地址 */
            display_desc = "EGAm";        /* 设置显示描述字符串 */
        } else {
            video_type = VIDEO_TYPE_MDA; /* 设置显示类型(MDA单色) */
            video_mem_term = 0xb2000;    /* 设置显示内存末端地址 */
            display_desc = "*MDA";       /* 设置显示描述字符串 */
        }
    } else { /* If not, it is color. */
        /* 如果显示方式不为 7, 说明是彩色显示卡. 此时:
         *  1. 文本方式下所用显示内存起始地址为0xb8000
         *  2. 显示控制索引寄存器端口地址为 0x3d4
         *  3. 数据寄存器端口地址为 0x3d5 */
        can_do_colour = 1;
        video_mem_base = 0xb8000;
        video_port_reg = 0x3d4;
        video_port_val = 0x3d5;

        if ((ORIG_VIDEO_EGA_BX & 0xff) != 0x10) {
            video_type = VIDEO_TYPE_EGAC; /* 设置显示类型(EGA彩色) */
            video_mem_term = 0xc0000;
            display_desc = "EGAc";
        } else {
            video_type = VIDEO_TYPE_CGA; /* 设置显示类型(CGA) */
            video_mem_term = 0xba000;
            display_desc = "*CGA";
        }
    }

    /* 现在我们来计算当前显示卡内存上可以开设的虚拟控制台数量
     * 硬件允许开设的虚拟控制台数量等于总显示内存量 video_memory 除以每个虚拟控制台
     * 占用的字节数. 每个虚拟控制台占用的显示内存数等于屏幕显示行数 video_num_lines
     * 乘上每行字符占有的字节数 video_size_row.
     * 如果硬件允许开设的虚拟控制台数量大于系统限定的最大数量 MAX_CONSOLES, 就把虚拟
     * 控制台数量设置为 MAX_CONSOLES. 若这样计算出的虚拟控制台数量为 0, 则设置为 1
     * 最后总显示内存数除以判断出的虚拟控制台数即得到每个虚拟控制台占用显示内存字节数 */

    video_memory = video_mem_term - video_mem_base; /* 可用显存大小 */

    /* 根据显存情况, 计算最多能支持的控制台数量 */
    NR_CONSOLES = video_memory / (video_num_lines * video_size_row);
    if (NR_CONSOLES > MAX_CONSOLES) {
        NR_CONSOLES = MAX_CONSOLES;
    }

    if (!NR_CONSOLES) {
        NR_CONSOLES = 1; /* 最少也要支持一个 */
    }

    /* 每个控制台可用的显存数量
     * 注意这里不是一个屏幕对应的字符数量, 使用多于一个屏幕的
     * 字符数量的内存空间, 可以做一些多页之类的高级操作 */
    video_memory /= NR_CONSOLES;

    /* Let the user known what kind of display driver we are using
     * 在屏幕第一行最后 4 个字符位置, 填充 display_desc 字符串 */
    display_ptr = ((char *)video_mem_base) + video_size_row - 8;
    while (*display_desc) {
        *display_ptr++ = *display_desc++;
        display_ptr++;
    }

    /* Initialize the variables used for scrolling (mostly EGA/VGA)
     * 初始化滚屏变量
     *
     * 此时当前虚拟控制台号 currcons 已被初始化为 0, 因此下面实际上是初始化 0 号虚拟控
     * 制台的结构 vc_cons[0] 中的所有字段值 */
    base = origin = video_mem_start = video_mem_base;             /* 默认滚屏开始内存位置 */
    term = video_mem_end = base + video_memory;                   /* 0 号屏幕内存末端位置 */
    scr_end = video_mem_start + video_num_lines * video_size_row; /* 滚屏末端位置(可视屏幕) */

    top = 0;                      /* 初始设置滚动时顶行行号 */
    bottom = video_num_lines;     /* 初始设置滚动时底行行号 */
    attr = 0x07;                  /* 字符属性(黑底白字) */
    def_attr = 0x07;              /* 默认字符属性 */
    restate = state = ESnormal;   /* 初始化转义序列操作的当前和下一状态 */
    checkin = 0;                  /* TODO: 干啥的??? */
    ques = 0;                     /* 收到问号字符标志 */
    iscolor = 0;                  /* 彩色显示标志 */
    translate = NORM_TRANS;       /* 使用的字符集 (普通ASCII码表) */
    vc_cons[0].vc_bold_attr = -1; /* 粗体字符属性标志 (-1表示不用) */

    gotoxy(currcons, ORIG_X, ORIG_Y); /* 设置当前光标位置 */

    /* 初始化非 0 号控制台参数, 与 0 号控制台的差异主要是内存位置不同 */
    for (currcons = 1; currcons < NR_CONSOLES; currcons++) {
        vc_cons[currcons] = vc_cons[0]; /* 复制 0 号结构参数, TODO: 研究一下这里生成的汇编代码 */
        origin = video_mem_start = (base += video_memory); /* 注意看 base 一直在增长 */
        scr_end = origin + video_num_lines * video_size_row;
        video_mem_end = (term += video_memory);
        gotoxy(currcons, 0, 0);
    }

    /* 设置前台控制台的屏幕原点(左上角)位置和显示控制器中光标显示位置 */
    update_screen();

    /* 设置键盘中断处理函数 */
    set_trap_gate(0x21, &keyboard_interrupt);
    outb_p(inb_p(0x21) & 0xfd, 0x21); /* 取消对键盘中断的屏蔽, 允许 IRQ1, 0xFD=1111_1101 */
    a = inb_p(0x61);                  /* 读取键盘端口 0x61 (8255A 端口 PB)  */
    outb_p(a | 0x80, 0x61);           /* 设置禁止键盘工作(位7置位) */
    outb_p(a, 0x61);                  /* 再允许键盘工作, 用以复位键盘 */
}

/**
 * @brief 更新当前前台控制台
 *
 * 把前台控制台转换为 fg_console 指定的虚拟控制台
 * fg_console 是设置的前台虚拟控制台号
 */
void update_screen(void)
{
    set_origin(fg_console); /* 设置滚屏起始显示内存地址 */
    set_cursor(fg_console); /* 设置显示控制器中光标显示内存位置 */
}

/**
 * @brief 停止蜂鸣
 *
 * 这个函数在 sched.c, do_timer 函数里面调用,
 * 那里面会根据 beepcount 的情况, 决定是否应该关闭 beep
 *
 * 复位 8255A PB 端口的位 1 和位 0
 *
 * 端口 0x61 的低两位控制着键盘和扬声器的状态
 *  - 位 0: 控制键盘时钟(0=启用, 1=禁用)
 *  - 位 1: 控制扬声器数据(0=关闭, 1=开启)
 * `& 0xFC` 操作会清除这两位, 从而:
 *  - 重新启用键盘时钟(允许继续接收键盘输入)
 *  - 关闭扬声器(如果之前被开启)
 *
 * from bsd-net-2:
 */
void sysbeepstop(void)
{
    /* disable counter 2, 禁止定时器 2 */
    outb(inb_p(0x61) & 0xFC, 0x61);
}

int beepcount = 0; /* sched.c 里面有用 */

/**
 * @brief 蜂鸣
 *
 * 8255A 芯片 PB 端口的位 1 用作扬声器的开门信号, 位 0 用作 8253 定时器 2 的门信号,
 * 该定时器的输出脉冲送往扬声器, 作为扬声器发声的频率. 因此要使扬声器蜂鸣, 需要两步:
 *  1. 首先置位 PB 端口(0x61)位 1 和位 0
 *  2. 然后设置定时器 2 通道发送一定的定时频率即可
 */
static void sysbeep(void)
{
    /* enable counter 2 */
    outb_p(inb_p(0x61) | 3, 0x61); /* 禁用键盘时钟(也禁用了输入), 启用扬声器 */
    /* set command for counter 2, 2 byte write */
    outb_p(0xB6, 0x43); /* 向端口 0x43 写入 0xB6, 配置计数器 2 为方波发生器模式 */
    /* send 0x637 for 750 HZ */
    outb_p(0x37, 0x42); /* 向端口 0x42 写入频率的低字节(0x37) */
    outb(0x06, 0x42);   /* 向端口 0x42 写入频率的高字节(0x06) */
    /* 1/8 second */
    beepcount = HZ / 8; /* 设置蜂鸣持续时间为1/8秒 */
}

/**
 * @brief 拷贝屏幕
 *
 * 把屏幕内容复制到参数指定的用户缓冲区 arg 中
 *
 * 参数 arg 有两个用途, 一是用于传递控制台号, 二是作为用户缓冲区指针
 *
 * @param arg 用户缓冲区指针(入参位置开始处需要保存具体的控制台号)
 * @return int 成功返回 0, 失败返回具体错误码
 */
int do_screendump(int arg)
{
    char *sptr, *buf = (char *)arg;
    int currcons, l;

    /* 验证 buf 是不是能写, 不能写就分配能写的新页面 */
    verify_area(buf, video_num_columns * video_num_lines);

    currcons = get_fs_byte(buf); /* 取得具体要操作的控制台号 */
    /* arg 传进来的台号, 是从 1 开始计数的 */
    if ((currcons < 1) || (currcons > NR_CONSOLES)) {
        return -EIO;
    }

    currcons--;

    /* 拷贝具体的内容到用户指定缓冲区 */
    sptr = (char *)origin;
    for (l = video_num_lines * video_num_columns; l > 0; l--) {
        put_fs_byte(*sptr++, buf++);
    }

    return (0);
}

/**
 * @brief 黑屏处理
 *
 * 当用户在 blankInterval 时间间隔内没有按任何按键时就让屏幕黑屏, 以保护屏幕
 */
void blank_screen()
{
    if (video_type != VIDEO_TYPE_EGAC && video_type != VIDEO_TYPE_EGAM) {
        return;
    }

    /* blank here. I can't find out how to do it, though */
}

/**
 * @brief 恢复黑屏的屏幕
 *
 * 当用户按下任何按键时, 就恢复处于黑屏状态的屏幕显示内容
 */
void unblank_screen()
{
    if (video_type != VIDEO_TYPE_EGAC && video_type != VIDEO_TYPE_EGAM) {
        return;
    }

    /* unblank here */
}

/**
 * @brief 控制台显示函数
 *
 * 该函数仅用于内核显示函数 printk (kernel/printk.c), 用于在当前前台控制台上显示内核信息
 * 处理方法是循环取出缓冲区中的字符, 并根据字符的特性控制光标移动或直接显示在屏幕上
 *
 * @param b null 结尾的字符串缓冲区指针
 */
void console_print(const char *b)
{
    int currcons = fg_console;
    char c;

    while (c = *(b++)) {
        /* 遇到 LF 的处理, 直接做一次 `回车+换行` 操作 */
        if (c == 10) {
            cr(currcons);
            lf(currcons);
            continue;
        }

        /* 遇到 CR 的处理, 正常 CR 即可 */
        if (c == 13) {
            cr(currcons);
            continue;
        }

        if (x >= video_num_columns) { /* 当前光标列位置 x 已经到达屏幕右末端 */
            x -= video_num_columns;   /* 让光标折返到下一行开始处 */
            pos -= video_size_row;    /* 这里是减去, 因为 lf 会帮我们在加上一行 */
            lf(currcons);
        }

        /* 在光标处显示字符, ah=attr, al=char */
        __asm__("movb %2, %%ah\n\t"
                "movw %%ax, %1\n\t"
                :
                : "a"(c), "m"(*(short *)pos), "m"(attr)
                : "ax");

        /* 移动光标位置和内存游标 */
        pos += 2;
        x++;
    }

    /* 重新设定显示控制器中光标显示内存位置 */
    set_cursor(currcons);
}
