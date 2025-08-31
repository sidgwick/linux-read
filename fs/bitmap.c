/*
 *  linux/fs/bitmap.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* bitmap.c contains the code that handles the inode and block bitmaps */
#include <string.h>

#include <linux/kernel.h>
#include <linux/sched.h>

/**
 * @brief 清除区块内容
 */
#define clear_block(addr)                                                                          \
    __asm__("cld\n\t"                                                                              \
            "rep stosl"                                                                            \
            :                                                                                      \
            : "a"(0), "c"(BLOCK_SIZE / 4), "D"((long)(addr))                                       \
            : "cx", "di")

/**
 * @brief 置 addr 的 nr 比特位为 1, 并返回原始比特位值
 */
#define set_bit(nr, addr)                                                                          \
    ({                                                                                             \
        register int res __asm__("ax");                                                            \
        __asm__ __volatile__("btsl %2, %3\n\t"                                                     \
                             "setb %%al"                                                           \
                             : "=a"(res)                                                           \
                             : "0"(0), "r"(nr), "m"(*(addr)));                                     \
        res;                                                                                       \
    })

/**
 * @brief 置 addr 的 nr 比特位为 0, 并返回原始比特位值
 */
#define clear_bit(nr, addr)                                                                        \
    ({                                                                                             \
        register int res __asm__("ax");                                                            \
        __asm__ __volatile__("btrl %2, %3\n\t"                                                     \
                             "setnb %%al"                                                          \
                             : "=a"(res)                                                           \
                             : "0"(0), "r"(nr), "m"(*(addr)));                                     \
        res;                                                                                       \
    })

/**
 * @brief 在 addr 指定的位图里面, 查找第一个 0 比特位并返回它的位置
 *
 * 扫描寻找的范围是 1024 字节(8192 比特位)
 *
 * TODO-DONE: 这里为啥不直接用 bt 指令?
 * 答: bt 指令需要指定索引, 实际上我们在这里是要寻找这个索引, 因此 bt 不合适
 */
#define find_first_zero(addr)                                                                      \
    ({                                                                                             \
        int __res;                                                                                 \
        __asm__(                                                                                   \
            "cld\n\t"                                                                              \
            "1:"                                                                                   \
            "lodsl\n\t"             /* ds:esi -> eax, esi+=4 */                                    \
            "notl %%eax\n\t"        /* eax 取反 */                                                 \
            "bsfl %%eax, %%edx\n\t" /* 查找 eax 里面首个 1bit, 位置记录在 edx, 如未找到 ZF=1  */   \
            "je 2f\n\t"             /* 未找到的情况 ZF=1, 跳转到 2f, 继续找下一个双字 */           \
            "addl %%edx, %%ecx\n\t" /* 找到了, 将相对于 addr 开始的比特位索引, 记录在 ecx 里面 */  \
            "jmp 3f\n"                                                                             \
            "2:"                                                                                   \
            "addl $32, %%ecx\n\t"   /* 装载下一个双字 */                                           \
            "cmpl $8192, %%ecx\n\t" /* 位图最长是 8192 个 */                                       \
            "jl 1b\n"                                                                              \
            "3:"                                                                                   \
            : "=c"(__res)                                                                          \
            : "c"(0), "S"(addr)                                                                    \
            : "ax", "dx", "si");                                                                   \
        __res;                                                                                     \
    })

/**
 * @brief 释放 dev 上数据区中的逻辑块 block
 *
 * 复位指定逻辑块block对应的逻辑块位图比特位
 *
 * @param dev 设备号
 * @param block 设备逻辑区块编号
 * @return int 成功返回1, 失败返回0
 */
int free_block(int dev, int block)
{
    struct super_block *sb;
    struct buffer_head *bh;

    /* 先找到超级块 */
    if (!(sb = get_super(dev))) {
        panic("trying to free block on nonexistent device");
    }

    /* 确保参数是合法的数据区块 */
    if (block < sb->s_firstdatazone || block >= sb->s_nzones) {
        panic("trying to free block not in datazone");
    }

    /* 如果该逻辑块目前存在于高速缓冲区中, 尝试释放对应的缓冲块 */
    bh = get_hash_table(dev, block);
    if (bh) {
        /* 如果区块也在被别人使用, 只释放自己(get_hash_table)这次引用就行 */
        if (bh->b_count > 1) {
            brelse(bh);
            return 0;
        }

        /* 否则把缓冲区失效 */
        bh->b_dirt = 0;
        bh->b_uptodate = 0;

        /* 释放自己(get_hash_table)引用 */
        if (bh->b_count) {
            brelse(bh);
        }
    }

    // 接着我们复位block在逻辑块位图中的比特位 (置0) . 先计算block在数据区开始算起的
    // 数据逻辑块号 (从1开始计数) . 然后对逻辑块(区块)位图进行操作, 复位对应的比特位.
    // 如果对应比特位原来就是0, 则出错停机. 由于1个缓冲块有1024字节, 即8192比特位,
    // 因此 block/8192 即可计算出指定块 block 在逻辑位图中的哪个块上. 而 block&8191 可
    // 以得到block在逻辑块位图当前块中的比特偏移位置.

    /* 计算数据区块索引(从 0 开始计数, 但是 0 不用)
     * TODO: 确认清楚 inode/zone 位图, 区块之间的计数关系
     * NOTICE: linux 系统中, 约定第 0 个 inode, 第 0 个 zone, 都不使用 */
    block -= sb->s_firstdatazone - 1;
    if (clear_bit(block & 8191, sb->s_zmap[block / 8192]->b_data)) {
        printk("block (%04x:%d) ", dev, block + sb->s_firstdatazone - 1);
        printk("free_block: bit already cleared\n");
    }

    /* zmap 对应的那个 bh 被标记为脏 */
    sb->s_zmap[block / 8192]->b_dirt = 1;
    return 1;
}

