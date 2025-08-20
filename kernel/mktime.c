/*
 *  linux/kernel/mktime.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <time.h>

/*
 * This isn't the library routine, it is only used in the kernel.
 * as such, we don't care about years<1970 etc, but assume everything
 * is ok. Similarly, TZ etc is happily ignored. We just do everything
 * as easily as possible. Let's find something public for the library
 * routines (although I think minix times is public).
 */
/*
 * PS. I hate whoever though up the year 1970 - couldn't they have gotten
 * a leap-year instead? I also hate Gregorius, pope or no. I'm grumpy.
 */
#define MINUTE 60
#define HOUR (60 * MINUTE)
#define DAY (24 * HOUR)
#define YEAR (365 * DAY)

// 下面以年为界限, 定义了每个月开始时的秒数时间
/* interestingly, we assume leap-years */
static int month[12] = {
    /* 01 */ 0,
    /* 02 */ DAY * (31),
    /* 03 */ DAY * (31 + 29),
    /* 04 */ DAY * (31 + 29 + 31),
    /* 05 */ DAY * (31 + 29 + 31 + 30),
    /* 06 */ DAY * (31 + 29 + 31 + 30 + 31),
    /* 07 */ DAY * (31 + 29 + 31 + 30 + 31 + 30),
    /* 08 */ DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31),
    /* 09 */ DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31),
    /* 10 */ DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31 + 30),
    /* 11 */ DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31),
    /* 12 */ DAY * (31 + 29 + 31 + 30 + 31 + 30 + 31 + 31 + 30 + 31 + 30)};

/*
 * 该函数计算从 1970 年 01 月 01 日 00 时起到开机当日经过的秒数, 作为开机时间
 *
 * 这里闰年计算只考虑了 1999 年之前, 能用就行~
 *
 * 普通年：如果年份能被4整除且不能被100整除，则为闰年。例如，2016年是闰年，而1900年不是闰年。
 * 世纪年：如果年份能被400整除，则为闰年。例如，2000年是闰年，而2100年不是闰年。
 * 能被4整除的年份是闰年，但如果是整百年，则必须能被400整除才是闰年 */
long kernel_mktime(struct tm *tm)
{
    long res;
    int year;

    // 这里用 199x 年来理解就好
    year = tm->tm_year - 70; // 千年虫 bug
    /* magic offsets (y+1) needed to get leapyears right.
     * 70, 71, $72, 73, 74, 75, $76 ...., 每 4 年出现一次闰年
     * 在 2000 年之前, ((当前年份-1970 +1) / 4) 就能找到出现的闰年的数量 */
    res = YEAR * year + DAY * ((year + 1) / 4);
    res += month[tm->tm_mon]; /* 到当前月份, 今年度过的秒数 */

    /* and (y+2) here. If it wasn't a leap-year, we have to adjust
     * 如果当前年, 不是闰年, month 表中 2 月份多加了一天, 这里给他减去 */
    if (tm->tm_mon > 1 && ((year + 2) % 4))
        res -= DAY;

    res += DAY * (tm->tm_mday - 1);
    res += HOUR * tm->tm_hour;
    res += MINUTE * tm->tm_min;
    res += tm->tm_sec;
    return res;
}
