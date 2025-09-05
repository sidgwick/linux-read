/*
 *  linux/kernel/hd.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This is the low-level hd interrupt support. It traverses the
 * request-list, using interrupts to jump between functions. As
 * all the functions are called within interrupts, we may not
 * sleep. Special care is recommended.
 *
 *  modified by Drew Eckhardt to check nr of hd's from the CMOS.
 *
 * 本程序是底层硬盘中断辅助程序。主要用于扫描请求项队列，使用中断
 * 在函数之间跳转。由于所有的函数都是在中断里调用的，所以这些函数
 * 不可以睡眠。请特别注意。
 */

#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/config.h>
#include <linux/fs.h>
#include <linux/hdreg.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#define MAJOR_NR 3 /* 硬盘主设备号是 3 */
#include "blk.h"

/* 读 CMOS 参数 */
#define CMOS_READ(addr)                                                                            \
    ({                                                                                             \
        outb_p(0x80 | addr, 0x70);                                                                 \
        inb_p(0x71);                                                                               \
    })

/* Max read/write errors/sector */
#define MAX_ERRORS 7 /* 每扇区读/写操作允许的最多出错次数 */
#define MAX_HD 2     /* 系统支持的最多硬盘数 */

static void recal_intr(void);  /* 重新校正处理函数 */
static void bad_rw_intr(void); /* 读写硬盘失败处理调用函数 */
extern void init_swapping(void);

static int recalibrate = 0; /* 重新校正标志 */
static int reset = 0;       /* 复位标志 */

/**
 * @brief 硬盘信息结构 (Harddisk information struct)
 *
 * This struct defines the HD's and their types.
 *
 *   head   磁头数
 *   sect   每磁道扇区数
 *   cyl    柱面数
 *   wpcom  写前预补偿柱面号
 *   lzone  磁头着陆区柱面号
 *   ctl    控制字节
 */
struct hd_i_struct {
    int head, sect, cyl, wpcom, lzone, ctl;
};

/* 如果已经在 include/linux/config.h 配置文件中定义了符号常数 HD_TYPE, 就使用定义好的
 * 参数作为硬盘信息数组 hd_info 中的数据
 * 否则先默认都设为 0 值, 在 setup 函数中会重新进行设置 */
#ifdef HD_TYPE
struct hd_i_struct hd_info[] = {HD_TYPE};                        /* 硬盘信息数组 */
#define NR_HD ((sizeof(hd_info)) / (sizeof(struct hd_i_struct))) /* 硬盘个数 */
#else
struct hd_i_struct hd_info[] = {
    {0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0},
};
static int NR_HD = 0;
#endif

/**
 * @brief 定义硬盘分区结构
 *
 * 给出每个分区从硬盘 0 道开始算起的物理起始扇区号和分区扇区总数
 *
 * 其中 5 的倍数处的项(例如 hd[0] 和 hd[5] 等)代表整个硬盘的参数
 */
static struct hd_struct {
    long start_sect; /* 分区在硬盘中的起始物理(绝对)扇区 */
    long nr_sects;   /* 分区中扇区总数 */
} hd[5 * MAX_HD] = {
    {0, 0},
};

/* 硬盘每个分区数据块总数数组 */
static int hd_sizes[5 * MAX_HD] = {
    0,
};

/**
 * @brief 读端口数据
 *
 * insw 从端口读取数据到 ES:EDI
 *
 * @param port 端口
 * @param buf 保存缓冲区
 * @param nr 读取数量
 */
#define port_read(port, buf, nr)                                                                   \
    __asm__("cld;rep insw" /* fmt */                                                               \
            :                                                                                      \
            : "d"(port), "D"(buf), "c"(nr))

/**
 * @brief 写端口数据
 *
 * outsw 从 DS:ESI 写出数据到端口
 *
 * @param port 端口
 * @param buf 数据缓冲区
 * @param nr 写出数量
 */
#define port_write(port, buf, nr)                                                                  \
    __asm__("cld\n\t"                                                                              \
            "rep outsw" /* fmt */                                                                  \
            :                                                                                      \
            : "d"(port), "S"(buf), "c"(nr))

extern void hd_interrupt(void); /* 硬盘中断过程 */
extern void rd_load(void);      /* 虚拟盘创建加载函数 */

