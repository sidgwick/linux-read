/*
 * This file has definitions for some important file table
 * structures etc.
 *
 * 本文件含有某些重要文件表结构的定义等
 */

#ifndef _FS_H
#define _FS_H

#include <sys/types.h>

/* 以下这些是主设备号
 *
 * devices are as follows: (same as minix, so we can use the minix
 * file system. These are major numbers.)
 *
 * 0 - unused (nodev)
 * 1 - /dev/mem
 * 2 - /dev/fd
 * 3 - /dev/hd
 * 4 - /dev/ttyx
 * 5 - /dev/tty
 * 6 - /dev/lp
 * 7 - unnamed pipes
 */

#define IS_SEEKABLE(x) ((x) >= 1 && (x) <= 3) // 判断设备是否是可以寻找定位的

#define READ 0
#define WRITE 1
#define READA 2  /* read-ahead(预读) - don't pause */
#define WRITEA 3 /* write-ahead(预写) - silly, but somewhat useful */

void buffer_init(long buffer_end); // 高速缓冲区初始化函数

#define MAJOR(a) (((unsigned)(a)) >> 8) // 取高字节 (主设备号)
#define MINOR(a) ((a) & 0xff)           // 取低字节 (次设备号)

#define NAME_LEN 14 // 名字长度值
#define ROOT_INO 1  // Minix 文件系统的根目录是 #1 inode

#define I_MAP_SLOTS 8      // inode 位图槽数
#define Z_MAP_SLOTS 8      // 逻辑块(区段块)位图槽数
#define SUPER_MAGIC 0x137F // 文件系统魔数

#define NR_OPEN 20            // 进程最多打开文件数
#define NR_INODE 64           // 系统同时最多使用 inode 个数
#define NR_FILE 64            // 系统最多文件个数 (文件数组项数)
#define NR_SUPER 8            // 系统所含超级块个数(超级块数组项数)
#define NR_HASH 307           // 缓冲区 Hash 表数组项数值
#define NR_BUFFERS nr_buffers // 系统所含缓冲块个数. 初始化后不再改变
#define BLOCK_SIZE 1024       // 数据块长度(字节值)
#define BLOCK_SIZE_BITS 10    // 数据块长度所占比特位数

#ifndef NULL
#define NULL ((void *)0)
#endif

// 每个逻辑块可存放的 inode 数
#define INODES_PER_BLOCK ((BLOCK_SIZE) / (sizeof(struct d_inode)))

// 每个逻辑块可存放的目录项数
#define DIR_ENTRIES_PER_BLOCK ((BLOCK_SIZE) / (sizeof(struct dir_entry)))

// 管道头/管道尾/管道大小/管道空？/管道满？/管道头指针递增
// 管道自身是一个环形缓冲区, zone[0] 是 HEAD 游标, zone[1] 是 tail 游标
#define PIPE_READ_WAIT(inode) ((inode).i_wait)                                     /* 读等待 */
#define PIPE_WRITE_WAIT(inode) ((inode).i_wait2)                                   /* 写等待 */
#define PIPE_HEAD(inode) ((inode).i_zone[0])                                       /* HEAD 游标 */
#define PIPE_TAIL(inode) ((inode).i_zone[1])                                       /* TAIL 游标 */
#define PIPE_SIZE(inode) ((PIPE_HEAD(inode) - PIPE_TAIL(inode)) & (PAGE_SIZE - 1)) /* 缓冲区大小 */
#define PIPE_EMPTY(inode) (PIPE_HEAD(inode) == PIPE_TAIL(inode))                   /* 缓冲区空 */
#define PIPE_FULL(inode) (PIPE_SIZE(inode) == (PAGE_SIZE - 1))                     /* 缓冲区满 */

#define NIL_FILP ((struct file *)0) // 空文件结构指针
#define SEL_IN 1
#define SEL_OUT 2
#define SEL_EX 4

// 块缓冲区
typedef char buffer_block[BLOCK_SIZE];

// 缓冲块头数据结构(极为重要!!!)
// 在程序中常用 bh 来表示 buffer_head 类型的缩写
struct buffer_head {
    char *b_data; /* pointer to data block (1024 bytes), 指针 */

