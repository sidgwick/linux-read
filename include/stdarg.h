#ifndef _STDARG_H
#define _STDARG_H

// 定义 va_list 是一个字符指针类型, 后面的可变参数才能以字节为单位控制
typedef char *va_list;

/* Amount of space required in an argument list for an arg of type TYPE.
 * TYPE may alternatively be an expression whose type is used.
 *
 * 下面给出了类型为 TYPE 的 arg 参数列表所要求的空间容量
 * TYPE也可以是使用该类型的一个表达式 */

// 这句定义了取整后的 TYPE 类型的字节长度值, 是 int 长度(4)的倍数
// 这里算出来的实际上就是 TYPE 在参数栈上面的大小(占不够的话, 会补齐)
#define __va_rounded_size(TYPE) (((sizeof(TYPE) + sizeof(int) - 1) / sizeof(int)) * sizeof(int))

// 函数 __builtin_saveregs() 是在gcc的库程序libgcc2.c中定义的，用于保存寄存器.
// 相关说明参见 gcc手册“Target Description Macros”章中“Implementing the Varargs
// Macros”小节

/**
 * 将 AP(va_list指针) 指向​​第一个可变参数​​的地址
 *
 * 在第一次调用 va_arg 或 va_end 之前, 必须首先调用 va_start 宏, 参数 LASTARG
 * 是函数定义 中最右边参数的标识符, 即 `...` 左边的一个标识符. AP
 * 是可变参数表参数指针, LASTARG 是 最后一个指定参数. &(LASTARG)
 * 用于取其地址(即其指针), 并且该指针是字符类型, 加上 LASTARG 的 宽度值后 AP
 * 就是可变参数表中第一个参数的指针. 该宏没有返回值 */
#ifndef __sparc__
#define va_start(AP, LASTARG) (AP = ((char *)&(LASTARG) + __va_rounded_size(LASTARG)))
#else
#define va_start(AP, LASTARG)                                                                      \
    (__builtin_saveregs(), AP = ((char *)&(LASTARG) + __va_rounded_size(LASTARG)))
#endif

/**
 * 下面该宏用于被调用函数完成一次正常返回
 * va_end 可以修改 AP 使其在重新调用 va_start 之前不能被使用
 * va_end 必须在 va_arg 读完所有的参数后再被调用
 * 正常操作会将 AP 指向 NULL, 这里是什么都没有做 */
void va_end(va_list); /* Defined in gnulib */
#define va_end(AP)

/**
 * 获取可变列表里面 AP 指向的那个参数
 *
 * 第一次使用 va_arg 时, 它返回表中的第一个参数(也是 AP 指向的), 然后 AP
 * 指向下一个参数 后续的每次调用都一样的处理, 返回 AP 指向的那个参数, 并将 AP
 * 指向下一个位置
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 * 这段宏非常精妙, `x=va_arg(y, int)` 展开之后是一个逗号表达式:
 * x, y += __va_rounded_size(int), *(int*)(AP-__va_rounded_size(int))`
 * 逗号表达式执行顺序为从左到右依次计算每个子表达式,
 * 整个表达式的值为最后一个子表达式的值 这样就实现了 y 指向下一个位置, x
 * 指向当前要获取的数据位置的操作 */
#define va_arg(AP, TYPE) (AP += __va_rounded_size(TYPE), *((TYPE *)(AP - __va_rounded_size(TYPE))))

#endif /* _STDARG_H */
