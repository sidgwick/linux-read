/*
 *  linux/fs/super.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * super.c contains code to handle the super-block tables.
 */
#include <asm/system.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#include <errno.h>
#include <sys/stat.h>

int sync_dev(int dev);        /* 对指定设备执行高速缓冲与设备上数据的同步操作 */
void wait_for_keypress(void); /* 等待击键 */

/**
 * @brief 如果地址 addr 的 bitnr 位置位, 返回 1, 否则返回 0
 *
 * 此宏取名 is_set_bit 更恰当
 *
 * set_bit uses setb, as gas doesn't recognize setc
 */
#define set_bit(bitnr, addr)                                                                       \
    ({                                                                                             \
        register int __res __asm__("ax");                                                          \
        __asm__(                                                                                   \
            "bt %2, %3"                                                                            \
            "setb %%al" /* Set if Below 在进位标志 CF 置位时将目标操作数设置为 1，否则设置为 0 */  \
            : "=a"(__res)                                                                          \
            : "a"(0), "r"(bitnr), "m"(*(addr)));                                                   \
        __res;                                                                                     \
    })

/* 超级块结构表数组 */
struct super_block super_block[NR_SUPER];
/* this is initialized in init/main.c */
int ROOT_DEV = 0;

/**
 * @brief 锁定超级块
 *
 * 如果超级块已被锁定, 则将当前任务置为不可中断的等待状态, 并添加到该超级块等待队列 s_wait 中.
 * 直到该超级块解锁并明确地唤醒本任务. 然后对其上锁
 *
 * @param sb
 */
static void lock_super(struct super_block *sb)
{
    cli();
    while (sb->s_lock) {
        sleep_on(&(sb->s_wait));
    }

    sb->s_lock = 1;
    sti();
}

/**
 * @brief 对指定超级块解锁
 *
 * 复位超级块的锁定标志, 并明确地唤醒等待在此超级块等待队列 s_wait 上的所有进程
 * 如果使用 ulock_super 这个名称则可能更妥帖
 *
 * @param sb
 */
static void free_super(struct super_block *sb)
{
    cli();
    sb->s_lock = 0;
    wake_up(&(sb->s_wait));
    sti();
}

/**
 * @brief 睡眠等待超级块解锁
 *
 * 如果超级块已被锁定, 则将当前任务置为不可中断的等待状态, 并添加到该
 * 超级块的等待队列 s_wait 中. 直到该超级块解锁并明确地唤醒本任务
 *
 * @param sb
 */
static void wait_on_super(struct super_block *sb)
{
    cli();

    while (sb->s_lock) {
        sleep_on(&(sb->s_wait));
    }

    sti();
}

/**
 * @brief 取指定设备的超级块
 *
 * 在超级块表(数组)中搜索指定设备 dev 的超级块结构信息
 *
 * @param dev
 * @return struct super_block* 找到则返回超级块的指针, 否则返回空指针
 */
struct super_block *get_super(int dev)
{
    struct super_block *s;

    if (!dev) {
        return NULL;
    }

    /* 从超级快数组开始的位置查找 */
    s = 0 + super_block;

    while (s < NR_SUPER + super_block) {
        if (s->s_dev == dev) {
            /* 先等待该超级块解锁(若已被其他进程上锁的话) */
            wait_on_super(s);

            /* 在等待期间, 该超级块项有可能被其他设备使用, 因此等待返回之后需
             * 再判断一次是否是指定设备的超级块, 如果是则返回该超级块的指针 */
            if (s->s_dev == dev) {
                return s;
            }

            /* 重新对超级块数组再搜索一遍 */
            s = 0 + super_block;
        } else {
            s++;
        }
    }

    return NULL;
}

/**
 * @brief 释放(放回)指定设备的超级块
 *
 * 释放设备所使用的超级块数组项(置 s_dev=0), 并释放该设备 inode 位图和逻辑块位图所
 * 占用的高速缓冲块. 如果超级块对应的文件系统是根文件系统, 或者其某个 inode 上已经安
 * 装有其他的文件系统, 则不能释放该超级块.
 *
 * @param dev
 */
