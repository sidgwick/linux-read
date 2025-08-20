#ifndef _UTIME_H
#define _UTIME_H

/**
 * 用户时间头文件
 */

#include <sys/types.h> /* I know - shouldn't do this, but .. */

struct utimbuf {
    // 文件访问时间, 从 1970-01-01 00:00:00 开始的秒数
    time_t actime;
    // 文件修改时间, 从 1970-01-01 00:00:00 开始的秒数
    time_t modtime;
};

// 设置文件的访问和修改时间
extern int utime(const char *filename, struct utimbuf *times);

#endif
