/*
 *  linux/fs/buffer.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'buffer.c' implements the buffer-cache functions. Race-conditions have
 * been avoided by NEVER letting a interrupt change a buffer (except for the
 * data, of course), but instead letting the caller do it. NOTE! As interrupts
 * can wake up a caller, some cli-sti sequences are needed to check for
 * sleep-on-calls. These should be extremely quick, though (I hope).
 */

/*
 * NOTE! There is one discordant note here: checking floppies for
 * disk change. This is where it fits best, I think, as it should
 * invalidate changed floppy-disk-caches.
 *
 * 注意！有一个程序应不属于这里: 检测软盘是否更换. 但我想这里是放置
 * 该程序最好的地方了, 因为它需要使已更换软盘缓冲失效.
 */

#include <stdarg.h>

#include <asm/io.h>
#include <asm/system.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>

extern void put_super(int dev);
extern void invalidate_inodes(int dev);

extern int end; /* 内核在内存中的末端位置 */
struct buffer_head *start_buffer = (struct buffer_head *)&end;

/* 这个哈希表是用来记录 (dev, block) 对应的缓冲块的
 * 里面的缓冲块可能有数据也可能没数据 NR_HASH = 307 项 */
struct buffer_head *hash_table[NR_HASH];
static struct buffer_head *free_list; /* 空闲缓冲块链表头指针 */

/* 等待空闲缓冲块而睡眠的任务队列头指针, 它与缓冲块头部结构中 b_wait
 * 指针的作用不同. 当任务申请一个缓冲块而正好遇到系统缺乏可用空闲缓
 * 冲块时, 当前任务就会被添加到 buffer_wait 睡眠等待队列中. 而 b_wait
 * 则是专门供等待指定缓冲块(即 b_wait 对应的缓冲块)的任务使用的等待队列头指针 */
static struct task_struct *buffer_wait = NULL;

/* 下面定义系统缓冲区中含有的缓冲块个数. 这里, NR_BUFFERS 是一个定义在 linux/fs.h 头
 * 文件的宏, 其值即是全局变量名 nr_buffers. 大写名称通常都是一个宏名称, Linus 这样编写
 * 代码是为了利用这个大写名称来隐含地表示 nr_buffers 是一个在内核初始化之后不再改变的'常量' */
int NR_BUFFERS = 0; /* 总共可用的 buffer_head 数量 */

/**
 * @brief 等待指定缓冲块解锁
 *
 * 如果指定的缓冲块 bh 已经上锁就让进程不可中断地睡眠在该缓冲块的等待队列 b_wait 中.
 * 在缓冲块解锁时, 其等待队列上的所有进程将被唤醒
 *
 * 虽然是在关闭中断(cli)之后去睡眠的, 但这样做并不会影响在其他进程上下文中响应中断.
 * 因为每个进程都在自己的 TSS 段中保存了标志寄存器 EFLAGS 的值, 所以在进程切换时
 * CPU 中当前 EFLAGS 的值也随之改变
 *
 * 使用 sleep_on 进入睡眠状态的进程需要用 wake_up 明确地唤醒
 *
 * @param bh 指定的缓冲块
 */
static inline void wait_on_buffer(struct buffer_head *bh)
{
    cli();

    while (bh->b_lock) {
        sleep_on(&bh->b_wait);
    }

    sti();
}

/**
 * @brief 缓冲区同步到磁盘
 *
 * 这个函数是 sync 系统调用的实现
 *
 * @return int
 */
int sys_sync(void)
{
    int i;
    struct buffer_head *bh;

    /* 首先调用 inode 同步函数, 把内存 inode 表中所有修改过的 inode 写入高速缓冲中 */
    sync_inodes(); /* write out inodes into buffers */

    bh = start_buffer; /* bh 指向缓冲区开始处 */

    for (i = 0; i < NR_BUFFERS; i++, bh++) {
        /* 等 bh 解锁 */
        wait_on_buffer(bh);

        /* 如果缓冲块是脏的, 就把缓冲块写到磁盘里面. 注意这里没有考虑 bh 的
         * b_count 之类的东西, 只要它脏, 就需要 suny 到磁盘设备 */
        if (bh->b_dirt) {
            ll_rw_block(WRITE, bh);
        }
    }

    return 0;
}

