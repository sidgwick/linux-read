/*
 *  linux/fs/pipe.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <signal.h>
#include <termios.h>

#include <asm/segment.h>
#include <linux/kernel.h>
#include <linux/mm.h> /* for get_free_page */
#include <linux/sched.h>

/**
 * @brief 从管道读取数据
 *
 * @param inode 管道对应的 inode
 * @param buf 用户态缓冲区
 * @param count 要读取的字节数
 * @return int 实际读取的字节数
 */
int read_pipe(struct m_inode *inode, char *buf, int count)
{
    int chars, size, read = 0;

    while (count > 0) {
        /* 判断管道内有没有数据 */
        while (!(size = PIPE_SIZE(*inode))) {
            /* 没有数据, 唤醒写进程 */
            wake_up(&PIPE_WRITE_WAIT(*inode));

            /* TODO: i_count != 2 的话 */
            if (inode->i_count != 2) { /* are there any writers? */
                return read;
            }

            /* 如果进程是因为信号被唤醒的, 返回 `ERESTARTSYS` 错误码 */
            if (current->signal & ~current->blocked) {
                return read ? read : -ERESTARTSYS;
            }

            /*  */
            interruptible_sleep_on(&PIPE_READ_WAIT(*inode));
        }

        /* 管道中有数据, 读取页面中剩余的字符
         * TODO: 弄清楚管道和内存页面有啥关系? 环形缓冲区
         * 答: 管道附加了一张内存页面, 这个内存页面作为环形缓冲区, 用于在管道两端交换数据 */
        chars = PAGE_SIZE - PIPE_TAIL(*inode);
        if (chars > count) {
            chars = count;
        }

        if (chars > size) {
            chars = size;
        }

        count -= chars;                       /* 待读取字节数 */
        read += chars;                        /* 已读字节数 */
        size = PIPE_TAIL(*inode);             /* 记录下来尾指针 */
        PIPE_TAIL(*inode) += chars;           /* 尾指针游标加上相应的数量 */
        PIPE_TAIL(*inode) &= (PAGE_SIZE - 1); /* 约束尾指针游标在一页之内 */

        /* 将 pipe 里面的数据, 复制到用户缓冲区
         *  - 管道 inode 的 i_size 存放的是内存地址页
         *  - chars 的数值保证了 size++ 不会数组越界 */
        while (chars-- > 0) {
            put_fs_byte(((char *)inode->i_size)[size++], buf++);
        }
    }

    /* 唤醒写进程 */
    wake_up(&PIPE_WRITE_WAIT(*inode));
    return read;
}

/**
 * @brief 向管道写入数据
 *
 * @param inode
 * @param buf
 * @param count
 * @return int
 */
int write_pipe(struct m_inode *inode, char *buf, int count)
{
    int chars, size, written = 0;

    while (count > 0) {
        /* 判断管道是否已满
         * TODO: 是否没有考虑游标回绕的情况?
         * 答: 似乎不需要考虑, 因为:
         *  1. 后面的写操作, 确保了每次写, 最多写到页面结尾位置就停了, head=0, tail=X
         *  2. 再次循环 `size = (PAGE_SIZE - 1) - PIPE_SIZE(*inode)` 这时候得到的计算结果,
         *     非常神奇的得到了 (PAGE_SIZE - X), 于是这次写, 还是不会越界
         *  3. 不是很理解 2 里面的数学过程, 但是验算出来确实没问题, 后续可以分析一下 `&(PS-1)` 的魔力 */
        while (!(size = (PAGE_SIZE - 1) - PIPE_SIZE(*inode))) {
            /* 管道满, 唤醒读进程 */
            wake_up(&PIPE_READ_WAIT(*inode));

            /* 没有读者, 发送 SIGPIPE 信号给进程 */
            if (inode->i_count != 2) { /* no readers */
                current->signal |= (1 << (SIGPIPE - 1));
                return written ? written : -1;
            }

            /* 写进程等待 */
            sleep_on(&PIPE_WRITE_WAIT(*inode));
        }

        /* 从 head 到页面结束, 都是可写入空间 */
        chars = PAGE_SIZE - PIPE_HEAD(*inode);
        if (chars > count) {
            chars = count;
        }

        if (chars > size) {
            chars = size;
        }

        count -= chars;                       /* 待写入计数 */
        written += chars;                     /* 已写入计数 */
        size = PIPE_HEAD(*inode);             /* 缓冲区头游标 */
        PIPE_HEAD(*inode) += chars;           /* 更新头游标 */
        PIPE_HEAD(*inode) &= (PAGE_SIZE - 1); /* 确保头游标在缓冲区内 */

        /* 将数据拷贝到环形缓冲区 */
        while (chars-- > 0) {
            ((char *)inode->i_size)[size++] = get_fs_byte(buf++);
        }
    }

    /* 唤醒读进程 */
    wake_up(&PIPE_READ_WAIT(*inode));
    return written;
}

/**
 * @brief pipe 系统调用实现
 *
 * @param fildes 用于接受 pipefd 的文件描述符数组(即 pipe 的参数 `pipefd[2]`)
 * @return int
 */
int sys_pipe(unsigned long *fildes)
{
    struct m_inode *inode;
    struct file *f[2];
    int fd[2];
    int i, j;

    /* 准备 2 个空闲的文件结构, 放在 f[2] 数组里面 */
    j = 0;
    for (i = 0; j < 2 && i < NR_FILE; i++) {
        if (!file_table[i].f_count) {
            (f[j++] = i + file_table)->f_count++;
        }
    }

    if (j == 1) {
        f[0]->f_count = 0;
    }

    if (j < 2) {
        return -1;
    }

    /* 将 f[2] 数组里面的数据, 复制到进程 filp 数组里面
     * 并把 f[2] 对应的两个文件描述符记录在 fd[2] 数组里面 */
    j = 0;
    for (i = 0; j < 2 && i < NR_OPEN; i++) {
        if (!current->filp[i]) {
            current->filp[fd[j] = i] = f[j];
            j++;
        }
    }

    if (j == 1) {
        current->filp[fd[0]] = NULL;
    }

    if (j < 2) {
        f[0]->f_count = f[1]->f_count = 0;
        return -1;
    }

    /* 分配一个 pipe 内存 inode */
    if (!(inode = get_pipe_inode())) {
        current->filp[fd[0]] = current->filp[fd[1]] = NULL;
        f[0]->f_count = f[1]->f_count = 0;
        return -1;
    }

    /* 文件结构的 inode 指向新分配的 pipe */
    f[0]->f_inode = f[1]->f_inode = inode;
    f[0]->f_pos = f[1]->f_pos = 0;
    f[0]->f_mode = 1; /* read */
    f[1]->f_mode = 2; /* write */

    /* 返回读/写文件描述符 */
    put_fs_long(fd[0], 0 + fildes);
    put_fs_long(fd[1], 1 + fildes);
    return 0;
}

/**
 * @brief 管道的 ioctl 函数
 *
 * @param pino 管道 inode
 * @param cmd 命令
 * @param arg
 * @return int
 */
int pipe_ioctl(struct m_inode *pino, int cmd, int arg)
{
    switch (cmd) {
    case FIONREAD:
        /* 查询输入缓冲区中的字节数 */
        verify_area((void *)arg, 4);
        put_fs_long(PIPE_SIZE(*pino), (unsigned long *)arg);
        return 0;
    default:
        return -EINVAL;
    }
}
