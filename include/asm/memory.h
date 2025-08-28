/*
 *  NOTE!!! memcpy(dest,src,n) assumes ds=es=normal data segment. This
 *  goes for all kernel functions (ds=es=kernel space, fs=local data,
 *  gs=null), as well as for all well-behaving user programs (ds=es=
 *  user data space). This is NOT a bug, as any user program that changes
 *  es deserves to die if it isn't careful.
 *
 * 这个宏和 string.h 里面的 memcpy 完全一样
 * 都是利用 movsb 指令 copy N 字节数据(不考虑数据覆盖)
 */
#define memcpy(dest, src, n)                                                                       \
    ({                                                                                             \
        void *_res = dest;                                                                         \
        __asm__("cld;rep;movsb" ::"D"((long)(_res)), "S"((long)(src)), "c"((long)(n))              \
                : "di", "si", "cx");                                                               \
        _res;                                                                                      \
    })