/**
 * @brief 对指定设备进行高速缓冲数据与设备上数据的同步操作
 *
 * TODO: 弄清楚为什么需要两个 for 循环??
 * 答: 这里采用两遍同步操作是为了提高内核执行效率
 *     第一遍缓冲区同步操作可以让内核中许多 '脏块' 变干净, 使得 inode 的同步操作能够高效执行.
 *     第二次缓冲区同步操作则把那些由于 inode 同步操作而又变脏的缓冲块与设备中数据同步
 *
 * @param dev 设备号
 * @return int
 */
int sync_dev(int dev)
{
    int i;
    struct buffer_head *bh;

    bh = start_buffer; /* bh 指向缓冲区开始处 */

    /* 搜索高速缓冲区中所有缓冲块, 对于指定设备 dev 的缓冲块
     * 若其数据已被修改过就写入盘中(同步操作) */
    for (i = 0; i < NR_BUFFERS; i++, bh++) {
        if (bh->b_dev != dev) {
            continue;
        }

        wait_on_buffer(bh);
        if (bh->b_dev == dev && bh->b_dirt) {
            ll_rw_block(WRITE, bh); /* 写操作会清楚 b_dirt */
        }
    }

    sync_inodes(); /* 然后把内存中 inode 表数据写入高速缓冲中 */
    bh = start_buffer;

    /* 再对指定设备 dev 执行一次与上述相同的写盘操作 */
    for (i = 0; i < NR_BUFFERS; i++, bh++) {
        if (bh->b_dev != dev) {
            continue;
        }

        wait_on_buffer(bh);
        if (bh->b_dev == dev && bh->b_dirt) {
            ll_rw_block(WRITE, bh);
        }
    }

    return 0;
}

/**
 * @brief 使指定设备在高速缓冲区中的数据无效
 *
 * 扫描高速缓冲区中所有缓冲块, 对指定设备的缓冲块复位其有效(更新)标志和已修改标志
 *
 * @param dev
 */
void invalidate_buffers(int dev)
{
    int i;
    struct buffer_head *bh;

    bh = start_buffer;
    for (i = 0; i < NR_BUFFERS; i++, bh++) {
        if (bh->b_dev != dev) {
            continue;
        }

        wait_on_buffer(bh);
        if (bh->b_dev == dev) {
            bh->b_uptodate = bh->b_dirt = 0;
        }
    }
}

/**
 * @brief 检查磁盘是否更换, 如果已更换就使对应高速缓冲区无效
 *
 * This routine checks whether a floppy has been changed, and
 * invalidates all buffer-cache-entries in that case. This
 * is a relatively slow routine, so we have to try to minimize using
 * it. Thus it is called only upon a 'mount' or 'open'. This
 * is the best way of combining speed and utility, I think.
 * People changing diskettes in the middle of an operation deserve
 * to loose :-)
 *
 * NOTE! Although currently this is only for floppies, the idea is
 * that any additional removable block-device will use this routine,
 * and that mount/open needn't know that floppies/whatever are
 * special.
 *
 * @param dev 设备号
 */
void check_disk_change(int dev)
{
    int i;

    if (MAJOR(dev) != 2) {
        return;
    }

    /* 如果没有更换, 直接退出函数执行 */
    if (!floppy_change(dev & 0x03)) {
        return;
    }

    /* 软盘已经更换, 所以释放对应设备的 inode 位图和逻辑块位图所占的高速缓冲区,
     * 并使该设备的 inode 和数据块信息所占踞的高速缓冲块无效 */
    for (i = 0; i < NR_SUPER; i++) {
        if (super_block[i].s_dev == dev) {
            put_super(super_block[i].s_dev); /* 这里释放对应的超级块 */
        }
    }

    invalidate_inodes(dev);
    invalidate_buffers(dev);
}

