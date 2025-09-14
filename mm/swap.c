/*
 *  linux/mm/swap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * This file should contain most things doing the swapping from/to disk.
 * Started 18.12.91
 */

#include <string.h>

#include <linux/head.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>

#define SWAP_BITS (4096 << 3) /* 一页(4KB)有多少个 bit 位 */

/* bt, BitTest, 用目的操作数的位值置 CR 标记
 * bts, BitTestAndSet, 目的操作数位值置 1, 并用目的操作数的原始位值置 CR 标记
 * btr, BitTestAndReset, 目的操作数位值置 0, 并用目的操作数的原始位值置 CR 标记
 *
 * adcl src, dst
 * dst = dst + src + CF
 */
#define bitop(name, op)                                                                            \
    static inline int name(char *addr, unsigned int nr)                                            \
    {                                                                                              \
        int __res;                                                                                 \
        __asm__ __volatile__("bt" op " %1,%2\n\t"                                                  \
                             "adcl $0,%0\n\t" /* adc 用于将 CR 标记体现到最终结果里面 */           \
                             : "=g"(__res)                                                         \
                             : "r"(nr), "m"(*(addr)), "0"(0));                                     \
        return __res;                                                                              \
    }

bitop(bit, "");     // bt
bitop(setbit, "s"); // bts
bitop(clrbit, "r"); // btr

/* swap_bitmap 是管理 SWAP 空间的位图, 相关的位置 1, 说明对应的页空间可用, 0 表示不可用 */
static char *swap_bitmap = NULL;

/* 交换设备号, 在 main 函数里面被设置, 最早在 bootsect.s 设置 */
int SWAP_DEV = 0;

/*
 * We never page the pages in task[0] - kernel memory.
 * We page all other pages.
 *
 * task-0 的页面不做交换, 因为它在内核内存范围
 *
 * 第一个虚拟页面号, 在 LOW_MEMORY + TASK_SIZE 处, 也就是从 idle 进程(不含)之后的内存空间
 * 对应的页面都可以做交换
 */
#define FIRST_VM_PAGE (TASK_SIZE >> 12)         /* 第一个虚拟页面号*/
#define LAST_VM_PAGE (1024 * 1024)              /* 最后一个虚拟页面号 */
#define VM_PAGES (LAST_VM_PAGE - FIRST_VM_PAGE) /* 虚拟页面数量 */

/* swap_bitmap 记录了现在那个 swap 页面是空闲的
 * 这里找到第一个可用的空闲页面, 返回这个页面的编号 */
static int get_swap_page(void)
{
    int nr;

    if (!swap_bitmap) {
        return 0;
    }

    for (nr = 1; nr < 32768; nr++) {
        if (clrbit(swap_bitmap, nr)) { /* bit 1 => 0 */
            return nr;
        }
    }

    return 0;
}

/* 把 swap_nr 对应的页面在 swap 释放 */
void swap_free(int swap_nr)
{
    if (!swap_nr) {
        return;
    }

    if (swap_bitmap && swap_nr < SWAP_BITS) {
        if (!setbit(swap_bitmap, swap_nr)) {
            return;
        }
    }

    printk("Swap-space bad (swap_free())\n\r");
    return;
}

/* 把 table_ptr 指向的页面, 交换会内存中  */
void swap_in(unsigned long *table_ptr)
{
    int swap_nr;
    unsigned long page;

    if (!swap_bitmap) {
        printk("Trying to swap in without swap bit-map");
        return;
    }

    if (1 & *table_ptr) {
        printk("trying to swap in present page\n\r");
        return;
    }

    swap_nr = *table_ptr >> 1;
    if (!swap_nr) {
        printk("No swap page in swap_in\n\r");
        return;
    }

    if (!(page = get_free_page()))
        oom();

    read_swap_page(swap_nr, (char *)page);
    if (setbit(swap_bitmap, swap_nr)) { /* bitmap 0 => 1 */
        printk("swapping in multiply from same page\n\r");
    }

    /* TODO: 这里为啥要把页面置为脏页?
     * 答: 不标记为脏的话, 可能这个页面很快又被 swap out 了 */
    *table_ptr = page | (PAGE_DIRTY | 7);
}