/**
 * @brief 初始化设置磁盘
 *
 * This may be used only once, enforced by 'static int callable'
 *
 * @param BIOS 缓冲区, 用于保存从 BIOS 里面获取到的磁盘参数信息
 * @return int 处理完成返回 0, 否则返回非 0
 */
int sys_setup(void *BIOS)
{
    static int callable = 1;
    int i, drive;
    unsigned char cmos_disks;
    struct partition *p;
    struct buffer_head *bh;

    /* 本函数预期只调用一遍 */
    if (!callable) {
        return -1;
    }

    callable = 0;

    /* 如果没有设置过磁盘参数, 使用 BIOS 里面读到的信息补充 hd_info 数组
     * 这些信息在 setup.s 阶段利用 BIOS 中断获取得到, 一共有 32B */
#ifndef HD_TYPE
    for (drive = 0; drive < 2; drive++) {
        hd_info[drive].cyl = *(unsigned short *)BIOS;          /* 2 字节柱面数 */
        hd_info[drive].head = *(unsigned char *)(2 + BIOS);    /* 1 字节磁头数 */
        hd_info[drive].wpcom = *(unsigned short *)(5 + BIOS);  /* 2 字节 写前预补偿柱面号 */
        hd_info[drive].ctl = *(unsigned char *)(8 + BIOS);     /* 1 字节, 控制字节 */
        hd_info[drive].lzone = *(unsigned short *)(12 + BIOS); /* 2 字节, 磁头着陆区柱面号 */
        hd_info[drive].sect = *(unsigned char *)(14 + BIOS);   /* 1 字节, 每磁道扇区数 */

        BIOS += 16; /* 下一块硬盘 */
    }

    /* 看看有几块硬盘
     * 此前处理中, 如果不存在第二块硬盘, 则它对应的 16B 数据会被清零 */
    if (hd_info[1].cyl) {
        NR_HD = 2;
    } else {
        NR_HD = 1;
    }
#endif

    // 到这里, 硬盘信息数组hd_info[]已经设置好, 并且确定了系统含有的硬盘数NR_HD. 现在
    // 开始[].
    // .  因此这里仅设置表示硬盘整体信息的两项 (项0和5) .

    /* 设置硬盘分区结构数组 hd
     * 该数组的 0/5 两项, 分别表示两个硬盘的整体参数
     * 1-4 和 6-9 分别表示两个硬盘的 4 个分区的参数
     *
     * 特别注意, CHS 寻址方式中, head/cyl 是从 0 开始计数的, sect 是从 1 开始计数
     * 但 BIOS 参数记录的数量是绝对数量, 都是从 1 开始计数 */
    for (i = 0; i < NR_HD; i++) {
        hd[i * 5].start_sect = 0;
        hd[i * 5].nr_sects = hd_info[i].head * hd_info[i].sect * hd_info[i].cyl;
    }

    /*
            We querry CMOS about hard disks : it could be that
            we have a SCSI/ESDI/etc controller that is BIOS
            compatable with ST-506, and thus showing up in our
            BIOS table, but not register compatable, and therefore
            not present in CMOS.

            Furthurmore, we will assume that our ST-506 drives
            <if any> are the primary drives in the system, and
            the ones reflected as drive 1 or 2.

            The first drive is stored in the high nibble of CMOS
            byte 0x12, the second in the low nibble.  This will be
            either a 4 bit drive type or 0xf indicating use byte 0x19
            for an 8 bit type, drive 1, 0x1a for drive 2 in CMOS.

            Needless to say, a non-zero value means we have
            an AT controller hard disk for that drive.

        TODO: 这部分似乎是一些历史包袱, 应该不影响对全局理解, 先跳过

        我们对CMOS有关硬盘的信息有些怀疑: 可能会出现这样的情况, 我们有一块 SCSI/ESDI/等的控制器,
        它是以ST-506方式与BIOS相兼容的, 因而会出现在我们的BIOS参数表中, 但却又不是寄存器兼容的,
        因此这些参数在CMOS中又不存在.

        另外, 我们假设ST-506驱动器 (如果有的话) 是系统中的基本驱动器, 也即以驱动器1或2出现的驱动器.

        第1个驱动器参数存放在CMOS字节0x12的高半字节中, 第2个存放在低半字节中. 该4位字节信息可以
        是驱动器类型, 也可能仅是0xf. 0xf表示使用CMOS中0x19字节作为驱动器1的8位类型字节, 使用
        CMOS中0x1A字节作为驱动器2的类型字节.

        总之, 一个非零值意味着硬盘是一个AT控制器兼容硬盘.
    */

    // 这里根据上述原理, 下面代码用来检测硬盘到底是不是 AT 控制器兼容的. 有关CMOS信息
    // 请参见第4章中4.2.3.1节. 这里从CMOS偏移地址0x12处读出硬盘类型字节. 如果低半
    // 字节值 (存放着第2个硬盘类型值) 不为0, 则表示系统有两硬盘, 否则表示系统只有1
    // 个硬盘. 如果0x12处读出的值为0, 则表示系统中没有AT兼容硬盘.
    if ((cmos_disks = CMOS_READ(0x12)) & 0xf0) {
        if (cmos_disks & 0x0f) {
            NR_HD = 2;
        } else {
            NR_HD = 1;
        }
    } else {
        NR_HD = 0;
    }

    // 若 NR_HD = 0, 则两个硬盘都不是AT控制器兼容的, 两个硬盘数据结构全清零
    // 若 NR_HD = 1, 则将第2个硬盘的参数清零
    for (i = NR_HD; i < 2; i++) {
        hd[i * 5].start_sect = 0;
        hd[i * 5].nr_sects = 0;
    }

    // 否则我们根据硬盘第1个扇区最后两个字节应该是0xAA55来判断扇
    // 区中数据的有效性, 从而可以知道扇区中位于偏移0x1BE开始处的分区表是否有效. 若有效
    // 则将硬盘分区表信息放入硬盘分区结构数组hd[]中. 最后释放bh缓冲区.

    /* 读取硬盘分区表的信息(在第一个扇区) */
    for (drive = 0; drive < NR_HD; drive++) {
        /* 读块函数 bread 可以读硬盘第1个数据块
         * 第一个参数 0x300, 0x305 是硬盘设备号
         * 第二个参数 0 是要读取的块号 */
        if (!(bh = bread(0x300 + drive * 5, 0))) {
            printk("Unable to read partition table of drive %d\n\r", drive);
            panic("");
        }

        /* 可启动盘的第一个扇区的最后两个字节应该是 0x55 0xAA */
        if (bh->b_data[510] != 0x55 || (unsigned char)bh->b_data[511] != 0xAA) {
            printk("Bad partition table on drive %d\n\r", drive);
            panic("");
        }

        /* 磁盘分区表放在 0x1BE 字节, 每个分区表大小是 16B */
        p = (struct partition *)(0x1BE + (void *)bh->b_data);
        for (i = 1; i < 5; i++, p++) {
            hd[i + 5 * drive].start_sect = p->start_sect;
            hd[i + 5 * drive].nr_sects = p->nr_sects;
        }

        brelse(bh);
    }

    /* 对每个分区中的数据块总数进行统计, 并保存在硬盘分区总
     * 数据块数组 hd_sizes 中. 注意这里对原始的扇区数量做了
     * 除以 2 处理, 硬盘设备是按照 2 个扇区对应一个逻辑块处理的 */
    for (i = 0; i < 5 * MAX_HD; i++) {
        hd_sizes[i] = hd[i].nr_sects >> 1;
    }

    /* 然后让设备数据块总数指针数组的本设备项指向该数组 */
    blk_size[MAJOR_NR] = hd_sizes;

    if (NR_HD) {
        printk("Partition table%s ok.\n\r", (NR_HD > 1) ? "s" : "");
    }

    rd_load();       /* 加载 RAMDISK 到内存中 */
    init_swapping(); /* 初始化交换分区 */
    mount_root();    /* 挂载根目录 */
    return (0);
}

