/*
 *  linux/kernel/floppy.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 02.12.91 - Changed to static variables to indicate need for reset
 * and recalibrate. This makes some things easier (output_byte reset
 * checking etc), and means less interrupt jumping in case of errors,
 * so the code is hopefully easier to understand.
 */

/*
 * This file is certainly a mess. I've tried my best to get it working,
 * but I don't like programming floppies, and I have only one anyway.
 * Urgel. I should check for more errors, and do more graceful error
 * recovery. Seems there are problems with several drives. I've tried to
 * correct them. No promises.
 */

/*
 * As with hd.c, all routines within this file can (and will) be called
 * by interrupts, so extreme caution is needed. A hardware interrupt
 * handler may not sleep, or a kernel panic will happen. Thus I cannot
 * call "floppy-on" directly, but have to set a special timer interrupt
 * etc.
 *
 * Also, I'm not certain this works on more than 1 floppy. Bugs may
 * abund.
 */

/**
 * 参考 [软盘控制器编程方法](https://mirror.math.princeton.edu/pub/oldlinux/download/fd-pro.pdf)
 */

#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/fdreg.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#define MAJOR_NR 2 /* 软盘设备号是 2 */
#include "blk.h"   /* 引入软盘设备对应的宏定义 */

static int recalibrate = 0; /* 1 表示需要重新校正磁头位置(磁头归零道) */
static int reset = 0;       /* 1 表示需要进行复位操作 */
static int seek = 0;        /* 1 表示需要执行寻道操作 */

/* 当前数字输出寄存器DOR (Digital Output Register, 定义在 kernel/sched.c)
 * 该变量含有软驱操作中的重要标志, 包括选择软驱/控制电机启动/启动复位软盘控制器以
 * 及允许/禁止DMA和中断请求 */
extern unsigned char current_DOR;

/* 字节直接数输出(嵌入汇编宏), 把值 val 输出到 port 端口 */
#define immoutb_p(val, port)                                                                       \
    __asm__("outb %0,%1\n\t"                                                                       \
            "jmp 1f\n"                                                                             \
            "1:\tjmp 1f\n"                                                                         \
            "1:"                                                                                   \
            :                                                                                      \
            : "a"((char)(val)), "i"(port))

/* 这两个宏定义用于计算软驱的设备号
 * 参数 x 是次设备号, 次设备号 = TYPE*4 + DRIVE. 计算方法参见列表后 */
#define TYPE(x) ((x) >> 2)    /* 软驱类型 (2--1.2Mb, 7--1.44Mb) */
#define DRIVE(x) ((x) & 0x03) /* 软驱序号 (0--3对应A--D) */

/* Note that MAX_ERRORS=8 doesn't imply that we retry every bad read
 * max 8 times - some types of errors increase the errorcount by 2,
 * so we might actually retry only 5-6 times before giving up.
 *
 * 注意, 下面定义MAX_ERRORS=8并不表示对每次读错误尝试最多8次 - 有些类型
 * 的错误会把出错计数值乘2, 所以我们实际上在放弃操作之前只需尝试5-6遍即可 */
#define MAX_ERRORS 8

/*
 * globals used by 'result()'
 *
 * 这些状态字节中各比特位的含义请参见 include/linux/fdreg.h 头文件
 */
#define MAX_REPLIES 7 /* FDC 最多返回7字节的结果信息 */
/* 存放 FDC 返回的应答结果信息 */
static unsigned char reply_buffer[MAX_REPLIES];

#define ST0 (reply_buffer[0]) /* 结果状态字节0 */
#define ST1 (reply_buffer[1]) /* 结果状态字节1 */
#define ST2 (reply_buffer[2]) /* 结果状态字节2 */
#define ST3 (reply_buffer[3]) /* 结果状态字节3 */

/*
 * This struct defines the different floppy types. Unlike minix
 * linux doesn't have a "search for right type"-type, as the code
 * for that is convoluted and weird. I've got enough problems with
 * this driver as it is.
 *
 * The 'stretch' tells if the tracks need to be boubled for some
 * types (ie 360kB diskette in 1.2MB drive etc). Others should
 * be self-explanatory.
 *
 * 定义软盘结构. 软盘参数有:
 *  size          大小(扇区数)
 *  sect          每磁道扇区数
 *  head          磁头数
 *  track         磁道数
 *  stretch       对磁道是否要特殊处理 (标志)
 *  gap           扇区间隙长度(字节数)
 *  rate          数据传输速率
 *  spec1         参数(高4位步进速率, 低四位磁头卸载时间)
 */
