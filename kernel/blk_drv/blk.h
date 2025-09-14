#ifndef _BLK_H
#define _BLK_H

#define NR_BLK_DEV 7 /* 块设备类型数量 */

/* NR_REQUEST is the number of entries in the request-queue.
 * NOTE that writes may use only the low 2/3 of these: reads
 * take precedence.
 *
 * 32 seems to be a reasonable number: enough to get some benefit
 * from the elevator-mechanism, but not so much as to lock a lot of
 * buffers when they are in the queue. 64 seems to be too many (easily
 * long pauses in reading when heavy writing/syncing is going on)

 * 下面定义的NR_REQUEST是请求队列中所包含的项数
 * 注意, 写操作仅使用这些项中低端的2/3项; 读操作优先处理
 *
 * 32 项好象是一个合理的数字: 该数已经足够从电梯算法中获得好处, 但当缓冲区
 * 在队列中而锁住时又不显得是很大的数. 64 就看上去太大了(当大量的写/同步操作
 * 运行时很容易引起长时间的暂停) */
#define NR_REQUEST 32

/* Ok, this is an expanded form so that we can use the same
 * request for paging requests when that is implemented. In
 * paging, 'bh' is NULL, and 'waiting' is used to wait for
 * read/write completion.
 *
 * OK, 下面是 request 结构的一个扩展形式, 因而当实现以后, 我们就可以
 * 在分页请求中使用同样的 request 结构
 * 在分页处理中, bh 是NULL, 而 waiting 则用于等待读/写的完成
 *
 * 下面是请求队列中项的结构
 * 其中如果字段 dev = -1, 则表示队列中该项没有被使用
 * 字段 cmd 可取 include/linux/fs.h 中常量 READ 或 WRITE
 * 其中, 内核并没有用到 waiting 指针, 取而代之地内核使用了缓冲块的等待队列,
 * 因为等待一个缓冲块与等待请求项完成是对等的 */
struct request {
    int dev;    /* -1 if no request, 发请求的设备号 */
    int cmd;    /* READ=0 or WRITE=1 */
    int errors; /* 操作时产生的错误次数 */

    unsigned long sector;     /* 起始扇区(相对于当前分区) */
    unsigned long nr_sectors; /* 读/写扇区数 */

    /* TODO: buffer 和 bh 的区别是什么?
     * 看完 FS 之后, 尝试来回答这个问题
     * 缓冲区似乎有 2 种, 一种是直接使用 buffer 的, 一种是使用 bh 的 */
    char *buffer;                /* 数据缓冲区 */
    struct task_struct *waiting; /* 任务等待请求完成操作的地方(队列) */
    struct buffer_head *bh;      /* 缓冲区头指针(include/linux/fs.h) */

    struct request *next; /* 指向下一请求项 */
};

/* 当 s1 顺序小于 s2 的时候(s1 先处理, s2 后处理), 就返回真
 *
 * 电梯算法的作用是让磁盘磁头的移动距离最小, 从而减少硬盘访问时间
 *
 * This is used in the elevator algorithm: Note that
 * reads always go before writes. This is natural: reads
 * are much more time-critical than writes.
 *
 * 下面宏中参数 s1 和 s2 的取值是上面定义的请求结构 request 的指针
 * 该宏定义用于根据两个参数指定的请求项结构中的信息(命令 cmd, 设备号 dev
 * 以及所操作的扇区号 sector)来判断出两个请求项结构的前后排列顺序, 这个顺序将
 * 用作访问块设备时的请求项执行顺序
 *
 * 这个宏会在程序 blk_drv/ll_rw_blk.c 中函数 add_request 中被调用
 * 该宏部分地实现了 I/O 调度功能, 即实现了对请求项的排序功能(另一个调度功能是请求项合并功能) */
