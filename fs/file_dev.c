/*
 *  linux/fs/file_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>

#include <asm/segment.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

/**
 * @brief 文件读函数
 *
 * 根据i节点和文件结构, 读取文件中数据
 *
 * @param inode 文件 inode 结构
 * @param filp 文件对象
 * @param buf 用户控件缓冲区
 * @param count 需要读取的字节数
 * @return int 实际读取的字节数, 出错返回出错号
 */
int file_read(struct m_inode *inode, struct file *filp, char *buf, int count)
{
    int left, chars, nr;
    struct buffer_head *bh;

    if ((left = count) <= 0) {
        return 0;
    }

    /* 剩余未读字节数不为 0, 就持续读 */
    while (left) {
        /* 找到待读取的文件指针相应的逻辑块, 然后吧相应的数据块读取到内存 */
        if ((nr = bmap(inode, (filp->f_pos) / BLOCK_SIZE))) {
            if (!(bh = bread(inode->i_dev, nr))) {
                break;
            }
        } else {
            bh = NULL;
        }

        /* (START, B0).....(B1)....(nr)....(B2).....  */
        nr = filp->f_pos % BLOCK_SIZE;      /* f_pos 指定的位置在某块 block 里面的余量(nr) */
        chars = MIN(BLOCK_SIZE - nr, left); /* 算一下, (nr-B2, left) 谁小, 这是读取的真实有效数据 */
        filp->f_pos += chars;               /* 加上读取的字节数 */
        left -= chars;                      /* 剩余字节数减去读取的字节数 */
        if (bh) {
            /* 块缓冲区已经存在的情况, 直接把快缓冲区数据, 复制到用户缓冲区 */
            char *p = nr + bh->b_data;
            while (chars-- > 0) {
                put_fs_byte(*(p++), buf++);
            }

            brelse(bh);
        } else {
            /* 没有快缓冲区, 读到的数据就全都是 0
             * TODO-DONE: 啥时候没有缓冲区呢?
             * 答: 可以研究上面那个 bmap 调用, 在新文件, 磁盘读取失败的时候, bh 就会是 NULL */
            while (chars-- > 0) {
                put_fs_byte(0, buf++);
            }
        }
    }

    inode->i_atime = CURRENT_TIME;
    return (count - left) ? (count - left) : -ERROR;
}

/**
 * @brief 文件写函数
 *
 * 根据i节点和文件结构信息, 将用户数据写入文件中
 *
 * @param inode 文件 inode
 * @param filp 文件指针
 * @param buf 用户态缓冲区
 * @param count 要写入的字节数
 * @return int 实际写入的字节数, 或出错号(小于0)
 */
int file_write(struct m_inode *inode, struct file *filp, char *buf, int count)
{
    off_t pos;
    int block, c;
    struct buffer_head *bh;
    char *p;
    int i = 0;

    /*
     * ok, append may not work when many processes are writing at the same time
     * but so what. That way leads to madness anyway.
     */
    if (filp->f_flags & O_APPEND) {
        pos = inode->i_size; /* flags 设置了 append 标记, 写操作从文件末尾开始 */
    } else {
        pos = filp->f_pos;
    }

    while (i < count) {
        /* 创建文件在 pos 处对应的数据区块 */
        if (!(block = create_block(inode, pos / BLOCK_SIZE))) {
            break;
        }

        /* 将对应的区块, 读入到缓冲区 */
        if (!(bh = bread(inode->i_dev, block))) {
            break;
        }

        c = pos % BLOCK_SIZE;
        p = c + bh->b_data; /* 写文件的真实位置 */
        bh->b_dirt = 1;     /* 缓冲块置脏位 */

        /* 计算写入的字节数 */
        c = BLOCK_SIZE - c;
        if (c > count - i) {
            c = count - i;
        }

        pos += c; /* 调整文件游标位置 */
        if (pos > inode->i_size) {
            inode->i_size = pos; /* 如果文件长度变长了, inode 也需要更新 */
            inode->i_dirt = 1;
        }

        i += c; /* 已读取字节数 */
        while (c-- > 0) {
            *(p++) = get_fs_byte(buf++);
        }

        brelse(bh);
    }

    inode->i_mtime = CURRENT_TIME;

    /* 不是 append 模式, 一律视作新建了文件 */
    if (!(filp->f_flags & O_APPEND)) {
        filp->f_pos = pos;
        inode->i_ctime = CURRENT_TIME;
    }

    return (i ? i : -1);
}
