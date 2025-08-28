/*
 * 'tty.h' defines some structures used by tty_io.c and some defines.
 *
 * NOTE! Don't touch this without checking that nothing in rs_io.s or
 * con_io.s breaks. Some constants are hardwired into the system (mainly
 * offsets into 'tty_queue'
 *
 * 在修改这里的定义时, 一定要检查 rs_io.s 或 con_io.s 程序中不会出现问题
 * 在系统中有些常量是直接写在程序中的(主要是一些tty_queue中的偏移值)
 */

#ifndef _TTY_H
#define _TTY_H

#define MAX_CONSOLES 8 // 最大虚拟控制台数量
#define NR_SERIALS 2   // 串行终端数量
#define NR_PTYS 4      // 伪终端数量

extern int NR_CONSOLES; // 虚拟控制台数量

#include <termios.h>

#define TTY_BUF_SIZE 1024 // tty缓冲区(缓冲队列)大小

/**
 * tty 字符缓冲队列数据结构(这是个环形缓冲区)
 * 用于 tty_struc 结构中的读/写和辅助(规范)缓冲队列 */
struct tty_queue {
    /* 队列缓冲区中含有字符行数值(不是当前字符数), 对于串口终端，则存放串行端口地址 */
    unsigned long data;            // TODO: 缓冲区中的行数? + 串口的地址
    unsigned long head;            // 缓冲区中数据头游标位置
    unsigned long tail;            // 缓冲区中数据尾游标位置
    struct task_struct *proc_list; // 等待本队列的进程列表
    char buf[TTY_BUF_SIZE];        // 队列的缓冲区
};

#define IS_A_CONSOLE(min) (((min) & 0xC0) == 0x00)    // 是一个控制终端
#define IS_A_SERIAL(min) (((min) & 0xC0) == 0x40)     // 是一个串行终端
#define IS_A_PTY(min) ((min) & 0x80)                  // 是一个伪终端
#define IS_A_PTY_MASTER(min) (((min) & 0xC0) == 0x80) // 是一个主伪终端
#define IS_A_PTY_SLAVE(min) (((min) & 0xC0) == 0xC0)  // 是一个辅伪终端
#define PTY_OTHER(min) ((min) ^ 0x40)                 // 其他伪终端

/**
 * 以下定义了 tty 等待队列中缓冲区操作宏函数
 * tail 在前, head在后. 参见 tty_io.c 的图
 * TODO: 是个循环缓冲区, 研究一下环形缓冲区的操作 */

/*

假如有以下内存区域(SIZE=8): 0-1-2-3-4-5-6-7

1. 最开始的时候 tail = head = 0
2. 开始往里面让字符, 比如放了 A/B/C 三个字符, 此时内存变为
                              0-1-2-3-4-5-6-7- 8- 9-10-11-12-13-14-15
    0-1-2-3-4-5-6-7           0-1-2-3-4-5-6-7-X0-X1-X2-X3-X4-X5-X6-X7
        A-B-C                     A-B-C              ^
        ^     ^     ----->        ^     ^            ^
        |     |                   |     |            ^
        |     head                |     head         ^
        tail                      tail               ^

    这个数学原理有点不太好解释, 大概想一下:

    如此, 求队列空闲字符数, 就是 R = SIZE - (head - tail) = (SIZE - 1) + (tail -
head + 1) 因为 SIZE 设定值符合 2 的幂, 因此 R 可取的最大值就是 SIZE-1,
这么考虑使用 `&(SIZE-1)` 的问题:

    (SIZE - 1) + X = (SIZE - 1) + (X/SIZE) + (X%SIZE)
                   = (SIZE - 1) + 1 + (X/SIZE) + (X%SIZE) - 1

    把这个式子看成 SIZE 进制的处理, 那么如果做 `&(SIZE-1)` 相当于我们只有一个
SIZE 进制数位可以用, 多出来的数位都要丢掉, 于是上面的式子里面 (SIZE-1) + 1  +
(X/SIZE) 因为都需要用多出来的数位表示, 他们都被丢掉, 于是这种情况下出现了
(SIZE-1) + X = (X%SIZE) - 1, 进一步得到 SIZE + X = (X % SIZE)

    R = (tail + SIZE) - head - 1
      = SIZE + (tail - head - 1)
      = (tail - head - 1) % SIZE

3. 如果队列因为读写操作, 变成了下面这样
    0-1-2-3-4-5-6-7
    E-F     A-B-C-D
        ^   ^
        |   |
        |   tail
        head

    如此, 求队列空闲字符数, 就是 R = tail - head - 1, 在数值山与 `(tail - head -
1) & (SIZE-1)` 也是一样的
*/

// a 是指向缓冲区的索引指针, INC 把索引往后移动一字节, 若已超出缓冲区右侧,
// 则指针循环
#define INC(a) ((a) = ((a) + 1) & (TTY_BUF_SIZE - 1))

// a 指针后退 1 字节, 并循环
#define DEC(a) ((a) = ((a) - 1) & (TTY_BUF_SIZE - 1))

// 清空指定队列的缓冲区
#define EMPTY(a) ((a)->head == (a)->tail)

// 缓冲区还可存放字符的长度(空闲区长度). 这里的 -1 可能是 buf 故意不放满,
// 以免区别不出来真满和空状态
#define LEFT(a) (((a)->tail - (a)->head - 1) & (TTY_BUF_SIZE - 1))

// 缓冲区中最后一个位置
#define LAST(a) ((a)->buf[(TTY_BUF_SIZE - 1) & ((a)->head - 1)])

// 缓冲区满
#define FULL(a) (!LEFT(a))

