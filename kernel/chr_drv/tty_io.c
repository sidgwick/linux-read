/*
 *  linux/kernel/tty_io.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'tty_io.c' gives an orthogonal feeling to tty's, be they consoles
 * or rs-channels. It also implements echoing, cooked mode etc.
 *
 * Kill-line thanks to John T Kohl, who also corrected VMIN = VTIME = 0.
 */

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

/* 给出定时警告(alarm)信号在信号位图中对应的比特屏蔽位 */
#define ALRMMASK (1 << (SIGALRM - 1))

#include <asm/segment.h>
#include <asm/system.h>
#include <linux/sched.h>
#include <linux/tty.h>

int kill_pg(int pgrp, int sig, int priv);
int is_orphaned_pgrp(int pgrp);

#define _L_FLAG(tty, f) ((tty)->termios.c_lflag & f) /* 本地模式标志 */
#define _I_FLAG(tty, f) ((tty)->termios.c_iflag & f) /* 输入模式标志 */
#define _O_FLAG(tty, f) ((tty)->termios.c_oflag & f) /* 输出模式标志 */

/* 取 termios 结构终端本地模式标志集中的一个标志
 *
 * 原始模式中用户以字符流作为读取对象, 不识别其中的控制字符(如文件结束符) */

/* 取规范模式标志 */
#define L_CANON(tty) _L_FLAG((tty), ICANON)
/* 取信号标志 */
#define L_ISIG(tty) _L_FLAG((tty), ISIG)
/* 取回显字符标志 */
#define L_ECHO(tty) _L_FLAG((tty), ECHO)
/* 规范模式时取回显擦出标志 */
#define L_ECHOE(tty) _L_FLAG((tty), ECHOE)
/* 规范模式时取KILL擦除当前行标志 */
#define L_ECHOK(tty) _L_FLAG((tty), ECHOK)
/* 取回显控制字符标志 */
#define L_ECHOCTL(tty) _L_FLAG((tty), ECHOCTL)
/* 规范模式时取KILL擦除行并回显标志 */
#define L_ECHOKE(tty) _L_FLAG((tty), ECHOKE)
/* 对于后台输出发送SIGTTOU信号 */
#define L_TOSTOP(tty) _L_FLAG((tty), TOSTOP)

/* 取 termios 结构输入模式标志集中的一个标志 */
#define I_UCLC(tty) _I_FLAG((tty), IUCLC) /* 取大写到小写转换标志 */
#define I_NLCR(tty) _I_FLAG((tty), INLCR) /* 取换行符NL转回车符CR标志 */
#define I_CRNL(tty) _I_FLAG((tty), ICRNL) /* 取回车符CR转换行符NL标志 */
#define I_NOCR(tty) _I_FLAG((tty), IGNCR) /* 取忽略回车符CR标志 */
#define I_IXON(tty) _I_FLAG((tty), IXON)  /* 取输入控制流标志XON */

/* 取 termios 结构输出模式标志集中的一个标志 */
#define O_POST(tty) _O_FLAG((tty), OPOST)   /* 取执行输出处理标志 */
#define O_NLCR(tty) _O_FLAG((tty), ONLCR)   /* 取换行符NL转回车换行符CR-NL标志 */
#define O_CRNL(tty) _O_FLAG((tty), OCRNL)   /* 取回车符CR转换行符NL标志 */
#define O_NLRET(tty) _O_FLAG((tty), ONLRET) /* 取换行符NL执行回车功能的标志 */
#define O_LCUC(tty) _O_FLAG((tty), OLCUC)   /* 取小写转大写字符标志 */

/* 取 termios 结构控制标志集中波特率. CBAUD 是波特率屏蔽码 */
#define C_SPEED(tty) ((tty)->termios.c_cflag & CBAUD)

/* 判断 tty 终端是否已挂线(hang up), 即其传输波特率是否为 0 */
#define C_HUP(tty) (C_SPEED((tty)) == B0)

#ifndef MIN
/* 取最小值 */
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* 下面定义 tty 终端使用的缓冲队列结构数组 tty_queues 和 tty 终端表结构数组 tty_table */

/* QUEUES 是 tty 终端使用的缓冲队列最大数量
 * 伪终端分主从两种(master和slave). 每个 tty 终端使用 3 个 tty 缓冲队列,
 * 它们分别是用于缓冲键盘或串行输入的读队列(read_queue), 用于缓冲屏幕或串行
 * 输出的写队列(write_queue), 以及用于保存规范模式字符的辅助缓冲队列(secondary)
 * 因此有下面的计算公式 */
#define QUEUES (3 * (MAX_CONSOLES + NR_SERIALS + 2 * NR_PTYS))

static struct tty_queue tty_queues[QUEUES]; /* tty 缓冲队列数组 */
struct tty_struct tty_table[256];           /* tty 表结构数组 */