/* 把 table_ptr 指向的 PTE 对应的页, 换出到 SWAP
 *
 * 若页面没有被修改过则不用保存在交换设备中,
 * 因为对应页面还可以再直接从相应映像文件 中读入, 可以直接释放掉相应物理页面了事
 * 否则就申请一个交换页面号, 然后把页面交换出去.
 * 此时交换页面号要保存在对应页表项中, 并且仍需要保持页表项存在位 P = 0
 *
 * TODO: 那些刚刚分配出来用于堆栈的空间, 岂不是有可能很快又被释放了?
 *
 * 页面交换或释放成功返回 1, 否则返回 0 */
int try_to_swap_out(unsigned long *table_ptr)
{
    unsigned long page;
    unsigned long swap_nr;

    page = *table_ptr; /* PTE 内容 = 页地址 + 属性 */
    if (!(PAGE_PRESENT & page)) {
        return 0;
    }

    /* 最多支持到 16M 内存, 去掉 1M 内核使用还剩余 15M 可供分页使用 */
    if (page - LOW_MEM > PAGING_MEMORY) {
        return 0;
    }

    /* 脏页? */
    if (PAGE_DIRTY & page) {
        page &= 0xfffff000;               /* 页基地址 */
        if (mem_map[MAP_NR(page)] != 1) { /* 共享页面, 不能换出 */
            return 0;
        }

        /* 找到页面即将被换出到的那个 SWAP 分区页面 */
        if (!(swap_nr = get_swap_page())) {
            return 0;
        }

        /* 被换出的页面, 页表项里面保存 swap_nr << 1
         * 这样 PTE 的 P 位还可以设置 0, 表示他不在内存里 */
        *table_ptr = swap_nr << 1;
        invalidate();

        /* 将页面正式写到 SWAP 里面  */
        write_swap_page(swap_nr, (char *)page);

        /* 释放页面所占用的物理内存 */
        free_page(page);

        return 1;
    }

    /* 不是脏页直接抹掉, 被抹掉的页面, 有需要的话, 会以缺页中断的方式重新被加载回来 */
    *table_ptr = 0;
    invalidate();
    free_page(page);
    return 1;
}

/*
 * Ok, this has a rather intricate logic - the idea is to make good
 * and fast machine code. If we didn't worry about that, things would
 * be easier.
 *
 * 把某个页, 交换到 SWAP 设备
 */
int swap_out(void)
{
    static int dir_entry = FIRST_VM_PAGE >> 10; /* 即任务 1 的第 1 个 PDT 索引 */
    static int page_entry = -1;
    int counter = VM_PAGES;
    int pg_table;

    /* 首先搜索页目录表, 查找存在二级页表页的页目录项, 这个二级页表页里面的页表项的某一个项,
     * 就是我们这次希望 swap 的对象
     *
     * 找到直接退出循环, 否则调整页目录项数对应剩余二级页表项数 counter, 然后继续检测下一页目录项.
     * 若全部搜索完还没有找到适合的(存在的)页目录项, 就重新继续搜索
     *
     * TODO-DONE: 这里重新搜索的意义是什么? 不是已经找不到了吗?
     * 答: counter 是最差的情况下要迭代的次数, 只要找不到符合交换条件的页面就需要一直找下去, 一直到 counter 消耗完毕 */
    while (counter > 0) {
        pg_table = pg_dir[dir_entry];
        if (pg_table & 1) {
            break;
        }

        counter -= 1024; /* 一个 PDT 已经检查完, 相当于一次性查找了 1024 个 PTE */
        dir_entry++;
        if (dir_entry >= 1024) { /* 避免越界 */
            dir_entry = FIRST_VM_PAGE >> 10;
        }
    }

    /* 在取得二级页表页地址之后, 针对该页表页中的所有 1024 个页面,
     * 逐一调用交换函数 try_to_swap_out 尝试交换出去.
     * 一旦某个页面成功交换到交换设备中就返回 1. 若对所
     * 有目录项的所有页表都已尝试失败, 则显示 "交换内存用完" 的警告, 并返回 0 */

    pg_table &= 0xfffff000; /* pg_table 现在指向页表页的基地址 */
    while (counter-- > 0) {
        page_entry++;
        if (page_entry >= 1024) {
            page_entry = 0;

        repeat:
            dir_entry++;
            if (dir_entry >= 1024) {
                dir_entry = FIRST_VM_PAGE >> 10;
            }

            pg_table = pg_dir[dir_entry];
            if (!(pg_table & 1)) { /* page 不在内存里 */
                if ((counter -= 1024) > 0) {
                    goto repeat;
                } else {
                    break;
                }
            }

            pg_table &= 0xfffff000;
        }

        if (try_to_swap_out(page_entry + (unsigned long *)pg_table)) {
            return 1;
        }
    }

    printk("Out of swap-memory\n\r");
    return 0;
}

