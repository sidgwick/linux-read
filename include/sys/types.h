#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

// 这个文件定义了很多常见的数据类型

#ifndef _SIZE_T
#define _SIZE_T
// 用于对象的大小(长度)
typedef unsigned int size_t;
#endif

#ifndef _TIME_T
#define _TIME_T
// 用于时间(以秒计)
typedef long time_t;
#endif

#ifndef _PTRDIFF_T
#define _PTRDIFF_T
/**
 * ptrdiff_t is the signed integer type of the result of subtracting two
 * pointers 比如整数的 size 是 4, 字符的 size 是 1,
 * 对整形/字符型数组里面的第四个元素(a[3])指针减去第一个(a[0]) 有:
 *  - 整数型: 虽然内存位置相差了 12, 但是指针相减的结果是 3
 *  - 整数型: 内存位置相差了 3, 指针相减的结果是 3
 */
typedef long ptrdiff_t;
#endif

#ifndef NULL
// 空指针
#define NULL ((void *)0)
#endif

// 用于进程号和进程组号
typedef int pid_t;
// 用于用户号(用户标识号)
typedef unsigned short uid_t;
// 用于用户组号
typedef unsigned short gid_t;
// 用于设备号
typedef unsigned short dev_t;
// 用于文件序列号
typedef unsigned short ino_t;
// 用于某些文件属性
typedef unsigned short mode_t;
// 文件类型和属性
typedef unsigned short umode_t;
// 用于链接计数 - 软硬链接?
typedef unsigned char nlink_t;
// TODO: ??? 是不是磁盘地址的意思 disk address?
typedef int daddr_t;
// 用于文件长度(大小)
typedef long off_t;
// 无符号字符类型
typedef unsigned char u_char;
// 无符号短整数类型
typedef unsigned short ushort;

// 下面这三个定义, 都和 termios 有关

// POSIX 的 termios 结构中控制字符数组
typedef unsigned char cc_t;
// 终端的接受或者发送波特率
typedef unsigned int speed_t;
// POSIX 的 termios 结构中控制模式标志
typedef unsigned long tcflag_t;

// 文件描述符集, 每比特代表 1 个描述符
typedef unsigned long fd_set;

// 用于 DIV 操作(TODO: 软件模式除法吗???)
typedef struct {
    int quot, rem;
} div_t;
// 用于长 DIV 操作
typedef struct {
    long quot, rem;
} ldiv_t;

// 文件系统参数结构, 用于 ustat() 函数
// 最后两个字段未使用, 总是返回 NULL 指针
struct ustat {
    // 系统总空闲块数
    daddr_t f_tfree;
    // 总空闲 i 节点数
    ino_t f_tinode;
    // 文件系统名称
    char f_fname[6];
    // 文件系统压缩名称
    char f_fpack[6];
};

#endif