/* 各前缀含义:
 *
 * con - console
 * rs - serial
 * mpty - master pseudo-terminal
 * spty - slave pseudo-terminal
 */

/* 设定各种类型的tty终端所使用缓冲队列结构在 tty_queues 数组中的起始项位置 */

/* 8 个虚拟控制台终端占用 tty_queues 数组开头 24 项 */
#define con_queues tty_queues
/* 两个串行终端占用随后的 6 项 */
#define rs_queues ((3 * MAX_CONSOLES) + tty_queues)
/* 4 个主伪终端占用随后的 12 项 */
#define mpty_queues ((3 * (MAX_CONSOLES + NR_SERIALS)) + tty_queues)
/* 4 个从伪终端占用随后的 12 项 */
#define spty_queues ((3 * (MAX_CONSOLES + NR_SERIALS + NR_PTYS)) + tty_queues)

/* 设定各种类型 tty 终端所使用的 tty 结构在 tty_table 数组中的起始项位置 */

/* 8 个虚拟控制台终端可用 tty_table 数组开头 64 项, 实际用到了 NR_CONSOLE 项 */
#define con_table tty_table
/* 两个串行终端使用从 64 开始的项, 最多 64 项, 实际用到了 NR_SERIALS 项 */
#define rs_table (64 + tty_table)
/* 4 个主伪终端使用从 128 开始的项, 最多 64 项, 实际用到了 NR_PTYS 项 */
#define mpty_table (128 + tty_table)
/* 4 个从伪终端使用从 192 开始的项, 最多 64 项, 实际用到了 NR_PTYS 项 */
#define spty_table (192 + tty_table)

int fg_console = 0; /* 当前前台控制台号(0-7) */

/**
 * @brief tty 读写缓冲队列结构地址表
 *
 * 供 rs_io.s 程序使用, 用于取得读写缓冲队列结构的地址
 *
 * these are the tables used by the machine code handlers.
 * you can implement virtual consoles.
 */
struct tty_queue *table_list[] = {
    con_queues + 0, con_queues + 1, /* 第一个控制台的 读/写队列 */
    rs_queues + 0,  rs_queues + 1,  /* 第一个串口的 读/写队列 */
    rs_queues + 3,  rs_queues + 4,  /* 第二个串口的读写队列 */
};

/**
 * @brief 改变前台控制台
 *
 * 将前台控制台设定为指定的虚拟控制台
 *
 * @param new_console 指定的新控制台号
 */
void change_console(unsigned int new_console)
{
    if (new_console == fg_console || new_console >= NR_CONSOLES) {
        return;
    }

    fg_console = new_console;

    /* 更新 table_list 中的前台控制台 读/写 队列结构地址 */
    table_list[0] = con_queues + 0 + fg_console * 3;
    table_list[1] = con_queues + 1 + fg_console * 3;

    /* 最后更新当前前台控制台屏幕 */
    update_screen();
}

/**
 * @brief 如果队列缓冲区空则让进程进入可中断睡眠状态
 *
 * 进程在取队列缓冲区中字符之前需要调用此函数加以验证
 *
 * @param queue 指定队列的指针
 */
static void sleep_if_empty(struct tty_queue *queue)
{
    cli();

    /* 如果当前进程没有信号要处理, 并且指定的队列缓冲区空, 则让进程进入可中断睡眠状态
     * 注意这里的 queue->proc_list 在 sched.c 里面被赋值为 current, 相当于记录
     * 了是那个任务被 sleep 了 */
    while (!(current->signal & ~current->blocked) && EMPTY(queue)) {
        interruptible_sleep_on(&queue->proc_list);
    }

    sti();
}

/**
 * @brief 若队列缓冲区满则让进程进入可中断的睡眠状态
 *
 * 进程在往队列缓冲区中写入字符之前需要调用此函数判断队列情况
 *
 * @param queue 指定队列的指针
 */
static void sleep_if_full(struct tty_queue *queue)
{
    /* 如果队列缓冲区不满则返回退出 */
    if (!FULL(queue)) {
        return;
    }

    cli();

    /* 否则若进程没有信号需要处理, 并且队列缓冲区中空闲剩余区长度 < 128, 则让进程进入可中断睡眠状态 */
    while (!(current->signal & ~current->blocked) && LEFT(queue) < 128) {
        interruptible_sleep_on(&queue->proc_list);
    }

    sti();
}

/**
 * @brief 等待按键
 *
 * 如果前台控制台 `读队列` 缓冲区空, 则让进程进入可中断睡眠状态
 */
void wait_for_keypress(void)
{
    sleep_if_empty(tty_table[fg_console].secondary);
}