static struct floppy_struct {
    unsigned int size, sect, head, track, stretch;
    unsigned char gap, rate, spec1;
} floppy_type[] = {
    {0, 0, 0, 0, 0, 0x00, 0x00, 0x00},      /* no testing */
    {720, 9, 2, 40, 0, 0x2A, 0x02, 0xDF},   /* 360kB PC diskettes */
    {2400, 15, 2, 80, 0, 0x1B, 0x00, 0xDF}, /* 1.2 MB AT-diskettes */
    {720, 9, 2, 40, 1, 0x2A, 0x02, 0xDF},   /* 360kB in 720kB drive */
    {1440, 9, 2, 80, 0, 0x2A, 0x02, 0xDF},  /* 3.5" 720kB diskette */
    {720, 9, 2, 40, 1, 0x23, 0x01, 0xDF},   /* 360kB in 1.2MB drive */
    {1440, 9, 2, 80, 0, 0x23, 0x01, 0xDF},  /* 720kB in 1.2MB drive */
    {2880, 18, 2, 80, 0, 0x1B, 0x00, 0xCF}, /* 1.44MB diskette */
};

/*
 * Rate is 0 for 500kb/s, 2 for 300kbps, 1 for 250kbps
 * Spec1 is 0xSH, where S is stepping rate (F=1ms, E=2ms, D=3ms etc),
 * H is head unload time (1=16ms, 2=32ms, etc)
 *
 * Spec2 is (HLD<<1 | ND), where HLD is head load time (1=2ms, 2=4 ms etc)
 * and ND is set means no DMA. Hardcoded to 6 (HLD=6ms, use DMA).
 *
 * 上面速率rate: 0-500kbps, 1-300kbps, 2-250kbps
 *
 * 参数
 *  spec1 是 0xSH, 其中 S 是步进速率(F-1ms, E-2ms, D=3ms等)
 *        H 是磁头卸载时间(1=16ms, 2=32ms等)
 *
 *  spec2 是 (HLD<<1 | ND), 其中 HLD 是磁头加载时间(1=2ms, 2=4ms, 3=6ms等), ND 置位表示
 *        不使用DMA(No DMA), 在程序中硬编码成 6(HLD=6ms, 使用DMA)
 */

/* floppy_interrupt 是 sys_call.s 程序中软驱中断处理过程标号
 * 这里将在软盘初始化函数 floppy_init 使用它初始化中断陷阱门描述符 */
extern void floppy_interrupt(void);

/* 这是 boot/head.s 里面定义的临时软盘缓冲区
 * 如果请求项的缓冲区处于内存 1MB 以上某个地方, 则需要将 DMA 缓冲区设在
 * 临时缓冲区域处. 因为 8237A 芯片只能在 1MB 地址范围内寻址 */
extern char tmp_floppy_area[1024];

/*
 * These are global variables, as that's the easiest way to give
 * information to interrupts. They are the data used for the current
 * request.
 *
 * 下面是一些全局变量, 因为这是将信息传给中断程序最简单的方式
 * 它们用于当前请求项的数据
 *
 * 这些所谓的 `全局变量` 是指在软盘中断处理程序中调用的 C 函数使用的变量
 * 当然这些 C 函数都在本程序内
 */
static int cur_spec1 = -1;                         /* 当前软盘参数 spec1 */
static int cur_rate = -1;                          /* 当前软盘转速 rate */
static struct floppy_struct *floppy = floppy_type; /* 软盘类型结构数组指针 */
static unsigned char current_drive = 0;            /* 当前驱动器号 */
static unsigned char sector = 0;                   /* 当前扇区号 */
static unsigned char head = 0;                     /* 当前磁头号 */
static unsigned char track = 0;                    /* 当前磁道号 */
static unsigned char seek_track = 0;               /* 寻道磁道号 */
static unsigned char current_track = 255;          /* 当前磁头所在磁道号 */
static unsigned char command = 0;                  /* 读/写命令 */
unsigned char selected = 0; /* 软驱已选定标志. 在处理请求项之前要首先选定软驱 */
struct task_struct *wait_on_floppy_select = NULL; /* 等待选定软驱的任务队列 */