/**
 * @brief 在 dev 上寻找一个空闲的数据区块, 并返回区块号
 *
 * @param dev 要搜寻的设备
 * @return int
 */
int new_block(int dev)
{
    struct buffer_head *bh;
    struct super_block *sb;
    int i, j;

    if (!(sb = get_super(dev))) {
        panic("trying to get new block from nonexistant device");
    }

    j = 8192; /* 默认给 j 一个非法值 */

    /* 扫描 zmap, 找一个空闲的 zone */
    for (i = 0; i < 8; i++) {
        if (bh = sb->s_zmap[i]) {
            if ((j = find_first_zero(bh->b_data)) < 8192) {
                break;
            }
        }
    }

    /* 检查结果是不是合法, 最多有 8 个位图区块, 每个区块最多是 8192 个 bit */
    if (i >= 8 || !bh || j >= 8192) {
        return 0;
    }

    /* 位图比特位置位 */
    if (set_bit(j, bh->b_data)) {
        panic("new_block: bit already set");
    }

    /* 位图对应的缓存快标记为脏
     * TODO-DONE: 是不是标记的太早了一点?
     * 答: 不早, 因为下面 bh 被当做数据区块缓冲区了 */
    bh->b_dirt = 1;

    /* 计算这个空闲区块对应的设备逻辑区块号 */
    j += i * 8192 + sb->s_firstdatazone - 1;
    if (j >= sb->s_nzones) {
        return 0;
    }

    /* 取得数据区块缓冲块 */
    if (!(bh = getblk(dev, j))) {
        panic("new_block: cannot get block");
    }

    /* 因为这个区块是一个全新的区块
     * 它的引用数只可能在 getblk 里面设置一次, 因此一定是 1 */
    if (bh->b_count != 1) {
        panic("new block: count is != 1");
    }

    /* 数据区置 0 */
    clear_block(bh->b_data);
    bh->b_uptodate = 1;
    bh->b_dirt = 1;
    brelse(bh);

    return j;
}

/**
 * @brief 释放 inode
 *
 * @param inode
 */
void free_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;

    if (!inode) {
        return;
    }

    /* inode 没有设备号(pipe或者没有使用), 清零 inode 数据, 返回即可
     * TODO: 确认是不是 pipe 的 dev == 0? */
    if (!inode->i_dev) {
        memset(inode, 0, sizeof(*inode));
        return;
    }

    /* 还有其他人一起用, 不能释放 */
    if (inode->i_count > 1) {
        printk("trying to free inode with count=%d\n", inode->i_count);
        panic("free_inode");
    }

    /* 未删除 - 引用的目录数量不为 0, 也不能释放
     * NOTICE: PS: 这种情况应该使用 iput 函数放回 inode */
    if (inode->i_nlinks) {
        panic("trying to free inode with links");
    }

    if (!(sb = get_super(inode->i_dev))) {
        panic("trying to free inode on nonexistent device");
    }

    /* 检查 inode 合法不合法
     * NOTICE: 注意看这里, inode 编号应该是 1 <= NR <= s_ninodes */
    if (inode->i_num < 1 || inode->i_num > sb->s_ninodes) {
        panic("trying to free inode 0 or nonexistant inode");
    }

    /* 找到 inode 对应的位图缓冲块 */
    if (!(bh = sb->s_imap[inode->i_num >> 13])) {
        panic("nonexistent imap in superblock");
    }

    /* 位图缓冲块对应的位清 0 */
    if (clear_bit(inode->i_num & 8191, bh->b_data)) {
        printk("free_inode: bit already cleared.\n\r");
    }

    /* 位图缓冲块标记为脏, 等待其他位置写磁盘
     * TODO: 在了解一下这里后续的写磁盘是怎么发生的? */
    bh->b_dirt = 1;
    memset(inode, 0, sizeof(*inode));
}

/**
 * @brief 在设备上面分配新的 inode
 *
 * @param dev
 * @return struct m_inode*
 */
struct m_inode *new_inode(int dev)
{
    struct m_inode *inode;
    struct super_block *sb;
    struct buffer_head *bh;
    int i, j;

    /* 先拿到一个空内存级 inode */
    if (!(inode = get_empty_inode())) {
        return NULL;
    }

    /* 取得超级块 */
    if (!(sb = get_super(dev))) {
        panic("new_inode with unknown device");
    }

    /* 在磁盘上找一个空的 inode, 和这个内存 inode 对应起来 */
    j = 8192;
    for (i = 0; i < 8; i++) {
        if (bh = sb->s_imap[i]) {
            if ((j = find_first_zero(bh->b_data)) < 8192) {
                break;
            }
        }
    }

    if (!bh || j >= 8192 || j + i * 8192 > sb->s_ninodes) {
        iput(inode);
        return NULL;
    }

    /* 磁盘 inode 位图置位 */
    if (set_bit(j, bh->b_data)) {
        panic("new_inode: bit already set");
    }

    /* inode 位图缓冲块标记为脏 */
    bh->b_dirt = 1;

    /* inode 标记为脏 */
    inode->i_count = 1;
    inode->i_nlinks = 1;
    inode->i_dev = dev;
    inode->i_uid = current->euid;
    inode->i_gid = current->egid;
    inode->i_dirt = 1;
    inode->i_num = j + i * 8192; /* TODO: 这里看 i_num 是从 0 开始的?? */
    inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
    return inode;
}