/**
 * @brief 复制为规范模式字符序列
 *
 * 根据终端 termios 结构中设置的各种标志, 将指定 tty 终端读队列缓冲区中
 * 的字符复制转换成规范模式(熟模式)字符并存放在辅助队列(规范模式队列)中
 *
 * @param tty 指定终端的 tty 结构指针
 */
void copy_to_cooked(struct tty_struct *tty)
{
    signed char c;

    /* tty 的 读/写/辅助 队列, 至少有一个非空 */
    if (!(tty->read_q || tty->write_q || tty->secondary)) {
        printk("copy_to_cooked: missing queues\n\r");
        return;
    }

    while (1) {
        /* 读入队列空 */
        if (EMPTY(tty->read_q)) {
            break;
        }

        /* 辅助队列已经满了 */
        if (FULL(tty->secondary)) {
            break;
        }

        /* 从读入队列, 取一个字符进来 */
        GETCH(tty->read_q, c);

        /* 13=CR, 10=LF, 这个版本的 LF 也写作 NL */
        if (c == 13) {
            /* 如果该字符是回车符 CR, 那么:
             * 1. 若回车转换行标志 CRNL 置位, 则将字符转换为换行符 LF
             * 2. 若忽略回车标志 NOCR 置位, 则忽略该字符
             * 3. 其他情况不做处理, 使用原始的 CR 字符 */
            if (I_CRNL(tty)) {
                c = 10;
            } else if (I_NOCR(tty)) {
                continue;
            }
        } else if (c == 10 && I_NLCR(tty)) {
            /* 如果字符是换行符 NL, 且换行转回车标志 NLCR 置位, 则将其转换为回车符 CR */
            c = 13;
        }

        /* 如果设置了 UpperCase to LowerCase, 则将大写转化为小写 */
        if (I_UCLC(tty)) {
            c = tolower(c);
        }

        /* 如果本地模式标志集中规范模式标志 CANON 已置位, 则对读取的字符进行以下处理 */
        if (L_CANON(tty)) {
            /* 键盘终止控制字符 KILL, 用于删除一行  */
            if ((KILL_CHAR(tty) != _POSIX_VDISABLE) && (c == KILL_CHAR(tty))) {
                /* deal with killing the input line
                 * 要求队列不能是以下情况:
                 *  1. 辅助队列为空
                 *  2. 辅助队列的最后一个字符是换行 LF
                 *  3. 辅助队列最后一个自复式 EOF */
                while (!(EMPTY(tty->secondary) || (c = LAST(tty->secondary)) == 10 ||
                         ((EOF_CHAR(tty) != _POSIX_VDISABLE) && (c == EOF_CHAR(tty))))) {
                    /* 本地回显标志 ECHO 置位 */
                    if (L_ECHO(tty)) {
                        if (c < 32) {
                            /* 若字符是控制字符(值 < 32), 往 tty 写队列中放入擦除控制字
                             * 符 ERASE (^H), 这么做的原因是, 控制字符在写队列是使用 2 个
                             * 字符表示的, 因此除了后面那个 ERASE, 这里需要额外加一个 */
                            PUTCH(127, tty->write_q);
                        }

                        /* 放入一个擦除字符 ERASE, 并且使用 tty 写函数将字符输出 */
                        PUTCH(127, tty->write_q);
                        tty->write(tty);
                    }

                    /* 最后将 tty 辅助队列头指针后退 1 字节
                     * 注意这里是循环, 结合循环条件, 这里最后实现的效果就是删除了一整行 */
                    DEC(tty->secondary->head);
                }

                continue;
            }

            /* 遇到的是擦除字符 */
            if ((ERASE_CHAR(tty) != _POSIX_VDISABLE) && (c == ERASE_CHAR(tty))) {
                /* 仅能擦除 `换行` 和 EOF 之外的字符 */
                if (EMPTY(tty->secondary) || (c = LAST(tty->secondary)) == 10 ||
                    ((EOF_CHAR(tty) != _POSIX_VDISABLE) && (c == EOF_CHAR(tty))))
                    continue;

                if (L_ECHO(tty)) {
                    if (c < 32) {
                        PUTCH(127, tty->write_q);
                    }

                    PUTCH(127, tty->write_q);
                    tty->write(tty);
                }

                /* 擦除辅助队列的数据 */
                DEC(tty->secondary->head);
                continue;
            }
        }

        /* 设置了 IXON 标志, 则使终端停止/开始输出控制字符起作用
         * 如果没有设置此标志, 那么停止和开始字符将被作为一般字符供进程读取
         *
         * 1. 对于控制台来说, 这里的 tty->write 是 console.c 中的 con_write 函数, 控制台
         *    将由于发现 stopped=1 而会立刻暂停在屏幕上显示新字符.
         * 2. 对于伪终端, 由于设置了终端 stopped 标志而会暂停写操作.
         * 3. 对于串行终端, 也应该在发送终端过程中根据终端 stopped 标志暂停发送, 但本版未实现 */
        if (I_IXON(tty)) {
            if ((STOP_CHAR(tty) != _POSIX_VDISABLE) && (c == STOP_CHAR(tty))) {
                tty->stopped = 1; /* 置 tty 停止标志, 让 tty 暂停输出 */
                tty->write(tty);  /* TODO: 不是已经暂停输出了吗?? */
                continue;
            }

            if ((START_CHAR(tty) != _POSIX_VDISABLE) && (c == START_CHAR(tty))) {
                tty->stopped = 0; /* 复位 tty 停止标志, 恢复 tty 输出 */
                tty->write(tty);
                continue;
            }
        }

        /* 若输入模式标志集中 ISIG 标志置位, 表示终端键盘可以产生信号,
         * 则在收到控制字符 INTR/QUIT/SUSP/DSUSP 时, 需要为进程产生相应的信号 */
        if (L_ISIG(tty)) {
            /* 键盘中断符(^C), 则向当前进程之进程组中所有进程发送键盘中断信号 SIGINT */
            if ((INTR_CHAR(tty) != _POSIX_VDISABLE) && (c == INTR_CHAR(tty))) {
                kill_pg(tty->pgrp, SIGINT, 1);
                continue;
            }

            /* 退出符(^\), 则向当前进程之进程组中所有进程发送键盘退出信号 SIGQUIT */
            if ((QUIT_CHAR(tty) != _POSIX_VDISABLE) && (c == QUIT_CHAR(tty))) {
                kill_pg(tty->pgrp, SIGQUIT, 1);
                continue;
            }

            /* 暂停符(^Z), 则向当前进程发送暂停信号 SIGTSTP */
            if ((SUSPEND_CHAR(tty) != _POSIX_VDISABLE) && (c == SUSPEND_CHAR(tty))) {
                /* TODO: 要求不能是孤儿进程组?? */
                if (!is_orphaned_pgrp(tty->pgrp)) {
                    kill_pg(tty->pgrp, SIGTSTP, 1);
                }

                continue;
            }
        }

        /* 如果该字符是换行符 NL, 或者是文件结束符 EO, 表示一行字符已处理完,
         * 则把辅助缓冲队列中当前含有字符行数值 secondary.data 增 1.
         * 如果在函数 tty_read 中取走一行字符, 该值即会被减 1. */
        if (c == 10 || (EOF_CHAR(tty) != _POSIX_VDISABLE && c == EOF_CHAR(tty))) {
            tty->secondary->data++;
        }

        /* 本地模式标志集中回显标志 ECHO 置位 */
        if (L_ECHO(tty)) {
            if (c == 10) {
                /* 字符是换行符 NL, 则将换行符 NL 和回车符 CR 放入 tty 写队列缓冲区中 */
                PUTCH(10, tty->write_q);
                PUTCH(13, tty->write_q);
            } else if (c < 32) {
                /* 如果字符是控制字符(值<32), 且回显控制字符标志 ECHOCTL 置位
                 * 则将字符 '^' 和字符 c+64 放入 tty 写队列中 (也即会显示^C、^H等) */
                if (L_ECHOCTL(tty)) {
                    PUTCH('^', tty->write_q);
                    PUTCH(c + 64, tty->write_q);
                }
            } else {
                /* 其他情况将原始字符放到写队列即可 */
                PUTCH(c, tty->write_q);
            }

            /* 在屏幕上回显数据 */
            tty->write(tty);
        }

        /* 把处理好的数据放到辅助队列里面 */
        PUTCH(c, tty->secondary);
    }

    /* 如果有进程在等待辅助队列, 因为现在已经有数据了, 唤醒它 */
    wake_up(&tty->secondary->proc_list);
}

