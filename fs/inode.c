/*
 *  linux/fs/inode.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <string.h>
#include <sys/stat.h>

#include <asm/system.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>

extern int *blk_size[];

/* inode 表 */
struct m_inode inode_table[NR_INODE] = {{0}};

static void read_inode(struct m_inode *inode);
static void write_inode(struct m_inode *inode);

/**
 * @brief 等待指定的 inode 可用
 *
 * 如果 inode 已被锁定, 则将当前任务置为不可中断的等待状态, 并添加到该 inode 的等待
 * 队列 i_wait 中. 直到该 inode 解锁并明确地唤醒本任务
 *
 * @param inode 被锁定的节点
 */
static inline void wait_on_inode(struct m_inode *inode)
{
    cli();

    while (inode->i_lock) {
        sleep_on(&inode->i_wait);
    }

    sti();
}

/**
 * @brief 对 inode 上锁(锁定指定的 inode )
 *
 * 如果 inode 已被锁定, 则将当前任务置为不可中断的等待状态, 并添加到该 inode 的等待队
 * 列 i_wait 中. 直到该 inode 解锁并明确地唤醒本任务, 然后对其上锁
 *
 * @param inode
 */
static inline void lock_inode(struct m_inode *inode)
{
    cli();

    /* 如果在锁定状态, 先睡觉 */
    while (inode->i_lock) {
        sleep_on(&inode->i_wait);
    }

    /* 别人释放了锁, 马上拿到 */
    inode->i_lock = 1;
    sti();
}

/**
 * @brief 对指定的 inode 解锁
 *
 * 复位 inode 的锁定标志, 并明确地唤醒等待在此 inode 等待队列 i_wait 上的所有进程
 *
 * @param inode
 */
static inline void unlock_inode(struct m_inode *inode)
{
    inode->i_lock = 0;

    /* 唤醒等待的进程 */
    wake_up(&inode->i_wait);
}

/**
 * @brief 释放设备 dev 在内存 inode 表中的所有 inode
 *
 * 扫描内存中的 inode 表数组, 如果是指定设备使用的 inode 就释放之
 *
 * @param dev
 */
void invalidate_inodes(int dev)
{
    int i;
    struct m_inode *inode;

    inode = 0 + inode_table; /* 让指针指向内存 inode 表数组首项 */
    for (i = 0; i < NR_INODE; i++, inode++) {
        /* 对每个 inode , 先等待该 inode 解锁可用(若目前正被上锁的话) */
        wait_on_inode(inode);

        /* 属于指定设备的 inode  */
        if (inode->i_dev == dev) {
            if (inode->i_count) {
                /* 理论上节点不该在有引用, 如果还有就报个警告 */
                printk("inode in use on removed disk\n\r");
            }

            /* 通过把 inode 的设备号字段 i_dev 置 0, 达到释放的效果 */
            inode->i_dev = inode->i_dirt = 0;
        }
    }
}

/**
 * @brief 同步所有 inode
 *
 * 把内存 inode 表中所有 inode 与设备上 inode 作同步操作
 */
void sync_inodes(void)
{
    int i;
    struct m_inode *inode;

    inode = 0 + inode_table;
    for (i = 0; i < NR_INODE; i++, inode++) {
        wait_on_inode(inode);                  /* 等 inode 解锁 */
        if (inode->i_dirt && !inode->i_pipe) { /* inode 已修改且不是管道节点 */
            write_inode(inode);
        }
    }
}

/**
 * @brief 查询或者建立 inode 和 block 之间的关系
 *
 * @param inode 文件的 inode 指针
 * @param block 文件对应的 block 序号
 * @param create 创建还是查询
 * @return int 返回 block 对应的设备逻辑 block 编号
 */