/* 取消选定软驱 */
void floppy_deselect(unsigned int nr)
{
    /* 数字输出寄存器(DOR)的低 2 位用于指定选择的软驱(0-3对应A-D)
     * 如果函数参数指定的软驱 nr 当前并没有被选定, 则显示警告信息 */
    if (nr != (current_DOR & 3)) {
        printk("floppy_deselect: drive not selected\n\r");
    }

    /* 复位软驱已选定标志 selected, 并唤醒等待选择该软驱的任务 */
    selected = 0;
    wake_up(&wait_on_floppy_select);
}

/**
 * @brief 检测指定软驱中软盘更换情况
 *
 * floppy-change is never called from an interrupt, so we can relax a bit
 * here, sleep etc. Note that floppy-on tries to set current_DOR to point
 * to the desired drive, but it will probably not survive the sleep if
 * several floppies are used at the same time: thus the loop.
 *
 * floppy-change 不是从中断程序中调用的, 所以这里我们可以轻松一下, 睡眠等
 * 注意 floppy-on 会尝试设置 current_DOR 指向所需的驱动器, 但当同时使用几个
 * 软盘时不能睡眠: 因此此时只能使用循环方式
 *
 * 该函数首先选定参数指定的软驱nr, 然后测试软盘控制器的数字输入寄存器DIR的值, 以判
 * 断驱动器中的软盘是否被更换过. 该函数由程序fs/buffer.c中的 check_disk_change()函
 * 数调用 (第119行) .
 *
 * @param nr 软驱号
 * @return int 如果软盘更换了则返回1, 否则返回0
 */
int floppy_change(unsigned int nr)
{
repeat:
    /* 首先要让软驱中软盘旋转起来并达到正常工作转速, 这需要花费一定时间.
     * 采用的方法是利用 kernel/sched.c 中软盘定时函数 do_floppy_timer 进行一定的
     * 延时处理. floppy_on 函数则用于判断延时是否到, 若没有到则让当前进程继续睡眠等待.
     * 若延时到则 do_floppy_timer 会唤醒当前进程 */
    floppy_on(nr);

    /* 等软盘驱动器为当前进程提供服务
     *
     * 在软盘启动(旋转)之后, 查看一下当前选择的软驱是不是函数参数指定的软驱 nr
     *
     * 如果当前选择的软驱不是指定的软驱 nr, 并且已经选定了其他软驱, 则让当前任务进入可
     * 中断等待状态, 以等待其他软驱被取消选定. 参见上面 floppy_deselect
     *
     * 如果当前没有选择其他软驱或者其他软驱被取消选定而使当前任务被唤醒时, 当前软驱仍然不是指定
     * 的软驱 nr, 则跳转到函数开始处重新循环等待 */
    while ((current_DOR & 3) != nr && selected) {
        sleep_on(&wait_on_floppy_select);
    }

    if ((current_DOR & 3) != nr) {
        goto repeat;
    }

    /* 现在软盘控制器已选定我们指定的软驱 nr
     *
     * 取数字输入寄存器 DIR 的值, 如果其 `位7` 置位, 则表示软盘已更换,
     * 此时即可关闭马达并返回 1 退出, 否则关闭马达返回 0 退出. 表示磁盘没有被更换
     *
     * TODO-DONE: 了解一下 DIR 各位的含义
     * 答: DIR 寄存器只有位 7 有效, 用于表示软盘更换状态, 其余用于硬盘控制器
     */
    if (inb(FD_DIR) & 0x80) {
        floppy_off(nr);
        return 1;
    }

    floppy_off(nr);
    return 0;
}

/* 从 from 拷贝 1024 字节, 到 to */
#define copy_buffer(from, to)                                                                      \
    __asm__("cld\n\t"                                                                              \
            "rep movsl"                                                                            \
            :                                                                                      \
            : "c"(BLOCK_SIZE / 4), "S"((long)(from)), "D"((long)(to)))