// 实际上, 我们仅需检测状态寄存器忙位 (位7) 是否为1来判断控制器是否处于忙状态, 驱动
// 器是否就绪 (即位6是否为1) 与控制器的状态无关. 因此我们可以把第172行语句改写成:
// “while (--retries && (inb_p(HD_STATUS)&0x80));”另外, 由于现在的PC机速度都很快,
// 因此我们可以把等待的循环次数再加大一些, 例如再增加10倍！

/**
 * @brief 等硬盘控制器就绪
 *
 * @return int 返回值等于 0 表示控制器尚未就绪, 大于 0 表示控制器已经就绪
 */
static int controller_ready(void)
{
    int retries = 100000;

    /* 读硬盘控制器状态寄存器端口 0x1f7, 循环检测其中的驱动器就绪比特位(位6)是
     * 否被置位并且控制器忙位(位7)是否被复位 */
    while (--retries && (inb_p(HD_STATUS) & 0x80))
        ;

    // 下面的语句不好使, 请参考: https://bbs.eetop.cn/blog-193015-21187.html
    // while (--retries && (inb_p(HD_STATUS) & 0xc0) != 0x40)
    //     ;

    return (retries);
}

/**
 * @brief 检测硬盘执行命令后的状态
 *
 * win 前缀表示温切斯特(Winchester)硬盘的缩写
 *
 * @return int 0-表示正常, 1-表示出错
 */
