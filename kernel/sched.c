/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <asm/io.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/fdreg.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sys.h>

#include <signal.h>

/* 信号在信号位图中的索引 */
#define _S(nr) (1 << ((nr) - 1))

/* SIGKILL 和 SIGSTOP 不可被 block */
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr, struct task_struct *p)
{
    int i, j = 4096 - sizeof(struct task_struct);

    printk("%d: pid=%d, state=%d, father=%d, child=%d, ", nr, p->pid, p->state, p->p_pptr->pid,
           p->p_cptr ? p->p_cptr->pid : -1);

    /* 检测指定任务数据结构以后等于 0 的字节数 */
    i = 0;
    while (i < j && !((char *)(p + 1))[i]) {
        i++;
    }

    printk("%d/%d chars free in kstack\n\r", i, j);
    // TODO: 1019 是 tss.eip 的偏移量???
    //       看着似乎不是, tss.eip 偏移量是 0x3d0, 1019x4 算下来是 0xfec,
    //       差的还挺多的
    printk("   PC=%08X.", *(1019 + (unsigned long *)p));
    if (p->p_ysptr || p->p_osptr) {
        printk("   Younger sib=%d, older sib=%d\n\r", p->p_ysptr ? p->p_ysptr->pid : -1,
               p->p_osptr ? p->p_osptr->pid : -1);
    } else {
        printk("\n\r");
    }
}

// 显示所有任务的任务号、进程号、进程状态和内核堆栈空闲字节数(大约)
// NR_TASKS是系统能容纳的最大进程(任务)数量(64个, 定义在 include/kernel/sched.h)
void show_state(void)
{
    int i;

    printk("\rTask-info:\n\r");
    for (i = 0; i < NR_TASKS; i++) {
        if (task[i]) {
            show_task(i, task[i]);
        }
    }
}

// PC 机 8253 定时芯片的输入时钟频率约为 1.193180MHz, Linux
// 内核希望定时器发出中断的频率是 100Hz, 也即每 10ms 发出一次时钟中断. 因此这里
// LATCH 是设置 8253 芯片的初值
#define LATCH (1193180 / HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

// 每个任务(进程)在内核态运行时都有自己的内核态堆栈.
// 这里定义了任务的内核态堆栈结构
// 这里定义任务联合(任务结构成员和stack字符数组成员),
// 因为一个任务的数据结构与其内核态堆栈放在同一内存页中,
// 所以从堆栈段寄存器ss可以获得其数据段选择符
union task_union {
    struct task_struct task;
    char stack[PAGE_SIZE];
};

static union task_union init_task = {
    INIT_TASK,
};

// 从开机开始算起的滴答数时间值全局变量(10ms/滴答),
// 系统时钟中断每发生一次即一个滴答 前面的限定符 volatile,
// 这个限定词的含义是向编译器指明变量的内容可能会由于被其他程 序修改而变化,
// 通常在程序中申明一个变量时, 编译器会尽量把它存放在通用寄存器中, 例如 %ebx,
// 以提高访问效率. 当 CPU 把其值放到 ebx 中后一般就不会再关心该变量对应内存位
// 置中的内容, 若此时其他程序(例如内核程序或一个中断过程)修改了内存中该变量的值,
// ebx 中的值并不会随之更新. 为了解决这种情况就创建了 volatile 限定符,
// 让代码在引用该变 量时一定要从指定内存位置中取得其值. 这里即是要求 gcc 不要对
// jiffies 进行优化处理, 也不要挪动位置, 并且需要从内存中取其值.
// 因为时钟中断处理过程等程序会修改它的值

unsigned long volatile jiffies = 0; /* 开机后的滴答计数(每次滴答是 10ms) */
unsigned long startup_time = 0;     /* 开机时间(单位为秒) */
int jiffies_offset = 0;             /* clock ticks to add to get "true
                                           time".  Should always be less than
                                           1 second's worth.  For time fanatics
                                           who like to syncronize their machines
                                           to WWV :-) */

