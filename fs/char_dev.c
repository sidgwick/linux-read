/*
 *  linux/fs/char_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>
#include <sys/types.h>

#include <linux/kernel.h>
#include <linux/sched.h>

#include <asm/io.h>
#include <asm/segment.h>

extern int tty_read(unsigned minor, char *buf, int count);
extern int tty_write(unsigned minor, char *buf, int count);

/* 定义字符设备读写函数指针类型
 * Character device Read Write PoinTeR */
typedef int (*crw_ptr)(int rw, unsigned minor, char *buf, int count, off_t *pos);

/**
 * @brief 串口终端读写操作函数
 *
 * @param rw 读写命令
 * @param minor 终端子设备号
 * @param buf 缓冲区
 * @param count 读写字节数
 * @param pos 读写操作当前指针, 对于终端操作, 该指针无用
 * @return int 实际读写的字节数. 若失败则返回出错码
 */
static int rw_ttyx(int rw, unsigned minor, char *buf, int count, off_t *pos)
{
    return ((rw == READ) ? tty_read(minor, buf, count) : tty_write(minor, buf, count));
}

/**
 * @brief 终端读写操作函数
 *
 * 本函数逻辑同上 rw_ttyx, 只是增加了对进程是否有控制终端的检测
 *
 * @param rw 读写命令
 * @param minor 终端子设备号
 * @param buf 缓冲区
 * @param count 读写字节数
 * @param pos 读写操作当前指针, 对于终端操作, 该指针无用
 * @return int 实际读写的字节数. 若失败则返回出错码
 */
static int rw_tty(int rw, unsigned minor, char *buf, int count, off_t *pos)
{
    /* 若进程没有对应的控制终端, 则返回出错号 */
    if (current->tty < 0) {
        return -EPERM;
    }

    return rw_ttyx(rw, current->tty, buf, count, pos);
}

/**
 * @brief 进程虚拟内存数据读写
 *
 * @param rw 读写命令
 * @param buf 缓冲区
 * @param count 读写字节数
 * @param pos 读写操作当前指针, 对于终端操作, 该指针无用
 * @return int 实际读写的字节数. 若失败则返回出错码
 */
static int rw_ram(int rw, char *buf, int count, off_t *pos)
{
    // printk("rw_ram, rw=%d, buf=%p, count=%d, pos=%p\n", rw, buf, count, pos);

    return -EIO;
}

/**
 * @brief 物理内存数据读写
 *
 * @param rw 读写命令
 * @param buf 缓冲区
 * @param count 读写字节数
 * @param pos 读写操作当前指针, 对于终端操作, 该指针无用
 * @return int 实际读写的字节数. 若失败则返回出错码
 */
static int rw_mem(int rw, char *buf, int count, off_t *pos)
{
    // printk("rw_mem, rw=%d, buf=%p, count=%d, pos=%p\n", rw, buf, count, pos);

    return -EIO;
}

/**
 * @brief 内核虚拟内存数据读写
 *
 * @param rw 读写命令
 * @param buf 缓冲区
 * @param count 读写字节数
 * @param pos 读写操作当前指针, 对于终端操作, 该指针无用
 * @return int 实际读写的字节数. 若失败则返回出错码
 */
static int rw_kmem(int rw, char *buf, int count, off_t *pos)
{
    // printk("rw_kmem, rw=%d, buf=%p, count=%d, pos=%p\n", rw, buf, count, pos);

    /* NOTICE: 这个 if 是我自己的修改, 不是 linux 0.12 里面的代码
     * 主要是为了测试 ps 命令, 需要用到这里 */
    if (rw == READ) {
        int i = 0;

        for (i = 0; i < count; i++) {
            put_fs_byte(*(pos + i), buf);
        }

        return i;
    }

    return -EIO;
}

/**
 * @brief 端口读写操作
 *
 * 读操作: 从 [pos, pos+count] 范围的端口中读取数据, 每个端口读取一字节, 保存到 buf 里面
 * 写操作: 往 [pos, pos+count] 范围的端口中写入数据, 每个端口写入一字节, 数据来源在 buf 里面
 *
 * 完成后调整 pos 指向 pos+count 的位置
 *
 * 如 pos+count > 65536, 则上述区间范围最多到 65536
 *
 * @param rw 读写命令
 * @param buf 缓冲区
 * @param count 读写字节数
 * @param pos 端口地址
 * @return int 实际读写的字节数
 */
static int rw_port(int rw, char *buf, int count, off_t *pos)
{
    int i = *pos;

    /*  对于所要求读写的字节数, 并且端口地址小于 64k 时, 循环执行单个字节的读写操作 */
    while (count-- > 0 && i < 65536) {
        if (rw == READ) {
            put_fs_byte(inb(i), buf++);
        } else {
            outb(get_fs_byte(buf++), i);
        }

        i++;
    }

    i -= *pos;
    *pos += i;
    return i;
}

/**
 * @brief 内存读写操作函数
 *
 * 内存主设备号是 1, 这里仅给出对 0-5 子设备的处理
 *
 * @param rw 读写命令
 * @param minor 子设备号
 * @param buf 缓冲区
 * @param count 读写的字节数
 * @param pos 读写操作指针位置
 * @return int 实际读写的字节数
 */
static int rw_memory(int rw, unsigned minor, char *buf, int count, off_t *pos)
{
    switch (minor) {
    case 0:
        /* 对应设备文件名是 /dev/ram0 或 /dev/ramdisk */
        return rw_ram(rw, buf, count, pos);
    case 1:
        /* 对应设备文件名是 /dev/ram1 或 /dev/mem 或 /dev/ram */
        return rw_mem(rw, buf, count, pos);
    case 2:
        /* 对应设备文件名是 /dev/ram2 或 /dev/kmem */
        return rw_kmem(rw, buf, count, pos);
    case 3:
        /* 对应设备文件名是 /dev/null */
        return (rw == READ) ? 0 : count; /* rw_null */
    case 4:
        /* 对应设备文件名是 /dev/port */
        return rw_port(rw, buf, count, pos);
    default:
        return -EIO;
    }
}

/* 系统中设备种数 */
#define NRDEVS ((sizeof(crw_table)) / (sizeof(crw_ptr)))

/* 字符设备读写函数指针表 */
static crw_ptr crw_table[] = {
    /* dev list */
    NULL,      /* nodev */
    rw_memory, /* /dev/mem etc */
    NULL,      /* /dev/fd */
    NULL,      /* /dev/hd */
    rw_ttyx,   /* /dev/ttyx, 具体的终端设备(串口/伪终端等) */
    rw_tty,    /* /dev/tty, 当前进程的控制终端 */
    NULL,      /* /dev/lp */
    NULL,      /* unnamed pipes */
};

/**
 * @brief 字符设备读写操作
 *
 * @param rw 读写命令
 * @param dev 设备号
 * @param buf 缓冲区
 * @param count 读写字节数
 * @param pos 读写指针
 * @return int 实际读/写字节数
 */
int rw_char(int rw, int dev, char *buf, int count, off_t *pos)
{
    crw_ptr call_addr;

    if (MAJOR(dev) >= NRDEVS) {
        return -ENODEV;
    }

    if (!(call_addr = crw_table[MAJOR(dev)])) {
        return -ENODEV;
    }

    return call_addr(rw, MINOR(dev), buf, count, pos);
}