static int _bmap(struct m_inode *inode, int block, int create)
{
    struct buffer_head *bh;
    int i;

    if (block < 0) {
        panic("_bmap: block<0");
    }

    if (block >= 7 + 512 + 512 * 512) {
        panic("_bmap: block>big");
    }

    /* 如果 block 编号小于 7, 在 zone 区域即可放下 */
    if (block < 7) {
        /* 如果是建立关系, 并且 block 之前对应的设备逻辑块不存在 */
        if (create && !inode->i_zone[block]) {
            if ((inode->i_zone[block] = new_block(inode->i_dev))) {
                inode->i_ctime = CURRENT_TIME;
                inode->i_dirt = 1;
            }
        }

        return inode->i_zone[block];
    }

    /* 如果 block >= 7, 先把 zone 本身能放得下的 7 个逻辑块去掉 */
    block -= 7;

    /* 如果现在 block 小于 512, 因为一次间接块可以存放 512 个逻辑块
     * 所以只需要使用一次间接块即可存放 */
    if (block < 512) {
        /* 如果是保存, 但是现在还没有一次间接块, 创建这个一次间接块 */
        if (create && !inode->i_zone[7]) {
            if ((inode->i_zone[7] = new_block(inode->i_dev))) {
                inode->i_dirt = 1;
                inode->i_ctime = CURRENT_TIME;
            }
        }

        /* 一次间接块不存在, 报错 */
        if (!inode->i_zone[7]) {
            return 0;
        }

        /* 读取一次间接块 */
        if (!(bh = bread(inode->i_dev, inode->i_zone[7]))) {
            return 0;
        }

        /* 索引到 block 指定的位置上 */
        i = ((unsigned short *)(bh->b_data))[block];

        /* 如果是保存, 则创建对应的新块, 并更新一次间接块数据内容. 看这里可能
         * 不涉及到 inode 的 dirt, 只需要把一次间接块对应的缓冲区标识为脏就行了 */
        if (create && !i) {
            if ((i = new_block(inode->i_dev))) {
                ((unsigned short *)(bh->b_data))[block] = i;
                bh->b_dirt = 1;
            }
        }

        brelse(bh);
        return i;
    }

    /* block 编号过大, 需要使用二次间接块来保存 */

    block -= 512;

    /* 如果二次间接块还不存在, 创建它 */
    if (create && !inode->i_zone[8]) {
        if ((inode->i_zone[8] = new_block(inode->i_dev))) {
            inode->i_dirt = 1;
            inode->i_ctime = CURRENT_TIME;
        }
    }

    if (!inode->i_zone[8]) {
        return 0;
    }

    /* 读取二次间接块 */
    if (!(bh = bread(inode->i_dev, inode->i_zone[8]))) {
        return 0;
    }

    /* 找到 block 对应的一次间接块
     * 这里 `block >> 9` 是因为, 一个二次间接块的条目, 代表了 512 个一次间接块,
     * 这里实际上就是在 `block % 512`, 得到的是 block 所在的那个一次间接块的索引 */
    i = ((unsigned short *)bh->b_data)[block >> 9];

    /* 如果一次间接块不存在, 创建它 */
    if (create && !i) {
        if ((i = new_block(inode->i_dev))) {
            ((unsigned short *)(bh->b_data))[block >> 9] = i;
            bh->b_dirt = 1;
        }
    }

    brelse(bh);
    if (!i) {
        return 0;
    }

    /* 读出来一次间接块 */
    if (!(bh = bread(inode->i_dev, i))) {
        return 0;
    }

    /* 找到 block 对应的一次间接块索引 */
    i = ((unsigned short *)bh->b_data)[block & 511];

    /* 创建 block 和逻辑块的关联关系 */
    if (create && !i) {
        if ((i = new_block(inode->i_dev))) {
            ((unsigned short *)(bh->b_data))[block & 511] = i;
            bh->b_dirt = 1;
        }
    }

    brelse(bh);
    return i;
}

/**
 * @brief 取文件数据块block在设备上对应的逻辑块号
 *
 * TODO: 内存缺页那个地方有调用, 回过头去品味相关逻辑
 *
 * @param inode 文件的内存 inode 指针
 * @param block 文件中的数据块号
 * @return int 成功返回对应的逻辑块号, 否则返回0
 */
int bmap(struct m_inode *inode, int block)
{
    return _bmap(inode, block, 0);
}

/**
 * @brief 取文件数据块 block 在设备上对应的逻辑块号
 *
 * 如果对应的逻辑块不存在就创建一块并返回设备上对应的逻辑块号
 *
 * @param inode 文件的内存 inode 指针
 * @param block 文件中的数据块号
 * @return int 成功返回对应的逻辑块号, 否则返回0
 */
int create_block(struct m_inode *inode, int block)
{
    return _bmap(inode, block, 1);
}

