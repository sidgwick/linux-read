#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H

// 系统时钟频率, 每秒中断 100 次
#define HZ 100
// 页面大小
#define EXEC_PAGESIZE 4096

// 每个进程最多组号
#define NGROUPS 32 /* Max number of groups per user */
#define NOGROUP -1

// 主机名最大长度, 8 字节
#define MAXHOSTNAMELEN 8

#endif