#define IN_ORDER(s1, s2)                                                                           \
    (((s1)->cmd < (s2)->cmd) /* s1 读, s2 写, 读优先处理 */ ||                                     \
     (((s1)->cmd == (s2)->cmd) &&                                 /* s1, s2 操作一样的时候 */      \
      ((s1)->dev < (s2)->dev ||                                   /* 先读设备号较小的 */           \
       ((s1)->dev == (s2)->dev && (s1)->sector < (s2)->sector)))) /* 先读扇区号较小的 */

/* 块设备处理结构 */
struct blk_dev_struct {
    void (*request_fn)(void);        /* 请求处理函数指针 */
    struct request *current_request; /* 当前待处理的请求队列(链表) */
};

/* 块设备表(数组). 每种块设备占用一项, 共7项 */
extern struct blk_dev_struct blk_dev[NR_BLK_DEV];

/* 请求队列数组, 共32项 */
extern struct request request[NR_REQUEST];

/* 等待空闲请求项的进程队列头指针 */
extern struct task_struct *wait_for_request;

/* 设备数据块总数指针数组
 * 每个指针项指向指定主设备号的总块数数组 hd_sizes, 该总块数
 * 数组每一项对应子设备号确定的一个子设备上所拥有的数据块总数(1块大小 = 1KB) */
extern int *blk_size[NR_BLK_DEV];

// 在块设备驱动程序(如 hd.c)包含此头文件时, 必须先定义驱动程序处理设备的主设备号
// 这样, 在下面就能为包含本文件的驱动程序给出正确的宏定义
#ifdef MAJOR_NR /* 主设备号 */

/*
 * Add entries as needed. Currently the only block devices
 * supported are hard-disks and floppies.
 */

#if (MAJOR_NR == 1)
/* ram disk */
#define DEVICE_NAME "ramdisk"            /* 设备名称(内存虚拟盘)  */
#define DEVICE_REQUEST do_rd_request     /* 设备请求项处理函数 */
#define DEVICE_NR(device) ((device) & 7) /* 设备号 (0-7)  */
#define DEVICE_ON(device)                /* 开启设备 (虚拟盘无须开启和关闭)  */
#define DEVICE_OFF(device)               /* 关闭设备 */

#elif (MAJOR_NR == 2)
/* floppy */
#define DEVICE_NAME "floppy"                             /* 设备名称(软盘驱动器)  */
#define DEVICE_INTR do_floppy                            /* 设备中断处理函数 */
#define DEVICE_REQUEST do_fd_request                     /* 设备请求项处理函数 */
#define DEVICE_NR(device) ((device) & 3)                 /* 设备号 (0-3)  */
#define DEVICE_ON(device) floppy_on(DEVICE_NR(device))   /* 开启设备宏 */
#define DEVICE_OFF(device) floppy_off(DEVICE_NR(device)) /* 关闭设备宏 */

#elif (MAJOR_NR == 3)
/* harddisk */
#define DEVICE_NAME "harddisk"                /* 设备名称(硬盘) */
#define DEVICE_INTR do_hd                     /* 设备中断处理函数 */
#define DEVICE_TIMEOUT hd_timeout             /* 设备超时值 */
#define DEVICE_REQUEST do_hd_request          /* 设备请求项处理函数 */
#define DEVICE_NR(device) (MINOR(device) / 5) /* 设备号 */
#define DEVICE_ON(device)                     /* 开启设备 */
#define DEVICE_OFF(device)                    /* 关闭设备 */

#elif

/* unknown blk device
 * 否则在编译预处理阶段显示出错信息: `未知块设备` */
#error "unknown blk device"

#endif

// CURRENT 是指定设备号的当前请求结构项指针
// CURRENT_DEV 是当前请求项 CURRENT 中设备号
#define CURRENT (blk_dev[MAJOR_NR].current_request)
#define CURRENT_DEV DEVICE_NR(CURRENT->dev)

/* 如果定义了设备中断处理符号, 把它声明为一个函数指针, 默认为 NULL */
#ifdef DEVICE_INTR
void (*DEVICE_INTR)(void) = NULL;
#endif