    unsigned long b_blocknr;    /* block number, 块号 */
    unsigned short b_dev;       /* device (0 = free), 数据源的设备号 */
    unsigned char b_uptodate;   /* 更新标志: 表示数据是否已更新 */
    unsigned char b_dirt;       /* 0-clean,1-dirty, 修改标志:0未修改,1已修改 */
    unsigned char b_count;      /* users using this block, 使用的用户数 */
    unsigned char b_lock;       /* 0 - ok, 1 -locked, 缓冲区是否被锁定 */
    struct task_struct *b_wait; /* 指向等待该缓冲区解锁的任务 */

    /* 以下四个指针用于缓冲区的管理 */
    struct buffer_head *b_prev;      /* hash 队列上前一块 */
    struct buffer_head *b_next;      /* hash 队列上下一块 */
    struct buffer_head *b_prev_free; /* free_list 前一块 */
    struct buffer_head *b_next_free; /* free_list 下一块 */
};

/**
 * @brief 磁盘上的索引节点(inode)数据结构
 *
 * inode 被储存在磁盘的 inode 块(inodes zone)中, 并按排列顺序被编号, 其使用状况
 * 存储在磁盘的 inode 位图块(inodes bitmap)中. 一个 inode 对应磁盘里的一个文件,
 * 储存着文件的元信息, 值得注意的是 inode 并未储存该文件的文件名.
 *
 * zone 中储存了虚拟块的号码, 这些号码指向磁盘的数据块(data zone), 这些块中直接或
 * 间接储存了文件的数据.
 *
 * 在 Linux 下创建某个文件的硬连接, 其实就是为目标目录增加对指定文件对应的 inode 的引用,
 * 注意 inode 号只在一个磁盘中唯一, 所以硬链接无法跨磁盘.
 *
 * nlinks 元素指示了有多少个目录引用了这个文件, 当删除一个文件时, 对应的 inode 的引用数就
 * 会减一, 当引用数为 0 时, 该节点就可以从磁盘上被释放了.
 *
 * zone[0] 到 zone[6] 用于存放文件开始的 7 个磁盘块号, 称为直接块. 若文件长度小于等于 7K 字节,
 * 则根据其 inode 可以很快就找到它所使用的盘块.
 * 若文件大一些时, 就需要用到一次间接块(zone[7])了, 这个盘块中存放着附加的盘块号, 对于 MINIX 文件
 * 系统一个盘块中可以存放(1024/2=512)个盘块号, 因此可以再寻址 512 个盘块
 * 若文件还要大, 则需要使用二次间接盘块(zone[8]). 二次间接块的存放的是和 zone[7] 一样的的一次间接块,
 * 因此使用二次间接盘块可以寻址 512*512 个盘块.
 *
 * 因此对于 minix 文件系统来说, 一个文件最大是 (7+512+512*512)*1024 = 256MB
 */
struct d_inode {
    unsigned short i_mode;    // 文件类型和属性(rwx位)
    unsigned short i_uid;     // 用户id (文件拥有者标识符)
    unsigned long i_size;     // 文件大小 (字节数)
    unsigned long i_time;     // 修改时间 (自1970.1.1:0算起, 秒)
    unsigned char i_gid;      // 组id(文件拥有者所在的组)
    unsigned char i_nlinks;   // 链接数 (多少个文件目录项指向该inode )
    unsigned short i_zone[9]; // 直接(0-6)、间接(7)或双重间接(8)逻辑块号, zone 可译成区段, 或逻辑块
};

// 这是在内存中的 inode 结构, 前 7 项与 d_inode 完全一样
struct m_inode {
    unsigned short i_mode;    // 文件类型和属性(rwx位)
    unsigned short i_uid;     // 用户id (文件拥有者标识符)
    unsigned long i_size;     // 文件大小 (字节数)
    unsigned long i_mtime;    // 修改时间 (自1970.1.1:0算起, 秒)
    unsigned char i_gid;      // 组id(文件拥有者所在的组)
    unsigned char i_nlinks;   // 链接数 (多少个文件目录项指向该inode )
    unsigned short i_zone[9]; // 直接(0-6)、间接(7)或双重间接(8)逻辑块号, zone 可译成区段, 或逻辑块