/**
 * @brief 哈希函数
 *
 * 用 dev 异或上 block 编号, 然后对 NR_HASH 取余
 *
 * 建立 hash 函数的指导条件主要是尽量确保散列到任何数组项的概率基本相等, 建立函数
 * 的方法有多种, 这里 Linux 0.12 主要采用了关键字除留余数法, 因为我们寻找的缓冲块
 * 有两个条件, 即设备号 dev 和缓冲块号 block, 因此设计的 hash 函数肯定需要包含这
 * 两个关键值, 这里两个关键字的异或操作只是计算关键值的一种方法, 再对关键值进行 MOD
 * 运算就可以保证函数所计算得到的值都处于函数数组项范围内
 *
 * TODO: 这个哈希函数设计有没有什么讲究(更多考虑??) ?
 */
#define _hashfn(dev, block) (((unsigned)(dev ^ block)) % NR_HASH)
#define hash(dev, block) hash_table[_hashfn(dev, block)]

/**
 * @brief 从 hash 队列和空闲缓冲队列中移走缓冲块
 *
 * hash 队列是双向链表结构, 空闲缓冲块队列是双向循环链表结构
 *
 * @param bh
 */
static inline void remove_from_queues(struct buffer_head *bh)
{
    /* remove from hash-queue */
    if (bh->b_next) {
        bh->b_next->b_prev = bh->b_prev;
    }

    if (bh->b_prev) {
        bh->b_prev->b_next = bh->b_next;
    }

    /* 如果 bh 是哈希桶里面的头结点, 需要从桶里面挪走, 桶里放置下一个元素 */
    if (hash(bh->b_dev, bh->b_blocknr) == bh) {
        hash(bh->b_dev, bh->b_blocknr) = bh->b_next;
    }

    /* remove from free list */

    /* free_list 是循环双向链表, 出现前 free 或者后 free,
     * 说明链表结构被破坏了, 这时候要 panic */
    if (!(bh->b_prev_free) || !(bh->b_next_free)) {
        panic("Free block list corrupted");
    }

    bh->b_prev_free->b_next_free = bh->b_next_free;
    bh->b_next_free->b_prev_free = bh->b_prev_free;
    if (free_list == bh) {
        free_list = bh->b_next_free;
    }
}

/**
 * @brief 插入缓冲块
 *
 * 缓冲块一定会插入 free_list, 当缓冲块有关联设备号的时候, 也会插入对应的哈希桶
 *
 * @param bh
 */
static inline void insert_into_queues(struct buffer_head *bh)
{
    /* put at end of free list */
    bh->b_next_free = free_list;
    bh->b_prev_free = free_list->b_prev_free;
    free_list->b_prev_free->b_next_free = bh;
    free_list->b_prev_free = bh;

    /* put the buffer in new hash-queue if it has a device */
    bh->b_prev = NULL;
    bh->b_next = NULL;
    if (!bh->b_dev) {
        return;
    }

    /* 放在哈希桶头部位置 */
    bh->b_next = hash(bh->b_dev, bh->b_blocknr);
    hash(bh->b_dev, bh->b_blocknr) = bh;
    bh->b_next->b_prev = bh;
}

/**
 * @brief 寻找 (dev, buffer) 对应的那个缓存区快
 *
 * @param dev 设备号
 * @param block 区块号
 * @return struct buffer_head* 返回对应的缓冲区块或者 NULL
 */
static struct buffer_head *find_buffer(int dev, int block)
{
    struct buffer_head *tmp;

    for (tmp = hash(dev, block); tmp != NULL; tmp = tmp->b_next) {
        if (tmp->b_dev == dev && tmp->b_blocknr == block) {
            return tmp;
        }
    }