/**
 * @brief 放回一个 inode
 *
 * 该函数主要用于把 inode 引用计数值递减 1, 并且若是管道 inode, 则唤醒等待的进程
 * 若是块设备文件 inode 则刷新设备. 并且若 inode 的链接计数为0, 则释放该 inode 占用
 * 的所有磁盘逻辑块, 并释放该 inode
 *
 * TODO: exit 的时候用到了, 回过头来看看
 *
 * @param inode
 */
void iput(struct m_inode *inode)
{
    if (!inode) {
        return;
    }

    /* 等 inode 空闲 */
    wait_on_inode(inode);
    if (!inode->i_count) {
        panic("iput: trying to free free inode");
    }

    /* 如果是一个 pipe 类型的 inode
     * 唤醒等待该管道的进程, 引用次数减 1, 如果彻底没有引用了, 释放管道占用的内存页面,
     * 并复位该节点的引用计数值/已修改标志/管道标志.
     *
     * 对于管道节点, i_size 存放着内存页地址. 参见 get_pipe_inode()
     */
    if (inode->i_pipe) {
        wake_up(&inode->i_wait);
        wake_up(&inode->i_wait2);
        if (--inode->i_count) {
            return;
        }

        free_page(inode->i_size);
        inode->i_count = 0;
        inode->i_dirt = 0;
        inode->i_pipe = 0;
        return;
    }

    /* TODO-DONE: 节点没有对应的设备, 什么情况会这样?
     * 答: 用于管道操作的 inode , 其 inode 的设备号为 0
     */
    if (!inode->i_dev) {
        inode->i_count--;
        return;
    }

    /* 块设备文件的 i 节点, 此时逻辑块字段 i_zone[0] 中是设备号, 刷新该设备
     * TODO: 找一下这个 i_zone[0] 保存设备号, 是在哪里操作的
     * TODO: 为啥不直接用 inode->i_dev ??? */
    if (S_ISBLK(inode->i_mode)) {
        sync_dev(inode->i_zone[0]);
        wait_on_inode(inode);
    }

repeat:
    if (inode->i_count > 1) {
        inode->i_count--;
        return;
    }

    /* 到这里一定有 inode->i_count == 1 */

    /* 已经没有文件目录引用这个文件了, 应该将 inode 删除掉
     * TODO: 回头再看 truncate, free_node 操作 */
    if (!inode->i_nlinks) {
        truncate(inode);

        /* free_inode 用于实际释放 inode 操作, 即复位 inode 对应的
         * inode 位图比特位, 清空 inode 结构内容 */
        free_inode(inode);
        return;
    }

    /* 如果节点脏, 需要把节点写回到磁盘 */
    if (inode->i_dirt) {
        write_inode(inode); /* we can sleep - so do again */
        wait_on_inode(inode);
        goto repeat;
    }

    /* 减少引用计数, i_count 只是一个在内存中的计数值, 不会写到磁盘里面
     *
     * 该 inode 的引用计数值 i_count 是 1, 链接数不为零, 并且内容没有被修改过,
     * 此时只要把 inode 引用计数递减1, 此时该 inode 的 i_count=0, 表示在内存里面已释放
     * 但是磁盘上还是存在的 */
    inode->i_count--;
    return;
}

/**
 * @brief 从 inode 表中获取一个空闲 inode 项
 *
 * 寻找引用计数 count=0 的 inode, 并将其写盘后清零, 引用计数被置 1, 返回其指针
 *
 * @return struct m_inode* 找到的节点
 */
struct m_inode *get_empty_inode(void)
{
    struct m_inode *inode;
    static struct m_inode *last_inode = inode_table;
    int i;

    do {
        inode = NULL;
        for (i = NR_INODE; i; i--) { /* i 只用来计数 */
            /* 找到了末尾, 从头重新开始 */
            if (++last_inode >= inode_table + NR_INODE) {
                last_inode = inode_table;
            }

            /* 如果当前节点的 i_count == 0, 说明节点可能是空闲的
             * 再配合上加上 dirt 和 lock 没有置位, 说明节点确实空闲 */
            if (!last_inode->i_count) {
                inode = last_inode;
                if (!inode->i_dirt && !inode->i_lock) {
                    break;
                }
            }
        }

        /* 没找到, 打印全部的 inode 信息, 然后退出 */
        if (!inode) {
            for (i = 0; i < NR_INODE; i++) {
                printk("%04x: %6d\t", inode_table[i].i_dev, inode_table[i].i_num);
            }

            panic("No free inodes in mem");
        }

        /* 上面拿到的 inode 可能是上锁的, 那就需要等待 inode 空闲 */
        wait_on_inode(inode);

        /* 等待过程弄脏了的话, 就写 inode 到磁盘, 然后再等 */
        while (inode->i_dirt) {
            write_inode(inode);
            wait_on_inode(inode);
        }

        /* dirt 不脏了, 但是下面还有 while 条件判断, 如果引用计数现在大于 0 了,
         * 也说明在别的地方被用了, 这时候还要再次重新找 */
    } while (inode->i_count);

