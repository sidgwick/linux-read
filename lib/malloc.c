/*
 * malloc.c --- a general purpose kernel memory allocator for Linux.
 *
 * Written by Theodore Ts'o (tytso@mit.edu), 11/29/91
 *
 * This routine is written to be as fast as possible, so that it
 * can be called from the interrupt level.
 *
 * Limitations: maximum size of memory we can allocate using this routine
 *    is 4k, the size of a page in Linux.
 *
 * The general game plan is that each page (called a bucket) will only hold
 * objects of a given size.  When all of the object on a page are released,
 * the page can be returned to the general free pool.  When malloc() is
 * called, it looks for the smallest bucket size which will fulfill its
 * request, and allocate a piece of memory from that bucket pool.
 *
 * Each bucket has as its control block a bucket descriptor which keeps
 * track of how many objects are in use on that page, and the free list
 * for that page.  Like the buckets themselves, bucket descriptors are
 * stored on pages requested from get_free_page().  However, unlike buckets,
 * pages devoted to bucket descriptor pages are never released back to the
 * system.  Fortunately, a system should probably only need 1 or 2 bucket
 * descriptor pages, since a page can hold 256 bucket descriptors (which
 * corresponds to 1 megabyte worth of bucket pages.)  If the kernel is using
 * that much allocated memory, it's probably doing something wrong.  :-)
 *
 * Note: malloc() and free() both call get_free_page() and free_page()
 *    in sections of code where interrupts are turned off, to allow
 *    malloc() and free() to be safely called from an interrupt routine.
 *    (We will probably need this functionality when networking code,
 *    particularily things like NFS, is added to Linux.)  However, this
 *    presumes that get_free_page() and free_page() are interrupt-level
 *    safe, which they may not be once paging is added.  If this is the
 *    case, we will need to modify malloc() to keep a few unused pages
 *    "pre-allocated" so that it can safely draw upon those pages if
 *     it is called from an interrupt routine.
 *
 *     Another concern is that get_free_page() should not sleep; if it
 *    does, the code is carefully ordered so as to avoid any race
 *    conditions.  The catch is that if malloc() is called re-entrantly,
 *    there is a chance that unecessary pages will be grabbed from the
 *    system.  Except for the pages for the bucket descriptor page, the
 *    extra pages will eventually get released back to the system, though,
 *    so it isn't all that bad.
 */

#include <asm/system.h>
#include <linux/kernel.h>
#include <linux/mm.h>

/* 桶描述符 */
struct bucket_desc { /* 16 bytes */
    void *page;
    struct bucket_desc *next;
    void *freeptr; /* 指向分配的内存 */
    unsigned short refcnt;
    unsigned short bucket_size;
};

/* 同一个区块大小的桶集合 */
struct _bucket_dir { /* 8 bytes */
    int size;
    struct bucket_desc *chain;
};

/*
 * The following is the where we store a pointer to the first bucket
 * descriptor for a given size.
 *
 * If it turns out that the Linux kernel allocates a lot of objects of a
 * specific size, then we may want to add that specific size to this list,
 * since that will allow the memory to be allocated more efficiently.
 * However, since an entire page must be dedicated to each specific size
 * on this list, some amount of temperance must be exercised here.
 *
 * Note that this list *must* be kept in order.
 */
struct _bucket_dir bucket_dir[] = {
    {16, (struct bucket_desc *)0},   /*   16 byte 空间管理桶 */
    {32, (struct bucket_desc *)0},   /*   32 byte 空间管理桶 */
    {64, (struct bucket_desc *)0},   /*   64 byte 空间管理桶 */
    {128, (struct bucket_desc *)0},  /*  128 byte 空间管理桶 */
    {256, (struct bucket_desc *)0},  /*  256 byte 空间管理桶 */
    {512, (struct bucket_desc *)0},  /*  512 byte 空间管理桶 */
    {1024, (struct bucket_desc *)0}, /* 1024 byte 空间管理桶 */
    {2048, (struct bucket_desc *)0}, /* 2048 byte 空间管理桶 */
    {4096, (struct bucket_desc *)0}, /* 4096 byte 空间管理桶 */
    {0, (struct bucket_desc *)0},    /* End of list marker */
};

/*
 * This contains a linked list of free bucket descriptor blocks
 */
struct bucket_desc *free_bucket_desc = (struct bucket_desc *)0;

/**
 * @brief 初始化桶描述符页面
 *
 * This routine initializes a bucket description page.
 */
static inline void init_bucket_desc()
{
    struct bucket_desc *bdesc, *first;
    int i;

    /* 分配桶描述符页面 */
    first = bdesc = (struct bucket_desc *)get_free_page();
    if (!bdesc) {
        panic("Out of memory in init_bucket_desc()");
    }

    /* 串联成链表结构 */
    for (i = PAGE_SIZE / sizeof(struct bucket_desc); i > 1; i--) {
        bdesc->next = bdesc + 1;
        bdesc++;
    }

    /*
     * This is done last, to avoid race conditions in case
     * get_free_page() sleeps and this routine gets called again....
     */
    bdesc->next = free_bucket_desc;
    free_bucket_desc = first;
}

/**
 * @brief 分配内存
 *
 * @param len
 * @return void*
 */