/**
 * @brief 向使用终端的进程组中所有进程发送信号
 *
 * 在后台进程组中的一个进程访问控制终端时, 该函数用于向后台进程组中的所有
 * 进程发送 SIGTTIN 或 SIGTTOU 信号
 *
 * 无论后台进程组中的进程是否已经阻塞或忽略掉了这两个信号, 当前进程都将立刻
 * 退出读写操作而返回
 *
 * Called when we need to send a SIGTTIN or SIGTTOU to our process
 * group
 *
 * We only request that a system call be restarted if there was if the
 * default signal handler is being used.  The reason for this is that if
 * a job is catching SIGTTIN or SIGTTOU, the signal handler may not want
 * the system call to be restarted blindly.  If there is no way to reset the
 * terminal pgrp back to the current pgrp (perhaps because the controlling
 * tty has been released on logout), we don't want to be in an infinite loop
 * while restarting the system call, and have it always generate a SIGTTIN
 * or SIGTTOU.  The default signal handler will cause the process to stop
 * thus avoiding the infinite loop problem.  Presumably the job-control
 * cognizant parent will fix things up before continuging its child process.
 *
 * @param sig 待发送的信号
 * @param tty 发送信号的 TTY
 * @return int
 *
 * TODO: 重新理解这个函数
 */
