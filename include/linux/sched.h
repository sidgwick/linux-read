#ifndef _SCHED_H
#define _SCHED_H

/**
 * 调度程序头文件, 定义了任务结构 task_struct, 任务 0 的数据,
 * 还有一些有关描述符参数设置和获取的嵌入式汇编函数宏语句 */

// 定义系统时钟滴答频率(1百赫兹, 每个滴答10ms)
#define HZ 100

#define NR_TASKS 64             // 系统中同时最多任务(进程)数
#define TASK_SIZE 0x04000000    // 每个任务的长度(64MB)
#define LIBRARY_SIZE 0x00400000 // 动态加载库长度(4MB)

// 任务长度必须是4MB的倍数
#if (TASK_SIZE & 0x3fffff)
#error "TASK_SIZE must be multiple of 4M"
#endif

// 库长度也必须是4MB的倍数
#if (LIBRARY_SIZE & 0x3fffff)
#error "LIBRARY_SIZE must be a multiple of 4M"
#endif

// 加载库的长度不得大于任务长度的一半
#if (LIBRARY_SIZE >= (TASK_SIZE / 2))
#error "LIBRARY_SIZE too damn big!"
#endif

// 任务长度*任务总个数必须为4GB
#if (((TASK_SIZE >> 16) * NR_TASKS) != 0x10000)
#error "TASK_SIZE*NR_TASKS must be 4GB"
#endif

// 在进程逻辑地址空间中动态库被加载的位置(60MB处)
#define LIBRARY_OFFSET (TASK_SIZE - LIBRARY_SIZE)

#define CT_TO_SECS(x) ((x) / HZ) /* 滴答数转秒数 */
#define CT_TO_USECS(x)                                                                             \
    (((x) % HZ) * 1000000 /                                                                        \
     HZ) /* 滴答数对应的不足 1s 的毫秒数. 取余是求出不足 1s 的滴答数, 把它乘以 \
            1_000_000 / HZ 得到对应的毫秒数  */

#define FIRST_TASK task[0]           // 任务 0 比较特殊, 所以特意给它单独定义一个符号
#define LAST_TASK task[NR_TASKS - 1] // 任务数组中的最后一项任务

#include <linux/fs.h>
#include <linux/head.h>
#include <linux/mm.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/time.h>

#if (NR_OPEN > 32)
#error "Currently the close-on-exec-flags and select masks are in one long, max 32 files/proc"
#endif

// 这里定义了进程运行时可能处的状态

#define TASK_RUNNING 0         // 进程正在运行或已准备就绪
#define TASK_INTERRUPTIBLE 1   // 进程处于可中断等待状态
#define TASK_UNINTERRUPTIBLE 2 // 进程处于不可中断等待状态, 主要用于I/O操作等待
#define TASK_ZOMBIE 3          // 进程处于僵死状态, 已经停止运行, 但父进程还没发信号
#define TASK_STOPPED 4         // 进程已停止

#ifndef NULL
#define NULL ((void *)0)
#endif

// 复制进程的页目录页表. Linus认为这是内核中最复杂的函数之一. ( mm/memory.c, 105
// )
extern int copy_page_tables(unsigned long from, unsigned long to, long size);
// 释放页表所指定的内存块及页表本身. ( mm/memory.c, 150 )
extern int free_page_tables(unsigned long from, unsigned long size);

// 调度程序的初始化函数. ( kernel/sched.c, 385 )
extern void sched_init(void);

// 进程调度函数. ( kernel/sched.c, 104 )
extern void schedule(void);

// 异常(陷阱)中断处理初始化函数, 设置中断调用门并允许中断请求信号. (
// kernel/traps.c, 181 )
extern void trap_init(void);

// 显示内核出错信息, 然后进入死循环. ( kernel/panic.c, 16 ).
extern void panic(const char *str);

// 往tty上写指定长度的字符串. ( kernel/chr_drv/tty_io.c, 290 ).
extern int tty_write(unsigned minor, char *buf, int count);

// 定义函数指针类型
typedef int (*fn_ptr)();

// 下面是数学协处理器使用的结构, 主要用于保存进程切换时i387的执行状态信息
struct i387_struct {
    long cwd;          // 控制字(Control word).
    long swd;          // 状态字(Status word).
    long twd;          // 标记字(Tag word).
    long fip;          // 协处理器代码指针.
    long fcs;          // 协处理器代码段寄存器.
    long foo;          // 内存操作数的偏移位置.
    long fos;          // 内存操作数的段值.
    long st_space[20]; /* 8*10 bytes for each FP-reg = 80 bytes,
                          8个10字节的协处理器累加器. */
};

