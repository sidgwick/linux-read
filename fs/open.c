/*
 *  linux/fs/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/tty.h>

#include <asm/segment.h>

/**
 * @brief 取文件系统信息
 *
 * 系统调用用于返回已安装(mounted)文件系统的统计信息
 *
 * @param dev 含有已安装文件系统的设备号
 * @param ubuf 缓冲区指针, 用于存放系统返回的文件系统信息
 * @return int
 */
int sys_ustat(int dev, struct ustat *ubuf)
{
    return -ENOSYS;
}

/**
 * @brief 更新文件的访问时间和更新时间
 *
 * @param filename
 * @param times
 * @return int
 */
int sys_utime(char *filename, struct utimbuf *times)
{
    struct m_inode *inode;
    long actime, modtime;

    if (!(inode = namei(filename))) {
        return -ENOENT;
    }

    if (times) {
        actime = get_fs_long((unsigned long *)&times->actime);
        modtime = get_fs_long((unsigned long *)&times->modtime);
    } else {
        actime = modtime = CURRENT_TIME;
    }

    inode->i_atime = actime;
    inode->i_mtime = modtime;
    inode->i_dirt = 1;
    iput(inode);
    return 0;
}

/*
 * XXX should we use the real or effective uid?  BSD uses the real uid,
 * so as to make this call useful to setuid programs.
 */

/**
 * @brief 检查当前进程是否对文件具有 mode 权限
 *
 * @param filename
 * @param mode
 * @return int 有权限返回 0, 否则返回错误码
 */
int sys_access(const char *filename, int mode)
{
    struct m_inode *inode;
    int res, i_mode;

    mode &= 0007;
    if (!(inode = namei(filename))) {
        return -EACCES;
    }

    i_mode = res = inode->i_mode & 0777;
    iput(inode);

    if (current->uid == inode->i_uid) {
        /* 是自己 */
        res >>= 6;
    } else if (current->gid == inode->i_gid) {
        /* 是自己组, 似乎有 BUG 应该是 `res >>= 3` */
        res >>= 6;
    }

    /* 检查是不是具有 mode 指定的属性 */
    if ((res & 0007 & mode) == mode) {
        return 0;
    }

    /*
     * XXX we are doing this test last because we really should be
     * swapping the effective with the real user id (temporarily),
     * and then calling suser() routine.  If we do call the
     * suser() routine, it needs to be called last.
     *
     * 如果当前用户为超级用户(uid=0)并且屏蔽码执行位是 0 或者文件可以被任何人执行/搜索, 则返回 0
     */
    if ((!current->uid) && (!(mode & 1) || (i_mode & 0111))) {
        return 0;
    }

    return -EACCES;
}

/**
 * @brief 切换当前进程工作目录
 *
 * @param filename
 * @return int
 */
int sys_chdir(const char *filename)
{
    struct m_inode *inode;

    if (!(inode = namei(filename))) {
        return -ENOENT;
    }

    if (!S_ISDIR(inode->i_mode)) {
        iput(inode);
        return -ENOTDIR;
    }

    iput(current->pwd);
    current->pwd = inode;
    return (0);
}

/**
 * @brief 切换当前进程根目录
 *
 * @param filename
 * @return int
 */
int sys_chroot(const char *filename)
{
    struct m_inode *inode;

    if (!(inode = namei(filename))) {
        return -ENOENT;
    }

    if (!S_ISDIR(inode->i_mode)) {
        iput(inode);
        return -ENOTDIR;
    }

    iput(current->root);
    current->root = inode;
    return (0);
}

/**
 * @brief 更新文件属性
 *
 * @param filename
 * @param mode
 * @return int
 */
int sys_chmod(const char *filename, int mode)
{
    struct m_inode *inode;

    if (!(inode = namei(filename))) {
        return -ENOENT;
    }

    /* 判断有没有操作权限 */
    if ((current->euid != inode->i_uid) && !suser()) {
        iput(inode);
        return -EACCES;
    }

    inode->i_mode = (mode & 07777) | (inode->i_mode & ~07777);
    inode->i_dirt = 1;
    iput(inode);
    return 0;
}

/**
 * @brief 更新文件属主
 *
 * @param filename
 * @param uid
 * @param gid
 * @return int
 */
int sys_chown(const char *filename, int uid, int gid)
{
    struct m_inode *inode;

    if (!(inode = namei(filename))) {
        return -ENOENT;
    }

    if (!suser()) {
        iput(inode);
        return -EACCES;
    }

    inode->i_uid = uid;
    inode->i_gid = gid;
    inode->i_dirt = 1;
    iput(inode);
    return 0;
}

/**
 * @brief 检查字符设备类型
 *
 * 该函数仅用于下面文件打开系统调用 sys_open, 用于检查若打开的文件是 tty 终端字符设备时,
 * 需要对当前进程的设置和对 tty 表的设置
 *
 * @param inode
 * @param dev
 * @param flag
 * @return int 检测处理成功返回 0, 失败返回 -1
 */