/**
 * @brief 分配一个空闲的物理页面
 *
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 *
 * @return unsigned long 返回页面的物理地址
 */
unsigned long get_free_page(void)
{
    register unsigned long __res asm("ax");

    // scasb 比较 %al 和 (%edi), 比较完后 %edi+1, %ecx-1

repeat:
    __asm__(
        "std\n\t"
        "repne scasb\n\t" /* 倒着找, mem_map 里面不为 0 的字节 */
        "jne 1f\n\t" /* jne 说明没有找到, 否则说明找到了, 相关的值为 0 的字节指针在 %edi+1 位置 */
        "movb $1, 1(%%edi)\n\t" /* 将那个 0 字节置 1, 表示这个页面现在被用了 */
        "sall $12, %%ecx\n\t" /* sall = shll, 都是左移, (PAGING_PAGS << 12) 得相对页面起始地址 - 这个地址是相对 LOW_MEM 的 */
        "addl %2, %%ecx\n\t" /* 加上 LOW_MEM 之后, 得到的就是那个值为 0 的字节, 对应的物理地址了 */
        "movl %%ecx, %%edx\n\t"       /* 物理地址存到 EDX */
        "movl $1024, %%ecx\n\t"       /* ECX = 1024 */
        "leal 4092(%%edx), %%edi\n\t" /* EDI = 物理地址+4092, 实际上就是页的末尾 */
        "rep stosl\n\t"               /* 上面已经 std, 这里倒着给页面填充 0 */
        "movl %%edx, %%eax\n"         /* 返回页面地址 */
        "1:"
        : "=a"(__res)
        : "0"(0), "i"(LOW_MEM), "c"(PAGING_PAGES), "D"(mem_map + PAGING_PAGES - 1)
        : "dx");

    /* 找的结果不对, 重找 */
    if (__res >= HIGH_MEMORY)
        goto repeat;

    /* 没找到, 交换出去老页面重新来过 */
    if (!__res && swap_out())
        goto repeat;

    /* 最后这里要是还没找到 __res 预期应该也是 0 */
    return __res;
}

/**
 * @brief 初始化交换分区
 *
 * swap_bitmap 首尾部分的 bits 应该是 0, 其他部分的 bits 是 1
 * bits=1 表示这个分页是可以用的, bits=0 表示对应的交换区分页不可用
 *
 *  - 第一个 bit 是 0 的原因是, 第一个 page 被用作了 swap_bitmap, 不做普通的交换功能使用
 *  - swap_size 之后的 bits 置 0 的原因是, 这些位置不是合法的 swap 交换分区范围, 因此不可用
 *
 * 比方说像下面这个 swap 分区:
 *
 *  00000000  fe ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff  |................|
 *  00000010  ff ff ff ff ff ff ff ff  ff ff ff ff ff ff ff ff  |................|
 *  *
 *  000003e0  ff ff ff ff ff ff ff ff  00 00 00 00 00 00 00 00  |................|
 *  000003f0  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
 *  *
 *  00000ff0  00 00 00 00 00 00 53 57  41 50 2d 53 50 41 43 45  |......SWAP-SPACE|
 *  00001000  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
 *
 * 可以看到第一个 bit=0, 第 ((0x3e0 + 8) * 8 + 1) 位置开始的 bits 也是 0, 在靠近页面结束
 * 的地方, 有 `SWAP-SPACE` 文本标记.
 *
 * 这个例子里面, swap 分区的大小是 ((0x3e0 + 8) * 8) = 8000 个页面, 也就是 8000 * 4KB = 32M 字节
 */
