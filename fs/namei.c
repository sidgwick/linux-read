/*
 *  linux/fs/namei.c
 *
 *  (C) 1991  Linus Torvalds
 *
 * namei 是 name inode 的意思
 */

/*
 * Some corrections by tytso.
 */

#include <asm/segment.h>
#include <const.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <string.h>
#include <sys/stat.h>

static struct m_inode *_namei(const char *filename, struct m_inode *base, int follow_links);

#define ACC_MODE(x) ("\004\002\006\377"[(x) & O_ACCMODE])

/*
 * comment out this line if you want names > NAME_LEN chars to be
 * truncated. Else they will be disallowed.
 */
/* #define NO_TRUNCATE */

#define MAY_EXEC 1  /* X 可执行(可进入) */
#define MAY_WRITE 2 /* W 可写 */
#define MAY_READ 4  /* R 可读 */

/*
 *    permission()
 *
 */

/**
 * @brief 检查进程是否有文件的许可(读写执行)权限
 *
 * UNIX 系统中, 权限属性 USER_GROUP_OTHERS
 *
 * is used to check for read/write/execute permissions on a file.
 * I don't know if we should look at just the euid or both euid and
 * uid, but that should be easily changed.
 *
 * @param inode
 * @param mask
 * @return int
 */
static int permission(struct m_inode *inode, int mask)
{
    int mode = inode->i_mode;

    /* special case: not even root can read/write a deleted file
     * root 用户也不能读写一个删除的文件 */
    if (inode->i_dev && !inode->i_nlinks) {
        return 0;
    }

    else if (current->euid == inode->i_uid) {
        /* 当前进程和文件属主相同, 检查 USER 对应的 rwx 部分 */
        mode >>= 6;
    } else if (in_group_p(inode->i_gid)) {
        /* 当前进程和文件属主相同, 检查 GROUP 对应的 rwx 部分 */
        mode >>= 3;
    }

    /* 其他情况, 检查 Other 对应的 rwx 部分, 这个部分不需要位移操作 */

    /* 检查 mode 是否具有 mask 要求的权限, 如果是 root, 直接判定为有权限 */
    if (((mode & mask & 0007) == mask) || suser()) {
        return 1;
    }

    return 0;
}

/**
 * @brief 判断 name 和 de->name 是不是匹配
 *
 * ok, we cannot use strncmp, as the name is not in our data space.
 * Thus we'll have to use match. No big problem. Match also makes
 * some sanity tests.
 *
 * NOTE! unlike strncmp, match returns 1 for success, 0 for failure.
 *
 * TODO: 目录项里面的 `.`, `..` 是怎么写进去的?
 *
 * @param len 比较长度
 * @param name 文件名字符串
 * @param de 目录项
 * @return int 匹配返回 1, 否则返回 0
 */
static int match(int len, const char *name, struct dir_entry *de)
{
    register int same __asm__("ax");

    if (!de || !de->inode || len > NAME_LEN) {
        return 0;
    }

    /* "" means "." ---> so paths like "/usr/lib//libc.a" work */
    if (!len && (de->name[0] == '.') && (de->name[1] == '\0')) {
        return 1;
    }

    /* de->name[len] 应该是 NULL 结尾字符串 */
    if (len < NAME_LEN && de->name[len]) {
        return 0;
    }

    /* 逐字符比较 */
    __asm__("cld\n\t"
            "fs ; repe ; cmpsb\n\t" /* fs 指定 cmpsb 使用 fs 段 */
            "setz %%al"             /* equal -> ZF=1 -> al=1 */
            : "=a"(same)
            : "0"(0), "S"((long)name), "D"((long)de->name), "c"(len));

    /* 匹配返回 1, 不匹配返回 0 */
    return same;
}

/**
 * @brief
 *
 * finds an entry in the specified directory with the wanted name. It
 * returns the cache buffer in which the entry was found, and the entry
 * itself (as a parameter - res_dir). It does NOT read the inode of the
 * entry - you'll have to do that yourself if you want to.
 *
 * This also takes care of the few special cases due to '..'-traversal
 * over a pseudo-root and a mount point.
 *
 * @param dir 代表目录的 inode, TODO: 为啥给个二级指针?
 * @param name 目录中的文件名
 * @param namelen 文件名的长度
 * @param res_dir 接受搜索结果的 dir_entry 结构指针
 * @return struct buffer_head* 包含结果 dir_entry 的缓冲区块
 */
static struct buffer_head *find_entry(struct m_inode **dir, const char *name, int namelen,
                                      struct dir_entry **res_dir)
{
    int entries;
    int block, i;
    struct buffer_head *bh;
    struct dir_entry *de;
    struct super_block *sb;

#ifdef NO_TRUNCATE
    if (namelen > NAME_LEN)
        return NULL;
#else
    if (namelen > NAME_LEN)
        namelen = NAME_LEN;
#endif

