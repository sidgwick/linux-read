/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

/*
 * Real VM (paging to/from disk) started 18.12.91. Much more work and
 * thought has to go into this. Oh, well..
 * 19.12.91  -  works, somewhat. Sometimes I get faults, don't know why.
 *        Found it. Everything seems to work now.
 * 20.12.91  -  Ok, making the swap-device changeable like the root.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/sched.h>

/**
 * 该宏用于判断给定线性地址是否位于当前进程的代码段中
 * `(((addr)+4095)&~4095)` 用于取得线性地址 addr 所在内存页面的末端地址
 * 这实际上就是 (addr + 0xFFF) & 0xFFFFF000 */
#define CODE_SPACE(addr) ((((addr) + 4095) & ~4095) < current->start_code + current->end_code)

unsigned long HIGH_MEMORY = 0; // 内存高端地址

/* 拷贝一页内容 */
#define copy_page(from, to) __asm__("cld ; rep ; movsl" ::"S"(from), "D"(to), "c"(1024))

// 物理内存映射字节图(1字节代表1页内存)
// 每个页面对应的字节用于标志页面当前被引用(占用)次数, 使用 PAGING_PAGES
// 最大可以映射 15Mb 的内存空间 在初始化函数 mem_init 中,
// 对于不能用作主内存区页面的位置均都预先被设置成 USED
unsigned char mem_map[PAGING_PAGES] = {
    0,
};

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 *
 * 从 addr 处释放所属的内从空间
 */