static int win_result(void)
{
    int i = inb_p(HD_STATUS); /* 取状态信息 */

    if ((i & (BUSY_STAT | READY_STAT | WRERR_STAT | SEEK_STAT | ERR_STAT)) ==
        (READY_STAT | SEEK_STAT)) {
        return (0); /* ok */
    }

    /* 如果执行命令错, 则需要再读错误寄存器 0x1f1 */
    if (i & 1) {
        i = inb(HD_ERROR);
    }

    return (1);
}

/**
 * @brief 向硬盘控制器发送命令
 *
 * @param drive 硬盘号(0-1)
 * @param nsect 读写扇区数
 * @param sect 起始扇区
 * @param head 磁头号
 * @param cyl 柱面号
 * @param cmd 命令码
 * @param intr_addr 硬盘中断处理程序中将调用的 C 处理函数指针
 */
static void hd_out(unsigned int drive, unsigned int nsect, unsigned int sect, unsigned int head,
                   unsigned int cyl, unsigned int cmd, void (*intr_addr)(void))
{
    register int port asm("dx"); /* 定义局部寄存器变量并放在指定寄存器 dx 中 */

    /* 驱动器号大于 1(只能是 0/1)或者磁头号大于 15, 则程序不支持 */
    if (drive > 1 || head > 15) {
        panic("Trying to write bad sector");
    }

    /* 等待驱动器就绪 */
    if (!controller_ready()) {
        panic("HD controller not ready");
    }

    SET_INTR(intr_addr);                /* 更新中断处理函数指针(do_hd)为 intr_addr */
    outb_p(hd_info[drive].ctl, HD_CMD); /* 向控制寄存器输出控制字节 */

    port = HD_DATA;                             /* 置dx为数据寄存器端口(0x1f0) */
    outb_p(hd_info[drive].wpcom >> 2, ++port);  /* 写预补偿柱面号(需除4) */
    outb_p(nsect, ++port);                      /* 读/写扇区总数 */
    outb_p(sect, ++port);                       /* 起始扇区 */
    outb_p(cyl, ++port);                        /* 柱面号低8位 */
    outb_p(cyl >> 8, ++port);                   /* 柱面号高8位 */
    outb_p(0xA0 | (drive << 4) | head, ++port); /* 驱动器号+磁头号 */
    outb(cmd, ++port);                          /* 硬盘控制命令 */
}

/**
 * @brief 等待硬盘不忙
 *
 * 该函数循环等待主状态控制器忙标志位复位
 *
 * @return int 0-硬盘不忙, 1-硬盘忙
 */
static int drive_busy(void)
{
    unsigned int i;
    unsigned char c;

    for (i = 0; i < 50000; i++) {
        c = inb_p(HD_STATUS);
        /* 忙, 就绪, 寻道结束 */
        c &= (BUSY_STAT | READY_STAT | SEEK_STAT);
        if (c == (READY_STAT | SEEK_STAT)) {
            return 0;
        }
    }

    printk("HD controller times out\n\r");
    return (1);
}

/**
 * @brief 重置硬盘控制器
 */
