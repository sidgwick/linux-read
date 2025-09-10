/*
 *  linux/kernel/chr_drv/tty_ioctl.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <termios.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/tty.h>

#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>

extern int session_of_pgrp(int pgrp);
extern int tty_signal(int sig, struct tty_struct *tty);

/**
 * @brief 波特率因子数组(或称为除数数组)
 *
 * 波特率与波特率因子的对应关系参见列表后说明.
 */
static unsigned short quotient[] = {
    0,   2304, 1536, 1047, /*  */
    857, 768,  576,  384,  /*  */
    192, 96,   64,   48,   /* 48=2400bps */
    24,  12,   6,    3,    /* 12=9600bps */
};

/**
 * @brief 修改传输波特率
 *
 * 在除数锁存标志 DLAB 置位情况下, 通过端口 0x3f8 和 0x3f9 向 UART 分别写入波特率因子低
 * 字节和高字节. 写完后再复位 DLAB 位
 *
 * 对于串口 2, 这两个端口分别是 0x2f8 和 0x2f9
 *
 * @param tty 终端对应的 tty 数据结构
 */
static void change_speed(struct tty_struct *tty)
{
    unsigned short port, quot;

    /* 检查参数 tty 指定的终端是否是串行终端, 若不是则退出
     * 串口终端 tty 结构, 其读缓冲队列 data 字段存放着串行端口基址(0x3f8 或 0x2f8),
     * 而一般控制台终端的 tty 结构的 read_q.data 字段值为 0 */
    if (!(port = tty->read_q->data)) {
        return;
    }

    /* 从终端 termios 结构的控制模式标志集中取得已设置的波特率索引号,
     * 并据此从波特率因子数组 quotient 中取得对应的波特率因子值 quot
     * CBAUD 是控制模式标志集中波特率位屏蔽码*/
    quot = quotient[tty->termios.c_cflag & CBAUD];

    /* 接下来把波特率因子 quot 写入串行端口对应 UART 芯片的波特率因子锁存器
     * 在写之前先把线路控制寄存器 LCR 的除数锁存访问比特位(DLAB, 位7)置 1.
     * 然后把 16 位的波特率因子低高字节分别写入端口 0x3f8/0x3f9 (分别对应
     * 波特率因子低/高字节锁存器), 最后再复位 DLAB 标志位 */
    cli();
    outb_p(0x80, port + 3);      /* set DLAB, 除数锁定标志 DLAB */
    outb_p(quot & 0xff, port);   /* LS of divisor, 低 8 位 */
    outb_p(quot >> 8, port + 1); /* MS of divisor, 高 8 位 */
    outb(0x03, port + 3);        /* reset DLAB */
    sti();
}

/**
 * @brief 刷新 tty 缓冲队列
 *
 * 令缓冲队列的头指针等于尾指针, 从而达到清空缓冲区的目的
 *
 * @param queue 指定的缓冲队列指针
 */
static void flush(struct tty_queue *queue)
{
    cli();
    queue->head = queue->tail;
    sti();
}

/**
 * @brief 等待字符发送出去
 *
 * @param tty
 */
static void wait_until_sent(struct tty_struct *tty)
{ /* do nothing - not implemented */
}

/**
 * @brief 发送 BREAK 控制符
 *
 * @param tty
 */
static void send_break(struct tty_struct *tty)
{ /* do nothing - not implemented */
}

/**
 * @brief 获取 tty 对应的 termios 结构
 *
 * @param tty 提供 termios 的 tty 设备
 * @param termios 接受结果的 termios 指针
 * @return int
 */
static int get_termios(struct tty_struct *tty, struct termios *termios)
{
    int i;

    verify_area(termios, sizeof(*termios));
    for (i = 0; i < (sizeof(*termios)); i++) {
        put_fs_byte(((char *)&tty->termios)[i], i + (char *)termios);
    }

    return 0;
}

/**
 * @brief 设置终端 termios 结构信息
 *
 * @param tty 指定终端的 tty 结构指针
 * @param termios 用户数据区 termios 结构指针
 * @param channel 从设备号
 * @return int
 */