void free_page(unsigned long addr)
{
    // 物理地址 1MB 以下的内存空间用于内核程序和缓冲, 不作为分配页面的内存空间
    if (addr < LOW_MEM)
        return;

    // 如果物理地址 addr 大于等于系统所含物理内存最高端,
    // 则显示出错信息并且内核停止工作
    if (addr >= HIGH_MEMORY)
        panic("trying to free nonexistent page");

    // 计算 addr 所属的页面编号 == `addr = MAP_NR(addr)`
    addr -= LOW_MEM;
    addr >>= 12;

    // 如果页面还有引用, 就不做处理
    if (mem_map[addr]--)
        return;

    // 否则页面已经是 0 引用, 这种情况就是重复释放页面, 看错是出错了
    mem_map[addr] = 0;
    panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 *
 * 释放从线性地址 from 开始, 大小为 size 的这部分内存空间所属的页面
 * 这个函数是以页目录项为单位操作的, 一口气操作 4MB
 */
int free_page_tables(unsigned long from, unsigned long size)
{
    unsigned long *pg_table;
    unsigned long *dir, nr;

    /* 因为每个页表最多映射 4M 的内存, 所以 from 表示的线性基地址, 必然是 4M
     * 对齐的 */
    if (from & 0x3fffff)
        panic("free_page_tables called with wrong alignment");

    /* 0~4M 开始的这部分空间是内核专用, 也不允许释放 */
    if (!from)
        panic("Trying to free up swapper memory space");

    /* size 换算成有几个 4M 页面簇 */
    size = (size + 0x3fffff) >> 22;

    /* 因为 page directory 在内存地址 0 处, 这里直接算出 from 所属的页目录项(高
     * 10 位) 然后从此页目录项开始遍历页面簇 没有 `>> 20` 而不是 22
     * 是因为还要考虑每个 PDE 占据 4 字节 */
    dir = (unsigned long *)((from >> 20) & 0xffc); /* _pg_dir = 0 */
    for (; size-- > 0; dir++) {
        if (!(1 & *dir)) /* 页目录项不存在自然不需要处理 */
            continue;
        /* 解引用页目录项, 得到页表地址 */
        pg_table = (unsigned long *)(0xfffff000 & *dir);
        for (nr = 0; nr < 1024; nr++) {
            /* 对 pg_table 解引用, 得到的是物理页面的地址 */
            if (*pg_table) {
                if (1 & *pg_table)
                    /* 页面要是存在就回收, `&` 操作是在清理页表项属性 */
                    free_page(0xfffff000 & *pg_table);
                else
                    /* 被交换出去的页面, 也需要清理
                     * swap 出去的页面, PTE 记录的是 (swap_nr << 1) */
                    swap_free(*pg_table >> 1);
                *pg_table = 0;
            }
            pg_table++;
        }

        /* page table 所在的页面也要被释放 */
        free_page(0xfffff000 & *dir);
        *dir = 0; /* 清空 PDE */
    }

    /* 重新加载 CR0, 使得 TLB 失效 */
    invalidate();
    return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 *
 * 从 from 到 to 复制 size 对应线性空间对应的 PDE 和 PTE
 */
int copy_page_tables(unsigned long from, unsigned long to, long size)
{
    unsigned long *from_page_table;
    unsigned long *to_page_table;
    unsigned long this_page;
    unsigned long *from_dir, *to_dir;
    unsigned long new_page;
    unsigned long nr;

    /* 指定的线性地址都应该是 4MB 对齐的 */
    if ((from & 0x3fffff) || (to & 0x3fffff))
        panic("copy_page_tables called with wrong alignment");

    /* 找到 from, to 所属的 PDE */
    from_dir = (unsigned long *)((from >> 20) & 0xffc); /* _pg_dir = 0 */
    to_dir = (unsigned long *)((to >> 20) & 0xffc);
    size = ((unsigned)(size + 0x3fffff)) >> 22; /* size 跨度有几个 PDE */
    for (; size-- > 0; from_dir++, to_dir++) {
        /* 如果目标页面已经存在, 就报错退出 */
        if (1 & *to_dir)
            panic("copy_page_tables: already exist");

        /* 如果源页面不存在, 那也不处理 */
        if (!(1 & *from_dir))
            continue;

        /* 源页表, 目标页表新分配 */
        from_page_table = (unsigned long *)(0xfffff000 & *from_dir);
        if (!(to_page_table = (unsigned long *)get_free_page()))
            return -1; /* Out of memory, see freeing */

        /* 目标页目录项属性 (PDE) */
        *to_dir = ((unsigned long)to_page_table) | 7;

        /* from=0 有特殊含义, 表示复制内核页表内容, 这时候只复制 160 项
         * 其他情况下, 复制整个页面 1024 项 */
        nr = (from == 0) ? 0xA0 : 1024;
        for (; nr-- > 0; from_page_table++, to_page_table++) {
            this_page = *from_page_table; // PTE
            if (!this_page)
                continue;

            /* 这个表示页面存在, 但是被交换到了 SWAP */
            if (!(1 & this_page)) {
                if (!(new_page = get_free_page()))
                    return -1;
                read_swap_page(this_page >> 1, (char *)new_page);
                *to_page_table = this_page;
                *from_page_table =
                    new_page | (PAGE_DIRTY | 7); /* TODO: 这里设置 DIRTY 的含义, 可能是
                                       '标记为 dirty 之后无法共享这个页' ??? */
                continue;
            }

            /* 页面清理掉写标记, 主要是如果有页面是公用的, 就可以实现
             * copy-on-write 操作 */
            this_page &= ~2; /* 清理掉 RW 属性 */
            *to_page_table = this_page;
            /* 位于主内存区域的页面, 需要追加引用计数 */
            if (this_page > LOW_MEM) {
                *from_page_table = this_page; /* 源页表也只读 */
                this_page -= LOW_MEM;
                this_page >>= 12;
                mem_map[this_page]++;
            }
        }
    }

    /* 因为上面有 page 的属性调整, 这里需要重载页表, 使 TLB 失效 */
    invalidate();
    return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 *
 * 把物理内存页面映射到线性地址空间指定处
 *
 * 参数 page 是分配的主内存区中某一页面(页帧/页框)的指针, address
 * 是对应的线性地址 */
static unsigned long put_page(unsigned long page, unsigned long address)
{
    unsigned long tmp, *page_table;

    /* NOTE !!! This uses the fact that _pg_dir=0 */

    if (page < LOW_MEM || page >= HIGH_MEMORY)
        printk("Trying to put page %p at %p\n", page, address);

    if (mem_map[(page - LOW_MEM) >> 12] != 1)
        printk("mem_map disagrees with %p at %p\n", page, address);

    page_table = (unsigned long *)((address >> 20) & 0xffc); // PDT
    if ((*page_table) & 1)
        page_table = (unsigned long *)(0xfffff000 & *page_table); // PTE
    else {
        if (!(tmp = get_free_page()))
            return 0;
        *page_table = tmp | 7;             // PTE
        page_table = (unsigned long *)tmp; // page table 只想最新分配的页面
    }
    page_table[(address >> 12) & 0x3ff] = page | 7;
    /* no need for invalidate */
    return page;
}

/*
 * The previous function doesn't work very well if you also want to mark
 * the page dirty: exec.c wants this, as it has earlier changed the page,
 * and we want the dirty-status to be correct (for VM). Thus the same
 * routine, but this time we mark it dirty too.
 *
 * 同 put_page, 但是会把 page 标记为脏页
 */
unsigned long put_dirty_page(unsigned long page, unsigned long address)
{
    unsigned long tmp, *page_table;

    /* NOTE !!! This uses the fact that _pg_dir=0 */

    if (page < LOW_MEM || page >= HIGH_MEMORY)
        printk("Trying to put page %p at %p\n", page, address);
    if (mem_map[(page - LOW_MEM) >> 12] != 1)
        printk("mem_map disagrees with %p at %p\n", page, address);
    page_table = (unsigned long *)((address >> 20) & 0xffc);
    if ((*page_table) & 1)
        page_table = (unsigned long *)(0xfffff000 & *page_table);
    else {
        if (!(tmp = get_free_page()))
            return 0;
        *page_table = tmp | 7;
        page_table = (unsigned long *)tmp;
    }
    page_table[(address >> 12) & 0x3ff] = page | (PAGE_DIRTY | 7);
    /* no need for invalidate */
    return page;
}

/* 这个函数在页面触发写保护的时候执行 Copy-On_Write */
void un_wp_page(unsigned long *table_entry)
{
    unsigned long old_page, new_page;

    old_page = 0xfffff000 & *table_entry;

    /* 如果这个页面只有一个引用, 那就直接让这个页面可写, 然后失效 TLB */
    if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)] == 1) {
        *table_entry |= 2;
        invalidate();
        return;
    }

    /* 如果不是, 我们就看到了所谓的 copy on write 技术
     * 这里需要复制一份页面, 并把老页面计数减一, 然后更新 PTE 为可读写的新页面
     */
    if (!(new_page = get_free_page()))
        oom();

    if (old_page >= LOW_MEM)
        mem_map[MAP_NR(old_page)]--;

    copy_page(old_page, new_page);
    *table_entry = new_page | 7;
    invalidate();
}

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 *
 * 当用户试图往一共享页面上写时, 该函数处理已存在的内存页面(写时复制)
 * 它是通过将页面复制到一个新地址上并且递减原页面的共享计数值实现的
 * 如果它在代码空间，我们就显示段出错信息并退出。
 *
 * 写共享页面时需复制页面(写时复制)
 * 页异常中断处理过程中调用的 C 函数, 在 page.s 程序中被调用
 * 函数参数 error_code 和 address 是进程在写写保护页面时由
 * CPU产生异常而自动生成的
 *
 * error_code 指出出错类型, 参见本章开始处的"内存页面出错异常"一节
 * address 是产生异常的页面线性地址 */
