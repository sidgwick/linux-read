/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/**
 * 块设备 Low Level 读写接口 */

/*
 * This handles all read/write requests to block devices
 */
#include <asm/system.h>
#include <errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 *
 * 请求队列数组, 共有 32 个请求项
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 * 是用于在请求数组没有空闲项时进程的临时等待处
 */
struct task_struct *wait_for_request = NULL;

/* blk_dev_struct is:
 *    do_request-address
 *    next-request
 *
 * 块设备数组. 该数组使用主设备号作为索引. 实际内容将在各块设备驱动程序初始化时填入
 */
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
    {NULL, NULL}, /* no_dev */
    {NULL, NULL}, /* dev mem */
    {NULL, NULL}, /* dev fd */
    {NULL, NULL}, /* dev hd */
    {NULL, NULL}, /* dev ttyx, ttyx 设备 */
    {NULL, NULL}, /* dev tty, tty 设备 */
    {NULL, NULL}  /* dev lp, lp 打印机 */
};

/*
 * blk_size contains the size of all block-devices:
 *
 * blk_size[MAJOR][MINOR]
 *
 * if (!blk_size[MAJOR]) then no minor size checking is done.
 *
 * blk_size 数组含有所有块设备的大小(块总数)
 *
 * 设备数据块总数指针数组, 每个指针(注意这个声明是个二维数组)项指向指定
 * 主设备号的总块数数组, 该总块数数组每一项对应子设备号确定的一个子设备
 * 上所拥有的数据块总数(1 块大小 = 1KB)
 */
int *blk_size[NR_BLK_DEV] = {
    NULL,
    NULL,
};

/**
 * @brief 锁定指定缓冲块
 *
 * 如果指定的缓冲块已经被其他任务锁定, 则使自己睡眠(不可中断地等待),
 * 直到被执行解锁缓冲块的任务明确地唤醒
 *
 * @param bh 缓冲区指针
 */
static inline void lock_buffer(struct buffer_head *bh)
{
    cli();                     /* 清中断许可 */
    while (bh->b_lock) {       /* 如果缓冲区已被锁定则睡眠, 直到缓冲区解锁 */
        sleep_on(&bh->b_wait); /* TODO: 探究这里为啥传递二级指针进去??? */
    }

    bh->b_lock = 1; /* 立刻锁定该缓冲区 */
    sti();          /* 开中断 */
}

/**
 * @brief 解锁锁定的缓冲区
 *
 * @param bh 缓冲区指针
 */
static inline void unlock_buffer(struct buffer_head *bh)
{
    if (!bh->b_lock) {
        printk("ll_rw_block.c: buffer not locked\n\r");
    }

    bh->b_lock = 0;

    /* 唤醒等待该缓冲区的任务 */
    wake_up(&bh->b_wait);
}

/**
 * @brief 向链表中加入请求项
 *
 * 本函数把已经设置好的请求项 req 添加到指定设备的请求项链表中. 如果该设备
 * 的当前请求请求项指针为空, 则可以设置 req 为当前请求项并立刻调用设备请求
 * 项处理函数. 否则就把 req 请求项插入到该请求项链表中
 *
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 *
 * Note that swapping requests always go before other requests,
 * and are done in the order they appear.
 *
 * @param dev 指定块设备结构指针, 含有处理请求函数指针和当前正在请求项指针
 * @param req 已设置好内容的请求项结构指针
 */
static void add_request(struct blk_dev_struct *dev, struct request *req)
{
    struct request *tmp;

    req->next = NULL;
    cli();

    if (req->bh) {
        req->bh->b_dirt = 0; /* 清缓冲区 '脏' 标志 */
    }

    /* 如果该设备的当前请求请求项指针为空, 设置 req 为当前请求项并立刻调用设备请求项处理函数 */
    if (!(tmp = dev->current_request)) {
        dev->current_request = req;
        sti();
        (dev->request_fn)();
        return;
    }

    /* 否则就把 req 请求项插入到该请求项链表中, 这个循环确定 req 插在什么位置比较合适 */
    for (; tmp->next; tmp = tmp->next) {
        /* 如果 req 没有指定缓冲块, 这种 req 来自 read_page 请求
         * TODO: 这段逻辑什么意思??? */
        if (!req->bh) {
            if (tmp->next->bh) {
                break;
            } else {
                continue;
            }
        }

        /* 电梯算法判断 tmp, req 的执行顺序, 如果:
         *  1. tmp 顺序在 req 之前, 且 req 在 tmp->next 之前
         *  2. tmp 顺序在 tmp->next 之后, 且 req 在 tmp->next 之前
         *
         * TODO: 情况 2 啥时候出现?
         * 就找到了插入位置 */
        if ((IN_ORDER(tmp, req) || !IN_ORDER(tmp, tmp->next)) && IN_ORDER(req, tmp->next)) {
            break;
        }
    }

    /* 按照电梯算法, 维护读写请求链表 */
    req->next = tmp->next;
    tmp->next = req;
    sti();
}

/**
 * @brief 构造块设备访问请求并插入请求队列中
 *
 * @param major 设备号
 * @param rw 读/写操作命令
 * @param bh 缓存区快
 */