    /* these are in memory also */
    struct task_struct *i_wait;  // 等待该inode 的进程
    struct task_struct *i_wait2; /* for pipes */
    unsigned long i_atime;       // 最后访问时间
    unsigned long i_ctime;       // inode 自身修改时间
    unsigned short i_dev;        // inode 所在的设备号
    unsigned short i_num;        // inode 号, TODO: 从 1 开始计数? - 从 0 开始计数的
    unsigned short i_count;      // inode 被使用的次数, 0 表示空闲
    unsigned char i_lock;        // 锁定标志
    unsigned char i_dirt;        // 已修改(脏)标志
    unsigned char i_pipe;        // 管道标志
    unsigned char i_mount;       // 安装标志
    unsigned char i_seek;        // 搜寻标志(lseek时)
    unsigned char i_update;      // 更新标志
};

// 文件结构(用于在文件句柄与inode 之间建立关系)
struct file {
    unsigned short f_mode;   // 文件操作模式 (RW位)
    unsigned short f_flags;  // 文件打开和控制的标志
    unsigned short f_count;  // 对应文件引用计数值
    struct m_inode *f_inode; // 指向对应inode
    off_t f_pos;             // 文件位置 (读写偏移值)
};

/**
 * @brief 内存中磁盘超级块结构
 */
struct super_block {
    unsigned short s_ninodes;       // 节点数
    unsigned short s_nzones;        // 逻辑块数
    unsigned short s_imap_blocks;   // inode 位图所占用的数据块数
    unsigned short s_zmap_blocks;   // 逻辑块位图所占用的数据块数
    unsigned short s_firstdatazone; // 第一个数据逻辑块
    unsigned short s_log_zone_size; // 以 2 为底 log(数据块数/逻辑块)
    unsigned long s_max_size;       // 文件最大长度
    unsigned short s_magic;         // 文件系统魔数

    /* These are only in memory */
    struct buffer_head *s_imap[8]; // inode 位图缓冲块指针数组(占用8块, 可表示64M)
    struct buffer_head *s_zmap[8]; // 逻辑块位图缓冲块指针数组 (占用8块)
    unsigned short s_dev;          // 超级块所在的设备号
    struct m_inode *s_isup;        // 被安装的文件系统根目录的 inode. (isup-super i)
    struct m_inode *s_imount;      // 被安装到的 inode
    unsigned long s_time;          // 修改时间
    struct task_struct *s_wait;    // 等待该超级块的进程
    unsigned char s_lock;          // 被锁定标志
    unsigned char s_rd_only;       // 只读标志
    unsigned char s_dirt;          // 已修改(脏)标志
};

/**
 * @brief 磁盘中超级块结构
 *
 * 超级块(superblock)用来存放整个文件系统的元信息, 比如磁盘大小, inode 数目, 虚拟块数目, Magic nubmer 等
 */
struct d_super_block {
    unsigned short s_ninodes;       // 节点数
    unsigned short s_nzones;        // 逻辑块数
    unsigned short s_imap_blocks;   // inode 位图所占用的数据块数
    unsigned short s_zmap_blocks;   // 逻辑块位图所占用的数据块数
    unsigned short s_firstdatazone; // 第一个数据逻辑块
    unsigned short s_log_zone_size; // 以 2 为底 log(数据块数/逻辑块)
    unsigned long s_max_size;       // 文件最大长度
    unsigned short s_magic;         // 文件系统魔数
};

/**
 * @brief 文件目录项结构
 *
 * 目录被实现为一种特殊的文件, 目录的数据由一个或多个 dir_entry 结构组成.
 *
 * 第一个元素代表该目录下的文件的 inode 号, 第二个元素则是文件名, 文件的文件名储存
 * 在引用它的目录文件中, 这使得一个文件可以有多个名字.
 */
struct dir_entry {
    unsigned short inode; // inode 号
    char name[NAME_LEN];  // 文件名, 长度 NAME_LEN=14
};