// 缓冲区中已存放字符的长度(字符数)
#define CHARS(a) (((a)->head - (a)->tail) & (TTY_BUF_SIZE - 1))

// 从 queue 队列项缓冲区中取一字符(从tail处，并且tail+=1)
#define GETCH(queue, c)                                                                            \
    (void)({                                                                                       \
        c = (queue)->buf[(queue)->tail];                                                           \
        INC((queue)->tail);                                                                        \
    })

// 往 queue 队列项缓冲区中放置一字符(在head处，并且head+=1)
#define PUTCH(c, queue)                                                                            \
    (void)({                                                                                       \
        (queue)->buf[(queue)->head] = (c);                                                         \
        INC((queue)->head);                                                                        \
    })

// 判断终端键盘字符类型

#define INTR_CHAR(tty) ((tty)->termios.c_cc[VINTR])    // 中断符。发中断信号SIGINT
#define QUIT_CHAR(tty) ((tty)->termios.c_cc[VQUIT])    // 退出符。发退出信号SIGQUIT
#define ERASE_CHAR(tty) ((tty)->termios.c_cc[VERASE])  // 削除符。擦除一个字符
#define KILL_CHAR(tty) ((tty)->termios.c_cc[VKILL])    // 删除行。删除一行字符
#define EOF_CHAR(tty) ((tty)->termios.c_cc[VEOF])      // 文件结束符
#define START_CHAR(tty) ((tty)->termios.c_cc[VSTART])  // 开始符。恢复输出
#define STOP_CHAR(tty) ((tty)->termios.c_cc[VSTOP])    // 停止符。停止输出
#define SUSPEND_CHAR(tty) ((tty)->termios.c_cc[VSUSP]) // 挂起符, 发挂起信号SIGTSTP

// tty 数据结构
struct tty_struct {
    struct termios termios; // 终端 io 属性和控制字符数据结构

    int pgrp;    // 所属进程组
    int session; // 会话号
    int stopped; // 停止标志

    /* tty 写函数指针 */
    void (*write)(struct tty_struct *tty);

    /* tty 读队列 */
    struct tty_queue *read_q;

    /* tty 写队列 */
    struct tty_queue *write_q;

    /* tty 辅助队列(存放规范模式字符序列), 可称为规范(熟)模式队列 */
    struct tty_queue *secondary;
};

extern struct tty_struct tty_table[]; // tty结构数组
extern int fg_console;                // 前台控制台号

/**
 * 根据终端类型在 tty_table[] 中取对应终端号 nr 的 tty 结构指针
 * 第73行后半部分用于根据子设备号 dev 在 tty_table[] 表中选择对应的 tty 结构.
 * 如果 dev = 0, 表示正在使用前台终端, 因此直接使用终端号 fg_console 作为
 * tty_table[] 项索引取 tty 结构. 如果 dev 大于 0, 那么就要分两种情况考虑:
 *
 *  1. dev 是虚拟终端号
 *  2. dev 是串行终端号或者伪终端号
 *
 * 对于虚拟终端其 tty 结构在 tty_table[] 中索引项是 dev-1(0 -- 63),
 * 对于其它类型终端, 它们的 tty 结构索引项就是 dev. 例如, 如果 dev = 64,
 * 表示是一个串行终端 1, 则其 tty 结构就是 tty_table[dev]. 如果 dev = 1,
 * 则对应终端的 tty 结构是 tty_table[0]. 参见 tty_io.c 程序 */
#define TTY_TABLE(nr) (tty_table + ((nr) ? (((nr) < 64) ? (nr) - 1 : (nr)) : fg_console))

/**
 * @brief 这里给出了终端 termios 结构中可更改的特殊字符数组 c_cc[] 的初始值
 *
 * 该 termios 结构定义在 include/termios.h 中
 * 参考从 `#define VINTR 0` 行开始的十几个常量
 *
 * POSIX.1 定义了 11 个特殊字符, 但是 Linux 系统还另外定义了 SVR4 使用的 6
 * 个特殊字符 如果定义了 `_POSIX_VDISABLE(\0), 那么当某一项值等于 _POSIX_VDISABLE
 * 的值时, 表示 禁止使用相应的特殊字符
 *
 *    intr=^C        quit=^|        erase=del    kill=^U
 *    eof=^D        vtime=\0    vmin=\1        sxtc=\0
 *    start=^Q    stop=^S        susp=^Z        eol=\0
 *    reprint=^R    discard=^U    werase=^W    lnext=^V
 *    eol2=\0
 */
#define INIT_C_CC "\003\034\177\025\004\0\1\0\021\023\032\0\022\017\027\026\0"

void rs_init(void);  // 异步串行通信初始化。(kernel/chr_drv/serial.c)
void con_init(void); // 控制终端初始化。    (kernel/chr_drv/console.c)
void tty_init(void); // tty初始化。         (kernel/chr_drv/tty_io.c)

int tty_read(unsigned c, char *buf, int n);  // (kernel/chr_drv/tty_io.c)
int tty_write(unsigned c, char *buf, int n); // (kernel/chr_drv/tty_io.c)

void con_write(struct tty_struct *tty);  // (kernel/chr_drv/console.c)
void rs_write(struct tty_struct *tty);   // (kernel/chr_drv/serial.c)
void mpty_write(struct tty_struct *tty); // (kernel/chr_drv/pty.c)
void spty_write(struct tty_struct *tty); // (kernel/chr_drv/pty.c)

void copy_to_cooked(struct tty_struct *tty); // (kernel/chr_drv/tty_io.c)

void update_screen(void); // (kernel/chr_drv/console.c)

#endif