static void reset_controller(void)
{
    int i;

    /* 向控制寄存器端口发送允许复位(0x04)控制字节 */
    outb(4, HD_CMD);

    /* 等待一段时间 */
    for (i = 0; i < 1000; i++) {
        nop();
    }

    /* 发送正常的控制字节(不禁止重试/重读) */
    outb(hd_info[0].ctl & 0x0f, HD_CMD);
    if (drive_busy()) {
        printk("HD-controller still busy\n\r");
    }

    /* 然后读取错误寄存器内容, 若有错误(不等于 1)
     * 则显示硬盘控制器复位失败信息 */
    if ((i = inb(HD_ERROR)) != 1) {
        printk("HD-controller reset failed: %02x\n\r", i);
    }
}

// 在本命令引起的硬盘中断处理程序中又会调用本函数. 此时该函数会根据执行该命令的结果判
// 断是否要进行出错处理或是继续执行请求项处理操作.

/**
 * @brief 硬盘复位操作
 *
 */
static void reset_hd(void)
{
    static int i;

    /* 如果复位标志 reset 是置位的, 则在把复位标志清零后, 执行复位硬盘控制器操作. 然后
     * 针对第 i 个硬盘向控制器发送 “建立驱动器参数” 命令. 当控制器执行了该命令后, 又会
     * 发出硬盘中断信号. 此时本函数会被中断过程调用而再次执行. 由于 reset 已经标志复位,
     * 因此会首先去执行 `win_result` 行, 判断命令执行是否正常. 若还是发生错误就会调用
     * bad_rw_intr 函数以统计出错次数并根据次数确定是否在设置 reset 标志. 如果又设置了
     * reset 标志则跳转到 repeat 重新执行本函数.
     *
     * 若复位操作正常, 则针对下一个硬盘发送 “建立驱动器参数” 命令, 并作上述同样处理. 如果
     * 系统中 NR_HD 个硬盘都已经正常执行了发送的命令, 则再次 do_hd_request 函数开始对
     * 请求项进行处理. */

repeat:
    if (reset) {
        reset = 0;
        i = -1;
        reset_controller(); /* 复位硬盘控制器 */
    } else if (win_result()) {
        bad_rw_intr();
        if (reset) {
            goto repeat;
        }
    }

    /* 注意 i 声明为 `static int`, 在硬盘中断处理程序中,
     * 会再次调用本函数, 这时候我们就可以处理下一块硬盘了 */
    i++;

    if (i < NR_HD) {
        /* 发送硬盘控制器命令 `建立驱动器参数` */
        hd_out(i, hd_info[i].sect, hd_info[i].sect, hd_info[i].head - 1, hd_info[i].cyl,
               WIN_SPECIFY, &reset_hd);
    } else {
        do_hd_request();
    }
}

/**
 * @brief 意外硬盘中断调用函数
 *
 * 发生意外硬盘中断时, 硬盘中断处理程序中调用的默认 C 处理函数
 * 在硬盘中断处理程序中, 没有指定 do_hd 的时候调用该函数
 *
 * 此函数设置复位标志 reset, 然后继续调用请求项函数 go_hd_request 并
 * 在其中执行复位处理操作
 */
void unexpected_hd_interrupt(void)
{
    printk("Unexpected HD interrupt\n\r");
    reset = 1;
    do_hd_request();
}

/**
 * @brief 读写硬盘失败处理调用函数
 */
static void bad_rw_intr(void)
{
    /* 如果读扇区时的出错次数大于或等于 7 次时, 结束当前请求项并唤醒等待该请求的进程
     * 而且对应缓冲区更新标志复位(uptodate = 0), 表示数据没有更新 */
    if (++CURRENT->errors >= MAX_ERRORS) {
        end_request(0);
    }

    /* 如果读写一扇区时的出错次数已经大于 3 次, 则要求执行复位硬盘控制器操作(设置复位标志)  */
    if (CURRENT->errors > MAX_ERRORS / 2) {
        reset = 1;
    }
}

/**
 * @brief 该函数将在硬盘读命令结束时引发的硬盘中断过程中被调用
 *
 * 在读命令执行后会产生硬盘中断信号, 并执行硬盘中断处理程序, 此时在硬盘中断处理
 * 程序中调用的 C 函数指针 do_hd 已经指向 read_intr, 因此会在一次读扇区操作
 * 完成(或出错)后就会执行该函数
 */