    return NULL;
}

/**
 * @brief 高速缓冲区中寻找指定的缓冲块
 *
 * 若找到则对该缓冲块上锁并返回块头指针
 *
 * Why like this, I hear you say... The reason is race-conditions.
 * As we don't lock buffers (unless we are readint them, that is),
 * something might happen to it while we sleep (ie a read-error
 * will force it bad). This shouldn't really happen currently, but
 * the code is ready.
 *
 * 代码为什么会是这样子的？我听见你问... 原因是竞争条件. 由于我们没有对
 * 缓冲块上锁(除非我们正在读取它们中的数据), 那么当我们(进程)睡眠时
 * 缓冲块可能会发生一些问题(例如一个读错误将导致该缓冲块出错). 目前
 * 这种情况实际上是不会发生的, 但处理的代码已经准备好了
 *
 * @param dev
 * @param block
 * @return struct buffer_head*
 */
struct buffer_head *get_hash_table(int dev, int block)
{
    struct buffer_head *bh;

    for (;;) {
        if (!(bh = find_buffer(dev, block))) {
            return NULL;
        }

        /* 对该缓冲块增加引用计数, 并等待该缓冲块解锁(如果已被上锁)
         * 由于经过了睡眠状态, 因此有必要再验证该缓冲块的正确性, 并返回缓冲块头指针 */
        bh->b_count++;
        wait_on_buffer(bh);
        if (bh->b_dev == dev && bh->b_blocknr == block) {
            return bh;
        }

        /* 如果在睡眠时该缓冲块所属的设备号或块号发生了改变, 则撤消对它的引用计数, 重新寻找 */
        bh->b_count--;
    }
}

/* 下面宏用于同时判断缓冲区的修改标志和锁定标志, 并且定义修改标志的权重要比锁定标志大 */
#define BADNESS(bh) (((bh)->b_dirt << 1) + (bh)->b_lock)

/**
 * @brief 取高速缓冲中指定的缓冲块
 *
 * 检查指定(设备号和块号)的缓冲区是否已经在高速缓冲中
 *
 * 如果指定块已经在高速缓冲中, 则返回对应缓冲区头指针退出;
 * 如果不在, 就需要在高速缓冲中设置一个对应设备号和块号的新项. 返回相应缓冲区头指针.
 *
 * Ok, this is getblk, and it isn't very clear, again to hinder
 * race-conditions. Most of the code is seldom used, (ie repeating),
 * so it should be much more efficient than it looks.
 *
 * The algoritm is changed: hopefully better, and an elusive bug removed.
 *
 * OK, 下面是getblk函数, 该函数的逻辑并不是很清晰, 同样也是因为要考虑
 * 竞争条件问题. 其中大部分代码很少用到(例如重复操作语句), 因此它应该
 * 比看上去的样子有效得多.
 *
 * 算法已经作了改变: 希望能更好, 而且一个难以琢磨的错误已经去除
 *
 * @param dev
 * @param block
 * @return struct buffer_head*
 */
struct buffer_head *getblk(int dev, int block)
{
    struct buffer_head *tmp, *bh;

repeat:
    if ((bh = get_hash_table(dev, block))) {
        return bh;
    }

