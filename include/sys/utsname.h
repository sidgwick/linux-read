#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

/**
 * 系统名称结构头文件
 */

#include <sys/param.h>
#include <sys/types.h>

struct utsname {
    char sysname[9];                   // 当前运行系统的名称
    char nodename[MAXHOSTNAMELEN + 1]; // 与实现相关的网络中节点名称（主机名称）
    char release[9];                   // 本操作系统实现的当前发行级别
    char version[9];                   // 本次发行的操作系统版本级别
    char machine[9];                   // 系统运行的硬件类型名称
};

// returns system information in the structure pointed to by utsbuf
extern int uname(struct utsname *utsbuf);

#endif