    /* 目录是一种特殊的文件, 它的文件内容是一系列的 dir_entry */
    entries = (*dir)->i_size / (sizeof(struct dir_entry));
    *res_dir = NULL;
    /* check for '..', as we might have to do some "magic" for it
     * name == '..' 的时候, 这里做一些特别的处理 */
    if (namelen == 2 && get_fs_byte(name) == '.' && get_fs_byte(name + 1) == '.') {
        /* '..' in a pseudo-root results in a faked '.' (just change namelen)
         * 在 dir 是当前进程 root 目录的情况下, `..` 被视作 `.` 处理 */
        if ((*dir) == current->root) {
            namelen = 1;
        } else if ((*dir)->i_num == ROOT_INO) {
            /* '..' over a mount-point results in 'dir' being exchanged for the
             * mounted directory-inode. NOTE! We set mounted, so that we can
             * iput the new dir
             * 如果 dir 是一个挂载点, 就要把 dir 换到被挂载的那个设备和目录上 */
            sb = get_super((*dir)->i_dev);
            if (sb->s_imount) { /* s_mount 是提供挂载点的那个设备的 inode */
                iput(*dir);
                (*dir) = sb->s_imount;
                (*dir)->i_count++;
            }
        }
    }

    /* 目录的 zone[0] 为空的话, 说明是一个空目录 */
    if (!(block = (*dir)->i_zone[0])) {
        return NULL;
    }

    /* 读取第一个文件数据块 */
    if (!(bh = bread((*dir)->i_dev, block))) {
        return NULL;
    }

    i = 0;
    de = (struct dir_entry *)bh->b_data;
    while (i < entries) {
        /* 如果一个 block 读取完毕, 把下一个 block 装载到内存 */
        if ((char *)de >= BLOCK_SIZE + bh->b_data) {
            brelse(bh);
            bh = NULL;

            /* 因为删除操作可能会使得某些 de 是空洞状态 */
            if (!(block = bmap(*dir, i / DIR_ENTRIES_PER_BLOCK)) ||
                !(bh = bread((*dir)->i_dev, block))) {
                i += DIR_ENTRIES_PER_BLOCK; /* 这里是直接跳过这些读不到的数据的意思, 似乎有 bug */
                continue;                   /* TODO: 下一个块加载失败, 重试. bh == NULL 了啊??? */
            }

            de = (struct dir_entry *)bh->b_data;
        }

        /* 看下是不是目标
         * 是的话, 就记录目标所在的 directory entry 并返回目标所在的缓存区快 */
        if (match(namelen, name, de)) {
            *res_dir = de;
            return bh;
        }

        de++;
        i++;
    }

    brelse(bh);
    return NULL;
}

/**
 * @brief 往目录新加(或者修改)内容
 *
 * adds a file entry to the specified directory, using the same
 * semantics as find_entry(). It returns NULL if it failed.
 *
 * NOTE!! The inode part of 'de' is left at 0 - which means you
 * may not sleep between calling this and putting something into
 * the entry, as someone else might have used it while you slept.
 *
 * 要注意, 这个函数把 entry 的 inode 部分值为 0 了, 因此调用 add_entry 和
 * 往 entry 追加内容中间不应该有 sleep 操作.
 *
 * @param dir 要操作的目录 inode
 * @param name 文件名字
 * @param namelen 文件名字长度
 * @param res_dir 接受 directory entry 结果的结构体
 * @return struct buffer_head* 返回 entry 所在的缓存区快
 */
static struct buffer_head *add_entry(struct m_inode *dir, const char *name, int namelen,
                                     struct dir_entry **res_dir)
{
    int block, i;
    struct buffer_head *bh;
    struct dir_entry *de;

    *res_dir = NULL;

#ifdef NO_TRUNCATE
    if (namelen > NAME_LEN)
        return NULL;
#else
    if (namelen > NAME_LEN)
        namelen = NAME_LEN;
#endif

    if (!namelen) {
        return NULL;
    }

    if (!(block = dir->i_zone[0])) {
        return NULL;
    }

    if (!(bh = bread(dir->i_dev, block))) {
        return NULL;
    }