static void read_intr(void)
{
    /* 检查读命令处理状态状态, 只有正常状态(READY_STAT | SEEK_STAT)
     * 才做后续处理, 否则做硬盘读取失败的错误处理 */
    if (win_result()) {
        /* 每次读操作出错都会对当前请求项作出错次数累计, 若出错次数不到最大允许
         * 出错次数的一半, 则会先执行硬盘复位操作, 然后再执行本次请求项处理. 若
         * 出错次数已经大于等于最大允许出错次数 MAX_ERRORS (7次), 则结束本次请
         * 求项的处理而去处理队列中下一个请求项 */
        bad_rw_intr();
        do_hd_request();
        return;
    }

    /* 运行到此说明之前的 `读命令` 没有出错 */

    /* 从数据寄存器端口把 1 个扇区的数据读到请求项的缓冲区中(512B) */
    port_read(HD_DATA, CURRENT->buffer, 256);
    CURRENT->errors = 0;
    CURRENT->buffer += 512;
    CURRENT->sector++;

    /* 递减请求项所需读取的扇区数值, 如果所需读取的扇区数值 > 0, 则继续读取
     * TODO: 下一次硬盘中断是如何触发的? - 是否是硬盘自己主动发起的? */
    if (--CURRENT->nr_sectors) {
        SET_INTR(&read_intr);
        return;
    }

    /* 读取完成, 标记缓冲区现在是足够新(uptodate)的 */
    end_request(1);

    /* 继续处理其他请求动作 */
    do_hd_request();
}

/**
 * @brief 该函数将在硬盘写命令结束时引发的硬盘中断过程中被调用
 *
 * 在写命令执行后会产生硬盘中断信号, 并执行硬盘中断处理程序, 此时在硬盘中断处理程序
 * 中调用的 C 函数指针 do_hd 已经指向 write_intr, 因此会在一次写扇区操作完成(或出错)
 * 后就会执行该函数
 */
static void write_intr(void)
{
    /* 判断此次写命令操作是否出错, 类似 read_intr 里面的操作 */
    if (win_result()) {
        bad_rw_intr();
        do_hd_request();
        return;
    }

    /* 如果没写完所有的扇区, 继续写下一个 */
    if (--CURRENT->nr_sectors) {
        CURRENT->sector++;
        CURRENT->buffer += 512;
        SET_INTR(&write_intr);
        port_write(HD_DATA, CURRENT->buffer, 256);
        return;
    }

    /* 所有的扇区已经全部写回去到 HD, 标记缓冲区为很新(uptodate) */
    end_request(1);

    /* 处理其他请求 */
    do_hd_request();
}

/**
 * @brief 该函数会在硬盘执行复位操作而引发的硬盘中断中被调用
 */
static void recal_intr(void)
{
    /* 如果硬盘控制器返回错误信息, 则函数首先进行硬盘读写失败处理 */
    if (win_result()) {
        bad_rw_intr();
    }

    /* 执行更多的请求动作 */
    do_hd_request();
}

/**
 * @brief 硬盘超时操作
 *
 * 在 `sched.c` 里面的 `do_timer` 中被调用. 向硬盘控制器发送了一个命令后, 经过
 * hd_timeout 个系统滴答后控制器还没有发出一个硬盘中断信号, 则说明控制器(或硬盘)操
 * 作超时. 此时 do_timer 会调用本函数设置复位标志 reset 并调用 do_hd_request
 * 执行复位处理. 若在预定时间内(200滴答)硬盘控制器发出了硬盘中断并开始执行硬盘中断
 * 处理程序, 那么 ht_timeout 值就会在中断处理程序中被置 0, do_timer 就会跳过本函数
 */
void hd_times_out(void)
{
    if (!CURRENT) {
        return;
    }

    printk("HD timeout");
    if (++CURRENT->errors >= MAX_ERRORS) {
        end_request(0);
    }

    SET_INTR(NULL);
    reset = 1;
    do_hd_request();
}

/**
 * @brief 执行硬盘读写操作请求
 */
