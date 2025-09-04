/*
 *  linux/kernel/vsprintf.c
 *
 *  (C) 1991  Linus Torvalds
 */

/* vsprintf.c -- Lars Wirzenius & Linus Torvalds. */
/*
 * Wirzenius wrote this portably, Torvalds fucked it up :-)
 */

#include <stdarg.h>
#include <string.h>

/* we use this so that we can do without the ctype library */
#define is_digit(c) ((c) >= '0' && (c) <= '9')

/** 字符串转整形函数 */
static int skip_atoi(const char **s)
{
    int i = 0;

    while (is_digit(**s)) {
        i = i * 10 + *((*s)++) - '0';
    }

    return i;
}

#define ZEROPAD 1  /* pad with zero */
#define SIGN 2     /* unsigned/signed long */
#define PLUS 4     /* show plus */
#define SPACE 8    /* space if plus */
#define LEFT 16    /* left justified */
#define SPECIAL 32 /* 0x */
#define SMALL 64   /* use 'abcdef' instead of 'ABCDEF' */

/** 求 n / base, 最后商存在 n 里面, 余数作为 do_div 的值返回 */
#define do_div(n, base)                                                                            \
    ({                                                                                             \
        int __res;                                                                                 \
        __asm__("divl %4" : "=a"(n), "=d"(__res) : "0"(n), "1"(0), "r"(base));                     \
        __res;                                                                                     \
    })

// 将整数转换为指定进制的字符串。
// 输入：num-整数；base-进制；size-字符串长度；precision-数字长度(精度)；type-类型选项。
// 输出：数字转换成字符串后指向该字符串末端后面的指针。
static char *number(char *str, int num, int base, int size, int precision, int type)
{
    char c, sign, tmp[36];
    const char *digits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int i;

    /* 小写显示 */
    if (type & SMALL) {
        digits = "0123456789abcdefghijklmnopqrstuvwxyz";
    }

    /* 如果要求左对齐, 把 type 里面 zeropad 位清理掉 */
    if (type & LEFT) {
        type &= ~ZEROPAD;
    }

    /* 最多支持到 2~36 进制 */
    if (base < 2 || base > 36) {
        return 0;
    }

    /* c 将来用于在前面部分 padding */
    c = (type & ZEROPAD) ? '0' : ' ';

    /* 正负号的处理 */
    if (type & SIGN && num < 0) {
        sign = '-';
        num = -num;
    } else {
        sign = (type & PLUS) ? '+' : ((type & SPACE) ? ' ' : 0);
    }

    /* 如果有符号, 则符号肯定已经占据了一个显示位置 */
    if (sign) {
        size--;
    }

    /* 16 进制或者 8 进制, 减去前缀 */
    if (type & SPECIAL) {
        if (base == 16) {
            size -= 2;
        } else if (base == 8) {
            size--;
        }
    }

    /* 按照 base 进制, 拆出来每一位上面的数值 */
    i = 0;
    if (num == 0) {
        tmp[i++] = '0';
    } else {
        while (num != 0) {
            tmp[i++] = digits[do_div(num, base)];
        }
    }

    /* 如果想要的精度太小, 显示不下, 那就要把精度调整大一些, 以完全显示 */
    if (i > precision) {
        precision = i;
    }

    size -= precision; /* 去掉数字显示本身, 还剩下的 size */

    /* 如果不是 0 填充前部, 也不是左对齐, 就 padding 进 size 个空格 */
    if (!(type & (ZEROPAD + LEFT))) {
        while (size-- > 0) {
            *str++ = ' ';
        }
    }

    /* 有符号? 追加符号到结果 */
    if (sign) {
        *str++ = sign;
    }

    /* 特殊前缀? 追加特殊前缀内容 */
    if (type & SPECIAL) {
        if (base == 8) {
            *str++ = '0';
        } else if (base == 16) {
            *str++ = '0';
            *str++ = digits[33];
        }
    }

    /* 不是左对齐, 填充空格进来: 类似 0x003A 中的 00 */
    if (!(type & LEFT)) {
        while (size-- > 0) {
            *str++ = c;
        }
    }

    /* i 存有数值 num 的数字个数, 若数字个数小于精度值, 则str中放入(精度值-i)个
     * 0 */
    while (i < precision--) {
        *str++ = '0';
    }

    /* 数字本身 */
    while (i-- > 0) {
        *str++ = tmp[i];
    }

    /* 如果还有 size, 一律使用空格填充 */
    while (size-- > 0) {
        *str++ = ' ';
    }

    return str;
}