    i = 0;
    de = (struct dir_entry *)bh->b_data;
    while (1) {
        if ((char *)de >= BLOCK_SIZE + bh->b_data) {
            brelse(bh);
            bh = NULL;

            /* 查找 i 所在的区块, 没有的话, 就创建 */
            block = create_block(dir, i / DIR_ENTRIES_PER_BLOCK);
            if (!block) {
                return NULL;
            }

            /* 区块读入内存
             * TODO: 读取的缓冲块指针为空, 说明逻辑块可能是因为不存在而新创建的空块??? */
            if (!(bh = bread(dir->i_dev, block))) {
                i += DIR_ENTRIES_PER_BLOCK;
                continue;
            }

            de = (struct dir_entry *)bh->b_data;
        }

        /* 增加目录大小记录 */
        if (i * sizeof(struct dir_entry) >= dir->i_size) {
            de->inode = 0; /* 把新的 directory entry 的 inode 初始化为 0 */
            dir->i_size = (i + 1) * sizeof(struct dir_entry);
            dir->i_dirt = 1; /* dir 标记为有修改 */
            dir->i_ctime = CURRENT_TIME;
        }

        /* inode=0 的话, 说明这个 entry 是闲置状态, 这时候直接使用这个节点 */
        if (!de->inode) {
            dir->i_mtime = CURRENT_TIME;

            /* 记录文件名字 */
            for (i = 0; i < NAME_LEN; i++) {
                de->name[i] = (i < namelen) ? get_fs_byte(name + i) : 0;
            }

            /* de 所在的缓存区快, 标记为脏, 将来回写磁盘 */
            bh->b_dirt = 1;
            *res_dir = de;
            return bh;
        }

        de++;
        i++;
    }

    brelse(bh);
    return NULL;
}

/**
 * @brief 符号链接指向的真实文件
 *
 * @param dir 符号链接所在的目录
 * @param inode 符号链接
 * @return struct m_inode* 符号链接指向的文件 inode
 */
static struct m_inode *follow_link(struct m_inode *dir, struct m_inode *inode)
{
    unsigned short fs;
    struct buffer_head *bh;

    if (!dir) {
        dir = current->root;
        dir->i_count++;
    }

    if (!inode) {
        iput(dir);
        return NULL;
    }

    if (!S_ISLNK(inode->i_mode)) {
        iput(dir);
        return inode;
    }

    __asm__("mov %%fs,%0" : "=r"(fs));
    if (fs != 0x17 || !inode->i_zone[0] || !(bh = bread(inode->i_dev, inode->i_zone[0]))) {
        iput(dir);
        iput(inode);
        return NULL;
    }

    iput(inode);

    /* 一般情况下 syscall 的时候 fs 指向的是用户数据段, 这里设置指向内核数据段 */
    __asm__("mov %0, %%fs" ::"r"((unsigned short)0x10));
    inode = _namei(bh->b_data, dir, 0);
    __asm__("mov %0, %%fs" ::"r"(fs)); /* 恢复 FS */
    brelse(bh);

    return inode;
}

/**
 * @brief 返回指定目录的 inode
 *
 * Getdir traverses the pathname until it hits the topmost directory.
 * It returns NULL on failure.
 *
 * 这里 'topmost' 是指路径名中最靠近末端的目录
 *
 * @param pathname 路径名
 * @param inode 起始搜索目录的 inode
 * @return struct m_inode* 目录或文件的 inode 指针. 失败时返回NULL
 */
static struct m_inode *get_dir(const char *pathname, struct m_inode *inode)
{
    char c;
    const char *thisname;
    struct buffer_head *bh;
    int namelen, inr;
    struct dir_entry *de;
    struct m_inode *dir;

    if (!inode) {
        inode = current->pwd;
        inode->i_count++;
    }

    /* 路径名从根目录开始的情况 */
    if ((c = get_fs_byte(pathname)) == '/') {
        iput(inode);
        inode = current->root;
        pathname++;
        inode->i_count++;
    }

    /* 在刚进入循环时, 当前目录的 inode 就是进程根 inode 或者是
     * 当前工作目录的 inode , 或者是参数指定的某个搜索起始目录的 inode
     *
     * !!!注意:
     * 这个 while 循环对(第一个里面 yyy 不要求一定是目录):
     *  - `path/to/xxx/yyy` 这样的情况最终计算好的 inode 指向的是 xxx
     *  - `path/to/xxx/yyy/` 这样的情况最终计算好的 inode 指向的是 yyy */

    while (1) {
        thisname = pathname;

        /* 不是目录项, 或者无权限检索 inode */
        if (!S_ISDIR(inode->i_mode) || !permission(inode, MAY_EXEC)) {
            iput(inode);
            return NULL;
        }

        /* 在文件路径中找到下一个 `/` 字符 */
        for (namelen = 0; (c = get_fs_byte(pathname++)) && (c != '/'); namelen++) {
            /* nothing */;
        }

        /* c == NULL, 说明已经完全遍历了 pathname, 返回结果 */
        if (!c) {
            return inode;
        }

        /* 寻找 inode 中, 名字叫做 thisname 的项 */
        if (!(bh = find_entry(&inode, thisname, namelen, &de))) {
            iput(inode);
            return NULL;
        }

        inr = de->inode; /* 目录项中记录的文件 inode number */
        brelse(bh);
        dir = inode;

        /* 拿到目录项对应的 inode */
        if (!(inode = iget(dir->i_dev, inr))) {
            iput(dir);
            return NULL;
        }

        /* 处理符号链接的情况, 把 inode 指向真实数据对应的那个 inode */
        if (!(inode = follow_link(dir, inode))) {
            return NULL;
        }
    }
}