    tmp = free_list;
    do {

        if (tmp->b_count) {
            /* 该缓冲区正被使用(引用计数不等于0), 继续扫描下一项 */
            continue;
        }

        /* 对于 b_count=0 的块, 即高速缓冲中当前没有引用的块不一定就是干净的(b_dirt=0)或
         * 没有锁定的(b_lock=0)
         *
         * TODO: 在梳理一遍 b_lock, b_count 的设定和取消, 在熟悉下磁盘读写流程
         *
         * 例如当一个任务改写过一块内容后就释放了, 于是该块 b_count=0, 但 b_lock=1
         * 再比如一个任务执行 breada 预读几个块时, 只要 ll_rw_block 命令发出后, 它就会递减
         * b_count, 但此时实际上硬盘访问操作可能还在进行, 因此此时 b_lock=1, 但 b_count=0
         *
         * 如果缓冲头指针 bh 为空, 或者 tmp 所指缓冲头的标志(修改、锁定)权重小于 bh 头标志的权
         * 重, 则让 bh 指向 tmp 缓冲块.  如果该 tmp 缓冲块头表明缓冲块既没有修改也没有锁定标
         * 志置位, 则说明已为指定设备上的块取得对应的高速缓冲块, 则退出循环. 否则我们就继续
         * 执行本循环, 看看能否找到一个 BADNESS 最小的缓冲快
         *
         * 参考下面的梳理
         *
         * BADNESS = ${DIRT}_${LOCK}
         *
         * 0_0 < 0_0 = TRUE
         * 0_0 < 0_1 = TRUE
         * 0_0 < 1_0 = TRUE
         * 0_0 < 1_1 = TRUE
         *
         * 上面四种情况, 说明 tmp 是一个完全空闲的快, 使用它没有任何问题
         * 下面的组合(tmp.lock == 1 && bh.dirty == 1)也可能让 tmp 当做 bh 使用
         *
         * ----------------------------
         * 01 < 10 = TRUE
         * 01 < 11 = TRUE
         * ----------------------------
         *
         * 这里的原因是, bh 的 dirty 位置位, 相比 lock 置位被覆盖要严重(dirty置位被覆盖
         * 会丢数据), 因此会优先使用 lock 被置位的数据块, 最坏的情况, 只要在后面等它被 release 就行了
         */
        if (!bh || BADNESS(tmp) < BADNESS(bh)) {
            bh = tmp;
            if (!BADNESS(tmp)) {
                break; /* 要求 tmp 必须是 dirty=0 && lock=0 */
            }
        }
        /* and repeat until we find something good */
    } while ((tmp = tmp->b_next_free) != free_list);

    if (!bh) {
        /* 没找到空闲块 */
        sleep_on(&buffer_wait);
        goto repeat;
    }

    wait_on_buffer(bh); /* 等区块解锁 */

    /* 等的过程中可能有会出现区块被占用的情况, 这时候再回去重新找 */
    if (bh->b_count) {
        goto repeat;
    }

    /* 等的过程中可能有会出现缓冲区被修改的情况, 这时候也要再回去重新找 */
    while (bh->b_dirt) {
        sync_dev(bh->b_dev);
        wait_on_buffer(bh);
        if (bh->b_count) {
            goto repeat;
        }
    }

    /* NOTE!! While we slept waiting for this block, somebody else might
     * already have added "this" block to the cache. check it
     * 这里也是因为 sleep 被占用的情况 */
    if (find_buffer(dev, block)) {
        goto repeat;
    }

    /* OK, FINALLY we know that this buffer is the only one of it's kind,
     * and that it's unused (b_count=0), unlocked (b_lock=0), and clean */
    bh->b_count = 1;
    bh->b_dirt = 0;
    bh->b_uptodate = 0;
    remove_from_queues(bh);
    bh->b_dev = dev;
    bh->b_blocknr = block;
    insert_into_queues(bh);
    return bh;
}

/**
 * @brief 释放指定缓冲块
 *
 * 等待该缓冲块解锁
 * 然后引用计数递减 1, 并明确地唤醒等待空闲缓冲块的进程
 *
 * @param buf
 */
void brelse(struct buffer_head *buf)
{
    if (!buf) {
        return;
    }

    wait_on_buffer(buf);
    if (!(buf->b_count--)) {
        panic("Trying to free free buffer");
    }

    wake_up(&buffer_wait);
}

/**
 * @brief 从设备上读取数据块
 *
 * bread() reads a specified block and returns the buffer that contains
 * it. It returns NULL if the block was unreadable.
 *
 * @param dev
 * @param block
 * @return struct buffer_head*
 */