/* 初始化软盘 DMA 通道
 * 软盘中数据读写操作是使用 DMA 进行的, 因此在每次进行数据传输之前需要设置 DMA 芯片
 * 上专门用于软驱的通道 2. 有关 DMA 编程方法请参见程序列表后的信息 */
static void setup_DMA(void)
{
    long addr = (long)CURRENT->buffer;

    cli();

    /* 首先检测请求项的缓冲区所在位置
     * 如果缓冲区处于内存 1MB 以上的某个地方, 则需要将 DMA 缓冲区设在临
     * 时缓冲区域(tmp_floppy_area)处, 因为 8237A 芯片只能在 1MB 地址范围内寻址.
     * 如果是写盘命令, 则还需要把数据从请求项缓冲区复制到该临时区域
     *
     * 8237A 是 DMA 控制器芯片 */
    if (addr >= 0x100000) {
        addr = (long)tmp_floppy_area;
        if (command == FD_WRITE) {
            copy_buffer(CURRENT->buffer, tmp_floppy_area);
        }
    }

    /* 接下来我们开始设置 DMA 通道 2
     * 在开始设置之前需要先屏蔽该通道, 单通道屏蔽寄存器端口为 0x0A
     *
     * 位 0-1 指定 DMA 通道(0-3)
     * 位 2: 1-表示屏蔽, 0-表示允许请求
     *
     * 然后向 DMA 控制器端口 12 和 11 写入方式字(读盘是0x46, 写盘则是0x4A)
     * 再写入传输使用缓冲区地址 addr 和需要传输的字节数 0x3ff (0~1023).
     * 最后复位对 DMA 通道 2 的屏蔽, 开放 DMA2 请求 DREQ 信号 */

    /* mask DMA 2, 暂不允许请求, 通道 2 */
    immoutb_p(4 | 2, 10);

    /* output command byte. I don't know why, but everyone (minix, */
    /* sanches & canton) output this twice, first to 12 then to 11 */
    __asm__("outb %%al,$12\n\t"
            "jmp 1f\n"
            "1:\t jmp 1f\n"
            "1:\t outb %%al,$11\n\t"
            "jmp 1f\n"
            "1:\t jmp 1f\n"
            "1:"
            :
            : "a"((char)((command == FD_READ) ? DMA_READ : DMA_WRITE)));

    /* 8 low bits of addr */
    immoutb_p(addr, 4);
    addr >>= 8;
    /* bits 8-15 of addr */
    immoutb_p(addr, 4);
    addr >>= 8;
    /* bits 16-19 of addr */
    immoutb_p(addr, 0x81);

    /* low 8 bits of count-1 (1024-1=0x3ff) */
    immoutb_p(0xff, 5);
    /* high 8 bits of count-1 */
    immoutb_p(3, 5);

    /* activate DMA 2, 允许请求通道 2 */
    immoutb_p(0 | 2, 10);

    sti(); /* 恢复中断 */
}

/* 向软驱控制器输出一个字节命令或参数
 *
 * 若出错, 则会设置复位标志 reset */
static void output_byte(char byte)
{
    int counter;
    unsigned char status;

    if (reset) {
        return;
    }

    /* 在向控制器发送一个字节之前, 控制器需要处于准备好状态
     * 因此首先读取控制器状态信息 */
    for (counter = 0; counter < 10000; counter++) {
        /* 确保控制器准备好, 且数据传输方向为从 CPU 到 FDC */
        status = inb_p(FD_STATUS) & (STATUS_READY | STATUS_DIR);
        if (status == STATUS_READY) {
            outb(byte, FD_DATA);
            return;
        }
    }

    /* 如果循环 1 万次结束还不能发送, 则置复位标志 */
    reset = 1;
    printk("Unable to send byte to FDC\n\r");
}

/* 读取 FDC 执行的结果信息
 * 结果信息最多 7 个字节, 存放在数组 reply_buffer 中
 * 返回读入的结果字节数, 若返回值 = -1, 则表示出错 */