/**
 * @brief 返回 pathname 指定的目录名对应的 inode
 *
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 *
 * @param pathname 目录路径
 * @param namelen 接受最后的目录名字长度
 * @param name 接受最后(topmost)的目录名字
 * @param base 目录路径对应(或者要寻找)的 inode
 * @return struct m_inode*
 */
static struct m_inode *dir_namei(const char *pathname, int *namelen, const char **name,
                                 struct m_inode *base)
{
    char c;
    const char *basename;
    struct m_inode *dir;

    if (!(dir = get_dir(pathname, base))) {
        return NULL;
    }

    basename = pathname;
    while ((c = get_fs_byte(pathname++))) {
        if (c == '/') {
            basename = pathname;
        }
    }

    /* 路径中 topmost 的目录名字 */
    *namelen = pathname - basename - 1;
    *name = basename;
    return dir;
}

/**
 * @brief 由路径名查找对应 inode 的内部函数
 *
 * @param pathname 路径名
 * @param base 搜索的 inode
 * @param follow_links
 * @return struct m_inode*
 */
struct m_inode *_namei(const char *pathname, struct m_inode *base, int follow_links)
{
    const char *basename;
    int inr, namelen;
    struct m_inode *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    /* 获取到 pathname 对应的 topmost 目录 inode, 顺带也获知路径中的最终名字部分 */
    if (!(base = dir_namei(pathname, &namelen, &basename, base))) {
        return NULL;
    }

    /* 这里处理的是 pathname 以 `path/to/xxx/` 结尾的特殊情况
     * 此时 base 已经指向 xxx 的 inode, 返回即可 */
    if (!namelen) { /* special case: '/usr/' etc */
        return base;
    }

    /* 这里处理的是 pathname 以 `path/to/xxx/yyy` 的情况(也即 pathname 最后不是以 `/` 结尾)
     * 到这里的 base 指向的是 xxx 对应的 inode, basename=yyy
     * 因此后续工作就是在 xxx 里面查询 yyy 相关的信息 */

    /* 拿到 yyy 对应的目录项和缓冲区 */
    bh = find_entry(&base, basename, namelen, &de);
    if (!bh) {
        iput(base);
        return NULL;
    }

    inr = de->inode; /* 取出来目录项里面记录的关于 yyy 文件节点号 */
    brelse(bh);

    /* 拿到目录项(yyy)对应的文件(目录)的 inode */
    if (!(inode = iget(base->i_dev, inr))) {
        iput(base);
        return NULL;
    }

    /* 处理符号链接 */
    if (follow_links) {
        inode = follow_link(base, inode);
    } else {
        iput(base);
    }

    inode->i_atime = CURRENT_TIME;
    inode->i_dirt = 1;
    return inode;
}

/**
 * @brief 取指定路径名的i节点, 不跟随符号链接
 *
 * @param pathname 路径名
 * @return struct m_inode* 对应的i节点
 */
struct m_inode *lnamei(const char *pathname)
{
    return _namei(pathname, NULL, 0);
}

/*
 *    namei()
 *
 * is used by most simple commands to get the inode of a specified name.
 * Open, link etc use their own routines, but this is enough for things
 * like 'chmod' etc.
 */

/**
 * @brief 取指定路径名的i节点, 跟随符号链接
 *
 * 该函数被许多简单命令用于取得指定路径名称的i节点
 * open/link等则使用它们自己的相应函数
 * 但对于象修改模式'chmod'等这样的命令, 该函数已足够用了
 *
 * @param pathname 路径名
 * @return struct m_inode* 对应的i节点
 */
struct m_inode *namei(const char *pathname)
{
    return _namei(pathname, NULL, 1);
}

/**
 * @brief 文件打开 namei 函数
 *
 * namei for open - this is in fact almost the whole open-routine.
 * open 函数使用的 namei 函数 - 这其实几乎是完整的打开文件程序
 *
 * @param pathname 文件路径名
 * @param flag 打开文件标志, 可取 O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_EXCL/O_APPEND 等其他一些标志的组合
 * @param mode 指定文件的许可属性, 可取 S_IRWXU/S_IRUSR/S_IRWXG 等, 对于新创建的文件, 这些属性只应用于将来对文件的访问, 创建了只读文件的打开调用也将返回一个可读写的文件句柄
 * @param res_inode 接受对应文件路径名的i节点指针
 * @return int 成功返回0, 否则返回出错码
 */