struct task_struct *current = &(init_task.task); /* current 初始化为 init_task*/
struct task_struct *last_task_used_math = NULL;  /* 上一个使用协处理器的任务 */

struct task_struct *task[NR_TASKS] = {
    &(init_task.task),
};

/* 栈大小定义为 1024 项, 每项 4B, 共计 4096KB */
long user_stack[PAGE_SIZE >> 2];

/* 栈段 */
struct {
    long *a; /* ESP = Stack Pointer */
    short b; /* SS = Segment Selector */
} stack_start = {&user_stack[PAGE_SIZE >> 2], 0x10};

/* 'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 *
 * 当任务被调度交换过以后,
 * 该函数用以保存原任务的协处理器状态(上下文)并恢复新调度进
 * 来的当前任务的协处理器执行状态 */
void math_state_restore()
{
    /* 如果上一个任务就是当前任务 */
    if (last_task_used_math == current) {
        return;
    }

    /* 在发送协处理器命令之前要先发 WAIT 指令
     * 如果上个任务使用了协处理器, 则保存其状态 */
    __asm__("fwait");
    if (last_task_used_math) {
        __asm__("fnsave %0" ::"m"(last_task_used_math->tss.i387));
    }

    /* 现在, last_task_used_math 指向当前任务, 以备当前任务被交换出去时使用
     * 此时如果当前任务用过协处理器, 则恢复其状态; 否则的话说明是第一次使用,
     * 于是就向协处理器发初始化命令, 并设置使用了协处理器标志 */
    last_task_used_math = current;
    if (current->used_math) {
        __asm__("frstor %0" : : "m"(current->tss.i387));
    } else {
        __asm__("fninit" ::);
        current->used_math = 1;
    }
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
    int i, next, c;
    struct task_struct **p;

    /* check alarm, wake up any interruptible tasks that have got a signal
     * 从最后一个 task 开始检查 task 的信号情况 */
    for (p = &LAST_TASK; p > &FIRST_TASK; --p)
        if (*p) {
            /* 如果设置过任务超时定时 timeout, 并且已经超时, 则复位超时定时值
             * 并且如果任务处于可中断睡眠状态下, 将其置为就绪状态(TASK_RUNNING)
             *
             * NOTICE: timeout 一般实在 tty_io 里面设置的 */
            if ((*p)->timeout && (*p)->timeout < jiffies) {
                (*p)->timeout = 0;
                if ((*p)->state == TASK_INTERRUPTIBLE) {
                    (*p)->state = TASK_RUNNING;
                }
            }

            /* 闹钟到时间, 发送闹钟信号
             * 如果设置过任务的定时值 alarm, 并且已经过期(alarm<jiffies),
             * 则在信号位图中置 SIGALRM 信号(即发送 SIGALARM 信号),
             * 该信号的默认操作是终止进程. */
            if ((*p)->alarm && (*p)->alarm < jiffies) {
                (*p)->signal |= (1 << (SIGALRM - 1));
                (*p)->alarm = 0;
            }

            /* 检查是否收到了待处理的信号
             * 如果信号位图中除被阻塞的信号外还有其他信号,
             * 并且任务处于可中断状态, 则置任务为就绪状态 其中 `~(_BLOCKABLE &
             * (*p)->blocked)` 用于忽略被阻塞的信号, 但 SIGKILL 和 SIGSTOP
             * 不能被阻塞 */
            if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
                (*p)->state == TASK_INTERRUPTIBLE) {
                (*p)->state = TASK_RUNNING;
            }
        }

    /* this is the scheduler proper:
     * 这里是调度程序的主要部分 */
    while (1) {
        c = -1;
        next = 0;
        i = NR_TASKS;
        p = &task[NR_TASKS];

        /* 这段代码也是从任务数组的最后一个任务开始循环处理,
         * 并跳过不含任务的数组槽
         * 比较每个就绪状态任务的运行时间的递减滴答计数(counter)值, 哪一个值大,
         * 说明这个任务运行时间还不长, next 就指向哪个的任务号 */
        while (--i) {
            if (!*--p) {
                continue;
            }

            if ((*p)->state == TASK_RUNNING && (*p)->counter > c) {
                c = (*p)->counter, next = i;
            }
        }

        /* 如果比较得出有 counter 值不等于 0 的结果,
         * 或者系统中没有一个可运行的任务存在(此时 c 仍然为 -1, next=0),
         * 则退出循环执行任务切换操作. 否则就根据每个任务的优先权值, 更新每
         * 一个任务的 counter 值, 然后重新比较. counter 值的计算方式为
         *
         * `counter = counter/2 + priority`
         *
         * 注意, 这里计算过程不考虑进程的状态 */
        if (c) {
            break;
        }

        /* 重新计算大家的 counter, 给下一次调度使用
         * 注意看这里并没有处理 task-0 的内容 */
        for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
            if (*p) {
                (*p)->counter = ((*p)->counter >> 1) + (*p)->priority;
            }
        }
    }

    switch_to(next);
}