void do_wp_page(unsigned long error_code, unsigned long address)
{
    // 首先判断 CPU 控制寄存器 CR2 给出的引起页面异常的线性地址在什么范围中
    // 如果 address 小于 TASK_SIZE=0x4000000(即64MB),
    // 表示异常页面位置在内核或任务 0 和任务 1 所处的线性地址范围内,
    // 于是发出警告信息“内核范围内存被写保护”
    if (address < TASK_SIZE)
        printk("\n\rBAD! KERNEL MEMORY WP-ERR!\n\r");

    // 如果 (address–当前进程代码起始地址) 大于一个进程的长度(64MB), 表示
    // address 所指的线性地址不在引起异常的进程线性地址空间范围内,
    // 则在发出出错信息后退出
    // TODO: 在了解一下进程内存分布
    if (address - current->start_code > TASK_SIZE) {
        printk("Bad things happen: page error in do_wp_page\n\r");
        do_exit(SIGSEGV);
    }

#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
/* 如果线性地址位于进程的代码空间中，则终止执行程序。因为代码是只读的 */
    if (CODE_SPACE(address))
        do_exit(SIGSEGV);
#endif

    /* 参数部分在计算 pte 地址, 仔细看能看懂 */
    un_wp_page((unsigned long *)(((address >> 10) & 0xffc) +
                                 (0xfffff000 & *((unsigned long *)((address >> 20) & 0xffc)))));
}