// 任务状态段数据结构
struct tss_struct {
    long back_link; /* 16 high bits zero */
    long esp0;
    long ss0; /* 16 high bits zero */
    long esp1;
    long ss1; /* 16 high bits zero */
    long esp2;
    long ss2; /* 16 high bits zero */
    long cr3;
    long eip;
    long eflags;
    long eax, ecx, edx, ebx;
    long esp;
    long ebp;
    long esi;
    long edi;
    long es;           /* 16 high bits zero */
    long cs;           /* 16 high bits zero */
    long ss;           /* 16 high bits zero */
    long ds;           /* 16 high bits zero */
    long fs;           /* 16 high bits zero */
    long gs;           /* 16 high bits zero */
    long ldt;          /* 16 high bits zero */
    long trace_bitmap; /* bits: trace 0, bitmap 16-31 */
    struct i387_struct i387;
};

// 下面是任务(进程)数据结构, 或称为进程描述符
struct task_struct {
    /* these are hardcoded - don't touch */

    /* -1 unrunnable, 0 runnable, >0 stopped,
     * 任务的运行状态(-1不可运行, 0可运行(就绪), >0已停止) */
    long state;

    long counter;  // 任务运行时间计数(递减)(滴答数), 运行时间片
    long priority; // 优先数. 任务开始运行时counter=priority, 越大运行越长

    /* 信号处理相关的字段 */
    long signal; /* 信号位图, 每个比特位代表一种信号, 信号值=位偏移值+1 */
    /* 信号执行属性结构, 对应信号将要执行的操作和标志信息 */
    struct sigaction sigaction[32];
    long blocked; /* bitmap of masked signals, 进程信号屏蔽码(对应信号位图) */

    /* various fields */

    /* 任务执行停止的退出码, 其父进程会取
     * 值等于 0 表示进程的停止状态并非由信号触发​​, 或者​​停止状态已被处理并重置 */
    int exit_code;

    unsigned long start_code; // 代码段开始地址(任务在 4GB 线性空间的线性地址,
                              // 在 64M 边界上)
    unsigned long end_code;   // 代码长度(字节数)
    unsigned long end_data;   // 代码长度 + 数据长度(字节数)
    unsigned long brk;        // 总长度(字节数)

    unsigned long start_stack; // 堆栈段地址
    long pid;                  // 进程标识号(进程号)
    long pgrp;                 // 进程组号
    long session;              // 会话号
    long leader;               // 会话首领
    int groups[NGROUPS]; /* 进程所属组号. 一个进程可属于多个组. 虽然是 int 类型,
                            但是 group id 都是 short 的 */

    /*
     * pointers to parent process, youngest child, younger sibling,
     * older sibling, respectively.  (p->father can be replaced with
     * p->p_pptr->pid)
     */
    struct task_struct *p_pptr; // Parent, 指向父进程的指针
    struct task_struct *p_cptr; // Child, 指向最新子进程的指针
    /* Younger Sibling 指向比自己后创建的相邻进程的指针 */
    struct task_struct *p_ysptr;
    /* Older Sibling 指向比自己早创建的相邻进程的指针 */
    struct task_struct *p_osptr;

    // TODO: 这些 UID/GID 的区别是什么?
    unsigned short uid;  // 用户标识号(用户id)
    unsigned short euid; // 有效用户id
    unsigned short suid; // 保存的用户id
    unsigned short gid;  // 组标识号(组id)
    unsigned short egid; // 有效组id
    unsigned short sgid; // 保存的组id

    unsigned long timeout; // 内核定时超时值(用于 tty 的读写队列控制)
    unsigned long alarm;   // 报警定时值(滴答数)

    long utime;      // 用户态运行时间(滴答数)
    long stime;      // 系统态运行时间(滴答数)
    long cutime;     // 子进程用户态运行时间(滴答数)
    long cstime;     // 子进程系统态运行时间(滴答数)
    long start_time; // 进程开始运行时刻

    struct rlimit rlim[RLIM_NLIMITS]; // 进程资源使用统计数组

    /* per process flags, defined below, 各进程的标志, 在下面第149行开始定义(还未使用) */
    unsigned int flags;

    unsigned short used_math; // 标志：是否使用了协处理器