static int set_termios(struct tty_struct *tty, struct termios *termios, int channel)
{
    int i, retsig;

    /* If we try to set the state of terminal and we're not in the
     * foreground, send a SIGTTOU.  If the signal is blocked or
     * ignored, go ahead and perform the operation.  POSIX 7.2)
     *
     * 如果当前进程使用的 tty 终端的进程组号与进程的进程组号不同, 即当前进程终端不在前台,
     * 表示当前进程试图修改不受控制的终端的 termios 结构. 因此根据 POSIX 标准的要求这里需
     * 要发送 SIGTTOU 信号让使用这个终端的进程先暂时停止执行, 以让我们先修改 termios 结构.
     * 如果发送信号函数 tty_signal 返回值是 ERESTARTSYS 或 EINTR, 则等一会再执行本次操作 */
    if ((current->tty == channel) && (tty->pgrp != current->pgrp)) {
        retsig = tty_signal(SIGTTOU, tty);
        if (retsig == -ERESTARTSYS || retsig == -EINTR) {
            return retsig;
        }
    }

    /* 拷贝用户态数据到内核态 */
    for (i = 0; i < (sizeof(*termios)); i++) {
        ((char *)&tty->termios)[i] = get_fs_byte(i + (char *)termios);
    }

    /* 因为用户有可能已修改了终端串行口传输波特率, 所以这里再根据 termios 结构中的控制模式标志
     * c_cflag 中的波特率信息修改串行 UART 芯片内的传输波特率. 最后返回 0 */
    change_speed(tty);

    return 0;
}

/**
 * @brief 读取 termio 结构中的信息
 *
 * 这个函数将 POSIX 标准的 termios 结构中相关的字段, 拷贝到 UNIX System V 标准的 termio 结构中
 *
 * @param tty 指定终端的 tty 结构指针
 * @param termio 保存 termio 结构信息的用户缓冲区
 * @return int
 */
static int get_termio(struct tty_struct *tty, struct termio *termio)
{
    int i;
    struct termio tmp_termio;

    verify_area(termio, sizeof(*termio));
    tmp_termio.c_iflag = tty->termios.c_iflag;
    tmp_termio.c_oflag = tty->termios.c_oflag;
    tmp_termio.c_cflag = tty->termios.c_cflag;
    tmp_termio.c_lflag = tty->termios.c_lflag;
    tmp_termio.c_line = tty->termios.c_line;

    for (i = 0; i < NCC; i++) {
        tmp_termio.c_cc[i] = tty->termios.c_cc[i];
    }

    for (i = 0; i < (sizeof(*termio)); i++) {
        put_fs_byte(((char *)&tmp_termio)[i], i + (char *)termio);
    }

    return 0;
}

/*
 */

/**
 * @brief 设置终端 termio 结构信息
 *
 * 将用户缓冲区termio的信息复制到终端的termios结构中
 *
 * This only works as the 386 is low-byt-first
 * 下面 termio 设置函数仅适用于低字节在前的 386 CPU
 *
 * @param tty 指定终端的 tty 结构指针
 * @param termio 用户数据区中 termio 结构
 * @param channel 从设备号
 * @return int
 */
static int set_termio(struct tty_struct *tty, struct termio *termio, int channel)
{
    int i, retsig;
    struct termio tmp_termio;

    /* 当前进程终端不在前台 */
    if ((current->tty == channel) && (tty->pgrp != current->pgrp)) {
        retsig = tty_signal(SIGTTOU, tty);
        if (retsig == -ERESTARTSYS || retsig == -EINTR)
            return retsig;
    }

    for (i = 0; i < (sizeof(*termio)); i++) {
        ((char *)&tmp_termio)[i] = get_fs_byte(i + (char *)termio);
    }

    *(unsigned short *)&tty->termios.c_iflag = tmp_termio.c_iflag;
    *(unsigned short *)&tty->termios.c_oflag = tmp_termio.c_oflag;
    *(unsigned short *)&tty->termios.c_cflag = tmp_termio.c_cflag;
    *(unsigned short *)&tty->termios.c_lflag = tmp_termio.c_lflag;
    tty->termios.c_line = tmp_termio.c_line;
    for (i = 0; i < NCC; i++) {
        tty->termios.c_cc[i] = tmp_termio.c_cc[i];
    }

    change_speed(tty);
    return 0;
}

/**
 * @brief tty 终端设备输入输出控制函数
 *
 * 该函数首先根据参数给出的设备号找出对应终端的 tty 结构, 然后根据控制命令 cmd 分别进行处理
 *
 * @param dev 设备号
 * @param cmd ioctl 命令
 * @param arg 操作参数指针
 * @return int
 *
 * TODO-DONE: 研究一下 dev 是哪里来的
 * 答: dev 是根据 ioctl 系统调用中的文件描述符, 找到对应的 inode, 然后在设备文件
 *     的 inode.zone[0] 里面获取到 dev
 */