int tty_signal(int sig, struct tty_struct *tty)
{
    /* 不能给孤儿进程组中的进程发送信号
    TODO: WHY??? */
    if (is_orphaned_pgrp(current->pgrp)) {
        return -EIO; /* don't stop an orphaned pgrp */
    }

    (void)kill_pg(current->pgrp, sig, 1);

    if ((current->blocked & (1 << (sig - 1))) ||
        ((int)current->sigaction[sig - 1].sa_handler == 1)) {
        /* 信号被屏蔽或者信号梳理函数是忽略函数 */
        return -EIO; /* Our signal will be ignored */
    } else if (current->sigaction[sig - 1].sa_handler) {
        /* 有合法的信号处理函数, 就正常处理信号 */
        return -EINTR; /* We _will_ be interrupted :-) */
    } else {
        /* We _will_ be interrupted :-) */
        /* (but restart after we continue) */
        return -ERESTARTSYS;
    }
}

/**
 * @brief tty 读函数
 *
 * 从终端辅助缓冲队列中读取指定数量的字符, 放到用户指定的缓冲区中
 *
 * @param channel 子设备号
 * @param buf 用户缓冲区指针
 * @param nr 欲读字节数
 * @return int 返回已读字节数
 */
int tty_read(unsigned channel, char *buf, int nr)
{
    struct tty_struct *tty;
    struct tty_struct *other_tty = NULL;
    char c, *b = buf;
    int minimum, time;

    if (channel > 255) {
        return -EIO;
    }

    tty = TTY_TABLE(channel);
    if (!(tty->write_q || tty->read_q || tty->secondary)) {
        return -EIO;
    }

    /* 如果当前进程使用的是这里正在处理的 tty 终端, 但该终端的进程组号却与当
     * 前进程组号不同, 表示当前进程是后台进程组中的一个进程, 即进程不在前台 */
    if ((current->tty == channel) && (tty->pgrp != current->pgrp)) {
        /* 向当前进程组发送 SIGTTIN 信号, 以便暂停当前进程组的所有进程,
         * 并返回等待成为前台进程组后再执行读操作 */
        return (tty_signal(SIGTTIN, tty));
    }

    /* TODO: 0x80 是什么? 0x40 又是什么?
     * Master PTY 固定使用 0x40标志位，Slave 使用 0x80标志位
     * 如果 tty 终端是一个伪终端, 则再取得另一个对应伪终端(主从
     * 伪终端)的 tty 结构 other_tty
     *
     *
     * 若 channel = 0xC0 是 Master PTY, 则 `0xC0 ^ 0x40 = 1100_0000 ^ 0100_0000 = 1000_0000`
     *
     * 即清除 Master 标志，转换为 Slave 的设备索引
     *
     * 在数据结构层面, tty_table 是存放所有 TTY 设备的结构数组, 每个伪终端在主设备表中索引为 N 时,
     * 其对应的从设备在从设备表中的索引就是 N+128 (因为 NR_PTYS 定义为 4, 主设备表从 128 开始,
     * 从设备表从 192 开始). 这种设计通过指针运算直接定位到配对设备, 避免了复杂的查找过程 */
    if (channel & 0x80) {
        other_tty = tty_table + (channel ^ 0x40);
    }

    /* VMIN 表示为了满足读操作而需要读取的最少字符个数. VTIME 是一个 1/10 秒计数计时值 */
    time = 10L * tty->termios.c_cc[VTIME];
    minimum = tty->termios.c_cc[VMIN];

    /* 如果 tty 终端处于规范模式, 则设置最小要读取字符数 minimum 等于进程欲读字符数 nr.
     * 同时把进程读取 nr 字符的超时时间值设置为极大值(不会超时)
     *
     * NOTICE: 注意这里设置的是进程的 timeout 字段
     *
     * 当终端处于非规范模式下:
     *
     *  1. 若设置了最少读取字符数 minimum, 则先临时设置进程读超时定时值为无限大, 以让
     *     进程先读取辅助队列中已有字符. 如果读到的字符数不足 minimum 的话, 后面代码会
     *     根据指定的超时值 time 来设置进程的读超时值 timeout, 并会等待读取其余字符
     *  2. 若没有设置最少读取字符数 minimum, 则将其设置为进程欲读字符数 nr, 并且如
     *     果设置了超时定时值 time 的话, 就把进程读字符超时定时值 timeout 设置为系统当
     *     前时间值 + 指定的超时值 time, 同时复位 time
     *
     * 即对于规范模式下的读取操作, 它不受 VTIME 和 VMIN 对应控制字符值的约束和控制,
     * 它们仅在非规范模式(生模式)操作中起作用 */
    if (L_CANON(tty)) {
        minimum = nr;
        current->timeout = 0xffffffff;
        time = 0;
    } else if (minimum) {
        current->timeout = 0xffffffff;
    } else {
        minimum = nr; /* 最多读取调用者要求的那么多字符 */
        if (time) {
            current->timeout = time + jiffies;
        }

        time = 0;
    }

    if (minimum > nr) {
        minimum = nr;
    }

    /* 现在我们开始从辅助队列中循环取出字符并放到用户缓冲区 buf 中 */
    while (nr > 0) {
        /* 在循环过程中, 如果当前终端是伪终端, 那么我们就执行其对应的另一个伪终
         * 端的写操作函数, 让另一个伪终端把字符写入当前伪终端辅助队列缓冲区中. 即让
         * 另一终端把写队列缓冲区中字符复制到当前伪终端读队列缓冲区中, 并经行规程
         * 函数转换后放入当前伪终端辅助队列中 */
        if (other_tty) {
            other_tty->write(other_tty);
        }

        cli();

        /* 1. 辅助队列为空
         * 2. 规范模式, 读队列不满, 辅助队列还不到一行
         *
         * 规范模式时内核以行为单位为用户提供数据, 因此在该模式下辅助队列中必须起码有一行
         * 字符可供取用, 即 secondary.data 起码是 1 才行 */
        if (EMPTY(tty->secondary) ||
            (L_CANON(tty) && !FULL(tty->read_q) && !tty->secondary->data)) {
            /* 没有设置过进程读字符超时值(为0), 或者当前进程目前收到信号, 就先退出循环体 */
            if (!current->timeout || (current->signal & ~current->blocked)) {
                sti();
                break;
            }

            /* 如果本终端是一个从伪终端, 且其对应的主伪终端已经挂断 */
            if (IS_A_PTY_SLAVE(channel) && C_HUP(other_tty)) {
                break;
            }

            /* 进程进入可中断睡眠模式等待 */
            interruptible_sleep_on(&tty->secondary->proc_list);
            sti();
            continue;
        }

        sti();

        /* 程序执行到这里, 说明读缓冲区有数据了 */

        /* 只要队列不空, 就一直读
         * do...while block 内部根据换行或者 EOF 条件做 break */
        do {
            GETCH(tty->secondary, c);

            /* 换行或者 EOF, 缓冲区行数减 1 */
            if ((EOF_CHAR(tty) != _POSIX_VDISABLE && c == EOF_CHAR(tty)) || c == 10) {
                tty->secondary->data--;
            }

            /* 规范模式下, 收到 EOF, 退出读取, 其他情况, 将读到的数据拷贝到用户控件缓冲区 */
            if ((EOF_CHAR(tty) != _POSIX_VDISABLE && c == EOF_CHAR(tty)) && L_CANON(tty)) {
                break;
            } else {
                put_fs_byte(c, b++);
                if (!--nr) {
                    break;
                }
            }

            /* 规范模式, 且收到了换行符, 结束读循环 */
            if (c == 10 && L_CANON(tty)) {
                break;
            }
        } while (nr > 0 && !EMPTY(tty->secondary));

        /* 执行到此, 那么如果 tty 终端处于规范模式下, 说明我们可能读到了换行符或者遇到了文件
         * 结束符. 如果是处于非规范模式下, 那么说明我们已经读取了nr个字符, 或者辅助队列已经
         * 被取空了 */

        /* 读到了数据, 就把进程唤醒 */
        wake_up(&tty->read_q->proc_list);

        /* 看看是否设置过超时定时值 time
         * 如果超时定时值 time 不为0, 我们就要求等待一定的时间让其他进程可以把字符写入
         * 读队列中, 于是设置进程读超时定时值为系统当前时间 jiffies + 读超时值 time */
        if (time) {
            current->timeout = time + jiffies;
        }

        /* 终端处于规范模式, 或者已经读取了 nr 个字符, 退出大循环 */
        if (L_CANON(tty) || b - buf >= minimum) {
            break;
        }
    }

    /* 读取 tty 字符循环操作结束, 因此复位进程的读取超时定时值 timeout */
    current->timeout = 0;

    /* 当前进程已收到信号并但还没有读取到任何字符, 返回重新启动系统调用 */
    if ((current->signal & ~current->blocked) && !(b - buf)) {
        return -ERESTARTSYS;
    }

    return (b - buf);
}

