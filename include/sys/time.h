#ifndef _SYS_TIME_H
#define _SYS_TIME_H

/**
 * 时间功能头文件
 */

/**
 * 一些关于 gettimeofday 的资料:
 *
 * The gettimeofday function in C retrieves the current time, with precision up
 * to microseconds. It requires two parameters: a struct timeval and a struct
 * timezone. The timezone structure is deprecated and should be passed as NULL.
 */

/* gettimofday returns this */
struct timeval {
    long tv_sec;  /* seconds */
    long tv_usec; /* microseconds - 微秒 */
};

// 时间区结构. tz 为时区(Time Zone)的缩写, DST(Daylight Saving
// Time)是夏令时的缩写
struct timezone {
    // 格林威治西部分钟时间
    int tz_minuteswest; /* minutes west of Greenwich */
                        // 夏令时区调整时间
    int tz_dsttime;     /* type of dst correction */
};

// 非夏令时
#define DST_NONE 0 /* not on dst */
// USA形式的夏令时
#define DST_USA 1      /* USA style dst */
#define DST_AUST 2     /* Australian style dst */
#define DST_WET 3      /* Western European dst */
#define DST_MET 4      /* Middle European dst */
#define DST_EET 5      /* Eastern European dst */
#define DST_CAN 6      /* Canada */
#define DST_GB 7       /* Great Britain and Eire */
#define DST_RUM 8      /* Rumania */
#define DST_TUR 9      /* Turkey */
#define DST_AUSTALT 10 /* Australian style with shift in 1986 */

// 文件描述符集的设置宏, 用于 select() 函数
#define FD_SET(fd, fdsetp) (*(fdsetp) |= (1 << (fd)))
#define FD_CLR(fd, fdsetp) (*(fdsetp) &= ~(1 << (fd)))
#define FD_ISSET(fd, fdsetp) ((*(fdsetp) >> fd) & 1)
#define FD_ZERO(fdsetp) (*(fdsetp) = 0)

/*
 * Operations on timevals.
 *
 * NB: timercmp does not work for >= or <=.
 */
// timeval 时间结构的操作函数
// 检查有没有时间设置
#define timerisset(tvp) ((tvp)->tv_sec || (tvp)->tv_usec)
// 时间相等比较
#define timercmp(tvp, uvp, cmp)                                                                    \
    ((tvp)->tv_sec cmp(uvp)->tv_sec ||                                                             \
     (tvp)->tv_sec == (uvp)->tv_sec && (tvp)->tv_usec cmp(uvp)->tv_usec)
// 时间清空
#define timerclear(tvp) ((tvp)->tv_sec = (tvp)->tv_usec = 0)

/*
 * Names of the interval timers, and structure
 * defining a timer setting.
 *
 * 内部定时器名称和结构, 用于定义定时器设置
 */
// 以实际时间递减
#define ITIMER_REAL 0
// 以进程虚拟时间递减
#define ITIMER_VIRTUAL 1
// 以进程虚拟时间或者当系统运行时以进程时间递减
#define ITIMER_PROF 2

// 内部时间结构. 其中 it=Internal Timer, 是内部定时器的缩写
struct itimerval {
    struct timeval it_interval; /* timer interval */
    struct timeval it_value;    /* current value */
};

#include <sys/types.h>
#include <time.h>

int gettimeofday(struct timeval *tp, struct timezone *tz);
int select(int width, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#endif /*_SYS_TIME_H*/