    /* 到这里找到了真的空闲的节点项 */

    /* 清空 inode 数据 */
    memset(inode, 0, sizeof(*inode));

    /* 引用数记 1 */
    inode->i_count = 1;

    return inode;
}

/**
 * @brief 获取管道节点
 *
 * 首先扫描 inode 表, 寻找一个空闲 inode 项, 然后取得一页空闲内存供管道使用. 然后将得
 * 到的 inode 的引用计数置为2(读者和写者), 初始化管道头和尾, 置 inode 的管道类型表示
 *
 * @return struct m_inode* 返回节点指针, 失败返回 NULL
 */
struct m_inode *get_pipe_inode(void)
{
    struct m_inode *inode;

    /* 先拿到一个空闲的 inode */
    if (!(inode = get_empty_inode())) {
        return NULL;
    }

    /* 把 pipe 对应的物理页的地址, 保存在 i_size 里面  */
    if (!(inode->i_size = get_free_page())) {
        inode->i_count = 0;
        return NULL;
    }

    inode->i_count = 2; /* sum of readers/writers */

    /* 复位管道头尾指针, zone[0] 是头指针, zone[1] 是尾指针 */
    PIPE_HEAD(*inode) = PIPE_TAIL(*inode) = 0;
    inode->i_pipe = 1; /* 置节点为管道使用的标志 */

    return inode;
}

/**
 * @brief 从设备上读取指定节点号的 inode 结构内容到内存 inode 表中, 并且返回该 inode 指针
 *
 * TODO: 确认是不是, iget 引用加一, iput 引用减一
 *
 * @param dev 设备号
 * @param nr  inode 号
 * @return struct m_inode* 返回该 inode 指针
 */
struct m_inode *iget(int dev, int nr)
{
    struct m_inode *inode, *empty;

    if (!dev) {
        panic("iget with dev==0");
    }

    empty = get_empty_inode(); /* 找一个空闲节点 */
    inode = inode_table;       /* 在 inode 表中查询 (dev, nr) 对应的节点 */
    while (inode < NR_INODE + inode_table) {
        if (inode->i_dev != dev || inode->i_num != nr) {
            inode++;
            continue;
        }

        /* 老套路, 等待 + 重新判断 */
        wait_on_inode(inode);
        if (inode->i_dev != dev || inode->i_num != nr) {
            inode = inode_table;
            continue;
        }

        inode->i_count++; /* 节点引用 +1 */

        /* 某个文件系统挂载在这个 inode
         * 真实要操作的目标应该是挂载的这个文件系统对应的设备和根节点编号 */
        if (inode->i_mount) {
            int i;

            /* 找到挂载的文件系统的超级块 */
            for (i = 0; i < NR_SUPER; i++) {
                if (super_block[i].s_imount == inode) {
                    break;
                }
            }

            if (i >= NR_SUPER) {
                printk("Mounted inode hasn't got sb\n");
                if (empty) {
                    iput(empty);
                }

                return inode;
            }

            iput(inode); /* inode 不是我们最终的处理对象, 把它放回去 */
            dev = super_block[i].s_dev;
            nr = ROOT_INO;
            inode = inode_table;
            continue;
        }

        if (empty) {
            /* empty 是拿来备用的, 现在找到了正经目标节点, 这个 empty 就用不上了 */
            iput(empty);
        }

        return inode;
    }

    if (!empty) {
        return (NULL);
    }

    /* 如果在 inode 表里面找不到相关数据, 那么尝试把这个 inode 节点读取到内存 */

    inode = empty;
    inode->i_dev = dev;
    inode->i_num = nr;
    read_inode(inode);
    return inode;
}