static int check_char_dev(struct m_inode *inode, int dev, int flag)
{
    struct tty_struct *tty;
    int min;

    if (MAJOR(dev) == 4 || MAJOR(dev) == 5) {
        if (MAJOR(dev) == 5) {
            min = current->tty;
        } else {
            min = MINOR(dev);
        }

        if (min < 0) {
            return -1;
        }

        /* 主伪终端设备文件只能被进程独占使用
         * 如果子设备号表明是一个主伪终端, 并且该打开文件 i 节点引用计数大于 1, 则说明该设备
         * 已被其他进程使用. 因此不能再打开该字符设备文件, 于是返回 -1 */
        if ((IS_A_PTY_MASTER(min)) && (inode->i_count > 1)) {
            return -1;
        }

        // 否则, 我们让tty结构指针tty指向tty表中对应结构项.
        // 若打开文件操作标志 flag 中不含无需控制终端标志 O_NOCTTY, 并且进程是进程组首领, 并且当前进
        // 程没有控制终端, 并且 tty 结构中 session 表示该终端还不是任何进程组的控制终端(值为 0), 那么
        // 就允许为进程设置这个终端设备 min 为其控制终端
        // 于是设置进程任务结构终端设备号字段 tty 值等于 min, 并且设置对应 tty 结构的会话号 session 和进程组号 pgrp 分别等于进程的会
        // 话号和进程组号.

        tty = TTY_TABLE(min);

        /* O_NOCTTY 表示不分配控制终端
         * O_NONBLOCK 以非阻塞方式打开/操作文件 */

        /* 如果 flag 没有指定不分配控制终端选项, 且当前进程是会话首领而且没有控制终端
         * 那么就把 tty 分配给当前进程做控制终端, 这时候 tty 的会话和进程组记录应该设置为当前进程的值 */
        if (!(flag & O_NOCTTY) && current->leader && current->tty < 0 && tty->session == 0) {
            current->tty = min;
            tty->session = current->session;
            tty->pgrp = current->pgrp;
        }

        /* 如果 flag 指定以非阻塞方式打开文件, 这里需要对该字符终端设备进行相关设置,
         * 设置为满足读操作需要读取的最少字符数为 0, 设置超时定时值为 0, 并把终端设备
         * 设置成非规范模式. 非阻塞方式只能工作于非规范模式. 在此模式下当 VMIN
         * 和 VTIME 均设置为 0 时, 辅助队列中有多少支付进程就读取多少字符, 并立刻返回 */
        if (flag & O_NONBLOCK) {
            TTY_TABLE(min)->termios.c_cc[VMIN] = 0;
            TTY_TABLE(min)->termios.c_cc[VTIME] = 0;
            TTY_TABLE(min)->termios.c_lflag &= ~ICANON;
        }
    }

    return 0;
}

int sys_open(const char *filename, int flag, int mode)
{
    struct m_inode *inode;
    struct file *f;
    int i, fd;

    mode &= 0777 & ~current->umask;

    /* 遍历文件描述符数组, 看看现在一共有多少描述符
     * 顺便也算出来了, 即将打开的这个文件描述符值 */
    for (fd = 0; fd < NR_OPEN; fd++) {
        if (!current->filp[fd]) {
            break;
        }
    }

    if (fd >= NR_OPEN) {
        return -EINVAL;
    }

    /* 清理掉这个文件描述符对应的 close_on_exec 标记 */
    current->close_on_exec &= ~(1 << fd);

    /* 找一下 file_table 里面的空闲项, 待会儿把 file 对象放到里面 */
    f = 0 + file_table;
    for (i = 0; i < NR_FILE; i++, f++) {
        if (!f->f_count) {
            break;
        }
    }

    if (i >= NR_FILE) {
        return -EINVAL;
    }

    (current->filp[fd] = f)->f_count++;

    /* 打开文件对应的 inode */
    if ((i = open_namei(filename, flag, mode, &inode)) < 0) {
        current->filp[fd] = NULL;
        f->f_count = 0;
        return i;
    }

    /* ttys are somewhat special (ttyxx major==4, tty major==5) */
    if (S_ISCHR(inode->i_mode)) {
        if (check_char_dev(inode, inode->i_zone[0], flag)) {
            iput(inode);
            current->filp[fd] = NULL;
            f->f_count = 0;
            return -EAGAIN;
        }
    }

    /* Likewise with block-devices: check for floppy_change */
    if (S_ISBLK(inode->i_mode)) {
        check_disk_change(inode->i_zone[0]);
    }

    f->f_mode = inode->i_mode;
    f->f_flags = flag;
    f->f_count = 1;
    f->f_inode = inode;
    f->f_pos = 0;
    return (fd);
}

/**
 * @brief 新建文件
 *
 * @param pathname
 * @param mode
 * @return int
 */
int sys_creat(const char *pathname, int mode)
{
    return sys_open(pathname, O_CREAT | O_TRUNC, mode);
}

/**
 * @brief 关闭文件
 *
 * @param fd
 * @return int
 */
int sys_close(unsigned int fd)
{
    struct file *filp;

    if (fd >= NR_OPEN) {
        return -EINVAL;
    }

    current->close_on_exec &= ~(1 << fd);
    if (!(filp = current->filp[fd])) {
        return -EINVAL;
    }

    current->filp[fd] = NULL;
    if (filp->f_count == 0) {
        panic("Close: file count is 0");
    }

    if (--filp->f_count) {
        return (0);
    }

    iput(filp->f_inode);
    return (0);
}