static void make_request(int major, int rw, struct buffer_head *bh)
{
    struct request *req;
    int rw_ahead;

    /* WRITEA/READA is special case - it is not really needed, so if the */
    /* buffer is locked, we just forget about it, else it's a normal read
     *
     * 对于这两个命令, 当指定的缓冲区正在使用而已被上锁时, 就放弃预读/写请求
     * 否则就作为普通的READ/WRITE命令进行操作 */
    if (rw_ahead = (rw == READA || rw == WRITEA)) {
        if (bh->b_lock) {
            return;
        }
        if (rw == READA) {
            rw = READ;
        } else {
            rw = WRITE;
        }
    }

    /* 只能支持 READ, WRITE 操作 */
    if (rw != READ && rw != WRITE) {
        panic("Bad block dev command, must be R/W/RA/WA");
    }

    /* 锁定缓冲区 */
    lock_buffer(bh);

    /* 以下两种操作, 实际上是不需要真的写入读出的:
     * 写入操作, 但是数据不脏
     * 读取操作, 但是数据很新 */
    if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
        unlock_buffer(bh);
        return;
    }

    /* 接下来生成并添加读/写请求项 */

repeat:
    /* we don't allow the write-requests to fill up the queue completely:
     * we want some room for reads: they take precedence. The last third
     * of the requests are only for reads.
     *
     * 我们不能让队列中全都是写请求项:
     * 我们需要为读请求保留一些空间: 读操作是优先的.
     * 请求队列的后三分之一空间仅用于读请求项
     *
     * 这块逻辑表示接下来将从请求数组末端开始搜索空闲槽来存放新请求项.
     * 读请求直接从队列末尾开始搜索, 而写请求就只能从队列 2/3 处向队列头处搜索
     */
    if (rw == READ) {
        req = request + NR_REQUEST;
    } else {
        req = request + ((NR_REQUEST * 2) / 3);
    }

    /* find an empty request */
    while (--req >= request) {
        if (req->dev < 0) {
            break;
        }
    }

    /* if none found, sleep on new requests: check for rw_ahead
     * 找不到空闲的请求槽位, 预读写直接放弃操作, 真实读写等待有资源 */
    if (req < request) {
        if (rw_ahead) {
            unlock_buffer(bh);
            return;
        }

        sleep_on(&wait_for_request);
        goto repeat;
    }

    /* fill up the request-info, and add it to the queue */
    req->dev = bh->b_dev;
    req->cmd = rw;
    req->errors = 0;
    req->sector = bh->b_blocknr << 1; /* 块号转换成扇区号(1块=2扇区) */
    req->nr_sectors = 2;              /* 本请求项需要读写的扇区数 */
    req->buffer = bh->b_data;         /* 请求项缓冲区指针指向需读写的数据缓冲区 */
    req->waiting = NULL;
    req->bh = bh;
    req->next = NULL;

    /* 追加到等待队列里面 */
    add_request(major + blk_dev, req);
}

/**
 * @brief 低级页面读写函数 (Low Level Read Write Page)
 *
 * 以页面(4K)为单位访问块设备数据, 即每次读/写 8 个扇区
 *
 * @param rw 读写命令
 * @param dev 设备号
 * @param page 页面地址?
 * @param buffer 接收缓冲区
 */
void ll_rw_page(int rw, int dev, int page, char *buffer)
{
    struct request *req;
    unsigned int major = MAJOR(dev);

    if (major >= NR_BLK_DEV || !(blk_dev[major].request_fn)) {
        printk("Trying to read nonexistent block-device\n\r");
        return;
    }

    if (rw != READ && rw != WRITE) {
        panic("Bad block dev command, must be R/W");
    }

    /* 尝试找到空闲的 request 槽位(从尾部开始查找) */
repeat:
    req = request + NR_REQUEST;
    while (--req >= request) {
        if (req->dev < 0) {
            break;
        }
    }

    /* 找不到的话, 就歇一会儿继续找 */
    if (req < request) {
        sleep_on(&wait_for_request);
        goto repeat;
    }

    /* fill up the request-info, and add it to the queue */
    req->dev = dev;
    req->cmd = rw;
    req->errors = 0;
    req->sector = page << 3;
    req->nr_sectors = 8;
    req->buffer = buffer;
    req->waiting = current;
    req->bh = NULL; /* 注意这个 NULL */
    req->next = NULL;

    /* 进程置为不可中断状态, 等待数据就绪 */
    current->state = TASK_UNINTERRUPTIBLE;

    /* request 追加到待处理队列 */
    add_request(major + blk_dev, req);

    /* 重新调度 */
    schedule();
}

/**
 * @brief 低级数据块读写函数 (Low Level Read Write Block)
 *
 * 块设备驱动程序的接口函数, 主要功能是创建块设备读写请求项并插入到指
 * 定块设备请求队列中. 实际的读写操作则是由设备的 request_fn 函数完成
 *
 * 在调用该函数之前, 调用者需要首先把读/写块设备的信息保存在缓冲块头
 * 结构中, 如设备号和块号
 *
 * @param rw READ/READA/WRITE/WRITEA 命令
 * @param bh 数据缓冲块头指针
 */
void ll_rw_block(int rw, struct buffer_head *bh)
{
    unsigned int major; /* 主设备号(2: 软盘, 3: 硬盘) */

    if ((major = MAJOR(bh->b_dev)) >= NR_BLK_DEV || !(blk_dev[major].request_fn)) {
        printk("Trying to read nonexistent block-device\n\r");
        return;
    }

    make_request(major, rw, bh);
}

/**
 * @brief 初始化请求数组, 将所有请求项置为空闲项(dev = -1)
 */
void blk_dev_init(void)
{
    int i;

    for (i = 0; i < NR_REQUEST; i++) {
        request[i].dev = -1;
        request[i].next = NULL;
    }
}