void init_swapping(void)
{
    /* blk_size[] 指向指定主设备号的块设备块数数组.
     * 该块数数组每一项对应一个子设备上所拥有的数据块总数(1块大小=1KB) */
    extern int *blk_size[];
    int swap_size, i, j;

    /* 没有交换功能的, 就不初始化了 */
    if (!SWAP_DEV)
        return;

    /* 如果交换设备没有设置块数组, 则显示信息并返回
     * 这个值通常在软/硬盘的初始化里面就会设置好, 取不到说明设备初始化的不对 */
    if (!blk_size[MAJOR(SWAP_DEV)]) {
        printk("Unable to get size of swap device\n\r");
        return;
    }

    /* 取指定交换设备号的交换区数据块总数 swap_size
     * 每个 block 是 1KB, 交换分区一共就是 swap_size KB */
    swap_size = blk_size[MAJOR(SWAP_DEV)][MINOR(SWAP_DEV)];
    if (!swap_size) {
        return;
    }

    /* 交换设备需要足够大才行 */
    if (swap_size < 100) {
        printk("Swap device too small (%d blocks)\n\r", swap_size);
        return;
    }

    /* 交换数据块总数转换成对应可交换页面总数, 该值不能大于 SWAP_BITS
     * 所能表示的页面数即交换页面总数不得大于 32768.
     * 然后申请一页物理内存用来存放交换页面位映射数组 swap_bitmap, 其中每 1
     * 比特代表 1 页交换页面 */
    swap_size >>= 2; /* swap_size KB / 4KB = swap page number */
    if (swap_size > SWAP_BITS) {
        swap_size = SWAP_BITS;
    }

    /* 准备一张空页面, 这个页面使用位图来记录交换区域的使用情况 */
    swap_bitmap = (char *)get_free_page();
    if (!swap_bitmap) {
        printk("Unable to start swapping: out of memory :-)\n\r");
        return;
    }

    /* 从 SWAP 分区读取 0 号页面对应的数据页面, 这个页面是交换区管理页面
     * 管理页面需要以字符 `SWAP-SPACE` 结束, 这是 SWAP 区域的特征 */
    read_swap_page(0, swap_bitmap);
    if (strncmp("SWAP-SPACE", swap_bitmap + 4086, 10)) {
        printk("Unable to find swap-space signature\n\r");
        free_page((long)swap_bitmap);
        swap_bitmap = NULL;
        return;
    }

    /* 把 `SWAP-SPACE` 标记位置, 清零
     * NOTICE: 这些位置不能用于正常的交换位图信息 */
    memset(swap_bitmap + 4086, 0, 10);

    /* 然后检查读入的交换位映射图 */
    for (i = 0; i < SWAP_BITS; i++) {
        if (i == 1) {
            i = swap_size; /* 跳过中间的部分 */
        }

        /* 对应的 bit 位置不是 0? */
        if (bit(swap_bitmap, i)) {
            printk("Bad swap-space bit-map\n\r");
            free_page((long)swap_bitmap);
            swap_bitmap = NULL;
            return;
        }
    }

    /* 检查 swap_bitmap 的 [1, swap_size) 区间, 应该有最起码一个 bit 的值是 1
     * 否则的话, 这就不是一个正常的 swap 分区了 */
    j = 0;
    for (i = 1; i < swap_size; i++) {
        if (bit(swap_bitmap, i)) {
            j++;
        }
    }

    if (!j) {
        free_page((long)swap_bitmap);
        swap_bitmap = NULL;
        return;
    }

    printk("Swap device ok: %d pages (%d bytes) swap-space\n\r", j, j * 4096);
}