struct buffer_head *bread(int dev, int block)
{
    struct buffer_head *bh;

    /* 根据设备号 dev 和数据块号 block, 首先在高速缓冲区中申请一块缓冲块,
     * 如果该缓冲块中已经包含有有效的数据就直接返回该缓冲块指针, 否则就从设
     * 备中读取指定的数据块到该缓冲块中并返回缓冲块指针 */
    if (!(bh = getblk(dev, block))) {
        panic("bread: getblk returned NULL\n");
    }

    if (bh->b_uptodate) {
        return bh;
    }

    /* 从设备中读数据 */
    ll_rw_block(READ, bh);

    /* 然后等待指定数据块被读入, 并等待缓冲区解锁 */
    wait_on_buffer(bh);
    if (bh->b_uptodate) {
        return bh;
    }

    /* 这里是睡眠等待的时候, 导致 bh 数据不够新了, 这种情况按照读取失败处理 */
    brelse(bh);
    return NULL;
}

/* 从 from 拷贝 1024B 的数据 到 to */
#define COPYBLK(from, to)                                                                          \
    __asm__("cld\n\t"                                                                              \
            "rep movsl\n\t"                                                                        \
            :                                                                                      \
            : "c"(BLOCK_SIZE / 4), "S"(from), "D"(to))

/**
 * @brief 读设备上一个页面(4个缓冲块)的内容到指定内存地址处
 *
 * 该函数仅用于 mm/memory.c 文件的 do_no_page 函数中
 *
 * bread_page reads four buffers into memory at the desired address. It's
 * a function of its own, as there is some speed to be got by reading them
 * all at the same time, not waiting for one to be read, and then another
 * etc.
 *
 * bread_page 一次读四个缓冲块数据读到内存指定的地址处.
 * 它是一个完整的函数, 因为同时读取四块可以获得速度上的好处, 不用等着读一块, 再读一块了.
 *
 * @param address 保存页面数据的地址
 * @param dev 设备号
 * @param b 含有4个设备数据块号的数组
 */
void bread_page(unsigned long address, int dev, int b[4])
{
    struct buffer_head *bh[4];
    int i;

    /* 发起读磁盘请求 */
    for (i = 0; i < 4; i++) {
        if (b[i]) {
            if ((bh[i] = getblk(dev, b[i]))) {
                if (!bh[i]->b_uptodate) {
                    ll_rw_block(READ, bh[i]);
                }
            }
        } else {
            bh[i] = NULL;
        }
    }

    /* 等数据读取完毕, 然后拷贝到目标地址 */
    for (i = 0; i < 4; i++, address += BLOCK_SIZE) {
        if (bh[i]) {
            /* 这里对睡眠的处理就比较粗糙了, 没有考虑睡眠的时候被修改之后的情况 */
            wait_on_buffer(bh[i]);
            if (bh[i]->b_uptodate) {
                COPYBLK((unsigned long)bh[i]->b_data, address);
            }

            brelse(bh[i]);
        }
    }
}

/**
 * @brief 从指定设备读取指定的一些块
 *
 * Ok, breada can be used as bread, but additionally to mark other
 * blocks for reading as well. End the argument list with a negative
 * number.
 *
 * OK, breada 可以象 bread 一样使用, 但会另外预读一些块
 * 该函数参数列表需要使用一个负数来表明参数列表的结束
 *
 * @param dev 设备号
 * @param first 第一个指定的块号
 * @param ... 后续指定的块号
 * @return struct buffer_head* 成功时返回第1块的缓冲块头指针, 否则返回NULL
 */
struct buffer_head *breada(int dev, int first, ...)
{
    va_list args;
    struct buffer_head *bh, *tmp;

    va_start(args, first);
    if (!(bh = getblk(dev, first))) {
        panic("bread: getblk returned NULL\n");
    }