/* pause 系统调用, 用于转换当前任务的状态为可中断的等待状态, 并重新调度
 *
 * 该系统调用将导致进程进入睡眠状态, 直到收到一个信号,
 * 该信号用于终止进程或者使进程调用一个信号捕获函数, 只有当捕获了一个信号,
 * 并且信号捕获处理函数返回, pause 才会返回. 此时 pause 返回值应该是 -1, 并且
 * errno 被置为 EINTR. 这里还没有完全实现(直到0.95版) */
int sys_pause(void)
{
    current->state = TASK_INTERRUPTIBLE;
    schedule();
    return 0;
}

/*
// 把当前任务置为指定的睡眠状态(可中断的或不可中断的),
// 并让睡眠队列头指针指向当前任务。
// 函数参数p是等待任务队列头指针。指针是含有一个变量地址的变量。这里参数p使用了指针的
// 指针形式
// '**p'，这是因为C函数参数只能传值，没有直接的方式让被调用函数改变调用该函数
// 程序中变量的值。但是指针'*p'指向的目标（这里是任务结构）会改变，因此为了能修改调用该
// 函数程序中原来就是指针变量的值，就需要传递指针'*p'的指针，即'**p'。参见程序前示例图中
// p指针的使用情况。
// 参数state是任务睡眠使用的状态：TASK_UNINTERRUPTIBLE或TASK_INTERRUPTIBLE
// 处于不可中断睡眠状态 (TASK_UNINTERRUPTIBLE) 的任务需要内核程序利用 wake_up
// 函数明确唤醒之 处于可中断睡眠状态 (TASK_INTERRUPTIBLE)
// 可以通过信号、任务超时等手段唤醒(置为就绪状态 TASK_RUNNING)
// ***
// 注意，由于本内核代码不是很成熟，因此下列与睡眠相关的代码存在一些问题，不宜深究。
// TODO: 重新读一读这个函数 */
static inline void __sleep_on(struct task_struct **p, int state)
{
    struct task_struct *tmp;

    if (!p) {
        return;
    }

    /* task-0 不允许进入睡眠 */
    if (current == &(init_task.task)) {
        panic("task[0] trying to sleep");
    }

    /* 让 tmp 指向已经在等待队列上的任务(如果有的话), 例如 inode->i_wait.
     * 并且将睡眠队列头的等待指针指向当前任务, 这样就把当前任务插入到了 *p
     * 的等待队列中 然后将当前任务置为指定的等待状态, 并执行重新调度
     *
     * TODO-DONE: 有没有可能两个进程用了同一个 **p, 这样之前进程永远在 sleep 状态出不来?
     * 答: 是有可能的, 但是这里用 tmp 把 *p 保存了下来, 然后把 current 保存到 *p, 此时
     *     原来的进程和 current 进程都处于阻塞状态. 等以后 wake_up 的时候, wake_up 的
     *     是 current, 程序继续向本函数后面执行, 也有对 tmp 唤醒(恢复状态 0)的操作, 因此
     *     原来的那个线程也会被唤醒
     *     更多参考: https://blog.csdn.net/jmh1996/article/details/90139485
     */
    tmp = *p;
    *p = current;
    current->state = state;

    /* 这块实际上是把 current 给 sleep 了, 因此需要记录一下 current 当前是谁
     * 因此 p 是指向任务的二级指针, 就是为了做这个记录的 */

repeat:
    schedule();

    /* TODO: 唤醒逻辑是什么样的? */

    // 只有当这个等待任务被唤醒时, 程序才又会返回到这里,
    // 表示进程已被明确地唤醒并执行
    //
    // 如果等待队列中还有等待任务, 并且队列头指针 *p 所指向的任务不是当前任务时,
    // 说明在本任务插入等待队列后还有任务进入等待队列.
    // 于是我们应该也要唤醒这个任务, 而我们自己应按顺序让这些后面进入队列的任务唤醒,
    // 因此这里将等待队列头所指任务先置为就绪状态,
    // 而自己则置为不可中断等待状态, 即自己要等待这些后续进队列的任务被唤醒
    // 而执行时来唤醒本任务. 然后重新执行调度程序
    if (*p && *p != current) {
        (**p).state = 0;
        current->state = TASK_UNINTERRUPTIBLE;
        goto repeat;
    }

    // 执行到这里，说明本任务真正被唤醒执行。此时等待队列头指针应该指向本任务，若它为
    // 空，则表明调度有问题，于是显示警告信息。最后我们让头指针指向在我们前面进入队列
    // 的任务（*p = tmp）。
    // 若确实存在这样一个任务，即队列中还有任务（tmp不为空），就
    // 唤醒之。最先进入队列的任务在唤醒后运行时最终会把等待队列头指针置成NULL。
    if (!*p) {
        printk("Warning: *P = NULL\n\r");
    }

    if ((*p = tmp)) {
        tmp->state = 0;
    }
}