void put_super(int dev)
{
    struct super_block *sb;
    int i;

    /* 不允许释放根文件系统设备 */
    if (dev == ROOT_DEV) {
        printk("root diskette changed: prepare for armageddon\n\r");
        return;
    }

    /* 找到对应的超级块 */
    if (!(sb = get_super(dev))) {
        return;
    }

    /* 要先卸载掉挂载才能释放超级块 */
    if (sb->s_imount) {
        printk("Mounted disk changed - tssk, tssk\n\r");
        return;
    }

    lock_super(sb); /* 锁定该超级块 */
    sb->s_dev = 0;  /* 置超级块对应的设备号字段 s_dev 为 0 */

    /* 释放 inode 位图缓冲块 */
    for (i = 0; i < I_MAP_SLOTS; i++) {
        brelse(sb->s_imap[i]);
    }

    /* 释放 zone 位图缓冲块 */
    for (i = 0; i < Z_MAP_SLOTS; i++) {
        brelse(sb->s_zmap[i]);
    }

    free_super(sb); /* 解锁超级块 */
    return;
}

/**
 * @brief 读取指定设备的超级块
 *
 * @param dev
 * @return struct super_block* 返回超级块指针
 */
static struct super_block *read_super(int dev)
{
    struct super_block *s;
    struct buffer_head *bh;
    int i, block;

    if (!dev) {
        return NULL;
    }

    /* dev 上的文件系统超级块已经在超级块表中, 直接使用即可 */
    check_disk_change(dev);
    if (s = get_super(dev)) {
        return s;
    }

    /* 在超级块表里面, 找一个空闲的超级块 */
    for (s = 0 + super_block;; s++) {
        if (s >= NR_SUPER + super_block) {
            return NULL;
        }

        if (!s->s_dev) {
            break;
        }
    }

    /* 生成 dev 对应的文件系统超级块 */
    s->s_dev = dev;
    s->s_isup = NULL;
    s->s_imount = NULL;
    s->s_time = 0;
    s->s_rd_only = 0;
    s->s_dirt = 0;

    lock_super(s);

    /* 读取磁盘上的第二个区块(区块 #1), 这个区块对应的内容是超级块 */
    if (!(bh = bread(dev, 1))) {
        s->s_dev = 0;
        free_super(s);
        return NULL;
    }

    /* 拷贝缓冲区的内容 */
    *((struct d_super_block *)s) = *((struct d_super_block *)bh->b_data);

    brelse(bh); /* 释放掉磁盘读缓冲区 */

    /* 检查超级块的合法性 */
    if (s->s_magic != SUPER_MAGIC) {
        s->s_dev = 0;
        free_super(s);
        return NULL;
    }

    /* inode 位图信息置空 */
    for (i = 0; i < I_MAP_SLOTS; i++) {
        s->s_imap[i] = NULL;
    }

    /* zone 位图信息置空 */
    for (i = 0; i < Z_MAP_SLOTS; i++) {
        s->s_zmap[i] = NULL;
    }

    /* 读取 inode 位图区块 */
    block = 2;
    for (i = 0; i < s->s_imap_blocks; i++) {
        if (s->s_imap[i] = bread(dev, block)) {
            block++;
        } else {
            break;
        }
    }

    /* 读取 zone 位图区块 */
    for (i = 0; i < s->s_zmap_blocks; i++) {
        if (s->s_zmap[i] = bread(dev, block)) {
            block++;
        } else {
            break;
        }
    }

    /* 磁盘上内容如下:
     *  - #1 启动区块
     *  - #2 超级块
     *  - s_imap_blocks 个 inode 节点位图区块
     *  - s_zmap_blocks 个 zone 位图区块
     *  - 正经数据区块
     *
     * 因此下面有 `block != ...` 这个合法性检查, 满足的话说明文件系统不对, 直接释放资源退出 */
    if (block != 2 + s->s_imap_blocks + s->s_zmap_blocks) {
        for (i = 0; i < I_MAP_SLOTS; i++) {
            brelse(s->s_imap[i]);
        }

        for (i = 0; i < Z_MAP_SLOTS; i++) {
            brelse(s->s_zmap[i]);
        }

        s->s_dev = 0;
        free_super(s);
        return NULL;
    }