    if (!bh->b_uptodate) {
        ll_rw_block(READ, bh);
    }

    while ((first = va_arg(args, int)) >= 0) {
        tmp = getblk(dev, first);
        if (tmp) {
            if (!tmp->b_uptodate) {
                ll_rw_block(READA, bh); // BUG: ??? --- bh 应该是 tmp
            }

            tmp->b_count--; /* 这里直接释放掉预读的块, 后面不配合 brelse 了 */
        }
    }

    va_end(args);

    /* 我们只关心低一个区块的完成情况, 其他都是预读取, 不需要等他们读完 */
    wait_on_buffer(bh);
    if (bh->b_uptodate) {
        return bh;
    }

    brelse(bh);
    return (NULL);
}

/**
 * @brief 初始化块缓冲区
 *
 * 在 init.c#main 里面, 对于内存容量具有:
 *  - >= 12MB: 缓冲区末端被设置为 4MB
 *  - >=  6MB: 缓冲区末端被设置为 2MB
 *  - <   6MB: 缓冲区末端被设置为 1MB
 *
 * 该函数从缓冲区开始位置 start_buffer 处和缓冲区末端 buffer_end 处分别同时
 * 设置(初始化)缓冲块头结构和对应的数据块, 直到缓冲区中所有内存被分配完毕
 *
 * @param buffer_end 块缓冲区结束位置
 */
void buffer_init(long buffer_end)
{
    struct buffer_head *h = start_buffer;
    void *b;
    int i;

    /* 首先根据参数提供的缓冲区高端位置确定实际缓冲区高端位置 b. 如果缓冲区高端等于 1MB,
     * 则因为从 640KB - 1MB 被显示内存和 BIOS 占用, 所以实际可用缓冲区内存高端位置应该是
     * 640KB. 否则缓冲区内存高端一定大于1MB.
     *
     * TODO-DONE: 高端大于 1M 的情形, 是怎么规避 显存和 BIOS 被擦写的?
     * 答: 在下面 while 循环里面有跳过的逻辑
     */
    if (buffer_end == 1 << 20) {
        b = (void *)(640 * 1024);
    } else {
        b = (void *)buffer_end;
    }

    /* 循环设置空闲缓冲块链表, 这里对内存的使用是这样的:
     *  1. 开始部分的内存区域, 被用来作为 buffer_header 结构体了
     *  2. 末尾的部分内存区域, 作为真正的缓存区域使用, 每块是 BLOCK_SIZE 大小
     *  3. 1 里面的 buffer_header 里面有的 b_data 指针指向 2 里面的缓存区域
     *  4. 当出现 1 和 2 内存区域有交叠的时候, buffer_memory 就用完了 */
    while ((b -= BLOCK_SIZE) >= ((void *)(h + 1))) {
        h->b_dev = 0;
        h->b_dirt = 0;
        h->b_count = 0;
        h->b_lock = 0;
        h->b_uptodate = 0;
        h->b_wait = NULL;
        h->b_next = NULL;
        h->b_prev = NULL;
        h->b_data = (char *)b;
        h->b_prev_free = h - 1;
        h->b_next_free = h + 1;
        h++;
        NR_BUFFERS++;

        /* 若 b 递减到等于 1MB 的位置的时候, 跳过 BIOS 和显存
         * 使用的 384KB 空间, 直接重新从 640KB 处重新继续 */
        if (b == (void *)0x100000) {
            b = (void *)0xA0000;
        }
    }

    h--;                        /* 循环中, 最后一个 h 没用到 */
    free_list = start_buffer;   /* 登记 free_list 开始位置 */
    free_list->b_prev_free = h; /* 把 free_list 弄成循环链表 */
    h->b_next_free = free_list;

    /* 哈希桶里面初始化成空值 */
    for (i = 0; i < NR_HASH; i++) {
        hash_table[i] = NULL;
    }
}