static int result(void)
{
    int i = 0, counter, status;

    if (reset) {
        return -1;
    }

    /* 循环读取主状态控制器 FD_STATUS (0x3f4) 的状态.
     * 如果读取的控制器状态是 READY, 表示已经没有数据可取, 则返回已读取的字节数 i
     * 如果控制器状态是方向标志置位+已准备好+忙, 表示有数据可读取, 于是把控制器中的
     * 结果数据读入到应答结果数组中. 最多读取 MAX_REPLIES (=7) 个字节
     * TODO: 了解一下 FD_STATUS 读取出来的各个标志位 */
    for (counter = 0; counter < 10000; counter++) {
        status = inb_p(FD_STATUS) & (STATUS_DIR | STATUS_READY | STATUS_BUSY);
        if (status == STATUS_READY) {
            return i;
        }

        if (status == (STATUS_DIR | STATUS_READY | STATUS_BUSY)) {
            if (i >= MAX_REPLIES) {
                break;
            }

            reply_buffer[i++] = inb_p(FD_DATA);
        }
    }

    reset = 1;
    printk("Getstatus times out\n\r");
    return -1;
}

/* 软盘读写出错处理函数
 * 真正的复位和重新校正处理会在后续的程序中进行 */
static void bad_flp_intr(void)
{
    /* 如果当前处理的请求项出错次数大于规定的最大出错次数 MAX_ERRORS (8次),
     * 则不再对当前请求项作进一步的操作尝试 */
    CURRENT->errors++;
    if (CURRENT->errors > MAX_ERRORS) {
        floppy_deselect(current_drive);
        end_request(0);
    }

    /* 如果读/写出错次数已经超过 MAX_ERRORS/2, 设置复位标志 reset, 对软驱作复位处理
     * 若出错次数还不到最大值的一半, 设置重新校正标志 recalibrate, 重新校正一下磁头位置 */
    if (CURRENT->errors > MAX_ERRORS / 2) {
        reset = 1;
    } else {
        recalibrate = 1;
    }
}

/* 软盘读写操作中断调用函数
 * Ok, this interrupt is called after a DMA read/write has succeeded,
 * so we check the results, and copy any buffers.
 */
static void rw_interrupt(void)
{
    /* 读取 FDC 执行的结果信息
     * 如果返回结果字节数不等于 7, 或者状态字节 0/1/2 中存在出错标志, 那么:
     *  1. 若是写保护就显示出错信息, 释放当前驱动器, 并结束当前请求项.
     *  2. 否则就执行出错计数处理. 然后继续执行软盘请求项操作
     *
     * 以下状态的含义参见 fdreg.h 文件:
     *
     * 0xf8 = ST0_INTR | ST0_SE | ST0_ECE | ST0_NR
     * 0xbf = ST1_EOC | ST1_CRC | ST1_OR | ST1_ND | ST1_WP | ST1_MAM
     * 0x73 = ST2_CM | ST2_CRC | ST2_WC | ST2_BC | ST2_MAM
     *
     * 0xbf 估计是手误, 应该是 0xb7 */
    if (result() != 7 || (ST0 & 0xf8) || (ST1 & 0xbf) || (ST2 & 0x73)) {
        if (ST1 & ST1_WP) {
            printk("Drive %d is write protected\n\r", current_drive);
            floppy_deselect(current_drive);
            end_request(0);
        } else {
            bad_flp_intr();
        }

        do_fd_request();
        return;
    }

    /* 读/写操作成功 */

    /* 若请求项是读操作并且其缓冲区在内存 1MB 以上位置,
     * 则需要把数据从软盘临时缓冲区复制到请求项的缓冲区 */
    if (command == FD_READ && (unsigned long)(CURRENT->buffer) >= 0x100000) {
        copy_buffer(tmp_floppy_area, CURRENT->buffer);
    }

    floppy_deselect(current_drive);
    end_request(1);
    do_fd_request();
}

/* 初始化软盘读写设置 */
static inline void setup_rw_floppy(void)
{
    /* 设置 DMA 通道 2 */
    setup_DMA();

    do_floppy = rw_interrupt; /* 中断处理函数 */

    /* 向软盘控制器输出命令和参数(输出 1 字节命令 + 0~7字节参数) */
    output_byte(command);
    output_byte(head << 2 | current_drive);
    output_byte(track);
    output_byte(head);
    output_byte(sector);
    output_byte(2); /* sector size = 512 */
    output_byte(floppy->sect);
    output_byte(floppy->gap);
    output_byte(0xFF); /* sector size (0xff when n!=0 ?) */

    /* 若上述任何一个 output_byte 操作出错, 则会设置复位标志 reset
     * 此时即会立刻去执行 do_fd_request 中的复位处理代码 */
    if (reset) {
        do_fd_request();
    }
}