    /* file system info */

    /* -1 if no tty, so it must be signed, 进程使用tty终端的子设备号. -1表示没有使用 */
    int tty;

    /**
     * umask 用于限制新创建文件的默认权限:
     *  - umask 中的位为 1 表示要屏蔽的权限
     *  - umask 中的位为 0 表示允许的权限
     * 这行代码确保了新文件不会获得比 umask 允许的更多权限
     */

    unsigned short umask;       // 文件创建属性屏蔽位
    struct m_inode *pwd;        // 当前工作目录 i 节点结构指针
    struct m_inode *root;       // 根目录 i 节点结构指针
    struct m_inode *executable; // 执行文件 i 节点结构指针
    struct m_inode *library;    // 被加载库文件 i 节点结构指针

    /* 执行时关闭文件句柄位图标志. (参见include/fcntl.h) */
    unsigned long close_on_exec;

    /* 文件结构指针表, 最多 32 项, 表项号即是文件描述符的值 */
    struct file *filp[NR_OPEN];

    /* ldt for this task: 0-zero, 1-cs, 2-ds&ss
     局部描述符表. 0-空, 1-代码段cs, 2-数据和堆栈段ds&ss */
    struct desc_struct ldt[3];

    /* tss for this task, 进程的任务状态段信息结构 */
    struct tss_struct tss;
};

/*
 * Per process flags
 *
 * 每个进程的标志
 * 打印对齐警告信息. 还未实现, 仅用于486
 */
#define PF_ALIGNWARN 0x00000001 /* Print alignment warning msgs */
                                /* Not implemented yet, only for 486*/

/**
 * @brief idle 进程
 *
 *  INIT_TASK is used to set up the first task table, touch at
 * your own risk!. Base=0, limit=0x9ffff (=640kB)
 *
 * INIT_TASK 用于设置第 1 个任务表, 若想修改, 责任自负!
 * 基址 Base = 0, 段长 limit = 0x9ffff(=640kB).
 *
 * 对应上面任务结构的第1个任务的信息
 */
#define INIT_TASK                                                                                  \
    {                                                                                              \
        .state = 0,                                                                                \
        .counter = 15,                                                                             \
        .priority = 15,                                                                            \
        .signal = 0,                                                                               \
        .sigaction = {{}},                                                                         \
        .blocked = 0,                                                                              \
        .exit_code = 0,                                                                            \
        .start_code = 0,                                                                           \
        .end_code = 0,                                                                             \
        .end_data = 0,                                                                             \
        .brk = 0,                                                                                  \
        .start_stack = 0,                                                                          \
        .pid = 0,                                                                                  \
        .pgrp = 0,                                                                                 \
        .session = 0,                                                                              \
        .leader = 0,                                                                               \
        .groups = {NOGROUP},                                                                       \
        .p_pptr = &init_task.task, /* idle 进程的父进程还是他自己 */                               \
        .p_cptr = 0,                                                                               \
        .p_ysptr = 0,                                                                              \
        .p_osptr = 0,                                                                              \
        .uid = 0,                                                                                  \
        .euid = 0,                                                                                 \
        .suid = 0,                                                                                 \
        .gid = 0,                                                                                  \
        .egid = 0,                                                                                 \
        .sgid = 0,                                                                                 \
        .timeout = 0,                                                                              \
        .alarm = 0,                                                                                \
        .utime = 0,                                                                                \
        .stime = 0,                                                                                \
        .cutime = 0,                                                                               \
        .cstime = 0,                                                                               \
        .start_time = 0,                                                                           \
        .rlim =                                                                                    \
            {/* idle 进程资源被设置为没有限制 */                                                   \
             {.rlim_cur = 0x7fffffff, .rlim_max = 0x7fffffff},                                     \
             {.rlim_cur = 0x7fffffff, .rlim_max = 0x7fffffff},                                     \
             {.rlim_cur = 0x7fffffff, .rlim_max = 0x7fffffff},                                     \
             {.rlim_cur = 0x7fffffff, .rlim_max = 0x7fffffff},                                     \
             {.rlim_cur = 0x7fffffff, .rlim_max = 0x7fffffff},                                     \
             {.rlim_cur = 0x7fffffff, .rlim_max = 0x7fffffff}},                                    \
        .flags = 0,                                                                                \
        .used_math = 0,                                                                            \
        .tty = -1,                                                                                 \
        .umask = 0022,                                                                             \
        .pwd = NULL,                                                                               \
        .root = NULL,                                                                              \
        .executable = NULL,                                                                        \
        .library = NULL,                                                                           \
        .close_on_exec = 0,                                                                        \
        .filp = {NULL},                                                                            \
        .ldt =                                                                                     \
            {                                                                                      \
                {0, 0},                                                                            \
                {0x9f, 0xc0fa00},                                                                  \
                {0x9f, 0xc0f200},                                                                  \
            },                                                                                     \
        .tss =                                                                                     \
            {                                                                                      \
                .back_link = 0,                                                                    \
                .esp0 = PAGE_SIZE + (long)&init_task,                                              \
                .ss0 = 0x10,                                                                       \
                .esp1 = 0,                                                                         \
                .ss1 = 0,                                                                          \
                .esp2 = 0,                                                                         \
                .ss2 = 0,                                                                          \
                .cr3 = (long)&pg_dir,                                                              \
                .eip = 0,                                                                          \
                .eflags = 0,                                                                       \
                .eax = 0,                                                                          \
                .ecx = 0,                                                                          \
                .edx = 0,                                                                          \
                .ebx = 0,                                                                          \
                .esp = 0,                                                                          \
                .ebp = 0,                                                                          \
                .esi = 0,                                                                          \
                .edi = 0,                                                                          \
                .es = 0x17,                                                                        \
                .cs = 0x17,                                                                        \
                .ss = 0x17,                                                                        \
                .ds = 0x17,                                                                        \
                .fs = 0x17,                                                                        \
                .gs = 0x17,                                                                        \
                .ldt = _LDT(0),                                                                    \
                .trace_bitmap = 0x80000000,                                                        \
                .i387 = {},                                                                        \
            },                                                                                     \
    }

