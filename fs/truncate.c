/*
 *  linux/fs/truncate.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/sched.h>

#include <sys/stat.h>

/**
 * @brief 释放一次间接块
 *
 * @param dev 设备号
 * @param block 一次间接块所在的设备逻辑区块号
 * @return int
 */
static int free_ind(int dev, int block)
{
    struct buffer_head *bh;
    unsigned short *p;
    int i;
    int block_busy;

    if (!block) {
        return 1;
    }

    /* 将一次间接块 block 读入内存 */
    block_busy = 0;
    if ((bh = bread(dev, block))) {
        p = (unsigned short *)bh->b_data;

        /* 一次间接块里面可以容纳 512 个 block 信息 */
        for (i = 0; i < 512; i++, p++) {
            if (*p) {
                if (free_block(dev, *p)) {
                    *p = 0;
                    bh->b_dirt = 1;
                } else {
                    block_busy = 1;
                }
            }
        }

        brelse(bh);
    }

    if (block_busy) {
        return 0;
    } else {
        return free_block(dev, block);
    }
}

/**
 * @brief 释放二次间接块
 *
 * @param dev 设备号
 * @param block 二次间接块所在的设备逻辑区块号
 * @return int
 */
static int free_dind(int dev, int block)
{
    struct buffer_head *bh;
    unsigned short *p;
    int i;
    int block_busy;

    if (!block) {
        return 1;
    }

    block_busy = 0;
    if ((bh = bread(dev, block))) {
        p = (unsigned short *)bh->b_data;
        for (i = 0; i < 512; i++, p++) {
            if (*p) {
                if (free_ind(dev, *p)) {
                    *p = 0;
                    bh->b_dirt = 1;
                } else {
                    block_busy = 1;
                }
            }
        }

        brelse(bh);
    }

    if (block_busy) {
        return 0;
    } else {
        return free_block(dev, block);
    }
}

/**
 * @brief 将文件内容清空
 *
 * @param inode
 */
void truncate(struct m_inode *inode)
{
    int i;
    int block_busy;

    /* 只有常规文件, 目录, 符号链接文件可以做清空处理
     * TODO-DONE: 目录也可以???
     * 答: 从代码层面看, 是可以的. 目录是文件内容为 directory entry 的特殊的文件
     *     PS: 在现代的 linux 上试着用 truncate 命令清空目录是不允许的, 可以在探究
     *         一下现代 linux 在 truncate 方面的处理逻辑 */
    if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode))) {
        return;
    }

repeat:
    block_busy = 0;

    /* 先释放前面 7 个常规数据块 */
    for (i = 0; i < 7; i++) {
        if (inode->i_zone[i]) {
            if (free_block(inode->i_dev, inode->i_zone[i])) {
                inode->i_zone[i] = 0;
            } else {
                block_busy = 1;
            }
        }
    }

    /* 再释放一次间接块 */
    if (free_ind(inode->i_dev, inode->i_zone[7])) {
        inode->i_zone[7] = 0;
    } else {
        block_busy = 1;
    }

    /* 释放二次间接块 */
    if (free_dind(inode->i_dev, inode->i_zone[8])) {
        inode->i_zone[8] = 0;
    } else {
        block_busy = 1;
    }

    /* 更新文件 inode */
    inode->i_dirt = 1;

    /* 如果释放 block 有问题, 等一会儿重新再试
     * 这里将当前进程执行时间清零, 然后调度执行其他程序 */
    if (block_busy) {
        current->counter = 0;
        schedule();
        goto repeat;
    }

    /* 注意看 ctime 也被重置了 */
    inode->i_size = 0;
    inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}