/**
 * @brief tty 写函数
 *
 * 把用户缓冲区中的字符放入 tty 写队列缓冲区中
 *
 * @param channel 子设备号
 * @param buf 缓冲区指针
 * @param nr 写字节数
 * @return int 返回已写字节数
 */
int tty_write(unsigned channel, char *buf, int nr)
{
    static int cr_flag = 0;
    struct tty_struct *tty;
    char c, *b = buf;

    if (channel > 255) {
        return -EIO;
    }

    tty = TTY_TABLE(channel);
    if (!(tty->write_q || tty->read_q || tty->secondary)) {
        return -EIO;
    }

    /* 如果若终端本地模式标志集中设置了 TOSTOP, 表示后台进程输出时需要发送信号 SIGTTOU.
     * 如果当前进程使用的是这里正在处理的 tty 终端, 但该终端的进程组号却与当前进程组号不
     * 同, 即表示当前进程是后台进程组中的一个进程, 即进程不在前台. 于是我们要停止当前进
     * 程组的所有进程. 因此这里就需要向当前进程组发送 SIGTTOU 信号, 并返回等待成为前台进
     * 程组后再执行写操作
     *
     * TODO: 进程不在前台的标志是什么? */
    if (L_TOSTOP(tty) && (current->tty == channel) && (tty->pgrp != current->pgrp)) {
        return (tty_signal(SIGTTOU, tty));
    }

    while (nr > 0) {
        /* 写队列满的话, 就先等一会儿再操作
         * 这个等待可能会因为信号到来返回, 因此下一步就是检测信号 */
        sleep_if_full(tty->write_q);

        /* 检测到有信号发送过来, 跳出循环 */
        if (current->signal & ~current->blocked) {
            break;
        }

        /* 没读够, 就一直读 */
        while (nr > 0 && !FULL(tty->write_q)) {
            c = get_fs_byte(b);

            /* 设置了 `执行输出处理` 标记 */
            if (O_POST(tty)) {
                /* 如设置了对应的标志特性, 做回车(CR)~换行(LF) 之间的转换 */
                if (c == '\r' && O_CRNL(tty)) {
                    c = '\n';
                } else if (c == '\n' && O_NLRET(tty)) {
                    c = '\r';
                }

                /* 如果该字符是换行符 LF 并且回车标志 cr_flag 没有置位, 但换行转回车-换行标志 ONLCR
                 * 置位的话, 则将 cr_flag 标志置位, 并将一回车符放入写队列中
                 *
                 * TODO: 这是在干啥? 好像是吧 LF 替换换成了 CR 字符? */
                if (c == '\n' && !cr_flag && O_NLCR(tty)) {
                    cr_flag = 1;
                    PUTCH(13, tty->write_q);
                    continue;
                }

                /* 小写转大写标志 OLCUC 置位, 将该字符转成大写字符 */
                if (O_LCUC(tty)) {
                    c = toupper(c);
                }
            }

            b++;
            nr--;
            cr_flag = 0;

            /* 写一字节到输出队列 */
            PUTCH(c, tty->write_q);
        }

        /* 调用对应 tty 写函数, 把字符写到屏幕上或者发送出去 */
        tty->write(tty);

        /* 若还有字节要写, 这里调用调度程序先去执行其他任务, 这边等待写队列中字符被取走 */
        if (nr > 0) {
            schedule();
        }
    }

    return (b - buf);
}