    /* 由于对于申请空闲 inode 的函数来讲, 如果设备上所有的 inode 已经全被使用, 则查找函数
     * 会返回 0 值. 因此 0 号 inode 是不能用的, 所以这里将位图中第 1 块的最低比特位设置为
     * 1, 以防止文件系统分配 0 号 inode. 同样的道理, 也将逻辑块位图的最低位设置为 1
     *
     * TODO: 查找函数指的是那个查找函数? */
    s->s_imap[0]->b_data[0] |= 1;
    s->s_zmap[0]->b_data[0] |= 1;

    free_super(s);
    return s;
}

/**
 * @brief 卸载文件系统(系统调用)
 *
 * 该函数首先根据参数给出的块设备文件名获得设备号, 然后复位文件系统超级块中的相应字
 * 段, 释放超级块和位图占用的缓冲块, 最后对该设备执行高速缓冲与设备上数据的同步操作
 *
 * @param dev_name 文件系统所在设备的设备文件名
 * @return int 操作成功返回0, 否则返回出错码
 */
int sys_umount(char *dev_name)
{
    struct m_inode *inode;
    struct super_block *sb;
    int dev;

    /* 设备文件名找到对应的 inode */
    if (!(inode = namei(dev_name))) {
        return -ENOENT;
    }

    /* 设备文件所定义设备的设备号是保存在其 inode 的 i_zone[0] 中
     * 参考 namei.c#sys_mknod */
    dev = inode->i_zone[0];

    /* TODO: 了解一下 iput 是干什么的? */

    /* dev_name 对应的一定是个块设备文件 inode */
    if (!S_ISBLK(inode->i_mode)) {
        iput(inode);
        return -ENOTBLK;
    }

    iput(inode);

    /* 根文件系统不能卸载 */
    if (dev == ROOT_DEV) {
        return -EBUSY;
    }

    /* 找到设备对应的超级块, 并检查是否已经挂载 */
    if (!(sb = get_super(dev)) || !(sb->s_imount)) {
        return -ENOENT;
    }

    /* 超级块所指明的被安装到的 inode 并没有置位其安装标志 i_mount, 则显示警告信息 */
    if (!sb->s_imount->i_mount) {
        printk("Mounted inode has i_mount=0\n");
    }

    /* 查找 inode 表, 看看是否有进程在使用该设备上的文件, 如果有则返回忙出错码 */
    for (inode = inode_table + 0; inode < inode_table + NR_INODE; inode++) {
        if (inode->i_dev == dev && inode->i_count) {
            return -EBUSY;
        }
    }

    /* 正式卸载 */
    sb->s_imount->i_mount = 0;
    iput(sb->s_imount); /* 放回挂载点 inode */
    sb->s_imount = NULL;
    iput(sb->s_isup); /* 放回根目录 inode */
    sb->s_isup = NULL;
    put_super(dev);
    sync_dev(dev);
    return 0;
}

/**
 * @brief 安装文件系统(系统调用)
 *
 * 将被加载的地方必须是一个目录名, 并且对应的 inode 没有被其他程序占用
 *
 * @param dev_name 设备文件名
 * @param dir_name 安装到的目录名
 * @param rw_flag 被安装文件系统的可读写标志
 * @return int 操作成功返回0, 否则返回出错号
 */