/* 将当前任务置为可中断的等待状态(TASK_INTERRUPTIBLE)
 * 并放入头指针 *p 指定的等待队列中 */
void interruptible_sleep_on(struct task_struct **p)
{
    __sleep_on(p, TASK_INTERRUPTIBLE);
}

/* 把当前任务置为不可中断的等待状态(TASK_UNINTERRUPTIBLE)
 * 并让睡眠队列头指针指向当前任务. 只有明确地唤醒时才会返回, 该函数提供了进程
 * 与中断处理程序之间的同步机制 */
void sleep_on(struct task_struct **p)
{
    __sleep_on(p, TASK_UNINTERRUPTIBLE);
}

/* 唤醒 *p 指向的任务
 * TODO: 看这意思是僵尸进程和已经停止的进程, 还能被重新唤醒??? */
void wake_up(struct task_struct **p)
{
    if (p && *p) {
        if ((**p).state == TASK_STOPPED) { /* 处于停止状态 */
            printk("wake_up: TASK_STOPPED");
        }

        if ((**p).state == TASK_ZOMBIE) { /* 处于僵死状态 */
            printk("wake_up: TASK_ZOMBIE");
        }

        /* 置为就绪状态 0, 也即 TASK_RUNNING */
        (**p).state = TASK_RUNNING;
    }
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 *
 * 数组 wait_motor 用于存放等待软驱马达启动到正常转速的进程指针, 数组索引 0-3
 * 分别对应软驱 A-D 数组 mon_timer 存放各软驱马达启动所需要的滴答数.
 * 程序中默认启动时间为 50 个滴答(0.5秒) 数组 moff_timer
 * 存放各软驱在马达停转之前需维持的时间, 程序中设定为 10000 个滴答(100秒)
 *
 * 下面220 --
 * 281行代码用于处理软驱定时。在阅读这段代码之前请先看一下块设备一章中
 * 有关软盘驱动程序（floppy.c）后面的说明，
 * 或者到阅读软盘块设备驱动程序时在来看这 段代码。其中时间单位：1个滴答 =
 * 1/100秒 */
static struct task_struct *wait_motor[4] = {NULL, NULL, NULL, NULL};
static int mon_timer[4] = {0, 0, 0, 0};
static int moff_timer[4] = {0, 0, 0, 0};

/* 下面变量对应软驱控制器中当前数字输出寄存器, 该寄存器每位的定义如下:
 * 位 7~4: 分别控制驱动器 D~A 马达的启动, 1-启动; 0-关闭
 * 位 3  : 1-允许DMA和中断请求; 0-禁止DMA和中断请求
 * 位 2  : 1-启动软盘控制器; 0-复位软盘控制器
 * 位 1~0: 00~11, 用于选择控制的软驱 A-D
 * 这里设置初值为: 允许DMA和中断请求, 启动 FDC */
unsigned char current_DOR = 0x0C; /* 0000_1100 */

/**
 * @brief 查询指定软驱启动到正常运转状态所需等待时间
 *
 * selected 是选中软驱标志(blk_drv/floppy.c)
 * mask 是所选软驱对应 DOR 中启动马达比特位, 高 4 位是各软驱启动马达标志
 *
 * @param nr 软驱号(0~3)
 * @return int 等待的滴答数
 */
int ticks_to_floppy_on(unsigned int nr)
{
    extern unsigned char selected;
    unsigned char mask = 0x10 << nr;

    // 系统最多有4个软驱
    if (nr > 3) {
        panic("floppy_on: nr>3");
    }

    // 首先预先设置好指定软驱 nr 停转之前需要经过的时间(100秒), 这个关闭软驱的时候会用到(在 do_floppy_timer 里面)
    // 然后取当前 DOR 寄存器值到临时变量 mask 中, 并把指定软驱的马达启动标志置位
    moff_timer[nr] = 10000; /* 100 s = very big :-) */
    cli();                  /* use floppy_off to turn it off */
    mask |= current_DOR;

    // 如果当前没有选择软驱，则首先复位其他软驱的选择位，然后置指定软驱选择位
    if (!selected) {
        mask &= 0xFC; /* 1111_1100 */
        mask |= nr;
    }

    if (mask != current_DOR) {
        /* 如果数字输出寄存器的当前值与要求的值不同, 则向 FDC 的 DOR 输出新值(mask) */
        outb(mask, FD_DOR);

        /* 异或操作 `^` 结果是相同位置 0, 不同置 1
         * 下面这个条件就是在看 mask 和 current_DOR 高 4 位有没有不同值
         * 上面的逻辑给 mask 的高 4 bits 可能有赋 1 操作(表示启用对应的软驱),
         * 这里如果 if 条件为真 说明 nr 对应的那个软驱, 是从关闭变为启动,
         * 这时候就给 mon_timer 数组对应的定时器设置 0.5 秒延时 否则说明 nr
         * 对应的那个软驱已经是启动状态, 简单的设置为 2 个滴答, 以满足
         * do_floppy_timer 中先递 减后判断的要求 */
        if ((mask ^ current_DOR) & 0xf0) {
            mon_timer[nr] = HZ / 2;
        } else if (mon_timer[nr] < 2) {
            mon_timer[nr] = 2;
        }

        current_DOR = mask;
    }

    sti();

    /* 返回启动马达所需的时间值 */
    return mon_timer[nr];
}

/* 等待指定软驱马达启动所需的一段时间, 然后返回
 * 设置指定软驱的马达启动到正常转速所需的延时, 然后睡眠等待.
 * 在定时中断过程中会一直 递减判断这里设定的延时值. 当延时到期,
 * 就会唤醒这里的等待进程
 *
 * 如果马达启动定时还没到,
 * 就一直把当前进程置为不可中断睡眠状态并放入等待马达运行的队列中. 然后开中断 */
void floppy_on(unsigned int nr)
{
    /* IF 在 eflags 里面, 任务切换的时候, eflags 其实是会更新的, 因此这里关中断
     * 下面 sleep_on 切换任务之后, 不影响其他任务继续使用中断 */
    cli();

    while (ticks_to_floppy_on(nr)) {
        sleep_on(nr + wait_motor);
    }

    sti();
}

/* 置关闭相应软驱马达停转定时器(3秒)
 * 若不使用该函数明确关闭指定的软驱马达, 则在马达开启 100 秒之后也会被关闭 */
void floppy_off(unsigned int nr)
{
    moff_timer[nr] = 3 * HZ;
}

/* 软盘定时器程序, 用于更新马达启动定时值和马达关闭停转计时值
 * 该子程序会在时钟定时中断过程中被调用, 因此系统每经过一个滴答(10ms)
 * 就会被调用一次, 随时更新马达开启或停转定时器的值. 如果某一个马达停转
 * 定时到, 则将数字输出寄存器马达启动位复位
 * TODO: 了解一下软盘的操作 */
void do_floppy_timer(void)
{
    int i;
    unsigned char mask = 0x10;

    for (i = 0; i < 4; i++, mask <<= 1) {
        if (!(mask & current_DOR)) {
            /* 如果不是 DOR 指定的马达则跳过 */
            continue;
        }

        if (mon_timer[i]) {
            /* 软盘启动的时候, 达到正常转速需要一定的事件, mon_timer 定时器就是记录这个时间的
             * 进程在设置了 mon_timer 之后会进入休眠, 等马达转速够了的时候再被唤醒 */
            if (!--mon_timer[i]) {
                wake_up(i + wait_motor);
            }
        } else if (!moff_timer[i]) {
            /* 当软盘关闭的时候, 马达停下来也需要一定时间, moff_timer 就是做这个事情的
             * 计时结束, 说明软盘已经完全关闭, 这时候就把对应软驱的马达关掉 */
            current_DOR &= ~mask;
            outb(current_DOR, FD_DOR);
        } else {
            /* 否则马达停转计时递减 */
            moff_timer[i]--;
        }
    }
}

/* 下面是关于定时器的代码, 最多可有 64 个定时器 */
#define TIME_REQUESTS 64

/* 定时器链表结构和定时器数组,
 * 该定时器链表专用于供软驱关闭马达和启动马达定时操作 这种类型定时器类似现代
 * Linux 系统中的动态定时器(Dynamic Timer), 仅供内核使用 */
static struct timer_list {
    long jiffies; /* 定时器滴答数量(如果非链表头, 则表示上一个节点滴答完,
                     这个节点还需要多少滴答到期) */
    void (*fn)(); /* 定时器处理函数 */
    struct timer_list *next; /* 下一个链表节点 */
} timer_list[TIME_REQUESTS], *next_timer = NULL;

// 添加定时器
void add_timer(long jiffies, void (*fn)(void))
{
    struct timer_list *p;

    /* 必须有处理函数 */
    if (!fn) {
        return;
    }

    cli(); /* 清中断 */

    if (jiffies <= 0) { /* 如果定时值 <= 0, 则立刻调用其处理程序, 并且该定时器不加入链表中 */
        (fn)();
    } else {
        /* 在定时器数组里面, 找一个空闲的位置 */
        for (p = timer_list; p < timer_list + TIME_REQUESTS; p++) {
            if (!p->fn) {
                break;
            }
        }

        if (p >= timer_list + TIME_REQUESTS) {
            panic("No more time requests free");
        }

        /* 设置定时器 */
        p->fn = fn;
        p->jiffies = jiffies;
        p->next = next_timer;
        next_timer = p;

        // 链表项按定时值从小到大排序
        // 在排序时减去排在前面需要的滴答数, 这样在处理定时器时只要查看链表头的,
        // 第一项的定时是否到期即可.
        // ??? 这段程序好象没有考虑周全,
        // 如果新插入的定时器值小于原来头一个定时器值时则根本不会进入循环中,
        // 但此时还是应该将紧随其后面的一个定时器值减去新的第 1 个的定时值.
        // 即如果第 1 个定时值 <= 第 2 个, 则第 2 个定时值扣除第 1 个的值即可,
        // 否则进入下面循环中进行处理
        while (p->next && p->next->jiffies < p->jiffies) {
            p->jiffies -= p->next->jiffies;
            fn = p->fn;
            p->fn = p->next->fn;
            p->next->fn = fn;
            jiffies = p->jiffies;
            p->jiffies = p->next->jiffies;
            p->next->jiffies = jiffies;
            p = p->next;
        }
    }

    sti(); /* 开中断 */
}

/* 时钟中断 C 函数处理程序, 在 sys_call.s 中 timer_interrupt 例程中被调用
 * 参数 cpl 是当前特权级 0 或 3, 是时钟中断发生时正被执行的代码选择符中的特权级
 * cpl=0 时表示中断发生时正在执行内核代码, cpl=3
 * 时表示中断发生时正在执行用户代码 一个进程由于执行时间片用完时,
 * 则进行任务切换, 并执行一个计时更新工作 */
void do_timer(long cpl)
{
    static int blanked = 0; // 是否处于黑屏状态

    /* 首先判断是否经过了一定时间而让屏幕黑屏(blankout)
     *
     * 如果 blankcount 计数不为零, 或者黑屏延时间隔时间 blankinterval 为 0 的话,
     * 那么若已经处于黑屏状态(blanked = 1), 则让屏幕恢复显示. 若 blankcount
     * 计数不为零, 则递减之, 并且复位黑屏标志
     * TODO: 以后有时间再看看这块是怎么弄的, 和核心流程没关系, 跳过 */
    if (blankcount || !blankinterval) {
        if (blanked) {
            unblank_screen();
        }

        if (blankcount) {
            blankcount--;
        }
        blanked = 0;
    } else if (!blanked) {
        blank_screen();
        blanked = 1;
    }

    /* 硬盘访问超时处理 */
    if (hd_timeout) {
        if (!--hd_timeout) {
            hd_times_out();
        }
    }

    /* 蜂鸣计数次到, 关闭发声 */
    if (beepcount) {
        if (!--beepcount) {
            sysbeepstop();
        }
    }

    /* 根据 cpl, 追加时间片统计 */
    if (cpl) {
        current->utime++;
    } else {
        current->stime++;
    }

    /* 如果有定时器存在, 则遍历定时器列表 */
    if (next_timer) {
        next_timer->jiffies--; /* 减掉相应的时间片 */
        /* 时间到 */
        while (next_timer && next_timer->jiffies <= 0) {
            void (*fn)(void);

            fn = next_timer->fn;
            next_timer->fn = NULL;
            next_timer = next_timer->next;
            (fn)();
        }
    }

    /* 如果当前软盘控制器 FDC 的数字输出寄存器中马达启动位有置位的,
     * 则执行软盘定时程序 */
    if (current_DOR & 0xf0) {
        do_floppy_timer();
    }

    // 如果进程运行时间还没完, 则退出. 否则置当前任务运行计数值为 0,
    // 并且若发生时钟中断时 正在内核代码中运行则返回, 否则调用执行调度函数
    if ((--current->counter) > 0) {
        return;
    }

    current->counter = 0;
    if (!cpl) {
        return; /* 内核态程序不予重调度 */
    }

    schedule();
}

/*
 * 系统调用功能 - 设置报警定时时间值(秒)
 * 若参数 seconds 大于 0, 则设置新定时值, 并返回原定时时刻还剩余的间隔时间,
 * 否则返回 0 进程数据结构中报警定时值 alarm 的单位是系统滴答(1 滴答为 10 毫秒),
 * 它是系统开机起到 设置定时操作时系统滴答值 jiffies
 * 和转换成滴答单位的定时值之和, 即 `jiffies + HZ*定时 秒值`,
 * 而参数给出的是以秒为单位的定时值, 因此本函数的主要操作是进行两种单位的转换
 *
 * 其中常数 HZ = 100, 是内核系统运行频率. 定义在 include/sched.h
 * 参数 seconds 是新的定时时间值, 单位是秒 */
int sys_alarm(long seconds)
{
    int old = current->alarm;

    if (old) {
        old = (old - jiffies) / HZ;
    }

    current->alarm = (seconds > 0) ? (jiffies + HZ * seconds) : 0;
    return (old);
}

/* 取进程号 pid */
int sys_getpid(void)
{
    return current->pid;
}

/* 取父进程号 pid */
int sys_getppid(void)
{
    return current->p_pptr->pid;
}

/* 取用户号 uid */
int sys_getuid(void)
{
    return current->uid;
}

/* 取有效的用户号 euid */
int sys_geteuid(void)
{
    return current->euid;
}

/* 取组号 gid */
int sys_getgid(void)
{
    return current->gid;
}

/* 取有效的组号 egid */
int sys_getegid(void)
{
    return current->egid;
}

/* 系统调用功能 -- 降低对 CPU 的使用优先权
 * 应该限制 increment 为大于 0 的值, 否则可使优先权增大!!! */
int sys_nice(long increment)
{
    if (current->priority - increment > 0) {
        current->priority -= increment;
    }

    return 0;
}

/**
 * 调度程序的初始化函数 */
void sched_init(void)
{
    int i;
    struct desc_struct *p;

    if (sizeof(struct sigaction) != 16) {
        panic("Struct sigaction MUST be 16 bytes");
    }

    /* 在 GDT 里面设置第 0 个任务的 TSS 和 LDT 描述符 */
    set_tss_desc(gdt + FIRST_TSS_ENTRY, &(init_task.task.tss));
    set_ldt_desc(gdt + FIRST_LDT_ENTRY, &(init_task.task.ldt));

    /* 把 Task 1 到 NR_TASKS 的 TSS/LDT 描述符清空 */
    p = gdt + 2 + FIRST_TSS_ENTRY;
    for (i = 1; i < NR_TASKS; i++) {
        task[i] = NULL;
        p->a = p->b = 0;
        p++;
        p->a = p->b = 0;
        p++;
    }

    /* Clear NT, so that we won't have troubles with that later on
     * 清理 NT 位 */
    __asm__("pushfl\n\t"
            "andl $0xffffbfff, (%esp)\n\t"
            "popfl");

    ltr(0);  /* 装载第 0 个任务的 TSS */
    lldt(0); /* 装载第 0 个任务的 LDT */

    /* 下面代码用于初始化 8253 定时器. 通道 0, 选择工作方式 3, 二进制计数方式
     * 通道 0 的输出引脚接在中断控制主芯片的 IRQ0 上, 它每 10 毫秒发出一个 IRQ0
     * 请求 LATCH 是初始定时计数值 */
    outb_p(0x36, 0x43);         /* binary, mode 3, LSB/MSB, ch 0 */
    outb_p(LATCH & 0xff, 0x40); /* LSB */
    outb(LATCH >> 8, 0x40);     /* MSB */

    /* 设置时钟中断处理程序句柄(设置时钟中断门)
     * IRQ0 ~ 0x20 号中断 */
    set_intr_gate(0x20, &timer_interrupt);

    /* 修改 8259A 主片中断控制器屏蔽码, 允许时钟中断 */
    outb(inb_p(0x21) & ~0x01, 0x21);

    /* 安装系统调用中断门 */
    set_system_gate(0x80, &system_call);
}