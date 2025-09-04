// 从 `%fs:addr` 处, 获取 1 字节数据
static inline unsigned char get_fs_byte(const char *addr)
{
    unsigned register char _v; // 注意这是个寄存器变量声明

    __asm__("movb %%fs:%1,%0" : "=r"(_v) : "m"(*addr));
    return _v;
}

// 从 `%fs:addr` 处, 获取 2 字节数据
static inline unsigned short get_fs_word(const unsigned short *addr)
{
    unsigned short _v;

    __asm__("movw %%fs:%1,%0" : "=r"(_v) : "m"(*addr));
    return _v;
}

// 从 `%fs:addr` 处, 获取 4 字节数据
static inline unsigned long get_fs_long(const unsigned long *addr)
{
    unsigned long _v;

    __asm__("movl %%fs:%1,%0" : "=r"(_v) : "m"(*addr));
    return _v;
}

// 将 1 字节数据放到 `%fs:addr` 处
static inline void put_fs_byte(char val, char *addr)
{
    __asm__("movb %0,%%fs:%1" ::"r"(val), "m"(*addr));
}

// 将 2 字节数据放到 `%fs:addr` 处
static inline void put_fs_word(short val, short *addr)
{
    __asm__("movw %0,%%fs:%1" ::"r"(val), "m"(*addr));
}

// 将 4 字节数据放到 `%fs:addr` 处
static inline void put_fs_long(unsigned long val, unsigned long *addr)
{
    __asm__("movl %0,%%fs:%1" ::"r"(val), "m"(*addr));
}

/*
 * Someone who knows GNU asm better than I should double check the followig.
 * It seems to work, but I don't know if I'm doing something subtly wrong.
 * --- TYT, 11/24/91
 * [ nothing wrong here, Linus ]
 */

// 获取 %fs 的值
static inline unsigned long get_fs()
{
    unsigned short _v;
    __asm__("mov %%fs,%%ax" : "=a"(_v) :);
    return _v;
}

// 获取 %ds 的值
static inline unsigned long get_ds()
{
    unsigned short _v;
    __asm__("mov %%ds,%%ax" : "=a"(_v) :);
    return _v;
}

// 设置 %fs 的值
static inline void set_fs(unsigned long val)
{
    __asm__("mov %0,%%fs" ::"a"((unsigned short)val));
}