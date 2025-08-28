/*
 *  linux/fs/block_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <asm/segment.h>
#include <asm/system.h>
#include <linux/kernel.h>
#include <linux/sched.h>

extern int *blk_size[];

/**
 * @brief 向指定设备从给定偏移处写入指定长度数据
 *
 * 对于内核来说, 写操作是向高速缓冲区中写入数据. 什么时候数据最终写入设备是由高速缓
 * 冲管理程序决定并处理的.
 *
 * 另外, 因为块设备是以块为单位进行读写, 因此对于写开始位置不处于块起始处时, 需要先
 * 将开始字节所在的整个块读出, 然后将需要写的数据从写开始处填写满该块, 再将完整的一块
 * 数据写盘(即交由高速缓冲程序去处理).
 *
 * @param dev 设备号
 * @param pos 要写入的位置(单位字节)
 * @param buf 数据缓冲区
 * @param count 要传送的字节数
 * @return int 返回已写入的字节数, 若没有写入任何字节或出错, 则返回出错号
 */
int block_write(int dev, long *pos, char *buf, int count)
{
    int block = *pos >> BLOCK_SIZE_BITS;  /* pos 所在文件数据块号 */
    int offset = *pos & (BLOCK_SIZE - 1); /* pos 在数据块中偏移值 */
    int chars;
    int written = 0;
    int size; /* 设备最多可用的区块数量 */
    struct buffer_head *bh;
    register char *p;

    if (blk_size[MAJOR(dev)]) {
        /* 设定了 blk_size, 直接使用设定值 */
        size = blk_size[MAJOR(dev)][MINOR(dev)];
    } else {
        /* 没有设定 blk_size, 给一个默认值, 此值大小为大约 2G */
        size = 0x7fffffff;
    }

    while (count > 0) {
        /* 目标区块不存在 */
        if (block >= size) {
            return written ? written : -EIO;
        }

        /* 从 offset 开始, 到当前区块结束, 有多少数据 */
        chars = BLOCK_SIZE - offset;

        /* 如果上面计算出的当前区块数据, 大于我们要写入的 count 字节,
         * 那意味着只写这一个区块, 就能完成写入任务
         * 这时候把代表当前区块写如数量的 chars 变量, 赋值为 count */
        if (chars > count) {
            chars = count;
        }

        /* 正好要写 1 块数据内容, 则直接申请 1 块高速缓冲块, 并把用户数据放入即可
         * 否则就需要读入将被写入部分数据的数据块, 并预读下两块数据
         *
         * TODO-DONE: 为啥要预读后面的数据?
         * 答: 这里的原因是, getblk 只是找一块缓冲区, 它实际上并不读取块设备.
         *     但是 breada 会读取, 既然读取了, 磁盘在顺序读取的情况下速度比较快,
         *     因此这里不管用不用得上, 都多读两块, 可以加速后面可能的读取操作
         */
        if (chars == BLOCK_SIZE) {
            bh = getblk(dev, block);
        } else {
            bh = breada(dev, block, block + 1, block + 2, -1);
        }

        block++; /* 然后将块号递增 1, 为下次操作做好准备 */

        /* 如果缓冲块操作失败, 则返回已写字节数, 如果没有写入任何字节, 则返回出错号(负数) */
        if (!bh) {
            return written ? written : -EIO;
        }

        p = offset + bh->b_data; /* 区块对应的缓冲区 */
        offset = 0;
        *pos += chars;
        written += chars;
        count -= chars;

        /* 移动到 bh 的缓冲区 */
        while (chars-- > 0) {
            *(p++) = get_fs_byte(buf++);
        }

        /* 标记脏块 */
        bh->b_dirt = 1;

        /* bh 的作用是帮我们临时存放从磁盘到用户缓冲区的数据, 现在已经拷贝完毕, 因此 bh 可以直接释放了 */
        brelse(bh);
    }

    return written;
}

/**
 * @brief 数据块读函数
 *
 * 从指定设备和位置处读入指定长度数据到用户缓冲区中
 *
 * @param dev 设备号
 * @param pos 设备文件中偏移量指针
 * @param buf 用户空间中缓冲区地址
 * @param count 要传送的字节数
 * @return int 返回已读入字节数. 若没有读入任何字节或出错, 则返回出错号
 */
int block_read(int dev, unsigned long *pos, char *buf, int count)
{
    int block = *pos >> BLOCK_SIZE_BITS;  /* 逻辑块号 */
    int offset = *pos & (BLOCK_SIZE - 1); /* 块内偏移 */
    int chars;
    int size;
    int read = 0;
    struct buffer_head *bh;
    register char *p;

    if (blk_size[MAJOR(dev)]) {
        size = blk_size[MAJOR(dev)][MINOR(dev)];
    } else {
        size = 0x7fffffff;
    }

    while (count > 0) {
        if (block >= size) {
            return read ? read : -EIO;
        }

        /* 找出本次循环, 需要操作的数据大小 */
        chars = BLOCK_SIZE - offset;
        if (chars > count) {
            chars = count;
        }

        /* 读取的话, 一口气读取 3 块 */
        if (!(bh = breada(dev, block, block + 1, block + 2, -1))) {
            return read ? read : -EIO;
        }

        block++;
        p = offset + bh->b_data; /* 考虑上偏移, 这就是真实数据开始的地方 */
        offset = 0;
        *pos += chars;
        read += chars;
        count -= chars;

        /* 拷贝到 buf */
        while (chars-- > 0) {
            put_fs_byte(*(p++), buf++);
        }

        /* 因为已经拷贝到 buf, 因此这个区块已经用完了, 释放掉 */
        brelse(bh);
    }

    return read;
}