int tty_ioctl(int dev, int cmd, int arg)
{
    struct tty_struct *tty;
    int pgrp;

    /* 根据设备号取得 tty 子设备号, 从而取得终端的 tty 结构 */
    if (MAJOR(dev) == 5) {
        /* 若主设备号是控制终端(5), 则进程的 tty 字段即是 tty 子设备号.
         * 此时如果进程的 tty 子设备号是负数, 表明该进程没有控制终端, 即
         * 不能发出该 ioctl 调用, 于是显示出错信息并停机 */
        dev = current->tty;
        if (dev < 0) {
            panic("tty_ioctl: dev<0");
        }
    } else {
        /* 如果主设备号不是 5 而是 4, 我们就可以从设备号中取出子设备号
         * 子设备号可以是控制台终端(0), 串口1终端(1), 串口2终端(2) */
        dev = MINOR(dev);
    }

    /* 根据子设备号 dev 在 tty_table 表中选择对应的 tty 结构
     *
     * dev 大于 0, 要分两种情况考虑:
     *  1. dev 是虚拟终端号
     *  2. dev 是串行终端号或者伪终端号
     *
     * 对于虚拟终端其 tty 结构在 tty_table 中索引项是 0-63 范围的 dev-1
     * 对于其它类型终端, 则它们的 tty 结构索引项就是 dev. 例如,
     * 如果 dev = 64, 表示是一个串行终端 1, 则其 tty 结构就是 ttb_table[dev].
     * 如果 dev = 1, 则对应终端的 tty 结构是 tty_table[0]
     *
     * 参见 tty_io.c */
    tty = tty_table + (dev ? ((dev < 64) ? dev - 1 : dev) : fg_console);

    /* 根据参数提供的 ioctl 命令 cmd 进行分别处理 */
    switch (cmd) {
    case TCGETS:
        /* 取相应终端 termios 结构信息. 此时参数 arg 是用户缓冲区指针 */
        return get_termios(tty, (struct termios *)arg);
    case TCSETSF:
        /* 在设置 termios 结构信息之前, 需要先等待输出队列中所有数据处理完毕,
         * 并且刷新(清空)输入队列. 再接着执行下面设置终端 termios 结构的操作 */
        flush(tty->read_q); /* fallthrough */
    case TCSETSW:
        /* 在设置终端 termios 的信息之前, 需要先等待输出队列中所有数据处理完(耗尽)
         * 对于修改参数会影响输出的情况, 就需要使用这种形式 */
        wait_until_sent(tty); /* fallthrough */
    case TCSETS:
        /* 设置相应终端 termios 结构信息. 此时参数 arg 是保存 termios 结构的用户缓冲区指针 */
        return set_termios(tty, (struct termios *)arg, dev);
    case TCGETA:
        /* 取相应终端 termio 结构中的信息. 此时参数 arg 是用户缓冲区指针 */
        return get_termio(tty, (struct termio *)arg);
    case TCSETAF:
        /* 在设置 termio 结构信息之前, 需要先等待输出队列中所有数据处理完毕,
         * 并且刷新(清空)输入队列. 再接着执行下面设置终端 termio 结构的操作.  */
        flush(tty->read_q); /* fallthrough */
    case TCSETAW:
        /* 在设置终端 termios 的信息之前, 需要先等待输出队列中所有数据处理完(耗尽).
         * 对于修改参数会影响输出的情况, 就需要使用这种形式.  */
        wait_until_sent(tty); /* fallthrough */
    case TCSETA:
        /* 设置相应终端 termio 结构信息. 此时参数 arg 是保存 termio 结构的用户缓冲区指针 */
        return set_termio(tty, (struct termio *)arg, dev);
    case TCSBRK:
        /* 如果参数 arg 值是 0, 则等待输出队列处理完毕(空), 并发送一个 break */
        if (!arg) {
            wait_until_sent(tty);
            send_break(tty);
        }
        return 0;
    case TCXONC:
        /* 开始/停止流控制
         * TCOOFF = Terminal Control Output OFF
         * TCOON = Terminal Control Output ON
         * TCIOFF = Terminal Control Input OFF
         * TCION = Terminal Control Input ON
         *
         * - 如果 arg 是 TCOOFF, 则挂起输出; 如果是 TCOON, 则恢复挂起的输出. 在挂起或恢复
         *   输出同时需要把写队列中的字符输出, 以加快用户交互响应速度
         * - 如果 arg 是 TCIOFF, 则挂起输入; 如果是 TCION, 则重新开启挂起的输入
         *
         * TODO: console.c 里面的 con_write 函数在 stopped 的时候不会处理输出 */
        switch (arg) {
        case TCOOFF:
            tty->stopped = 1; /* 停止终端输出 */
            tty->write(tty);  /* 写缓冲队列输出 */
            return 0;
        case TCOON:
            tty->stopped = 0; /* 恢复终端输出 */
            tty->write(tty);  /* 写缓冲队列输出 */
            return 0;
        case TCIOFF:
            /* 要求终端停止输入, 于是我们往终端写队列中放入 STOP 字符. 当终端收到该字符时就会暂停输入 */
            if (STOP_CHAR(tty)) {
                PUTCH(STOP_CHAR(tty), tty->write_q);
            }

            return 0;
        case TCION:
            /* 发送一个START字符, 让终端恢复传输 */
            if (START_CHAR(tty)) {
                PUTCH(START_CHAR(tty), tty->write_q);
            }

            return 0;
        }
        return -EINVAL; /* not implemented */
    case TCFLSH:
        /* 刷新已写输出但还没有发送, 或已收但还没有读的数据
         * 如果参数 arg:
         *  - 0: 则刷新(清空)输入队列
         *  - 1: 则刷新输出队列
         *  - 2: 则刷新输入和输出队列 */
        if (arg == 0) {
            flush(tty->read_q);
        } else if (arg == 1) {
            flush(tty->write_q);
        } else if (arg == 2) {
            flush(tty->read_q);
            flush(tty->write_q);
        } else {
            return -EINVAL;
        }

        return 0;
    case TIOCEXCL:
        /* 设置终端串行线路专用模式 */
        return -EINVAL; /* not implemented */
    case TIOCNXCL:
        /* 复位终端串行线路专用模式 */
        return -EINVAL; /* not implemented */
    case TIOCSCTTY:
        /* 设置 tty 为控制终端, 对应的还有个 `TIOCNOTTY` 表示不要控制终端 */
        return -EINVAL; /* set controlling term NI */
    case TIOCGPGRP:
        /* 读取终端进程组号(即读取前台进程组号)
         * 这个 case 分支也是库函数 tcgetpgrp 的实现 */
        verify_area((void *)arg, 4);
        put_fs_long(tty->pgrp, (unsigned long *)arg);
        return 0;
    case TIOCSPGRP:
        /* 设置终端进程组号(即设置前台进程组号)
         * 这个 case 分支也是库函数 tcsetpgrp 的实现 */

        /* 执行该命令的前提条件是进程必须有控制终端
         * 如果当前进程没有控制终端或者 dev 不是其控制终端, 或者控制终端现在的确是
         * 正在处理的终端 dev, 但进程的会话号与该终端 dev 的会话号不同, 则返回无终端错误信息
         *
         * TODO-DONE: tty session 实在哪里赋值的?
         * 答: 在 open tty 设备文件的时候处理的. 执行任务的时候, 除了简单的使用 execve 执行程序之外,
         *     也可以通过打开 tty 设备, 来关联 tty 和 task session. 参考 init/main.c 里面的处理 */
        if ((current->tty < 0) || (current->tty != dev) || (tty->session != current->session)) {
            return -ENOTTY;
        }

        pgrp = get_fs_long((unsigned long *)arg);
        if (pgrp < 0) {
            return -EINVAL;
        }

        /* pgrp 的会话号与当前进程的不同, 则返回许可错误信息 */
        if (session_of_pgrp(pgrp) != current->session) {
            return -EPERM;
        }

        /* 终端的进程组号为 prgp, prgp 成为前台进程组 */
        tty->pgrp = pgrp;
        return 0;
    case TIOCOUTQ:
        /* 返回输出队列中还未送出的字符数 */
        verify_area((void *)arg, 4);
        put_fs_long(CHARS(tty->write_q), (unsigned long *)arg);
        return 0;
    case TIOCINQ:
        /* 返回输入队列中还未读取的字符数 */
        verify_area((void *)arg, 4);
        put_fs_long(CHARS(tty->secondary), (unsigned long *)arg);
        return 0;
    case TIOCSTI:
        /* 模拟终端输入操作.
         * 该命令以一个指向字符的指针作为参数, 并假设该字符是在终端上键入的.
         * 用户必须在该控制终端上具有超级用户权限或具有读许可权限 */
        return -EINVAL; /* not implemented */
    case TIOCGWINSZ:
        /* 读取终端设备窗口大小信息 */
        return -EINVAL; /* not implemented */
    case TIOCSWINSZ:
        /* 设置终端设备窗口大小信息 */
        return -EINVAL; /* not implemented */
    case TIOCMGET:
        /* 返回 MODEM 状态控制引线的当前状态比特位标志集 */
        return -EINVAL; /* not implemented */
    case TIOCMBIS:
        /* 设置单个 modem 状态控制引线的状态 */
        return -EINVAL; /* not implemented */
    case TIOCMBIC:
        /* 复位单个 MODEM 状态控制引线的状态 */
        return -EINVAL; /* not implemented */
    case TIOCMSET:
        /* 设置 MODEM 状态引线的状态. 如果某一比特位置位, 则 modem 对应的状态引线将置为有效 */
        return -EINVAL; /* not implemented */
    case TIOCGSOFTCAR:
        /* 读取软件载波检测标志(1-开启; 0-关闭) */
        return -EINVAL; /* not implemented */
    case TIOCSSOFTCAR:
        /* 设置软件载波检测标志(1-开启; 0-关闭) */
        return -EINVAL; /* not implemented */
    default:
        return -EINVAL;
    }
}