int open_namei(const char *pathname, int flag, int mode, struct m_inode **res_inode)
{
    const char *basename;
    int inr, dev, namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    // 参见下面411行上的注释.

    /* 指定清空但是文件访问模式标志是只读(0), 文件打开标志追加上只写标志
     * 原因是 O_TRUNC 必须在文件可写情况下才有效 */
    if ((flag & O_TRUNC) && !(flag & O_ACCMODE)) {
        flag |= O_WRONLY;
    }

    /* umask 中为 1 的权限, 是不希望新文件默认获得的
     * 下面的操作, 就是把 mode 里面不希望用户获得的位, 清理掉 */
    mode &= 0777 & ~current->umask;
    mode |= I_REGULAR; /* 添上普通文件标志 I_REGULAR */

    /* 找到 pathname 指定的目录项 inode */
    if (!(dir = dir_namei(pathname, &namelen, &basename, NULL))) {
        return -ENOENT;
    }

    /* 如果 pathname 以 `/` 结尾, 这次的操作目标就是打开这个文件目录, 已经达到目的 */
    if (!namelen) { /* special case: '/usr/' etc */
        if (!(flag & (O_ACCMODE | O_CREAT | O_TRUNC))) {
            /* 正常打开了目录 */
            *res_inode = dir;
            return 0;
        }

        /* 对目录而言, 不能做 读写, 创建, 截 0 操作 */
        iput(dir);
        return -EISDIR;
    }

    /* 找到目录下面的 basename 所在的缓冲块 */
    bh = find_entry(&dir, basename, namelen, &de);
    if (!bh) {
        /* 拿不到缓冲块, 原因可能是目标文件不存在 */

        /* 文件不存在, 又没有指定创建标志, 报错 */
        if (!(flag & O_CREAT)) {
            iput(dir);
            return -ENOENT;
        }

        /* 不允许写入 */
        if (!permission(dir, MAY_WRITE)) {
            iput(dir);
            return -EACCES;
        }

        /* 创建不存在的文件, 分配一个新 inode */
        inode = new_inode(dir->i_dev);
        if (!inode) {
            iput(dir);
            return -ENOSPC;
        }

        inode->i_uid = current->euid;
        inode->i_mode = mode;
        inode->i_dirt = 1;

        /* 在 dir 里面创建一个名字叫做 basename 的 entry */
        bh = add_entry(dir, basename, namelen, &de);
        if (!bh) {
            inode->i_nlinks--;
            iput(inode);
            iput(dir);
            return -ENOSPC;
        }

        /* 更新目录项 */
        de->inode = inode->i_num;
        bh->b_dirt = 1;
        brelse(bh);
        iput(dir);
        *res_inode = inode;
        return 0;
    }

    /* 文件已经存在, 从目录项里面取到文件数据节点 */
    inr = de->inode;
    dev = dir->i_dev;
    brelse(bh);

    /* 独占操作标志 O_EXCL 置位, 但文件已经存在, 返回文件已存在出错码 */
    if (flag & O_EXCL) {
        iput(dir);
        return -EEXIST;
    }

    /* 如果是符号链接, 找到符号链接底层的真实 inode */
    if (!(inode = follow_link(dir, iget(dev, inr)))) {
        return -EACCES;
    }

    /* 目录不支持读操作, 或者操作权限不够
     * TODO: flag 具体是怎么组成的?
     * 答: flag 的低 2 位是控制读写的, 可以是 O_RDONLY/O_WRONLY/O_RDWR 的位与组合
     *     ACC_MODE(flag) 可以取到一个对应的读写权限判断 mode:
     *          flag = 0 --> mode = 004
     *          flag = 1 --> mode = 002
     *          flag = 2 --> mode = 006
     *          flag = 3 --> mode = 377  --- 无效标志 ??? */
    if ((S_ISDIR(inode->i_mode) && (flag & O_ACCMODE)) || !permission(inode, ACC_MODE(flag))) {
        iput(inode);
        return -EPERM;
    }

    inode->i_atime = CURRENT_TIME;
    if (flag & O_TRUNC) {
        truncate(inode);
    }

    *res_inode = inode;
    return 0;
}