/* 寻道处理结束后中断过程中调用的 C 函数
 *
 * This is the routine called after every seek (or recalibrate) interrupt
 * from the floppy controller. Note that the "unexpected interrupt" routine
 * also does a recalibrate, but doesn't come here.
 *
 * 该子程序是在每次软盘控制器寻道(或重新校正)中断中被调用的
 * 注意意外中断(unexpected interrupt)子程序也会执行重新校正操作, 但不在此地.
 */
static void seek_interrupt(void)
{
    /* sense drive status
     * 检测中断状态命令, 返回结果为 2 字节: ST0 和磁头当前磁道号 */
    output_byte(FD_SENSEI);

    /* 读取 FDC 执行的结果信息
     * 如果返回结果字节数不等于 2, 或者 ST0 不为寻道结束, 或者
     * 磁头所在磁道(ST1)不等于设定磁道, 则说明发生了错误 */
    if (result() != 2 || (ST0 & 0xF8) != 0x20 || ST1 != seek_track) {
        bad_flp_intr();
        do_fd_request();
        return;
    }

    /* 若寻道操作成功, 则继续执行当前请求项的软盘操作 */
    current_track = ST1; /* 设置当前磁道 */
    setup_rw_floppy();   /* 设置 DMA 并输出软盘操作命令和参数 */
}

/* This routine is called when everything should be correctly set up
 * for the transfer (ie floppy motor is on and the correct floppy is
 * selected).
 *
 * 读写数据传输函数
 *
 * 该函数是在传输操作的所有信息都正确设置好后被调用的(即软驱马达已开启并且
 * 已选择了正确的软驱) */
static void transfer(void)
{
    /* 首先检查当前驱动器参数是否就是指定驱动器的参数 */
    if (cur_spec1 != floppy->spec1) {
        cur_spec1 = floppy->spec1;
        output_byte(FD_SPECIFY); /* 发送设置磁盘参数命令 */
        output_byte(cur_spec1);  /* hut etc, 发送参数 */
        output_byte(6);          /* 磁头加载时间=6ms, DMA */
    }

    /* 检测当前速率 */
    if (cur_rate != floppy->rate) {
        outb_p(cur_rate = floppy->rate, FD_DCR); /* 更新当前速率 */
    }

    /* 若上面任何一个 output_byte 操作执行出错, 则复位标志 reset 就会被置位
     * 因此这里需要检测一下 reset 标志, 若 reset 被置位了, 就立刻去执行
     * do_fd_request 中的复位处理代码 */
    if (reset) {
        do_fd_request();
        return;
    }

    /* 如果此时不需要寻道, 则设置 DMA 并向软盘控制器发送相应操作命令和参数后返回 */
    if (!seek) {
        setup_rw_floppy();
        return;
    }

    /* 置软盘中断处理调用函数为寻道中断函数 */
    do_floppy = seek_interrupt;
    if (seek_track) {
        /* 待寻道的磁道号不等于零, 发送磁头寻道命令和参数 */
        output_byte(FD_SEEK);                   /* 发送磁头寻道命令 */
        output_byte(head << 2 | current_drive); /* 参数: 磁头号+当前软驱号 */
        output_byte(seek_track);                /* 参数: 磁道号 */
    } else {
        /* 重新校正命令让磁头归零位 */
        output_byte(FD_RECALIBRATE);            /* 发送重新校正命令(磁头归零) */
        output_byte(head << 2 | current_drive); /* 参数: 磁头号+当前软驱号*/
    }

    /* 若上面任何一个 output_byte 操作执行出错, 就立刻去执行
     * do_fd_request 中的复位处理代码 */
    if (reset) {
        do_fd_request();
    }
}

/* 软驱重新校正中断调用函数
 *
 * Special case - used after a unexpected interrupt (or reset) */