/*
关于上面 INIT-TASK 的数据段, 代码段的段描述符情况解释

BBRL-RRBB == 00c0-fa00
BBBB-LLLL == 0000-009f

BASE= 00000000
Limit = 9fFFF
g_db_l_avl = 0xC = 1100 = 4Kb, 32Bits
p_dpl_s_type = 0xFA = 1111_1010 = 存在, dpl=3, s=1, XCRA=1010
可执行非依从可读代码段 p_dpl_s_type = f2 = 1111_0010 = 存在, dpl=3, s=1,
XERA=0010 不可执行正向生长可写数据段
*/

extern struct task_struct *task[NR_TASKS];      // 任务指针数组
extern struct task_struct *last_task_used_math; // 上一个使用过协处理器的进程
extern struct task_struct *current;             // 当前运行进程结构指针变量
extern unsigned long volatile jiffies;          // 从开机开始算起的滴答数(10ms/滴答)
extern unsigned long startup_time;              // 开机时间. 从1970:0:0:0开始计时的秒数
extern int jiffies_offset;                      // 用于累计需要调整的时间嘀嗒数

#define CURRENT_TIME (startup_time + (jiffies + jiffies_offset) / HZ) // 当前时间(秒数)

/* 添加定时器函数(定时时间jiffies滴答数, 定时到时调用函数 fn, 参考: kernel/sched.c) */
extern void add_timer(long jiffies, void (*fn)(void));

/* 不可中断的等待睡眠(参考 kernel/sched.c) */
extern void sleep_on(struct task_struct **p);

/* 可中断的等待睡眠(参考 kernel/sched.c) */
extern void interruptible_sleep_on(struct task_struct **p);

/* 明确唤醒睡眠的进程(参考 kernel/sched.c) */
extern void wake_up(struct task_struct **p);

/* 检查当前进程是否在指定的用户组 grp 中 */
extern int in_group_p(gid_t grp);

/*
 * Entry into gdt where to find first TSS. 0-nul, 1-cs, 2-ds, 3-syscall
 * 4-TSS0, 5-LDT0, 6-TSS1 etc ...
 *
 * 寻找第1个TSS在全局表中的入口. 0-没有用nul, 1-代码段cs, 2-数据段ds,
 * 3-系统段syscall 4-任务状态段TSS0, 5-局部表LTD0, 6-任务状态段TSS1, 等
 *
 * 从该英文注释可以猜想到, Linus当时曾想把系统调用的代码专门放在GDT表中
 * 第4个独立的段中但后来并没有那样做,
 * 于是就一直把GDT表中第4个描述符项(上面syscall项)闲置在一旁
 *
 * 下面的常量定义体现了 GDT 表中数据的组织
 *  - 0# 是 CPU 要求的 NULL 描述符
 *  - 1# 是代码段描述符
 *  - 2# 是数据段描述符
 *  - 3# 是系统段 syscall, 据说这个描述符没有用到
 *  - 4# 是第 1 个任务的 TSS
 *  - 5# 是第 1 个任务的 LDT
 *  - 6# 是第 2 个任务的 TSS
 *  - 7# 是第 2 个任务的 LDT
 *  - ....
 */