void do_hd_request(void)
{
    int i, r;
    unsigned int block, dev;
    unsigned int sec, head, cyl;
    unsigned int nsect;

    INIT_REQUEST;

    dev = MINOR(CURRENT->dev); /* 拿到当前请求的设备号 */
    block = CURRENT->sector;   /* 操作的起始扇区(相对本分区) */

    /* 检查参数的合法性, 入不合法丢弃本次请求后, 继续处理下一个请求 */
    if (dev >= 5 * NR_HD || block + 2 > hd[dev].nr_sects) {
        end_request(0);
        goto repeat;
    }

    block += hd[dev].start_sect; /* 拿到在磁盘维度的扇区编号 */
    dev /= 5;                    /* 得到硬盘编号 */

    // 然后根据求得的绝对扇区号 block 和硬盘号 dev, 我们就可以计算出对应硬盘中的磁道中扇
    // 区号 (sec) 、所在柱面号 (cyl)  和磁头号 (head) .  下面嵌入的汇编代码即用来根据硬
    // 盘信息结构中的每磁道扇区数和硬盘磁头数来计算这些数据. 计算方法为:

    /* 初始 EAX = block, EDX=0, 除以每磁道扇区数
     * 商 EAX 是到 block 对应的磁道数量, 余数 EDX 是到 block 的扇区位置
     * 商最后输出到 block, 余数最后输出到 sec */
    __asm__("divl %4" //
            : "=a"(block), "=d"(sec)
            : "0"(block), "1"(0), "r"(hd_info[dev].sect));

    /* 与上面计算类似, 不同的是这次结果
     * cyl 中保存的是到 block 需要的柱面数, head 是到 block 的磁头号 */
    __asm__("divl %4" //
            : "=a"(cyl), "=d"(head)
            : "0"(block), "1"(0), "r"(hd_info[dev].head));

    sec++;                       /* 扇区从 1 开始计数 */
    nsect = CURRENT->nr_sectors; /* 待读写的扇区数量 */

    /* 如果有复位标记, 先复位 */
    if (reset) {
        recalibrate = 1;
        reset_hd();
        return;
    }

    /* 如果有重置, 先重置磁头到 0 柱面 */
    if (recalibrate) {
        recalibrate = 0;
        hd_out(dev, hd_info[CURRENT_DEV].sect, 0, 0, 0, WIN_RESTORE, &recal_intr);
        return;
    }

    // 如果以上两个标志都没有置位, 那么我们就可以开始向硬盘控制器发送真正的数据读/写
    // 操作命令了. 如果当前请求是写扇区操作, 则发送写命令, . 这方面的信息可参
    // 见程序前面的硬盘操作读/写时序图. 如果请求服务DRQ置位则退出循环.  若等到循环结
    // 束也没有置位, 则表示发送的要求写硬盘命令失败, 于是跳转去处理出现的问题或继续执
    // 行下一个硬盘请求. 否则我们就可以向硬盘控制器数据寄存器端口HD_DATA写入1个扇区
    // 的数据.

    /* 当前请求是写命令 */
    if (CURRENT->cmd == WRITE) {
        hd_out(dev, nsect, sec, head, cyl, WIN_WRITE, &write_intr);

        /* 循环读取状态寄存器信息并判断请求服务标志 DRQ_STAT 是否置位
         * DRQ_STAT 是硬盘状态寄存器的请求服务位, 表示驱动器已经准备好
         * 在主机和数据端口之间传输一个字或一个字节的数据*/
        for (i = 0; i < 10000 && !(r = inb_p(HD_STATUS) & DRQ_STAT); i++) { /* do nothing */
        }

        if (!r) {
            bad_rw_intr();
            goto repeat;
        }

        port_write(HD_DATA, CURRENT->buffer, 256);
    } else if (CURRENT->cmd == READ) {
        /* 当前请求是读硬盘数据, 则向硬盘控制器发送读扇区命令 */
        hd_out(dev, nsect, sec, head, cyl, WIN_READ, &read_intr);
    } else {
        panic("unknown hd-command");
    }
}

/**
 * @brief 硬盘初始化
 */
void hd_init(void)
{
    blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;

    /* 设置硬盘中断处理函数 */
    set_intr_gate(0x2E, &hd_interrupt);

    /* 8259A, 主从片, 打开中断 */
    outb_p(inb_p(0x21) & 0xfb, 0x21);
    outb(inb_p(0xA1) & 0xbf, 0xA1);
}
