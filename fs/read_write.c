/*
 *  linux/fs/read_write.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <asm/segment.h>
#include <linux/kernel.h>
#include <linux/sched.h>

extern int rw_char(int rw, int dev, char *buf, int count, off_t *pos);
extern int read_pipe(struct m_inode *inode, char *buf, int count);
extern int write_pipe(struct m_inode *inode, char *buf, int count);
extern int block_read(int dev, off_t *pos, char *buf, int count);
extern int block_write(int dev, off_t *pos, char *buf, int count);
extern int file_read(struct m_inode *inode, struct file *filp, char *buf, int count);
extern int file_write(struct m_inode *inode, struct file *filp, char *buf, int count);

/**
 * @brief 重置文件操作偏移位置
 *
 * @param fd 文件描述符
 * @param offset 偏移值
 * @param origin 偏移值条件
 * @return int
 */
int sys_lseek(unsigned int fd, off_t offset, int origin)
{
    struct file *file;
    int tmp;

    if (fd >= NR_OPEN || !(file = current->filp[fd]) || !(file->f_inode) ||
        !IS_SEEKABLE(MAJOR(file->f_inode->i_dev))) {
        return -EBADF;
    }

    /* 管道文件, 不允许 seek
     * TODO: 但是 pipe 在上面 IS_SEEKABLE 应该就被拦截了啊? */
    if (file->f_inode->i_pipe) {
        return -ESPIPE;
    }

    switch (origin) {
    case 0:
        /* offset 是相对于文件开始 */
        if (offset < 0) {
            return -EINVAL;
        }

        file->f_pos = offset;
        break;
    case 1:
        /* offset 是相对于当前文件游标位置 */
        if (file->f_pos + offset < 0) {
            return -EINVAL;
        }

        file->f_pos += offset;
        break;
    case 2:
        /* offset 是相对于文件结束位置 */
        if ((tmp = file->f_inode->i_size + offset) < 0) {
            return -EINVAL;
        }

        file->f_pos = tmp;
        break;
    default:
        return -EINVAL;
    }

    return file->f_pos;
}

/**
 * @brief read 系统调用
 *
 * @param fd
 * @param buf
 * @param count
 * @return int
 */
int sys_read(unsigned int fd, char *buf, int count)
{
    struct file *file;
    struct m_inode *inode;

    if (fd >= NR_OPEN || count < 0 || !(file = current->filp[fd])) {
        return -EINVAL;
    }

    if (!count) {
        return 0;
    }

    verify_area(buf, count); /* 准备从内核态往用户态搬数据 */

    inode = file->f_inode;

    /* 如果是管道文件, 只有读端才允许读取(1 是读端标记) */
    if (inode->i_pipe) {
        return (file->f_mode & 1) ? read_pipe(inode, buf, count) : -EIO;
    }

    /* 如果是字符设备, 使用 rw_char 函数处理读取请求 */
    if (S_ISCHR(inode->i_mode)) {
        return rw_char(READ, inode->i_zone[0], buf, count, &file->f_pos);
    }

    /* 块设备使用 block_read 读取 */
    if (S_ISBLK(inode->i_mode)) {
        return block_read(inode->i_zone[0], &file->f_pos, buf, count);
    }

    /* 目录或者普通文件
     * TODO: S_ISDIR 这里允许读取目录吗??? */
    if (S_ISDIR(inode->i_mode) || S_ISREG(inode->i_mode)) {
        if (count + file->f_pos > inode->i_size) {
            count = inode->i_size - file->f_pos;
        }

        if (count <= 0) {
            return 0;
        }

        return file_read(inode, file, buf, count);
    }

    printk("(Read)inode->i_mode=%06o\n\r", inode->i_mode);
    return -EINVAL;
}

/**
 * @brief write 系统吊桶
 *
 * @param fd
 * @param buf
 * @param count
 * @return int
 */
int sys_write(unsigned int fd, char *buf, int count)
{
    struct file *file;
    struct m_inode *inode;

    if (fd >= NR_OPEN || count < 0 || !(file = current->filp[fd])) {
        return -EINVAL;
    }

    if (!count) {
        return 0;
    }

    inode = file->f_inode;

    /* 管道读端 */
    if (inode->i_pipe) {
        return (file->f_mode & 2) ? write_pipe(inode, buf, count) : -EIO;
    }

    /* 字符设备 */
    if (S_ISCHR(inode->i_mode)) {
        return rw_char(WRITE, inode->i_zone[0], buf, count, &file->f_pos);
    }

    /* 块设备 */
    if (S_ISBLK(inode->i_mode)) {
        return block_write(inode->i_zone[0], &file->f_pos, buf, count);
    }

    /* 普通文件 */
    if (S_ISREG(inode->i_mode)) {
        return file_write(inode, file, buf, count);
    }

    printk("(Write)inode->i_mode=%06o\n\r", inode->i_mode);
    return -EINVAL;
}