#define FIRST_TSS_ENTRY 4                     // GDT 中第 1 个 TSS 的索引号
#define FIRST_LDT_ENTRY (FIRST_TSS_ENTRY + 1) // GDT 中第 1 个 LDT 的索引号

/*   (n<<4)+(FIRST_TSS_ENTRY<<3)
 * = ((n<<1)+(FIRST_TSS_ENTRY)) << 3
 * = (TSS_n 的索引编号) << 3
 * = TSS_n 的描述符选择子, RPL=0, TI=0 */
#define _TSS(n) ((((unsigned long)n) << 4) + (FIRST_TSS_ENTRY << 3)) // 第 n 个任务的 TSS 选择子
#define _LDT(n) ((((unsigned long)n) << 4) + (FIRST_LDT_ENTRY << 3)) // 第 n 个任务的 LDT 选择子
#define ltr(n)                                                                                     \
    __asm__("ltr %%ax" ::"a"(_TSS(n)))              // 装载第 N 个任务的 TSS. TaskRegister \
                                       // 里面装载的是 Task State Segement
#define lldt(n) __asm__("lldt %%ax" ::"a"(_LDT(n))) // 装载第 N 个任务的 LDT

// 取当前运行任务的任务号(是任务数组中的索引值, 与进程号pid不同)
// 返回：n - 当前任务号. 用于( kernel/traps.c )
#define str(n)                                                                                     \
    __asm__("str %%ax\n\t"      /* 将任务寄存器中TSS段的选择符复制到ax中 */                        \
            "subl %2,%%eax\n\t" /* 当前任务的 TSS 指针减去                    \
                                   (FIRST_TSS_ENTRY<<3), 这实际上是 TSS(Task0)    \
                                   到 TSS(current) 的偏移量 */           \
            "shrl $4,%%eax"     /* 因为每个任务的 TSS + LDT 一共占据 16 字节, \
                               因此上面计算好的偏移量除以 16              \
                               就是当前任务的任务号 */       \
            : "=a"(n)                                                                              \
            : "a"(0), "i"(FIRST_TSS_ENTRY << 3))

/* switch_to(n) should switch tasks to task nr n, first
 * checking that n isn't the current task, in which case it does nothing.
 * This also clears the TS-flag if the task we switched to has used
 * tha math co-processor latest.
 *
 * TODO: 思考
 *          1. TSS 里面为啥不保存 CR0 ?
 *          2. _crrrent 哪里来的?
 *
 * 梳理一下结构体在内存中的布局(小端序)
 * struct {long a, b} X = {.a = 0xABCDEFGH, .b=012345678}
 * 内存中存储为: GH EF CD AB 78 56 34 12
 * 因此 ljmp X.a 可以实现跳转到 5678 表示的 TSS 任务
 *
 * TODO: 研究一下为啥任务切换回来之后, %ecx 里面是什么, 以及它和
 * last_task_used_math 的关系
 *
 * 任务切换回来之后, 在判断原任务上次执行是否使用过协处理器时, 是通过
 * 将原任务指针与保存在 last_task_used_math
 * 变量中的上次使用过协处理器任务指针进行比较而 作出的, 参见文件 kernel/sched.c
 * 中有关 math_state_restore 函数的说明
 *
 * switch_to(n)将切换当前任务到任务nr, 即n. 首先检测任务n不是当前任务,
 * 如果是则什么也不做退出. 如果我们切换到的任务最近(上次运行)使用过数学
 * 协处理器的话, 则还需复位控制寄存器 cr0 中的 TS 标志 */