/**
 * @brief tty 中断处理调用函数 - 字符规范模式处理
 *
 * 将指定 tty 终端队列缓冲区中的字符复制或转换成规范(熟)模式字符并存放在辅助队列中
 * 该函数会在串口读字符中断(rs_io.s)和键盘中断(kerboard.S)中被调用
 *
 * Jeh, sometimes I really like the 386.
 * This routine is called from an interrupt,
 * and there should be absolutely no problem
 * with sleeping even in an interrupt (I hope).
 * Of course, if somebody proves me wrong, I'll
 * hate intel for all time :-). We'll have to
 * be careful and see to reinstating the interrupt
 * chips before calling this, though.
 *
 * I don't think we sleep here under normal circumstances
 * anyway, which is good, as the task sleeping might be
 * totally innocent.
 *
 * @param tty 指定的 tty 终端号
 */
void do_tty_interrupt(int tty)
{
    copy_to_cooked(TTY_TABLE(tty));
}

/* 字符设备初始化函数
 * 当前为空, 为以后扩展做准备 */
void chr_dev_init(void)
{
}

/**
 * @brief tty 终端初始化函数
 *
 * 初始化所有终端缓冲队列, 初始化串口终端和控制台终端
 */
void tty_init(void)
{
    int i;

    // 首先
    //
    // 然后先初步设置所有终端的tty结构. .

    /* 初始化所有终端的缓冲队列结构, 设置初值 */
    for (i = 0; i < QUEUES; i++) {
        tty_queues[i] = (struct tty_queue){0, 0, 0, 0, ""};
    }

    /* 对于串行终端的读/写缓冲队列, 将它们的 data 字段设置为串行端口基地址值
     * 串口 1 是 0x3f8, 串口 2 是 0x2f8 */
    rs_queues[0] = (struct tty_queue){0x3f8, 0, 0, 0, ""};
    rs_queues[1] = (struct tty_queue){0x3f8, 0, 0, 0, ""};
    rs_queues[3] = (struct tty_queue){0x2f8, 0, 0, 0, ""};
    rs_queues[4] = (struct tty_queue){0x2f8, 0, 0, 0, ""};

    /* 填充空白的 tty 接口
     * 注意特殊字符数组 c_cc 的设置(include/linux/tty.h) */
    for (i = 0; i < 256; i++) {
        tty_table[i] = (struct tty_struct){
            // clang-format off
            {0, 0, 0, 0, 0, INIT_C_CC},
            0, 0, 0, NULL, NULL, NULL, NULL,
            // clang-format on
        };
    }

    /* 接着初始化控制台终端(console.c), 把 con_init 放在这里, 是因为我们需要根
     * 据显示卡类型和显示内存容量来确定系统中虚拟控制台的数量 NR_CONSOLES. 该值被用于随后
     * 的控制台 tty 结构初始化循环中 */
    con_init();

    for (i = 0; i < NR_CONSOLES; i++) {
        /* 对于控制台的 tty->termios 的结构:
         *  1. 输入模式标志集被初始化为 ICRNL 标志
         *  2. 输出模式标志被初始化为含有后处理标志 OPOST 和把 NL 转换成 CRNL 的标志 ONLCR
         *  3. 本地模式标志集被初始化含有 IXON/ICANON/ECHO/ECHOCTL/ECHOKE标志
         *  4. 控制字符数组 c_cc 被设置含有初始值 INIT_C_CC */

        con_table[i] = (struct tty_struct){
            // clang-format off
            .termios = {
                .c_iflag = ICRNL,         /* change incoming CR to NL */
                .c_oflag = OPOST | ONLCR, /* change outgoing NL to CRNL */
                .c_cflag = 0,
                .c_lflag = IXON | ISIG | ICANON | ECHO | ECHOCTL | ECHOKE,
                .c_line = 0, /* console termio */
                .c_cc = INIT_C_CC
            },
            .pgrp = 0, /* initial pgrp */
            .session = 0, /* initial session */
            .stopped = 0, /* initial stopped */
            .write = con_write, /* tty 的写函数 */
            .read_q = con_queues + 0 + i * 3, /* tty 的读队列 */
            .write_q = con_queues + 1 + i * 3, /* tty 的写队列 */
            .secondary = con_queues + 2 + i * 3 /* tty 的辅助队列 */
            // clang-format on
        };
    }

    /* 串行终端 */
    for (i = 0; i < NR_SERIALS; i++) {
        rs_table[i] = (struct tty_struct){
            // clang-format off
            {
                0, /* no translation */
                0, /* no translation */
                B2400 | CS8,
                0,
                0,
                INIT_C_CC
            },
            0,
            0,
            0,
            rs_write,
            rs_queues + 0 + i * 3,
            rs_queues + 1 + i * 3,
            rs_queues + 2 + i * 3,
            // clang-format on
        };
    }

    /* 伪终端
     * 伪终端是配对使用的, 即一个主(master)伪终端配有一个从(slave)伪终端 */
    for (i = 0; i < NR_PTYS; i++) {
        mpty_table[i] = (struct tty_struct){
            // clang-format off
            {
                0, /* no translation */
                0, /* no translation */
                B9600 | CS8,
                0,
                0,
                INIT_C_CC
            },
            0,
            0,
            0,
            mpty_write,
            mpty_queues + 0 + i * 3,
            mpty_queues + 1 + i * 3,
            mpty_queues + 2 + i * 3,
            // clang-format on
        };

        spty_table[i] = (struct tty_struct){
            // clang-format off
            {
                0, /* no translation */
                0, /* no translation */
                B9600 | CS8,
                IXON | ISIG | ICANON,
                0,
                INIT_C_CC
            },
            0,
            0,
            0,
            spty_write,
            spty_queues + 0 + i * 3,
            spty_queues + 1 + i * 3,
            spty_queues + 2 + i * 3
            // clang-format on
        };
    }

    /* 最后初始化串行中断处理程序和串行接口 1 和 2 (serial.c) */
    rs_init();

    /* 显示系统含有的虚拟控制台数 NR_CONSOLES 和伪终端数 NR_PTYS */
    printk("%d virtual consoles\n\r", NR_CONSOLES);
    printk("%d pty's\n\r", NR_PTYS);
}