static void recal_interrupt(void)
{
    output_byte(FD_SENSEI); /* 发送检测中断状态命令(无参数) */

    if (result() != 2 || (ST0 & 0xE0) == 0x60) {
        reset = 1; /* 返回结果表明出错, 则置复位标志 */
    } else {
        recalibrate = 0; /* 重新校正标志清零 */
    }

    /* 然后再次执行软盘请求项处理函数作相应操作 */
    do_fd_request();
}

/* 意外软盘中断请求引发的软盘中断处理程序中调用的函数 */
void unexpected_floppy_interrupt(void)
{
    output_byte(FD_SENSEI); /* 发送检测中断状态命令(无参数) */

    if (result() != 2 || (ST0 & 0xE0) == 0x60) {
        reset = 1; /* 返回结果表明出错, 则置复位标志 */
    } else {
        recalibrate = 1; /* 置重新校正标志 */
    }
}

/* 软盘重新校正处理函数 */
static void recalibrate_floppy(void)
{
    recalibrate = 0;             /* 复位重新校正标志 */
    current_track = 0;           /* 当前磁道号归零 */
    do_floppy = recal_interrupt; /* 指向重新校正中断调用的C函数 */

    output_byte(FD_RECALIBRATE);            /* 向软盘控制器 FDC 发送重新校正命令 */
    output_byte(head << 2 | current_drive); /* 参数: 磁头号 + 当前驱动器号 */

    if (reset) {
        do_fd_request();
    }
}

/* 软盘控制器 FDC 复位中断调用函数
 * 该函数会在向控制器发送了复位操作命令后引发的软盘中断处理程序中被调用 */
static void reset_interrupt(void)
{
    output_byte(FD_SENSEI);  /* 发送检测中断状态命令(无参数) */
    (void)result();          /* 读取命令执行结果字节 */
    output_byte(FD_SPECIFY); /* 发送设定软驱参数命令 */
    output_byte(cur_spec1);  /* hut etc, 发送参数 */
    output_byte(6);          /* Head load time =6ms, DMA */

    do_fd_request(); /* 调用执行软盘请求 */
}

/* 复位软盘控制器
 * reset is done by pulling bit 2 of DOR low for a while.
 */
//// .
// 该函数首先设置参数和标志, 把复位标志清0, 然后把软驱变量cur_spec1和cur_rate
// 置为无效. 因为复位操作后, 这两个参数就需要重新设置. 接着设置需要重新校正标志,
// 并设置FDC执行复位操作后引发的软盘中断中调用的C函数reset_interrupt(). 最后
// 把DOR寄存器位2置0一会儿以对软驱执行复位操作. 当前数字输出寄存器DOR的位2
// 是启动/复位软驱位.
static void reset_floppy(void)
{
    int i;

    reset = 0;      /* 复位标志置0 */
    cur_spec1 = -1; /* 使无效 */
    cur_rate = -1;
    recalibrate = 1;                     /* 重新校正标志置位 */
    printk("Reset-floppy called\n\r");   /* 显示执行软盘复位操作信息 */
    cli();                               /* 关中断 */
    do_floppy = reset_interrupt;         /* 设置在中断处理程序中调用的函数 */
    outb_p(current_DOR & ~0x04, FD_DOR); /* 对软盘控制器 FDC 执行复位操作 */

    for (i = 0; i < 100; i++) {
        __asm__("nop");
    }

    outb(current_DOR, FD_DOR); /* 再启动软盘控制器 */
    sti();                     /* 开中断 */
}

/* 软驱启动定时中断调用函数
 *
 * 在执行一个请求项要求的操作之前, 为了等待指定软驱马达旋转起来
 * 到达正常的工作转速, do_fd_request 函数为准备好的当前请求项
 * 添加了一个延时定时器, 本函数即是该定时器到期时调用的函数
 */
static void floppy_on_interrupt(void)
{
    /* We cannot do a floppy-select, as that might sleep. We just force it */
    selected = 1;

    /* 如果当前驱动器号与 DOR 中的不同, 则需要重新设置 DOR 为当前驱动器.
     * 在输出当前 DOR 以后, 使用定时器延迟 2 个滴答时间, 以让命令得到执行
     * 然后再使用 transfer 获取数据
     *
     * 如果当前 DOR 已经是选择的驱动器, 可以直接使用 transfer 获取数据 */
    if (current_drive != (current_DOR & 3)) {
        current_DOR &= 0xFC;
        current_DOR |= current_drive;
        outb(current_DOR, FD_DOR);
        add_timer(2, &transfer);
    } else {
        transfer();
    }
}

