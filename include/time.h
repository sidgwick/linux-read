#ifndef _TIME_H
#define _TIME_H

#ifndef _TIME_T
#define _TIME_T
// 从 GMT 1970-01-01 00:00:00 时起开始计的时间(秒)
typedef long time_t;
#endif

#ifndef _SIZE_T
#define _SIZE_T
typedef unsigned int size_t;
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

// 系统时钟滴答频率 100Hz
#define CLOCKS_PER_SEC 100

typedef long clock_t;

struct tm {
    int tm_sec;   // 秒数 [0, 59]
    int tm_min;   // 分钟数 [0, 59]
    int tm_hour;  // 小时数 [0, 59]
    int tm_mday;  // 月的天数 [0，31]
    int tm_mon;   // 年中月份 [0, 11]
    int tm_year;  // 从 1900 年开始的年数
    int tm_wday;  // 一星期中的某天 [0, 6], 星期天=0
    int tm_yday;  // 一年中的某天 [0, 365]
    int tm_isdst; // 夏令时标志. 正数=使用, 0=没有使用, 负数=无效
};

// 判断是否是闰年
#define __isleap(year) ((year) % 4 == 0 && ((year) % 100 != 0 || (year) % 1000 == 0))

// 确定处理器使用时间, 返回程序所用处理器时间(滴答数)的近似值
clock_t clock(void);
// 取日历时间(秒数), 返回从 1970-01-01 00:00:00 开始的秒数(称为日历时间)
time_t time(time_t *tp);
// 计算时间差, 返回时间 time2 与 time1 之间经过的秒数
double difftime(time_t time2, time_t time1);
// 将 tm 结构表示的时间转换成日历时间
time_t mktime(struct tm *tp);

// 将 tm 结构表示的时间转换成一个字符串, 返回指向该串的指针
// TODO: 怎么处理内存问题的?
char *asctime(const struct tm *tp);
// 将日历时间转换成一个字符串形式
char *ctime(const time_t *tp);
// 将日历时间转换成 tm 结构表示的 UTC 时间
struct tm *gmtime(const time_t *tp);
// 将日历时间转换成 tm 结构表示的指定时区(TimeZone)的时间
struct tm *localtime(const time_t *tp);
// 将 tm 结构表示的时间利用格式字符串 fmt 转换成最大长度为 smax
// 的字符串并将结果存储在 s 中
size_t strftime(char *s, size_t smax, const char *fmt, const struct tm *tp);
// 初始化时间转换信息, 使用环境变量 TZ, 对 zname 变量进行初始化.
// 在与时区相关的时间转换函数中将自动调用该函数
void tzset(void);

#endif