int sys_mknod(const char *filename, int mode, int dev)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    /* TODO: 为啥一定要是超级用户? */
    if (!suser()) {
        return -EPERM;
    }

    if (!(dir = dir_namei(filename, &namelen, &basename, NULL))) {
        return -ENOENT;
    }

    /* pathname 是以 `/` 结尾的字符串
     * TODO-DONE: 为什么不支持? - 是目录不支持 mknod 操作?
     * 答: 目录应该使用 sys_mkdir 创建 */
    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }

    /* 要对 dir 有写权限 */
    if (!permission(dir, MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }

    /* 在 dir 里面找到 basename 对应的 directory entry */
    bh = find_entry(&dir, basename, namelen, &de);
    if (bh) {
        brelse(bh);
        iput(dir);
        return -EEXIST;
    }

    /* 新建一个 inode */
    inode = new_inode(dir->i_dev);
    if (!inode) {
        iput(dir);
        return -ENOSPC;
    }

    inode->i_mode = mode;

    /* 如果是块设备或者字符设备, zone[0] 用来记录设备号 */
    if (S_ISBLK(mode) || S_ISCHR(mode)) {
        inode->i_zone[0] = dev;
    }

    inode->i_mtime = inode->i_atime = CURRENT_TIME;
    inode->i_dirt = 1;
    bh = add_entry(dir, basename, namelen, &de);
    if (!bh) {
        iput(dir);
        inode->i_nlinks = 0;
        iput(inode);
        return -ENOSPC;
    }
    de->inode = inode->i_num;
    bh->b_dirt = 1;
    iput(dir);
    iput(inode);
    brelse(bh);
    return 0;
}

/**
 * @brief 新建目录
 *
 * @param pathname
 * @param mode
 * @return int
 */
int sys_mkdir(const char *pathname, int mode)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh, *dir_block;
    struct dir_entry *de;

    if (!(dir = dir_namei(pathname, &namelen, &basename, NULL))) {
        return -ENOENT;
    }

    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }

    if (!permission(dir, MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }

    /* 检查 basename 是不是已经存在了 */
    bh = find_entry(&dir, basename, namelen, &de);
    if (bh) {
        brelse(bh);
        iput(dir);
        return -EEXIST;
    }

    /* 为目录分配新的 inode */
    inode = new_inode(dir->i_dev);
    if (!inode) {
        iput(dir);
        return -ENOSPC;
    }

    inode->i_size = 32; /* TODO: why ? */
    inode->i_dirt = 1;
    inode->i_mtime = inode->i_atime = CURRENT_TIME;
    if (!(inode->i_zone[0] = new_block(inode->i_dev))) {
        iput(dir);
        inode->i_nlinks--;
        iput(inode);
        return -ENOSPC;
    }

    inode->i_dirt = 1;
    if (!(dir_block = bread(inode->i_dev, inode->i_zone[0]))) {
        iput(dir);
        inode->i_nlinks--;
        iput(inode);
        return -ERROR;
    }

    /* 把 `.` 放到当前目录里面 */
    de = (struct dir_entry *)dir_block->b_data;
    de->inode = inode->i_num;
    strcpy(de->name, ".");

    /* 把 `..` 放到当前目录里面 */
    de++;
    de->inode = dir->i_num;
    strcpy(de->name, "..");

    inode->i_nlinks = 2; /* 新目录有 2 个引用, 父目录一个, `.` 一个 */
    dir_block->b_dirt = 1;
    brelse(dir_block);
    inode->i_mode = I_DIRECTORY | (mode & 0777 & ~current->umask);
    inode->i_dirt = 1;
    bh = add_entry(dir, basename, namelen, &de);
    if (!bh) {
        iput(dir);
        inode->i_nlinks = 0;
        iput(inode);
        return -ENOSPC;
    }
    de->inode = inode->i_num;
    bh->b_dirt = 1;
    dir->i_nlinks++; /* 来自上面的 `..` 引用计数 */
    dir->i_dirt = 1;
    iput(dir);
    iput(inode);
    brelse(bh);
    return 0;
}

/**
 * @brief 检查目录是否为空
 *
 * routine to check that the specified directory is empty (for rmdir)
 *
 * @param inode
 * @return int
 */
static int empty_dir(struct m_inode *inode)
{
    int nr, block;
    int len;
    struct buffer_head *bh;
    struct dir_entry *de;

    /* 空目录也有 2 个条目(分别是 `.` 和 `..`) */
    len = inode->i_size / sizeof(struct dir_entry);
    if (len < 2 || !inode->i_zone[0] || !(bh = bread(inode->i_dev, inode->i_zone[0]))) {
        printk("warning - bad directory on dev %04x\n", inode->i_dev);
        return 0;
    }

    de = (struct dir_entry *)bh->b_data;
    if (de[0].inode != inode->i_num || !de[1].inode || strcmp(".", de[0].name) ||
        strcmp("..", de[1].name)) {
        printk("warning - bad directory on dev %04x\n", inode->i_dev);
        return 0;
    }

    /* 从第二个 entry 开始检查 */
    nr = 2;
    de += 2;
    while (nr < len) {
        /* 如果查到下一个 block 了, 就把下一个 block 读入 */
        if ((void *)de >= (void *)(bh->b_data + BLOCK_SIZE)) {
            brelse(bh);
            block = bmap(inode, nr / DIR_ENTRIES_PER_BLOCK);
            if (!block) {
                nr += DIR_ENTRIES_PER_BLOCK;
                continue;
            }

            if (!(bh = bread(inode->i_dev, block))) {
                return 0;
            }

            de = (struct dir_entry *)bh->b_data;
        }

        /* inode 不为 0, 说明这是个有效的 entry, 目录不为空 */
        if (de->inode) {
            brelse(bh);
            return 0;
        }

        de++;
        nr++;
    }

    brelse(bh);
    return 1;
}

