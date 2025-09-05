/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>
#include <string.h>

#include <asm/segment.h>
#include <asm/system.h>
#include <linux/kernel.h>
#include <linux/sched.h>

extern void write_verify(unsigned long address);

// 最新进程号全局变量
// 其值会由 get_empty_process 递增, 在 copy_process 里面使用
long last_pid = 0;

// 对于 80386 CPU, 在执行特权级 0 代码时不会理会用户空间中的页面是否是页保护的,
// 因此 在执行内核代码时用户空间中数据页面保护标志起不了作用,
// 写时复制机制也就失去了作用. verify_area 函数就用于此目的. 但对于 80486
// 或后来的 CPU, 其控制寄存器 CR0 中有一个 写保护标志 WP(位16),
// 内核可以通过设置该标志来禁止特权级 0 的代码向用户空间只读 页面执行写数据,
// 否则将导致发生写保护异常. 从而 486 以上 CPU 可以通过设置该标志来达
// 到使用本函数同样的目的

/* 检查从进程线性地址 addr 开始的 size 范围空间, 是不是可写
 * 如果原来不可写, 在 write_verify 里面会复制新页并允许新页写
 * 这里涉及到两个地址概念
 *     第一个是进程内部相对于 code_base 或者 data_base 的进程逻辑地址
 *     第二个是 CPU 范围内的 4GB 线性空间
 *
 * 当给出进程逻辑地址的时候, 需要变换才能得到 4GB 线性空间的地址 */
void verify_area(void *addr, int size)
{
    unsigned long start;

    start = (unsigned long)addr;

    /* 因为操作是以页面为单位进行的, 在最开始处理的时候是在 addr 所属
     * 的页面开始位置. 因此 size 必须要考虑到 addr 被舍弃的这部分页面
     * 内偏移量, 这部分就是 += (start & 0xFFF) */
    size += start & 0xfff;
    start &= 0xfffff000; /* start 对齐页面边界 */

    /* addr 对应的 4GB 空间的线性地址对应的页面起始地址 */
    start += get_base(current->ldt[2]);

    while (size > 0) {
        size -= 4096;
        write_verify(start);
        start += 4096;
    }
}

/**
 * @brief 复制 p 的内存页表系统
 *
 * p 是从 current fork 出来的, 因此这里本质上是复制 current 的页表系统
 *
 * @param nr 新任务号
 * @param p 复制出来的新任务结构
 * @return int 返回不为 0 表示出错
 */
