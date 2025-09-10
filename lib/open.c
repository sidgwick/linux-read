/*
 *  linux/lib/open.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <stdarg.h>
#include <unistd.h>

/**
 * @brief open 系统调用
 *
 * 定义中使用可变参数是因为当使用 O_CREAT 标志时, 需要传递文件权限模式(mode)作为第三个参数(POSIX 标准规定)
 *
 * @param filename
 * @param flag
 * @param ...
 * @return int
 */
int open(const char *filename, int flag, ...)
{
    register int res;
    va_list arg;

    va_start(arg, flag);
    __asm__("int $0x80"
            : "=a"(res)
            : "0"(__NR_open), "b"(filename), "c"(flag), "d"(va_arg(arg, int)));

    if (res >= 0) {
        return res;
    }

    errno = -res;
    return -1;
}