int vsprintf(char *buf, const char *fmt, va_list args)
{
    int len;
    int i;
    char *str;
    char *s;
    int *ip;

    int flags; /* flags to number() */

    int field_width; /* width of output field */
    int precision;   /* min. # of digits for integers; max number of chars for from string */
    int qualifier __attribute__((unused)); /* 'h', 'l', or 'L' for integer fields */

    for (str = buf; *fmt; ++fmt) {
        // 如果没有遇到格式化字符, 就持续直接打印
        if (*fmt != '%') {
            *str++ = *fmt;
            continue;
        }

        /* process flags, 从格式化字符串中, 解析出打印 flags */
        flags = 0;
    repeat:
        ++fmt; /* this also skips first '%' */
        switch (*fmt) {
        case '-':
            flags |= LEFT;
            goto repeat; // 左靠齐调整
        case '+':
            flags |= PLUS;
            goto repeat; // 放加号
        case ' ':
            flags |= SPACE;
            goto repeat; // 放空格
        case '#':
            flags |= SPECIAL;
            goto repeat; // 是特殊转换
        case '0':
            flags |= ZEROPAD;
            goto repeat; // 要填零(即'0')
        }

        /**
         * 取当前参数字段宽度域值, 放入 field_width 变量中
         *
         * 如果宽度域中是数值则直接取其为宽度值
         * 如果宽度域中是字符 '*', 表示下一个参数指定宽度, 因此调用 va_arg
         * 取宽度值. 若此时宽度值小于 0, 则该负数表示其带有标志域 '-'
         * 标志(左靠齐), 因此还需在标志变量中添入该标志,
         * 并将字段宽度值取为其绝对值 */

        /* get field width, fmt中指定的打印的宽度 */
        field_width = -1;
        if (is_digit(*fmt)) {
            field_width = skip_atoi(&fmt);
        } else if (*fmt == '*') {
            /* it's the next argument */
            fmt++; // BUG fix
            field_width = va_arg(args, int);
            if (field_width < 0) {
                field_width = -field_width;
                flags |= LEFT;
            }
        }

        /* get the precision. 精度域开始的标志是 `.` */
        precision = -1;
        if (*fmt == '.') {
            ++fmt;
            if (is_digit(*fmt)) {
                precision = skip_atoi(&fmt);
            } else if (*fmt == '*') {
                /* it's the next argument */
                fmt++;
                precision = va_arg(args, int);
            }

            if (precision < 0) {
                precision = 0;
            }
        }

        /* get the conversion qualifier */
        qualifier = -1;
        if (*fmt == 'h' || *fmt == 'l' || *fmt == 'L') {
            qualifier = *fmt;
            ++fmt;
        }

        // 下面分析转换指示符
        switch (*fmt) {
        case 'c':
            // 如果转换指示符是 'c', 则表示对应参数应是字符
            // 此时如果标志域表明不是左靠齐, 则该字段前面放入 '宽度域值-1'
            // 个空格字符, 然后再放入参数字符 如果宽度域还大于0, 则表示为左靠齐,
            // 则在参数字符后面添加 '宽度值-1' 个空格字符
            if (!(flags & LEFT)) {
                while (--field_width > 0) {
                    *str++ = ' ';
                }
            }
            *str++ = (unsigned char)va_arg(args, int);
            while (--field_width > 0) {
                *str++ = ' ';
            }
            break;

        case 's':
            s = va_arg(args, char *);
            len = strlen(s);
            if (precision < 0) {
                precision = len;
            } else if (len > precision) {
                len = precision; /* 截断 */
            }

            if (!(flags & LEFT)) {
                while (len < field_width--) {
                    *str++ = ' ';
                }
            }

            for (i = 0; i < len; ++i) {
                *str++ = *s++;
            }

            while (len < field_width--) {
                *str++ = ' ';
            }

            break;

        case 'o':
            str = number(str, va_arg(args, unsigned long), 8, field_width, precision, flags);
            break;

        case 'p':
            // 如果格式转换符是 'p', 表示对应参数是一个指针类型
            // 此时若该参数没有设置宽度域, 则默认宽度为8, 并且需要添零
            if (field_width == -1) {
                field_width = 8; /* 这个打印 0xABCDEF, 有点不伦不类呢? */
                flags |= ZEROPAD;
            }
            str =
                number(str, (unsigned long)va_arg(args, void *), 16, field_width, precision, flags);
            break;

        case 'x':
            flags |= SMALL;
        case 'X':
            str = number(str, va_arg(args, unsigned long), 16, field_width, precision, flags);
            break;

        case 'd':
        case 'i':
            flags |= SIGN;
        case 'u':
            str = number(str, va_arg(args, unsigned long), 10, field_width, precision, flags);
            break;

        case 'n':
            // 若格式转换指示符是 'n',
            // 则表示要把到目前为止转换输出字符数保存到对应参数指针指定的位置中
            // 首先利用 va_arg 取得该参数指针,
            // 然后将已经转换好的字符数存入该指针所指的位置
            ip = va_arg(args, int *);
            *ip = (str - buf);
            break;

        default:
            if (*fmt != '%') {
                *str++ = '%';
            }

            if (*fmt) {
                *str++ = *fmt;
            } else {
                --fmt;
            }

            break;
        }
    }

    *str = '\0';
    return str - buf;
}