/* 软盘读写请求项处理函数
 *
 * 该函数是软盘驱动程序中最主要的函数, 它的作用是:
 *  1. 处理有复位标志或重新校正标志置位情况
 *  2. 利用请求项中的设备号计算取得请求项指定软驱的参数块
 *  3. 利用内核定时器启动软盘读/写操作
 */
static void do_fd_request(void)
{
    unsigned int block;

    seek = 0; /* 清理寻道标记 */

    /* 有复位标志, 执行复位操作. 复位完成后
     * 复位中断处理还会调用 do_fd_request 获取数据 */
    if (reset) {
        reset_floppy();
        return;
    }

    /* 有重新校准标志, 执行重新校准操作. 校准完成后
     * 校准中断处理还会调用 do_fd_request 获取数据 */
    if (recalibrate) {
        recalibrate_floppy();
        return;
    }

    /* 用 INIT_REQUEST 宏来检测请求项的合法性 */
    INIT_REQUEST;

    /* 根据请求项设备号, 反推软盘参数 */
    floppy = (MINOR(CURRENT->dev) >> 2) + floppy_type;

    /* 如果当前驱动器和请求项中驱动器不是同一个, 则在执行读/写操作之前
     * 需要先让驱动器执行寻道处理(transfer 函数里面会考虑寻道的问题) */
    if (current_drive != CURRENT_DEV) {
        seek = 1;
    }

    /* 然后把当前驱动器号设置为请求项中指定的驱动器号 */
    current_drive = CURRENT_DEV;

    /* 设置读写起始扇区 block */
    block = CURRENT->sector;

    /* 因为每次读写是以块为单位(1 块为 2 个扇区), 所以起始扇区需要
     * 起码比磁盘总扇区数小 2 个扇区. 否则说明这个请求项参数无效 */
    if (block + 2 > floppy->size) {
        end_request(0);
        goto repeat;
    }

    /* 求对应在磁道上的扇区号/磁头号/磁道号/搜寻磁道号(对于软驱读不同格式的盘) */
    sector = block % floppy->sect;         /* 磁道上扇区号 */
    block /= floppy->sect;                 /* 起始磁道数 */
    head = block % floppy->head;           /* 磁头号 */
    track = block / floppy->head;          /* 磁道号 */
    seek_track = track << floppy->stretch; /* 寻道号 */

    if (seek_track != current_track) {
        /* 如果寻道号和当前磁道号不一致, 先寻道 */
        seek = 1;
    }

    sector++; /* 磁盘上实际扇区计数是从 1 算起 */

    /* 更新操作命令 */
    if (CURRENT->cmd == READ) {
        command = FD_READ;
    } else if (CURRENT->cmd == WRITE) {
        command = FD_WRITE;
    } else {
        panic("do_fd_request: unknown command");
    }

    /* 使用内核定时器, 等软盘转起来之后, 在 floppy_on_interrupt
     * 里面调用 transfer 获取数据 */
    add_timer(ticks_to_floppy_on(current_drive), &floppy_on_interrupt);
}

/* 各种类型软驱磁盘含有的数据块总数 */
static int floppy_sizes[] = {
    0,    0,    0,    0,    360,  360,  360,  360,  /* clang-format */
    1200, 1200, 1200, 1200, 360,  360,  360,  360,  /* clang-format */
    720,  720,  720,  720,  360,  360,  360,  360,  /* clang-format */
    720,  720,  720,  720,  1440, 1440, 1440, 1440, /* clang-format */
};

/* 软盘系统初始化 */
void floppy_init(void)
{
    /* 软盘含有的区块总数 */
    blk_size[MAJOR_NR] = floppy_sizes;

    /* 设置软盘块设备请求项的处理函数 do_fd_request */
    blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;

    /* 设置 0x26 中断向量描述符(8259A 芯片 IRQ6) */
    set_trap_gate(0x26, &floppy_interrupt);

    /* 复位软盘中断请求屏蔽位 */
    outb(inb_p(0x21) & ~0x40, 0x21);
}