/* 如果定义了设备超时符号常数, 则令其值等于 0, 并定义 SET_INTR 宏. 否则只定义宏 */
#ifdef DEVICE_TIMEOUT
int DEVICE_TIMEOUT = 0;
#define SET_INTR(x) (DEVICE_INTR = (x), DEVICE_TIMEOUT = 200)
#else
#define SET_INTR(x) (DEVICE_INTR = (x))
#endif

/* 声明设备请求符号常数 DEVICE_REGUEST 是一个不带参数并无反回的静态函数指针 */
static void(DEVICE_REQUEST)(void);

/* 解锁缓冲区, 并唤醒等待该缓冲区的进程 */
static inline void unlock_buffer(struct buffer_head *bh)
{
    if (!bh->b_lock) {
        printk(DEVICE_NAME ": free buffer being unlocked\n");
    }

    bh->b_lock = 0;
    wake_up(&bh->b_wait);
}

/* 结束请求处理 */
static inline void end_request(int uptodate)
{
    DEVICE_OFF(CURRENT->dev); /* 首先关闭指定块设备 */

    /* 然后检查此次读写缓冲区是否有效, 如果有效则根据参数值设置缓冲区数据更新标志, 并解锁缓冲区 */
    if (CURRENT->bh) {
        CURRENT->bh->b_uptodate = uptodate;
        unlock_buffer(CURRENT->bh);
    }

    /* 如果更新标志参数值是 0, 表示此次请求项的操作已失败, 因此显示相关块设备 IO 错误信息 */
    if (!uptodate) {
        printk(DEVICE_NAME " I/O error\n\r");
        printk("dev %04x, block %d\n\r", CURRENT->dev, CURRENT->bh->b_blocknr);
    }

    /* 唤醒等待该请求项的进程以及等待空闲请求项出现的进程 */
    wake_up(&CURRENT->waiting);
    wake_up(&wait_for_request);

    /* 释放并从请求链表中删除本请求项, 并把当前请求项指针指向下一请求项 */
    CURRENT->dev = -1;
    CURRENT = CURRENT->next;
}

#ifdef DEVICE_TIMEOUT
#define CLEAR_DEVICE_TIMEOUT DEVICE_TIMEOUT = 0;
#else
#define CLEAR_DEVICE_TIMEOUT
#endif

#ifdef DEVICE_INTR
#define CLEAR_DEVICE_INTR DEVICE_INTR = 0;
#else
#define CLEAR_DEVICE_INTR
#endif

/* 定义初始化请求项宏
 *
 * 由于几个块设备驱动程序开始处对请求项的初始化操作相似, 因此这里为它们定义了一个
 * 统一的初始化宏, 该宏用于对当前请求项进行一些有效性判断, 所做工作如下:
 *
 *  1. 如果设备当前请求项为空(NULL), 表示本设备目前已无需要处理的请求项. 于是略作扫尾
 *     工作就退出相应函数
 *  2. 如果当前请求项中设备的主设备号不等于驱动程序定义的主设备号, 说明请求项队列乱掉了,
 *     于是内核显示出错信息并停机. 否则若请求项中用的缓冲块
 *  3. 没有被锁定, 也说明内核程序出了问题, 于是显示出错信息并停机 */
#define INIT_REQUEST                                                                               \
    repeat:                                                                                        \
    if (!CURRENT) { /* 无事可做 */                                                                 \
        CLEAR_DEVICE_INTR                                                                          \
        CLEAR_DEVICE_TIMEOUT                                                                       \
        return;                                                                                    \
    }                                                                                              \
    if (MAJOR(CURRENT->dev) != MAJOR_NR) { /* 设备号不对 */                                        \
        panic(DEVICE_NAME ": request list destroyed");                                             \
    }                                                                                              \
    if (CURRENT->bh) {                                                                             \
        if (!CURRENT->bh->b_lock) { /* 缓冲区未上锁 */                                             \
            panic(DEVICE_NAME ": block not locked");                                               \
        }                                                                                          \
    }

#endif

#endif
