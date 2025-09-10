/*
 *  linux/fs/fcntl.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <asm/segment.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <string.h>

#include <fcntl.h>
#include <sys/stat.h>

extern int sys_close(int fd);

/**
 * @brief 拷贝文件描述符
 *
 * @param fd 被拷贝的文件描述符
 * @param arg 新文件描述符值
 * @return int 拷贝后实际的文件描述符值
 */
static int dupfd(unsigned int fd, unsigned int arg)
{
    if (fd >= NR_OPEN || !current->filp[fd]) {
        return -EBADF;
    }

    if (arg >= NR_OPEN) {
        return -EINVAL;
    }

    while (arg < NR_OPEN) {
        if (current->filp[arg]) {
            arg++;
        } else {
            break;
        }
    }

    if (arg >= NR_OPEN) {
        return -EMFILE;
    }

    current->close_on_exec &= ~(1 << arg);
    (current->filp[arg] = current->filp[fd])->f_count++;
    return arg;
}

/**
 * @brief 将 oldfd 代表的文件描述符复制到 newfd
 *
 * @param oldfd
 * @param newfd
 * @return int 返回复制后的文件描述符(实际上就是 newfd)
 */
int sys_dup2(unsigned int oldfd, unsigned int newfd)
{
    sys_close(newfd);
    return dupfd(oldfd, newfd);
}

/**
 * @brief 拷贝 fildes 表示的文件描述符
 *
 * @param fildes
 * @return int 返回拷贝出来的新文件描述赋值
 */
int sys_dup(unsigned int fildes)
{
    return dupfd(fildes, 0);
}

/**
 * @brief fcntl 系统调用
 *
 * @param fd
 * @param cmd
 * @param arg
 * @return int
 */
int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{
    struct file *filp;

    if (fd >= NR_OPEN || !(filp = current->filp[fd])) {
        return -EBADF;
    }

    switch (cmd) {
    case F_DUPFD:
        return dupfd(fd, arg);
    case F_GETFD: /* 仅支持 FD_CLOEXEC 标记 */
        return (current->close_on_exec >> fd) & 1;
    case F_SETFD:
        if (arg & 1) {
            current->close_on_exec |= (1 << fd);
        } else {
            current->close_on_exec &= ~(1 << fd);
        }
        return 0;
    case F_GETFL:
        return filp->f_flags;
    case F_SETFL:
        /* 仅支持 O_APPEND, O_NONBLOCK 两个标记 */
        filp->f_flags &= ~(O_APPEND | O_NONBLOCK);
        filp->f_flags |= arg & (O_APPEND | O_NONBLOCK);
        return 0;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        return -1;
    default:
        return -1;
    }
}
