// 硬件端口字节输出
#define outb(value, port)                                                                          \
    __asm__("outb %%al,%%dx" /* outb value, port */                                                \
            :                                                                                      \
            : "a"(value), "d"(port))

// 硬件端口字节输入
#define inb(port)                                                                                  \
    ({                                                                                             \
        unsigned char _v;                                                                          \
        __asm__ volatile("inb %%dx,%%al" /* inb port, _v */                                        \
                         : "=a"(_v)                                                                \
                         : "d"(port));                                                             \
        _v;                                                                                        \
    })

// 带延迟的硬件端口字节输出函数, 使用两条跳转语句来延迟一会
#define outb_p(value, port)                                                                        \
    __asm__("outb %%al,%%dx\n\t"                                                                   \
            "jmp 1f\n"                                                                             \
            "1:  jmp 1f\n"                                                                         \
            "1:"                                                                                   \
            :                                                                                      \
            : "a"(value), "d"(port))

// 带延迟的硬件端口字节输入函数, 使用两条跳转语句来延迟一会
#define inb_p(port)                                                                                \
    ({                                                                                             \
        unsigned char _v;                                                                          \
        __asm__ volatile("inb %%dx, %%al\n\t"                                                      \
                         "jmp 1f\n"                                                                \
                         "1:  jmp 1f\n"                                                            \
                         "1:"                                                                      \
                         : "=a"(_v)                                                                \
                         : "d"(port));                                                             \
        _v;                                                                                        \
    })