#define switch_to(n)                                                                                \
    {                                                                                               \
        struct {                                                                                    \
            long a, b;                                                                              \
        } __tmp;                                                                                    \
        __asm__("cmpl %%ecx,current\n\t" /* 检查任务 n 是不是当前任务, 是直接退出 \
                                      */ \
                "je 1f\n\t"                                                                         \
                "movw %%dx,%1\n\t"        /* 将任务的 TSS 描述符指针, 移动到 __tmp.b */             \
                "xchgl %%ecx,current\n\t" /* 交换 %ecx 和 current, 这样 current              \
                                         里面就是任务 n 的 task 结构指针了 */ \
                "ljmp %0\n\t" /* 跳转到新任务执行, 这里主要用 TSS 描述符,         \
                             因此上面只填充了 b 字段 */ \
                "cmpl %%ecx,last_task_used_math\n\t" /* 任取切换回来之后,                 \
                                                    从这里继续,                          \
                                                    检查数学协处理器的情况 */ \
                "jne 1f\n\t"                                                                        \
                "clts\n" /* 如果本任务之前用到了协处理器, 需要清空 TS 标记 */                       \
                "1:" ::"m"(*&__tmp.a),                                                              \
                "m"(*&__tmp.b), "d"(_TSS(n)), "c"((long)task[n]));                                  \
    }

// 4Kb 对齐
#define PAGE_ALIGN(n) (((n) + 0xfff) & 0xfffff000)

// 更新位于地址 addr 处描述符中的基地址字段(基地址是base)
// 假如 base = %edx = HGEF_CDAB
#define _set_base(addr, base)                                                                      \
    __asm__("movw %%dx,%0\n\t"   /* base 一开始被加载到 %edx, 因此这里就是把 \
                                *(addr+2) = DCBA */       \
            "rorl $16,%%edx\n\t" /* ror $16, %ebx --> CDAB_HGEF */                                 \
            "movb %%dl,%1\n\t"   /* *(addr+4) = dl = EF */                                         \
            "movb %%dh,%2"       /* *(addr+7) = dh = GH */                                         \
            :                                                                                      \
            : "m"(*((addr) + 2)), "m"(*((addr) + 4)), "m"(*((addr) + 7)), "d"(base)                \
            : "dx")

// 更新位于地址 addr 处描述符的 limit 字段, 注意 limit 在高位字节部分是
// (BBRL-RRBB) 假如 limit = %edx = 000F_CDAB
#define _set_limit(addr, limit)                                                                    \
    __asm__("movw %%dx,%0\n\t"    /* limit 一开始被加载到 edx, 这里把 *addr=CDAB */                \
            "rorl $16,%%edx\n\t"  /* ror $16, %edx --> CDAB_000F, 需要 F_CDAB */                   \
            "movb %1,%%dh\n\t"    /* dx=RL0F */                                                    \
            "andb $0xf0,%%dh\n\t" /* dx=R00F */                                                    \
            "orb %%dh,%%dl\n\t"   /* dl=RF */                                                      \
            "movb %%dl,%1"        /* *(addr+6)=RF */                                               \
            :                                                                                      \
            : "m"(*(addr)), "m"(*((addr) + 6)), "d"(limit)                                         \
            : "dx")

// 设置局部描述符表中ldt描述符的基地址字段
#define set_base(ldt, base) _set_base(((char *)&(ldt)), base)

// 设置局部描述符表中ldt描述符的段长字段
#define set_limit(ldt, limit) _set_limit(((char *)&(ldt)), (limit - 1) >> 12)

// 从地址addr处描述符中取段基地址. 功能与 _set_base 正好相反
#define _get_base(addr)                                                                            \
    ({                                                                                             \
        unsigned long __base;                                                                      \
        __asm__("movb %3,%%dh\n\t"                                                                 \
                "movb %2,%%dl\n\t"                                                                 \
                "shll $16,%%edx\n\t"                                                               \
                "movw %1,%%dx"                                                                     \
                : "=d"(__base)                                                                     \
                : "m"(*((addr) + 2)), "m"(*((addr) + 4)), "m"(*((addr) + 7)));                     \
        __base;                                                                                    \
    })

// 取局部描述符表中 ldt 所指段描述符中的基地址
#define get_base(ldt) _get_base(((char *)&(ldt)))

/**
 * lsll 加载段限长指令, 用于把 segment=%1 段描述符中的段界限字段装入 __limit=%0
 * 此后 __limit += 1, 就是完整的段限长 */
#define get_limit(segment)                                                                         \
    ({                                                                                             \
        unsigned long __limit;                                                                     \
        __asm__("lsll %1,%0\n\tincl %0" : "=r"(__limit) : "r"(segment));                           \
        __limit;                                                                                   \
    })

#endif