void *malloc(unsigned int len)
{
    struct _bucket_dir *bdir;
    struct bucket_desc *bdesc;
    void *retval;

    /*
     * First we search the bucket_dir to find the right bucket change
     * for this request.
     *
     * 找一下应该使用哪个桶集合 bucket_dir
     */
    for (bdir = bucket_dir; bdir->size; bdir++) {
        if (bdir->size >= len) {
            break;
        }
    }

    if (!bdir->size) {
        printk("malloc called with impossibly large argument (%d)\n", len);
        panic("malloc: bad arg");
    }

    /*
     * Now we search for a bucket descriptor which has free space
     * 在桶集合里找一下, 那个桶是空闲的
     */
    cli(); /* Avoid race conditions */

    for (bdesc = bdir->chain; bdesc; bdesc = bdesc->next) {
        if (bdesc->freeptr) {
            break;
        }
    }

    /*
     * If we didn't find a bucket with free space, then we'll
     * allocate a new one.
     */
    if (!bdesc) {
        char *cp;
        int i;

        if (!free_bucket_desc) {
            init_bucket_desc();
        }

        /* 从空闲链表摘出来低一个空闲节点 */
        bdesc = free_bucket_desc;
        free_bucket_desc = bdesc->next;

        /* 然后分配内存页面, 并分配一张干净的页面 */
        bdesc->refcnt = 0;
        bdesc->bucket_size = bdir->size;

        cp = (char *)get_free_page();
        bdesc->freeptr = (void *)cp;
        bdesc->page = bdesc->freeptr;
        if (!cp) {
            panic("Out of memory in kernel malloc()");
        }

        /* Set up the chain of free objects
         * 以该桶目录项指定的桶大小为对象长度, 对该页内存进行划分, 并使每个对象的开始 4
         * 字节设置成指向下一对象的指针. 这就是一种链表, 虽然没有明确为它定义数据结构
         *
         * TODO-DONE: 为什么要这样做?
         * 答: 方便找到下一个可用内存位置, 在每次分配一个 chunk 之后, bucket 节点就可以
         *     直接读取下一个可用地址并记录, 这样就不用做 chunk 的遍历操作了
         * TODO-DONE: 这样会导致每次分配的内存数量不够 size 大小吗?
         * 答: 不会, 最开始的用来记录下一可用地址的四个字节, 在 malloc 之后, 是可以给 app
         *     使用的, 等 free 的时候, 再把下一个可用地址回写到这里来就行了 */
        for (i = PAGE_SIZE / bdir->size; i > 1; i--) {
            *((char **)cp) = cp + bdir->size;
            cp += bdir->size;
        }

        *((char **)cp) = 0;
        bdesc->next = bdir->chain; /* OK, link it in! */
        bdir->chain = bdesc;
    }

    retval = (void *)bdesc->freeptr;
    bdesc->freeptr = *((void **)retval); /* 下一个可用的空闲位置 */
    bdesc->refcnt++;

    sti(); /* OK, we're safe again */

    return (retval);
}

/**
 * @brief 释放内存
 *
 * Here is the free routine.  If you know the size of the object that you
 * are freeing, then free_s() will use that information to speed up the
 * search for the bucket descriptor.
 *
 * We will #define a macro so that "free(x)" is becomes "free_s(x, 0)"
 *
 * @param obj 当初 malloc 返回的地址
 * @param size 当初 malloc 分配的空间大小(可以不指定)
 */
void free_s(void *obj, int size)
{
    void *page;
    struct _bucket_dir *bdir;
    struct bucket_desc *bdesc, *prev;

    /* Calculate what page this object lives in */
    page = (void *)((unsigned long)obj & 0xfffff000);

    /* Now search the buckets looking for that page */
    for (bdir = bucket_dir; bdir->size; bdir++) {
        prev = 0;

        /* If size is zero then this conditional is always false */
        if (bdir->size < size) {
            continue;
        }

        for (bdesc = bdir->chain; bdesc; bdesc = bdesc->next) {
            if (bdesc->page == page) {
                goto found;
            }

            prev = bdesc;
        }
    }

    panic("Bad address passed to kernel free_s()");

found:
    cli(); /* To avoid race conditions */

    *((void **)obj) = bdesc->freeptr; /* 在这个 chunk 的开头位置, 记录下一个可用的内存块 */
    bdesc->freeptr = obj;             /* 正式释放 obj 代表的内存 */
    bdesc->refcnt--;                  /* 桶描述符指向的页面的引用计数递减 */

    /* 桶描述符指向的页面也可以被释放的情形 */
    if (bdesc->refcnt == 0) {
        /*
         * We need to make sure that prev is still accurate.  It
         * may not be, if someone rudely interrupted us....
         *
         * 如果还有别的描述符也管理了一样的 chunk size, 那 bdesc 应该是这个 size 的描述符集合
         * 链表里面的一个, 需要维护相关的链表结构
         *
         * 找到 found 的那个循环, 其实已经设置好了 prev 和 bdesc 的关系, 这里重算一遍, 是担心有
         * 别的程序也执行过存储描述符的维护, 导致这里的链表间关系错乱
         *
         * TODO-DONE: 实际上这里, 再检查一遍也不能完全避免吧?
         * 答: 能避免, 因为这里一直是在内核态执行的, 任务调度切换不会在内核态的时候发生
         *     那么, 这个 if 本身就是不需要的, 因为这个函数自始至终, 都是在内核态执行呢? */
        if ((prev && (prev->next != bdesc)) || (!prev && (bdir->chain != bdesc))) {
            for (prev = bdir->chain; prev; prev = prev->next) {
                if (prev->next == bdesc) {
                    break;
                }
            }
        }

        if (prev) {
            prev->next = bdesc->next;
        } else {
            if (bdir->chain != bdesc) {
                panic("malloc bucket chains corrupted");
            }

            bdir->chain = bdesc->next;
        }

        free_page((unsigned long)bdesc->page);
        bdesc->next = free_bucket_desc;
        free_bucket_desc = bdesc;
    }

    sti();
    return;
}