extern struct m_inode inode_table[NR_INODE];     // 定义inode 表数组 (32项)
extern struct file file_table[NR_FILE];          // 文件表数组 (64项)
extern struct super_block super_block[NR_SUPER]; // 超级块数组 (8项)
extern struct buffer_head *start_buffer;         // 缓冲区起始内存位置
extern int nr_buffers;                           // 缓冲块数

// 磁盘操作函数原型

/* 检测驱动器中软盘是否改变 */
extern void check_disk_change(int dev);

/* 检测指定软驱中软盘更换情况. 如果软盘更换了则返回1, 否则返回0 */
extern int floppy_change(unsigned int nr);

/* 设置启动指定驱动器所需等待的时间 (设置等待定时器)  */
extern int ticks_to_floppy_on(unsigned int dev);

/* 启动指定驱动器 */
extern void floppy_on(unsigned int dev);

/* 关闭指定的软盘驱动器 */
extern void floppy_off(unsigned int dev);

// 以下是文件系统操作管理用的函数原型

/* 将inode 指定的文件截为0 */
extern void truncate(struct m_inode *inode);

/* 刷新inode 信息 */
extern void sync_inodes(void);

/* 等待指定的inode  */
extern void wait_on(struct m_inode *inode);

/* 逻辑块(区段, 磁盘块)位图操作. 取数据块block在设备上对应的逻辑块号 */
extern int bmap(struct m_inode *inode, int block);

/* 创建数据块block在设备上对应的逻辑块, 并返回在设备上的逻辑块号 */
extern int create_block(struct m_inode *inode, int block);

/* 获取指定路径名的inode 号 */
extern struct m_inode *namei(const char *pathname);

/* 取指定路径名的inode , 不跟随符号链接 */
extern struct m_inode *lnamei(const char *pathname);

/* 根据路径名为打开文件操作作准备 */
extern int open_namei(const char *pathname, int flag, int mode, struct m_inode **res_inode);

/* 释放一个inode (回写入设备) */
extern void iput(struct m_inode *inode);

/* 从设备读取指定节点号的一个inode  */
extern struct m_inode *iget(int dev, int nr);
/* 从inode 表(inode_table)中获取一个空闲inode 项 */
extern struct m_inode *get_empty_inode(void);
/* 获取 (申请一) 管道节点. 返回为inode 指针 (如果是NULL则失败)  */
extern struct m_inode *get_pipe_inode(void);
/* 在哈希表中查找指定的数据块. 返回找到块的缓冲头指针 */
extern struct buffer_head *get_hash_table(int dev, int block);
/* 从设备读取指定块 (首先会在hash表中查找)  */
extern struct buffer_head *getblk(int dev, int block);
/* 读/写数据块 */
extern void ll_rw_block(int rw, struct buffer_head *bh);
/* 读/写数据页面, 即每次4块数据块 */
extern void ll_rw_page(int rw, int dev, int nr, char *buffer);
/* 释放指定缓冲块 */
extern void brelse(struct buffer_head *buf);
/* 读取指定的数据块 */
extern struct buffer_head *bread(int dev, int block);
/* 读4块缓冲区到指定地址的内存中 */
extern void bread_page(unsigned long addr, int dev, int b[4]);
/* 读取头一个指定的数据块, 并标记后续将要读的块 */
extern struct buffer_head *breada(int dev, int block, ...);
/* 向设备dev申请一个磁盘块 (区段, 逻辑块) . 返回逻辑块号 */
extern int new_block(int dev);
/* 释放设备数据区中的逻辑块(区段, 磁盘块)block. 复位指定逻辑块block的逻辑块位图比特位
 */
extern int free_block(int dev, int block);
/* 为设备dev建立一个新inode , 返回inode 号 */
extern struct m_inode *new_inode(int dev);
/* 释放一个inode  (删除文件时)  */
extern void free_inode(struct m_inode *inode);
/* 刷新指定设备缓冲区 */
extern int sync_dev(int dev);
/* 读取指定设备的超级块 */
extern struct super_block *get_super(int dev);

extern int ROOT_DEV;

// 安装根文件系统
extern void mount_root(void);

#endif