/*
 * 检查线性地址 address 对应的页面是否可写, 如果不可写, 则解除写保护 */
void write_verify(unsigned long address)
{
    unsigned long page;

    /* 先找到 PDE, 然后计算 PDE 指向的页表是否存在 */
    if (!((page = *((unsigned long *)((address >> 20) & 0xffc))) & 1))
        return;

    page &= 0xfffff000;                /* PDE 指向的页表地址 */
    page += ((address >> 10) & 0xffc); /* page 现在指向 PTE */

    /* PTE 如果最低两位是 01, if 条件就是真
     * 01 意味着不允许写但页面存在, 因此这时候做一次解除写保护 */
    if ((3 & *(unsigned long *)page) == 1) /* non-writeable, present */
        un_wp_page((unsigned long *)page);
    return;
}

/* 分配一个空白页面, 并将线性地址 address 和这个页面对应起来 */
void get_empty_page(unsigned long address)
{
    unsigned long tmp;

    if (!(tmp = get_free_page()) || !put_page(tmp, address)) {
        free_page(tmp); /* 0 is ok - ignored */
        oom();
    }
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable or library.
 *
 * 尝试对当前进程指定地址处的页面进行共享处理
 *
 * 当前进程与进程 p 是同一执行代码, 也可以认为当前进程是由 p 进程执行 fork
 * 操作产生的进程, 因此它们的代码内容一样.
 * 如果未对数据段内容作过修改那么数据段内容也应一样 如果 p 进程 address
 * 处的页面存在并且没有被修改过的话, 就让当前进程与 p 进程共享之.
 * 同时还需要验证指定的地址处是否已经申请了页面, 若是则出错/死机
 * 参数:
 *      address 是当前进程中的逻辑地址, 希望能与 p 进程共享页面的逻辑页面地址
 *      p 是将被共享页面的进程
 * 返回: 1 - 页面共享处理成功, 0 - 失败 */
static int try_to_share(unsigned long address, struct task_struct *p)
{
    unsigned long from;
    unsigned long to;
    unsigned long from_page;
    unsigned long to_page;
    unsigned long phys_addr;

    /* 页目录项索引 */
    from_page = to_page = ((address >> 20) & 0xffc);

    /* 这里的操作是 address 是进程空间里面的线性地址, 它的范围是 0-64MB
     * 因此 address 对应的 PDE, 是相对于进程空间本身的
     * 下面用 start_code 在计算一次 PDE, 这次算出来的是进程空间在 4GB
     * 空间上的偏移 因此两者相加, 就是 address 在 4GB 空间上的页表项
     *
     * TODO: 确认这部分分析 */
    from_page += ((p->start_code >> 20) & 0xffc);     // p进程目录项
    to_page += ((current->start_code >> 20) & 0xffc); // 当前进程目录项

    /* is there a page-directory at from?
     * 源页目录项内容. 如果标记不存在, 谈不上共享, 直接退出 */
    from = *(unsigned long *)from_page;
    if (!(from & 1))
        return 0;

    from &= 0xfffff000;                           /* 得到源页表页地址 */
    from_page = from + ((address >> 10) & 0xffc); /* 页表页基址加上页表页索引, 得指向 PTE 指针 */
    phys_addr = *(unsigned long *)from_page;      /* 取出来 PTE 内容 */
                                                  /* is the page clean and present?
                                              * 查看 PTE 内容是否是 (!Dirty && Present) */
    if ((phys_addr & 0x41) != 0x01)
        return 0;

    /* 源页面物理地址 */
    phys_addr &= 0xfffff000;
    if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
        /* 要求物理地址在主内存空间, 否则也不共享 */
        return 0;

    /* 目的页目录项内容, 如果标记不存在, 需要分配一个当做页表的页 */
    to = *(unsigned long *)to_page;
    if (!(to & 1)) {
        if ((to = get_free_page())) {
            *(unsigned long *)to_page = to | 7; /* 页表是存在,可读写,超级用户权限 */
        } else {
            oom();
        }
    }

    to &= 0xfffff000;                         /* 页表页地址 */
    to_page = to + ((address >> 10) & 0xffc); /* PTE 地址 */
    if (1 & *(unsigned long *)to_page) {
        /* 要把 from 共享到 to, 自然要求 to 不存在 */
        panic("try_to_share: to_page already exists");
    }

    /* share them: write-protect
     * 把写标记清除, 将来好 Copy-On-Write
     * 并让 from_page, to_page 都指向 from_page 对应的物理地址 */
    *(unsigned long *)from_page &= ~2;
    *(unsigned long *)to_page = *(unsigned long *)from_page;

    /* 更新 TLB */
    invalidate();

    // 将内存页引用计数 +1
    phys_addr -= LOW_MEM;
    phys_addr >>= 12;
    mem_map[phys_addr]++;

    return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 *
 * share_page 试图找到一个进程, 它可以与当前进程共享页面. 参数 address 是
 * 当前进程数据空间中期望共享的某页面地址
 *
 * 首先我们通过检测 `executable->i_count` 来查证是否可行, 如果有其他任务已共享
 * 该 inode, 则它应该大于 1
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * 在发生缺页异常时, 首先看看能否与运行同一个执行文件的其他进程进行页面共享处理
 * 该函数首先判断系统中是否有另一个进程也在运行当前进程一样的执行文件. 若有,
 * 则在 系统当前所有任务中寻找这样的任务.
 * 若找到了这样的任务就尝试与其共享指定地址处的 页面.
 * 若系统中没有其他任务正在运行与当前进程相同的执行文件, 那么共享页面操作的
 * 前提条件不存在, 因此函数立刻退出.
 *
 * 判断系统中是否有另一个进程也在执行同一个执行文件的方法是利用进程任务数据结构中
 * 的 executable 字段(或 library 字段),
 * 该字段指向进程正在执行程序(或使用的库文件) 在内存中的 i 节点. 根据该 i
 * 节点的引用次数 i_count 我们可以进行这种判断 若节点的 i_count 值大于 1,
 * 则表明系统中有两个进程正在运行同一个执行文件(或库文件),
 * 于是可以再对任务结构数组中所有任务比较是否有相同的 executable 字段(或 library
 * 字段) 来最后确定多个进程运行着相同执行文件的情况
 *
 * 参数: inode 是欲进行共享页面进程执行文件的内存 i 节点
 *      address 是进程中的逻辑地址, 即是当前进程欲与 p
 * 进程共享页面的逻辑页面地址 返回: 1-共享操作成功, 0-失败 */
static int share_page(struct m_inode *inode, unsigned long address)
{
    struct task_struct **p;

    /* 检查文件 i 节点的引用数量, 如果这个节点引用数 <= 1
     * 说明它在内存中只有一个进程在用, 谈不上共享 */
    if (inode->i_count < 2 || !inode)
        return 0;

    /* 遍历进程列表, 找找是那个进程和当前进程共用了 inode,
     * 然后试着共享这个进程的页面 */
    for (p = &LAST_TASK; p > &FIRST_TASK; --p) {
        if (!*p)
            continue;

        if (current == *p)
            continue;

        /* address 位于库区域还是自生可执行区域 */
        if (address < LIBRARY_OFFSET) {
            if (inode != (*p)->executable)
                continue;
        } else {
            if (inode != (*p)->library)
                continue;
        }

        /* 试着共享页面, 如果成功直接退出, 如果不成功继续尝试和其他的 task 共享
         */
        if (try_to_share(address, *p))
            return 1;
    }

    return 0;
}

/**
 * 这个函数用来处理缺页异常
 * 页面异常有两种类型, 一种是写保护异常, 还有一种是缺页异常 */
void do_no_page(unsigned long error_code, unsigned long address)
{
    int nr[4];
    unsigned long tmp;
    unsigned long page;
    int block, i;
    struct m_inode *inode;

    /* 只为主内存空间服务 */
    if (address < TASK_SIZE)
        printk("\n\rBAD!! KERNEL PAGE MISSING\n\r");

    /* 超出了应用自身的空间范围 */
    if (address - current->start_code > TASK_SIZE) {
        printk("Bad things happen: nonexistent page error in do_no_page\n\r");
        do_exit(SIGSEGV);
    }

    page = *(unsigned long *)((address >> 20) & 0xffc); /* PDE 内容 */
    if (page & 1) {                                     /* PDE 有记录 */
        page &= 0xfffff000;                             /* 页表页地址 */
        page += (address >> 10) & 0xffc;                /* PTE 地址 */
        tmp = *(unsigned long *)page;                   /* PTE 内容 */
        if (tmp && !(1 & tmp)) {                        /* P=0, 只需要把页面交换回来就行了 */
            swap_in((unsigned long *)page);
            return;
        }
    }

    /**
     * 到这里有 2 种情况
     *  1. (page & 1) = 0, 即页表页不存在
     *  2. PTE = nil 或者 (PTE & 1) == 1, 物理帧不存在
     * 2 似乎不太可能发生?
     * 这里先按 1 的情况理解 */

    address &= 0xfffff000;               /* 计算进程内地址的页面地址 */
    tmp = address - current->start_code; /* 得到 address 在进程内部的相对位置 */
    if (tmp >= LIBRARY_OFFSET) {
        inode = current->library;
        block = 1 + (tmp - LIBRARY_OFFSET) / BLOCK_SIZE;
    } else if (tmp < current->end_data) {
        inode = current->executable;
        block = 1 + tmp / BLOCK_SIZE;
    } else {
        inode = NULL;
        block = 0;
    }

    /* 对除了 library 和 executable 部分的缺页, 直接分配新页面
     * 这种页面一般是 '动态申请的页面或存放栈信息的页面' */
    if (!inode) {
        get_empty_page(address);
        return;
    }

    /* 尝试共享已有页面 */
    if (share_page(inode, tmp))
        return;

    /* 不能共享页面的, 分配新页面 */
    if (!(page = get_free_page()))
        oom();

    /* TODO: 下面和文件系统相关,
     * 根据这个块号和执行文件的 i 节点,
     * 我们就可以从映射位图中找到对应块设备中对应的设备 逻辑块号(保存在 nr[]
     * 数组中), 利用 bread_page 即可把这 4 个逻辑块读入到 物理页面 page 中 */

    /* remember that 1 block is used for header
     * 程序头要使用 1 个数据块(1024B) */
    for (i = 0; i < 4; block++, i++)
        nr[i] = bmap(inode, block);

    bread_page(page, inode->i_dev, nr); /* 把磁盘上的数据读取到内存里 */

    /**
     * tmp 是 address 在进程内部的相对位置
     *
     * 在读设备逻辑块操作时, 可能会出现这样一种情况,
     * 即在执行文件中的读取页面位置可能离 文件尾不到 1 个页面的长度,
     * 因此可能读入一些无用的信息. 下面的操作就是把这部分超 出执行文件 end_data
     * 以后的部分进行清零处理. 当然, 若该页面离末端超过 1 页,
     * 说明不是从执行文件映像中读取的页面, 而是从库文件中读取的
     * 因此不用执行清零操作
     *
     * tmp 表示从进程代码段起始地址到发生缺页地址的偏移量, current->end_data
     * 则是 进程数据段的结束地址, 这里加 4096 是因为一个内存页的大小是 4096
     * 字节, 所以 `tmp + 4096` 计算的是从缺页地址开始的整个页面的结束位置
     * TODO: 没看懂这里和 4095 比较是啥意思, 先接受结果: i
     * 算出来是多读进来的数据
     */
    i = tmp + 4096 - current->end_data;
    if (i > 4095)
        i = 0;

    /* page + 4096 表示的是 page 末尾地址
     * 下面的 for 循环, 从页面的结束为止开始, 擦除 i 字节数据 */
    tmp = page + 4096;
    while (i-- > 0) {
        tmp--;
        *(char *)tmp = 0;
    }

    /* 新页和线性地址关联起来 */
    if (put_page(page, address))
        return;

    free_page(page);
    oom();
}

/**
 * 内存初始化, 只管理 start ~ end 之间的内存空间
 * 这部分内从空间叫做主内存区域, 他们并不是全机器上的全部空间 */
void mem_init(long start_mem, long end_mem)
{
    int i;

    HIGH_MEMORY = end_mem;

    // 这里吧全部的内存都标记为占用状态
    for (i = 0; i < PAGING_PAGES; i++)
        mem_map[i] = USED;

    /* 找到 start_mem 所属的页面编号 */
    i = MAP_NR(start_mem);

    /* 计算主内存区域页面总量 */
    end_mem -= start_mem;
    end_mem >>= 12;

    /* 再把主内存区域所属的页面置为空闲
     * 请注意这里的处理主内存区域不是从 1M 开始的
     * Linux 主内存区域只是 **最多** 能管理 15MB 主内存区域 */
    while (end_mem-- > 0)
        mem_map[i++] = 0;
}

void show_mem(void)
{
    int i, j, k, free = 0, total = 0;
    int shared = 0;
    unsigned long *pg_tbl;

    printk("Mem-info:\n\r");
    for (i = 0; i < PAGING_PAGES; i++) {
        if (mem_map[i] == USED) /* 参看 mem_init, 如果内存不属于主存储区,
                                   它们的标记就是 USED */
            continue;

        total++; /* 主存储区的页面数量 */
        if (!mem_map[i])
            free++; /* 主存储区的空闲页面数量 */
        else
            shared += mem_map[i] - 1; /* 主存储区的页面共享数量(自己用不算共享) */
    }

    printk("%d free pages of %d\n\r", free, total);
    printk("%d pages shared\n\r", shared);

    /* 下面遍历页目录项 */
    k = 0; /* 一个进程占用页面统计值 */
    for (i = 4; i < 1024;) {
        /* 页目录项标记的页表页存在的话, 处理页表页的统计信息 */
        if (1 & pg_dir[i]) {
            /* pg_dir 记录的地址超过了可管理的物理内存 */
            if (pg_dir[i] > HIGH_MEMORY) {
                printk("page directory[%d]: %08X\n\r", i, pg_dir[i]);
                continue;
            }

            /*
             * 如果页目录项对应二级页表的'地址'大于 LOW_MEM(即1MB),
             * 则把一个进程占用的物理内存页统计值 k 增 1.
             * 把系统占用的所有物理内存页 统计值 free 增 1. 然后取对应页表地址
             * pg_tbl, 并对该页表中所有页表项进行统计.
             *
             * 如果当前页表项所指物理页面存在并且该物理页面 '地址' 大于 LOW_MEM,
             * 那么就将页表项对应页面纳入统计值
             * TODO: LOW_MEM 似乎没有约束主内存区域起始位置? */
            if (pg_dir[i] > LOW_MEM) {
                free++, k++;
            }

            pg_tbl = (unsigned long *)(0xfffff000 & pg_dir[i]);
            for (j = 0; j < 1024; j++) {
                if ((pg_tbl[j] & 1) && pg_tbl[j] > LOW_MEM) {
                    if (pg_tbl[j] > HIGH_MEMORY) {
                        printk("page_dir[%d][%d]: %08X\n\r", i, j, pg_tbl[j]);
                    } else {
                        k++, free++;
                    }
                }
            }
        }

        /* 处理页目录项的统计信息 */

        i++;

        /* 因每个任务线性空间长度是 64MB, 所以一个任务占用 16 个目录项
         * 因此这里每统计了 16 个目录项就把进程的任务结构占用的页表统计进来
         * 若此时 k=0 则表示当前的 16
         * 个页目录所对应的进程在系统中不存在(没有创建或者已经终止)
         * 在显示了对应进程号和其占用的物理内存页统计值 k 后, 将 k 清零,
         * 以用于统计下一个进程占用的内存页面数.
         *
         * 当 i <= 15 的时候, (i&15) 恒真, 当 16 < i <= 31 的时候, (i&15) 恒真,
         * 此后的数据类似 因此 (!(i&15)) 意思就是每次 i 是 16 的倍数的时候,
         * 就真一次
         * TODO: 但是这里的 i 是从 4 开始的啊? */
        if (!(i & 15) && k) { /* k !=0 表示相应进程存在 */
            k++, free++;      /* one page/process for task_struct */
            /* (i>>4) -1 可以求得任务编号(除以 16 减 1) */
            printk("Process %d: %d pages\n\r", (i >> 4) - 1, k);
            k = 0;
        }
    }

    /* 最后显示系统中正在使用的内存页面和主内存区中总的内存页面数 */
    printk("Memory found: %d (%d)\n\r", free - shared, total);
}