/**
 * @brief 删除目录
 *
 * @param name
 * @return int
 */
int sys_rmdir(const char *name)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    /* 找到父目录 */
    if (!(dir = dir_namei(name, &namelen, &basename, NULL))) {
        return -ENOENT;
    }

    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }

    if (!permission(dir, MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }

    /* 找到要删除的目录项 */
    bh = find_entry(&dir, basename, namelen, &de);
    if (!bh) {
        iput(dir);
        return -ENOENT;
    }

    /* 获取目录项对应的 inode */
    if (!(inode = iget(dir->i_dev, de->inode))) {
        iput(dir);
        brelse(bh);
        return -EPERM;
    }

    /* TODO-DONE: S_ISVTX 标记的特性
     * 答: The sticky bit (S_ISVTX) on a directory means that a file in that
     * directory can be renamed or deleted only by the owner of the file,
     * by the owner of the directory, and by a privileged process. */
    if ((dir->i_mode & S_ISVTX) && current->euid && inode->i_uid != current->euid) {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }

    /* dir 和 inode 属于不同的设备, 这说明
     * TODO: 再看一下挂载点的内容 */
    if (inode->i_dev != dir->i_dev || inode->i_count > 1) {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }

    /* 自己删除自己是删不掉的, 现代系统上, `.`, `..` 都不允许删除 */
    if (inode == dir) { /* we may not delete ".", but "../dir" is ok */
        iput(inode);
        iput(dir);
        brelse(bh);
        return -EPERM;
    }

    /* 目标不是一个目录, 也不予删除 */
    if (!S_ISDIR(inode->i_mode)) {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -ENOTDIR;
    }

    /* 目录不是空目录, 也不能删除 */
    if (!empty_dir(inode)) {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -ENOTEMPTY;
    }

    /* 数据错误, 空目录应该只有 2 个引用 */
    if (inode->i_nlinks != 2) {
        printk("empty directory has nlink!=2 (%d)", inode->i_nlinks);
    }

    de->inode = 0;
    bh->b_dirt = 1;
    brelse(bh);
    inode->i_nlinks = 0;
    inode->i_dirt = 1;
    dir->i_nlinks--;
    dir->i_ctime = dir->i_mtime = CURRENT_TIME;
    dir->i_dirt = 1;
    iput(dir);
    iput(inode);
    return 0;
}

/**
 * @brief 删除文件
 *
 * @param name
 * @return int
 */
int sys_unlink(const char *name)
{
    const char *basename;
    int namelen;
    struct m_inode *dir, *inode;
    struct buffer_head *bh;
    struct dir_entry *de;

    if (!(dir = dir_namei(name, &namelen, &basename, NULL))) {
        return -ENOENT;
    }

    if (!namelen) {
        iput(dir);
        return -ENOENT;
    }

    if (!permission(dir, MAY_WRITE)) {
        iput(dir);
        return -EPERM;
    }

    bh = find_entry(&dir, basename, namelen, &de);
    if (!bh) {
        iput(dir);
        return -ENOENT;
    }

    if (!(inode = iget(dir->i_dev, de->inode))) {
        iput(dir);
        brelse(bh);
        return -ENOENT;
    }

    if ((dir->i_mode & S_ISVTX) && !suser() && current->euid != inode->i_uid &&
        current->euid != dir->i_uid) {
        iput(dir);
        iput(inode);
        brelse(bh);
        return -EPERM;
    }

    /* 目录应该使用 sys_rmdir 删除 */
    if (S_ISDIR(inode->i_mode)) {
        iput(inode);
        iput(dir);
        brelse(bh);
        return -EPERM;
    }

    /* 如果 inode 的 nlinks 为空, 这属于系统的脏数据了 */
    if (!inode->i_nlinks) {
        printk("Deleting nonexistent file (%04x:%d), %d\n", inode->i_dev, inode->i_num,
               inode->i_nlinks);
        inode->i_nlinks = 1;
    }

    /* 删除目录项 */
    de->inode = 0;
    bh->b_dirt = 1;
    brelse(bh);

    /* 修改(有必要会删除)文件节点 */
    inode->i_nlinks--;
    inode->i_dirt = 1;
    inode->i_ctime = CURRENT_TIME;
    iput(inode); /* iput 里面会考虑文件节点的删除 */
    iput(dir);
    return 0;
}