/**
 * @brief 读取指定 inode 信息
 *
 * 从设备上读取含有指定 inode 信息的 inode 盘块, 然后复制到指定的 inode 结构中. 为了确定 inode
 * 所在的设备逻辑块号(或缓冲块), 必须首先读取相应设备上的超级块, 以获取用于计算逻辑块号的
 * 每块 inode 数信息 INODES_PER_BLOCK. 在计算出 inode 所在的逻辑块号后, 就把该逻辑块读入
 * 缓冲块中. 然后把缓冲块中相应位置处的 inode 内容复制到参数指定的位置处
 *
 * @param inode
 */
static void read_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;
    int block;

    /* 锁定节点, 然后尝试找到超级块 */
    lock_inode(inode);
    if (!(sb = get_super(inode->i_dev))) {
        panic("trying to read inode without dev");
    }

    /* 计算出 inode 对应的具体的逻辑块, 然后把它读入到内存里面 */
    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks + (inode->i_num - 1) / INODES_PER_BLOCK;
    if (!(bh = bread(inode->i_dev, block))) {
        panic("unable to read i-node block");
    }

    /* 把刚读到的数据拷贝到 inode 节点 */
    *(struct d_inode *)inode =
        ((struct d_inode *)bh->b_data)[(inode->i_num - 1) % INODES_PER_BLOCK];

    brelse(bh);

    /* 如果 inode 对应的是一个块设备文件, i_zone[0] 是设备号
     * 找到在 blk_size 里面配置的块设备数量, 就可以知道这个块设备有多大 */
    if (S_ISBLK(inode->i_mode)) {
        int i = inode->i_zone[0];
        if (blk_size[MAJOR(i)]) {
            inode->i_size = 1024 * blk_size[MAJOR(i)][MINOR(i)];
        } else {
            inode->i_size = 0x7fffffff;
        }
    }

    unlock_inode(inode);
}

/**
 * @brief 将 inode 信息写入缓冲区中
 *
 * 该函数把参数指定的 inode 写入缓冲区相应的缓冲块中, 待缓冲区刷新时会写入盘中
 * 为了确定 inode 所在的设备逻辑块号(或缓冲块), 必须首先读取相应设备上的超级块, 以获取
 * 用于计算逻辑块号的每块 inode 数信息 INODES_PER_BLOCK.  在计算出 inode 所在的逻辑块
 * 号后, 就把该逻辑块读入一缓冲块中. 然后把 inode 内容复制到缓冲块的相应位置处
 *
 * @param inode
 */
static void write_inode(struct m_inode *inode)
{
    struct super_block *sb;
    struct buffer_head *bh;
    int block;

    /* 锁定该 inode
     * 如果该 inode 没有被修改过或者该 inode 的设备号等于零, 无需处理写操作直接退出 */
    lock_inode(inode);
    if (!inode->i_dirt || !inode->i_dev) {
        unlock_inode(inode);
        return;
    }

    /* 该 inode 的超级块 */
    if (!(sb = get_super(inode->i_dev))) {
        panic("trying to write inode without device");
    }

    /* inode 所在的设备逻辑块号 = (启动块 + 超级块) + inode 位图占用的块数 + 逻辑块位图占用的块数 + (inode号 - 1)/每块含有的 inode 数
     * inode号 - 1, 是因为 inode 虽然是从 0 开始编号, 但是 0 没留没用, 事实上是从 1 开始编号的 */
    block = 2 + sb->s_imap_blocks + sb->s_zmap_blocks + (inode->i_num - 1) / INODES_PER_BLOCK;
    if (!(bh = bread(inode->i_dev, block))) {
        panic("unable to read i-node block");
    }

    /* 更新 inode 信息 */
    ((struct d_inode *)bh->b_data)[(inode->i_num - 1) % INODES_PER_BLOCK] =
        *(struct d_inode *)inode;

    bh->b_dirt = 1; /* 缓冲区已修改标志, sys_sync 或者 getblk 之类的函数会帮忙数据刷写到磁盘设备 */
    inode->i_dirt = 0; /* inode 和 buffer_head 一样了, 因此复位 inode 的 dirt  */

    brelse(bh); /* relse bh 不耽误未来它被 sync 到磁盘 */
    unlock_inode(inode);
}
