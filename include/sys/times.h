#ifndef _TIMES_H
#define _TIMES_H

/**
 * 进程中时间头文件
 */

#include <sys/types.h>

struct tms {
    // 用户使用的 CPU 时间
    time_t tms_utime;
    // 系统(内核) CPU 时间
    time_t tms_stime;
    // 已终止的子进程使用的用户CPU时间
    time_t tms_cutime;
    // 已终止的子进程使用的系统CPU时间
    time_t tms_cstime;
};

// stores the current process times in the struct tms that tp points to
extern time_t times(struct tms *tp);

#endif