/**
 * @brief 创建符号链接
 *
 * @param oldname
 * @param newname
 * @return int
 */
int sys_symlink(const char *oldname, const char *newname)
{
    struct dir_entry *de;
    struct m_inode *dir, *inode;
    struct buffer_head *bh, *name_block;
    const char *basename;
    int namelen, i;
    char c;

    dir = dir_namei(newname, &namelen, &basename, NULL);
    if (!dir) {
        return -EACCES;
    }

    if (!namelen) {
        iput(dir);
        return -EPERM;
    }

    if (!permission(dir, MAY_WRITE)) {
        iput(dir);
        return -EACCES;
    }

    if (!(inode = new_inode(dir->i_dev))) {
        iput(dir);
        return -ENOSPC;
    }

    inode->i_mode = S_IFLNK | (0777 & ~current->umask);
    inode->i_dirt = 1;

    /* 分配 zone[0] 对应的 block */
    if (!(inode->i_zone[0] = new_block(inode->i_dev))) {
        iput(dir);
        inode->i_nlinks--;
        iput(inode);
        return -ENOSPC;
    }

    inode->i_dirt = 1;
    if (!(name_block = bread(inode->i_dev, inode->i_zone[0]))) {
        iput(dir);
        inode->i_nlinks--;
        iput(inode);
        return -ERROR;
    }

    /* zone[0] 对应的 block 里面填充被链接到的文件路径 */
    i = 0;
    while (i < 1023 && (c = get_fs_byte(oldname++))) {
        name_block->b_data[i++] = c;
    }

    name_block->b_data[i] = 0;
    name_block->b_dirt = 1;
    brelse(name_block);

    inode->i_size = i;
    inode->i_dirt = 1;

    /* 确保 basename 不存在 */
    bh = find_entry(&dir, basename, namelen, &de);
    if (bh) {
        inode->i_nlinks--;
        iput(inode);
        brelse(bh);
        iput(dir);
        return -EEXIST;
    }

    /* 更新 directory entry */
    bh = add_entry(dir, basename, namelen, &de);
    if (!bh) {
        inode->i_nlinks--;
        iput(inode);
        iput(dir);
        return -ENOSPC;
    }

    de->inode = inode->i_num;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);
    iput(inode);
    return 0;
}

/**
 * @brief 创建硬链接
 *
 * @param oldname
 * @param newname
 * @return int
 */
int sys_link(const char *oldname, const char *newname)
{
    struct dir_entry *de;
    struct m_inode *oldinode, *dir;
    struct buffer_head *bh;
    const char *basename;
    int namelen;

    oldinode = namei(oldname);
    if (!oldinode) {
        return -ENOENT;
    }

    /* 不支持给目录创建硬链接 */
    if (S_ISDIR(oldinode->i_mode)) {
        iput(oldinode);
        return -EPERM;
    }

    /* 找到新文件路径对应的目录节点 */
    dir = dir_namei(newname, &namelen, &basename, NULL);
    if (!dir) {
        iput(oldinode);
        return -EACCES;
    }

    /* 没有文件名可不行.... */
    if (!namelen) {
        iput(oldinode);
        iput(dir);
        return -EPERM;
    }

    /* 不能跨文件系统创建硬链接 */
    if (dir->i_dev != oldinode->i_dev) {
        iput(dir);
        iput(oldinode);
        return -EXDEV;
    }

    /* 你得能写这个目录才能在里面创建一个硬链接 */
    if (!permission(dir, MAY_WRITE)) {
        iput(dir);
        iput(oldinode);
        return -EACCES;
    }

    /* 确认 dir 里面没有 basename */
    bh = find_entry(&dir, basename, namelen, &de);
    if (bh) {
        brelse(bh);
        iput(dir);
        iput(oldinode);
        return -EEXIST;
    }

    /* 把 basename 追加到 dir */
    bh = add_entry(dir, basename, namelen, &de);
    if (!bh) {
        iput(dir);
        iput(oldinode);
        return -ENOSPC;
    }

    de->inode = oldinode->i_num;
    bh->b_dirt = 1;
    brelse(bh);
    iput(dir);

    oldinode->i_nlinks++;
    oldinode->i_ctime = CURRENT_TIME; /* NOTICE: ctime 是这个 inode 自己的修改时间 */
    oldinode->i_dirt = 1;
    iput(oldinode);
    return 0;
}
