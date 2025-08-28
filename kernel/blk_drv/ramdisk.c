/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 */

#include <string.h>

#include <asm/memory.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <linux/config.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>

#define MAJOR_NR 1 /* 定义RAM盘主设备号符号常数 */
#include "blk.h"

char *rd_start;    /* ramdisk 在内从中的起始位置指针 */
int rd_length = 0; /* ramdisk 大小 */

/**
 * @brief 虚拟盘当前请求项操作函数
 *
 * 在低级块设备接口函数 ll_rw_block 建立起虚拟盘(rd)的请求项并
 * 添加到 rd 的链表中之后, 就会调用该函数对 rd 当前请求项进行处理
 */
void do_rd_request(void)
{
    int len;
    char *addr;

    INIT_REQUEST;

    /* 计算当前请求项中指定的起始扇区对应虚拟盘所处内存的起始位置 */
    addr = rd_start + (CURRENT->sector << 9);

    /* 要求的扇区数对应的字节长度值 */
    len = CURRENT->nr_sectors << 9;

    /* 检查 addr, len 是不是合法的 */
    if ((MINOR(CURRENT->dev) != 1) || (addr + len > rd_start + rd_length)) {
        end_request(0);
        goto repeat;
    }

    /* 读写操作直接 memcpy 就行了 */
    if (CURRENT->cmd == WRITE) {
        (void)memcpy(addr, CURRENT->buffer, len);
    } else if (CURRENT->cmd == READ) {
        (void)memcpy(CURRENT->buffer, addr, len);
    } else {
        panic("unknown ramdisk-command");
    }

    end_request(1);
    goto repeat;
}

/**
 * @brief Returns amount of memory which needs to be reserved.
 *
 * RAMDISK 在内存中的位置一般放在 buffer_memory 到 main_memory 之间
 *
 * @param mem_start RAMDISK在内存中的起始地址
 * @param length RAMDISK 的大小
 * @return long 返回 length 参数
 */
long rd_init(long mem_start, int length)
{
    int i;
    char *cp;

    blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;
    rd_start = (char *)mem_start;
    rd_length = length;
    cp = rd_start;

    /* 给 RAMDISK 所在的内存区域赋 0 */
    for (i = 0; i < length; i++) {
        *cp++ = '\0';
    }

    return (length);
}

/**
 * @brief 尝试把根文件系统加载到虚拟盘中
 *
 * If the root device is the ram disk, try to load it
 *
 * In order to do this, the root device is originally set to the
 * floppy, and we later change it to be ram disk.
 */
void rd_load(void)
{
    struct buffer_head *bh;
    struct super_block s; /* 文件超级块结构 */

    /* 根文件系统映像文件被存储于 boot 盘第 256 磁盘块开始处(即 128KB 处) */
    int block = 256;
    int i = 1;
    int nblocks; /* 文件系统盘总块数 */
    char *cp;    /* Move pointer */

    /* ramdisk 的长度一定是大于 0 的 */
    if (!rd_length) {
        return;
    }

    /* 打印 ramdisk 的长度和开始位置信息 */
    printk("Ram disk: %d bytes, starting at 0x%x\n", rd_length, (int)rd_start);
    if (MAJOR(ROOT_DEV) != 2) {
        /* ROOT_DEV 应该是软盘, 我们要从软盘里面加载 RAMDISK */
        return;
    }

    /* 然后读根文件系统的基本参数(读软盘块 257, 256, 258) */
    bh = breada(ROOT_DEV, block + 1, block, block + 2, -1);
    if (!bh) {
        printk("Disk error while looking for ramdisk!\n");
        return;
    }

    /* 从文件系统基本参数中获取磁盘超级块(d_super_block 是磁盘超级块结构) */
    *((struct d_super_block *)&s) = *((struct d_super_block *)bh->b_data);
    brelse(bh);

    /* 超级块中文件系统魔数不对, 则说明加载的数据块不是 MINIX 文件系统 */
    if (s.s_magic != SUPER_MAGIC) {
        /* No ram disk image present, assume normal floppy boot */
        return;
    }

    /* 尝试把整个根文件系统读入到内存虚拟盘区中
     * 对于一个文件系统来说, 其超级块结构的 s_nzones 字段中保存着总逻辑块数(或称
     * 为区段数), 一个逻辑块中含有的数据块数则由字段 s_log_zone_size 指定, 因此
     * 文件系统中的数据块总数 nblocks = (s_nzones * 2^s_log_zone_size) */

    nblocks = s.s_nzones << s.s_log_zone_size;

    /* 文件系统中数据块总数大于内存虚拟盘所能容纳的块数的情况, 则不能执行加载操作 */
    if (nblocks > (rd_length >> BLOCK_SIZE_BITS)) {
        printk("Ram disk image too big!  (%d blocks, %d avail)\n", nblocks,
               rd_length >> BLOCK_SIZE_BITS);
        return;
    }

    // 同时显示已加载的块数. 显示字符串中的八进制数'\010'表示显示一个制表符.

    printk("Loading %d bytes into ram disk... 0000k", nblocks << BLOCK_SIZE_BITS);
    cp = rd_start; /* cp 指向内存虚拟盘起始处 */
    /* 循环操作将磁盘上根文件系统映像文件加载到虚拟盘上 */
    while (nblocks) {
        /* 读取的数量比较多, 使用预读, 否则正常读 */
        if (nblocks > 2) {
            bh = breada(ROOT_DEV, block, block + 1, block + 2, -1);
        } else {
            bh = bread(ROOT_DEV, block);
        }

        /* I/O 操作错误, 就只能放弃加载返回 */
        if (!bh) {
            printk("I/O error on block %d, aborting load\n", block);
            return;
        }

        /* 已经读取的数据使用 memcpy 函数从高速缓冲区中复制到内存虚拟盘相应位置 */
        (void)memcpy(cp, bh->b_data, BLOCK_SIZE);
        brelse(bh);

        /* 显示已经加载的区块数 */
        printk("\010\010\010\010\010%4dk", i);
        cp += BLOCK_SIZE;
        block++;
        nblocks--;
        i++;
    }

    /* 最后把 ROOT_DEV 更新成 RAMDISK 的第一个分区(主设备 0x01, 从设备 0x01) */
    printk("\010\010\010\010\010done \n");
    ROOT_DEV = 0x0101;
}