int copy_mem(int nr, struct task_struct *p)
{
    unsigned long old_data_base, new_data_base, data_limit;
    unsigned long old_code_base, new_code_base, code_limit;

    /* 拿到 0000_1_1_11 和 0001_0_1_11 也就是 LDT 里面 1#, 2# 描述符的段限长
     * 这两个段分别是程序的代码段和数据段 */
    code_limit = get_limit(0x0f);
    data_limit = get_limit(0x17);
    old_code_base = get_base(current->ldt[1]);
    old_data_base = get_base(current->ldt[2]);
    if (old_data_base != old_code_base) {
        /* Linux 0.12 要求代码段和数据段, 从相同位置开始 */
        panic("We don't support separate I&D");
    }

    if (data_limit < code_limit) {
        /* 数据段长度必须要大于代码段 */
        panic("Bad data_limit");
    }

    /* 新的段位置对齐在 nr 任务的 64MB 边界上 */
    new_data_base = new_code_base = nr * TASK_SIZE;
    p->start_code = new_code_base; /* start_code 是任务在 4GB 线性空间的线性地址 */
    set_base(p->ldt[1], new_code_base);
    set_base(p->ldt[2], new_data_base);

    /* 只拷贝目录系统, 不拷贝具体的页面数据 */
    if (copy_page_tables(old_data_base, new_data_base, data_limit)) {
        free_page_tables(new_data_base, data_limit);
        return -ENOMEM;
    }

    return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 *
 * 为了进到这个函数里面来, 整个路径是这样的:
 *    int0x80 -> 中断处理函数 ----> call(sys_fork) -----> call(copy_process)
 *               (sys_call)
 * ---------------------------------------------------------------------
 * Stack:
 *    SS, ESP, > DS, ES, FS  > EIP1 >  GS, ESI, EDI > EIP2
 *    EFLAGS,    EAX(orig_eax)         EBP, EAX(nr)
 *    CS, EIP    EDX, ECX, EBX */
int copy_process(int nr, long ebp, long edi, long esi,
                 long gs,   /* 这几个参数是在 sys_call.s 设置的 */
                 long none, /* 这个 none 是 `call sys_fork 的时候带进来的 ip 指针 */
                 long ebx, long ecx, long edx, long orig_eax, long fs, long es, long ds, long eip,
                 long cs, long eflags, long esp, long ss)
{
    struct task_struct *p;
    int i;
    struct file *f;

    /* 首先为新任务数据结构分配内存 */
    p = (struct task_struct *)get_free_page();
    if (!p) {
        return -EAGAIN;
    }

    /* 将新任务结构指针放入任务数组 */
    task[nr] = p;

    /* 接着把当前进程任务结构内容复制到刚申请到的内存页面p开始处 */
    // TODO: 这个拷贝语句不好使, 换成 memcpy
    // *p = *current; /* NOTE! this doesn't copy the supervisor stack */
    memcpy((void *)p, (void *)current, sizeof(struct task_struct));

    /* 修改新任务的参数 */
    p->state = TASK_UNINTERRUPTIBLE; /* 新进程状态置为不可中断等待状态, 以防止内核调度其执行 */
    p->pid = last_pid;
    p->counter = p->priority;  /* 运行时间片(嘀嗒数), TODO: p->priority 最开始在哪里设置的? */
    p->signal = 0;             /* 信号位图 */
    p->alarm = 0;              /* 报警定时值(嘀嗒数) */
    p->leader = 0;             /* process leadership doesn't inherit, 进程的领导权是不能继承的 */
    p->utime = p->stime = 0;   /* 用户态时间和核心态运行时间 */
    p->cutime = p->cstime = 0; /* 子进程用户态和核心态运行时间 */
    p->start_time = jiffies;   /* 进程开始运行时间(当前时间滴答数) */

    /* 下面设置 TSS 结构体 */
    p->tss.back_link = 0;
    p->tss.esp0 = PAGE_SIZE + (long)p; /* 任务内核态栈指针 */
    p->tss.ss0 = 0x10;                 /* 内核态栈的段选择符 */
    /* 因为 linux 没有设计 1, 2 特权级别, 因此这里没有设置这两个特权级别的栈 */
    p->tss.eip = eip;       // 指令代码指针
    p->tss.eflags = eflags; // 标志寄存器
    p->tss.eax = 0;         // 这是当 fork 返回时新进程会返回 0 的原因所在
    p->tss.ecx = ecx;
    p->tss.edx = edx;
    p->tss.ebx = ebx;
    p->tss.esp = esp;
    p->tss.ebp = ebp;
    p->tss.esi = esi;
    p->tss.edi = edi;
    p->tss.es = es & 0xffff;
    p->tss.cs = cs & 0xffff;
    p->tss.ss = ss & 0xffff;
    p->tss.ds = ds & 0xffff;
    p->tss.fs = fs & 0xffff;
    p->tss.gs = gs & 0xffff;
    p->tss.ldt = _LDT(nr);            /* LDT 选择子 */
    p->tss.trace_bitmap = 0x80000000; /* TODO: 是否在 trace 置 1 ??? */

    /* 如果当前任务使用了协处理器, 就保存其上下文
     * 汇编指令 clts 用于清除控制寄存器 CR0 中的任务已交换(TS)标志.
     * 每当发生任务切换, CPU 都会设置该标志. 该标志用于管理数学协处理器:
     * 如果该标志置位, 那么每个 ESC 指 令都会被捕获(异常7). 如果协处理器存在标志
     * MP 也同时置位的话, 那么 WAIT 指令也会 捕获. 因此, 如果任务切换发生在一个
     * ESC 指令开始执行之后, 则协处理器中的内容就可能 需要在执行新的 ESC
     * 指令之前保存起来. 捕获处理句柄会保存协处理器的内容并复位 TS 标 志, 指令
     * fnsave 用于把协处理器的所有状态保存到目的操作数指定的内存区域中(tss.i387)
     */
    if (last_task_used_math == current)
        __asm__("clts\n\t"
                "fnsave %0\n\t"
                "frstor %0\n\t"
                :
                : "m"(p->tss.i387));

    /* 接下来复制进程页表, 即在线性地址空间中设置新任务代码段和数据段
     * 描述符中的基址和限长, 并复制页表 */
    if (copy_mem(nr, p)) { /* 返回不为 0 表示出错 */
        task[nr] = NULL;
        free_page((long)p);
        return -EAGAIN;
    }

    /* 如果父进程中有文件是打开的, 则将对应文件的打开次数增 1
     * 因为这里创建的子进程会与父进程共享这些打开的文件
     * TODO: 学完文件系统, 再回来看看这里的计数操作 */
    for (i = 0; i < NR_OPEN; i++) {
        if ((f = p->filp[i])) {
            f->f_count++;
        }
    }

    if (current->pwd) {
        current->pwd->i_count++;
    }

    if (current->root) {
        current->root->i_count++;
    }

    if (current->executable) {
        current->executable->i_count++;
    }

    if (current->library) {
        current->library->i_count++;
    }

    /* 在 GDT 里面, 更新 TSS 和 LDT 描述符的内容为当先 Task 的内容 */
    set_tss_desc(gdt + (nr << 1) + FIRST_TSS_ENTRY, &(p->tss));
    set_ldt_desc(gdt + (nr << 1) + FIRST_LDT_ENTRY, &(p->ldt));

    /* 维护进程关系链表
     * 设置 父, 子, 相邻进程的指针 */
    p->p_pptr = current;          /* 父进程 */
    p->p_cptr = 0;                /* 子进程 */
    p->p_ysptr = 0;               /* 比自己年轻的兄弟进程 */
    p->p_osptr = current->p_cptr; /* 比自己老的兄弟进程(是父进程的子进程) */
    if (p->p_osptr) {             /* 自己的老兄弟的最新兄弟是自己 */
        p->p_osptr->p_ysptr = p;
    }
    current->p_cptr = p; /* 父进程的子进程 */

    /* 最后改一下状态, 允许被调度 */
    p->state = TASK_RUNNING; /* do this last, just in case */
    return last_pid;
}

/* 为新进程取得不重复的进程号 last_pid, 函数返回在任务数组中的任务号(数组项) */
int find_empty_process(void)
{
    int i;

repeat:
    /* 超出最大整数表示范围, 从 0 重新开始 */
    if ((++last_pid) < 0) {
        last_pid = 1;
    }

    /* 如果 last_pid 已经被使用, 尝试下一个 last_pid 数值 */
    for (i = 0; i < NR_TASKS; i++) {
        if (task[i] && ((task[i]->pid == last_pid) || (task[i]->pgrp == last_pid))) {
            goto repeat;
        }
    }

    /* 检索任务数组, 找到空闲的数组项, 就返回这个项的索引
     * 注意这里任务 0 项被排除在外 */
    for (i = 1; i < NR_TASKS; i++) {
        if (!task[i]) {
            return i;
        }
    }

    return -EAGAIN;
}