int sys_mount(char *dev_name, char *dir_name, int rw_flag)
{
    struct m_inode *dev_i, *dir_i;
    struct super_block *sb;
    int dev;

    /* 取设备的 inode */
    if (!(dev_i = namei(dev_name))) {
        return -ENOENT;
    }

    dev = dev_i->i_zone[0];
    if (!S_ISBLK(dev_i->i_mode)) {
        iput(dev_i);
        return -EPERM;
    }

    iput(dev_i);

    /* 取目录的 inode */
    if (!(dir_i = namei(dir_name))) {
        return -ENOENT;
    }

    /* inode 不是仅在这里引用(引用计数不为1), 或者该 inode 的节点号是根文件系统的节点号 1 */
    if (dir_i->i_count != 1 || dir_i->i_num == ROOT_INO) {
        iput(dir_i);
        return -EBUSY;
    }

    /* 挂载点必须是一个目录 */
    if (!S_ISDIR(dir_i->i_mode)) {
        iput(dir_i);
        return -EPERM;
    }

    /* 找到 dev 的超级块 */
    if (!(sb = read_super(dev))) {
        iput(dir_i);
        return -EBUSY;
    }

    /* 已经挂载了, 报错 */
    if (sb->s_imount) {
        iput(dir_i);
        return -EBUSY;
    }

    /* 目录已经被其他问价系统挂载, 报错 */
    if (dir_i->i_mount) {
        iput(dir_i);
        return -EPERM;
    }

    /* NOTE! we don't iput(dir_i), we do that in umount
     * dir 挂载了文件系统之后, 相当于目录里面被写了数据, 因此 dirt 置位 */
    sb->s_imount = dir_i;
    dir_i->i_mount = 1;
    dir_i->i_dirt = 1;

    return 0;
}

/**
 * @brief 安装根文件系统
 *
 * 该函数属于系统初始化操作的一部分, 在系统开机进行初始化设置时调用(hd.c#sys_setup)
 *
 * TODO: 在块设备 hd.c 里面有使用, 联系起来看看
 */
void mount_root(void)
{
    int i, free;
    struct super_block *p;
    struct m_inode *mi;

    if (32 != sizeof(struct d_inode)) {
        panic("bad i-node size");
    }

    /* 初始化文件表数组 file_table */
    for (i = 0; i < NR_FILE; i++) {
        file_table[i].f_count = 0;
    }

    if (MAJOR(ROOT_DEV) == 2) {
        /* 等待按键 ENTRT
         * TODO: 这里似乎并仅仅是 ENTER, 任意键都会触发继续执行 */
        printk("Insert root floppy and press ENTER");
        wait_for_keypress();
    }

    /* 初始化超级块表(数组) */
    for (p = &super_block[0]; p < &super_block[NR_SUPER]; p++) {
        p->s_dev = 0;
        p->s_lock = 0;
        p->s_wait = NULL;
    }

    /* 读取根文件系统超级块 */
    if (!(p = read_super(ROOT_DEV))) {
        panic("Unable to mount root");
    }

    /* 取得文件系统根 inode */
    if (!(mi = iget(ROOT_DEV, ROOT_INO))) {
        panic("Unable to read root i-node");
    }

    /* iget 会增加一次引用, 但是这里下面实际上一共引用了 4 次, 因此要再加上 3
     * 本函数实在初始化的时候调用, 因此 current 一定是 idle 进程  */
    mi->i_count += 3; /* NOTE! it is logically used 4 times, not 1 */

    p->s_isup = p->s_imount = mi; /* 2 次引用 */
    current->pwd = mi;            /* 一次引用 */
    current->root = mi;           /* 一次引用 */

    /* 统计并显示出根文件系统上的可用资源(空闲块数和空闲 inode 数) */

    free = 0;
    i = p->s_nzones;
    while (--i >= 0) {
        /* 关于 8191=0x1FFF:
         *   实际上就是在计算 (i % 8192), 因为 block 在内存中是按照区块组织在不同的
         *   buffer_header 里面的, 这样处理之后 set_bit 就能在一个单独的缓冲区块里面做搜索
         *
         * 关于 i>>13:
         *   因为一个 block 是 1024KB, 也即 `1024 * 8 = 2^13` 个 bits,
         *   这里 i/(2^13) 就能算出, i 对应的 zone 在那个 zone 位图字节里面  */
        if (!set_bit(i & 8191, p->s_zmap[i >> 13]->b_data)) {
            free++;
        }
    }

    printk("%d/%d free blocks\n\r", free, p->s_nzones);

    free = 0;
    i = p->s_ninodes + 1;
    while (--i >= 0) {
        if (!set_bit(i & 8191, p->s_imap[i >> 13]->b_data)) {
            free++;
        }
    }

    printk("%d/%d free inodes\n\r", free, p->s_ninodes);
}
