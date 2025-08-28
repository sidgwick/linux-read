#ifndef _STRING_H_
#define _STRING_H_

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef _SIZE_T
#define _SIZE_T
typedef unsigned int size_t;
#endif

extern char *strerror(int errno);

/*
 * This string-include defines all string functions as inline
 * functions. Use gcc. It also assumes ds=es=data space, this should be
 * normal. Most of the string-functions are rather heavily hand-optimized,
 * see especially strtok,strstr,str[c]spn. They should work, but are not
 * very easy to understand. Everything is done entirely within the register
 * set, making the functions fast and clean. String instructions have been
 * used through-out, making for "slightly" unclear code :-)
 *
 *        (C) 1991 Linus Torvalds
 */

/**
 * 一些内联汇编的知识
 *
 *  ​​r​​  通用寄存器
 * 变量可存放于任意通用寄存器，如"r"(a)表示变量a存入寄存器
 * ​​m​​ 内存操作数
 * 变量直接存储在内存中，避免寄存器加载开销，如"m"(*ptr) ​​i​​ 立即数
 * 直接使用常量值（如"i"(5)），无需额外加载指令
 *  ​​g​​  通用/内存/立即数
 * 编译器自动选择最优存储方式（寄存器/内存/常量）
 */

/**
 * 从 src 拷贝数据到 dest */
extern inline char *strcpy(char *dest, const char *src)
{
    /**
     * ------------------------
     *     cld # 正向移动
     * 1:
     *     lodsb # 从 (%esi) 加载一个字节到 %al, %esi 自增
     *     stosb # 将 %al 移动到 (%edi), %edi 自增
     *     testb %al, %al # 遇到 %al == 0 结束, 否则继续拷贝
     *     jne 1b
     * ------------------------
     * 输出寄存器列表为空
     * 输入寄存器列表 S 表示 %esi, `"S"(src)` 表示将 src 装到 %esi 里面
     * 输入寄存器列表 D 表示 %edi, `"D"(dst)` 表示将 dst 装到 %edi 里面
     * 被破坏的寄存器列表有 si, di, ax */

    __asm__("cld\n"
            "1:\tlodsb\n\t"
            "stosb\n\t"
            "testb %%al,%%al\n\t"
            "jne 1b"
            :
            : "S"(src), "D"(dest)
            : "si", "di", "ax");
    return dest;
}

/**
 * 从 src 拷贝 count 字节到 dest
 * src 长度不够的, 使用 NULL 填充 */
extern inline char *strncpy(char *dest, const char *src, int count)
{
    /**
     * ------------------------
     *     cld # 正向移动
     * 1:
     *     decl %ecx # 计数器 -1
     *     js 2f # 如果变号了 (SF = 1) 跳出循环, 就是 %ecx < 0 了
     *     lodsb # 从 (%esi) 加载一个字节到 %al, %esi 自增
     *     stosb # 将 %al 移动到 (%edi), %edi 自增
     *     testsb %al, %al # 遇到 %al == 0 结束, 否则继续拷贝
     *     jne 1b
     *     rep stosb # 这里是复制中遇到 NULL 的情况, 剩下的空间全部使用 NULL
     * 字符填充 2:
     * ------------------------ */

    __asm__("cld\n"
            "1:\tdecl %2\n\t"
            "js 2f\n\t"
            "lodsb\n\t"
            "stosb\n\t"
            "testb %%al,%%al\n\t"
            "jne 1b\n\t"
            "rep\n\t"
            "stosb\n"
            "2:"
            :
            : "S"(src), "D"(dest), "c"(count)
            : "si", "di", "ax", "cx");
    return dest;
}

/**
 * 将 src 拼接在 dest 后面 */
extern inline char *strcat(char *dest, const char *src)
{
    /**
     * scasb 比较 %al 和 (%edi), 比较完后 %edi+1, %ecx-1
     * ------------------------
     *      cld # 正向移动
     *      repne scasb # REP-NOT-EQUAL
     *      decl %edi # %edi 现在指向的是 NULL, 因此要退一下
     * 1:
     *     lodsb # 从 (%esi) 加载一字节到 %al
     *     stosb # 将 %al 写入到 (%edi)
     *     testb %al, %al # 遇到 %al == 0 结束, 否则继续拷贝
     *     jne 1b
     * ------------------------ */

    __asm__("cld\n\t"
            "repne\n\t"
            "scasb\n\t"
            "decl %1\n"
            "1:\tlodsb\n\t"
            "stosb\n\t"
            "testb %%al,%%al\n\t"
            "jne 1b"
            :
            : "S"(src), "D"(dest), "a"(0), "c"(0xffffffff)
            : "si", "di", "ax", "cx");
    return dest;
}

/**
 * 将最多 count 字节 src 拼接在 dest 后面 */
extern inline char *strncat(char *dest, const char *src, int count)
{
    /**
     * scasb 比较 %al 和 (%edi), 比较完后 %edi+1, %ecx-1
     * ------------------------
     *     cld
     *     repne scasb
     *     decl %edi
     *     mov count, %cx # count 用 g 修饰, 被编译器合理安排存储位置了
     * 1:
     *     # 上面找到了 dest 的 NULL 位置, 接下来从 src 复制剩余的字节过来
     *     decl %cx
     *     js 2f
     *     lodsb
     *     stosb
     *     testb %al, %al
     *     jne 1b
     * 2:
     *     xorl %eax, %eax
     *     stosb # %al -> (%edi), %edi+=1
     * ------------------------ */
    __asm__("cld\n\t"
            "repne\n\t"
            "scasb\n\t"
            "decl %1\n\t"
            "movl %4,%3\n"
            "1:\tdecl %3\n\t"
            "js 2f\n\t"
            "lodsb\n\t"
            "stosb\n\t"
            "testb %%al,%%al\n\t"
            "jne 1b\n"
            "2:\txorl %2,%2\n\t"
            "stosb"
            :
            : "S"(src), "D"(dest), "a"(0), "c"(0xffffffff), "g"(count)
            : "si", "di", "ax", "cx");
    return dest;
}

/**
 * 比较 cs, ct, 返回 `sign(cs-ct) * 1` */
extern inline int strcmp(const char *cs, const char *ct)
{
    /**
     * scasb 比较 %al 和 (%edi), 比较完后 %edi+1, %ecx-1
     * ------------------------
     *     cld
     * 1:
     *     lodsb # %al = (%esi); %esi+=1
     *     scasb # [%al CMP (%edi)]; %edi+=1
     *     jne 2f # 不相等, 已经得到了结果
     *     testb %al, %al
     *     jne 1b # src 已经结束, 那就认为两者相等, 下一行设置 %eax = 0
     *     xorl %eax, %eax
     *     jmp 3f
     * 2:
     *     movl $1, %eax # 先假定 src > dst, %eax = 1
     *     jl 3f
     *     negl %eax # 如果 src < dst, %eax = -1
     * 3:
     * ------------------------ */

    register int __res __asm__("ax"); // 表示将 __res 直接绑定到 AX 寄存器
    __asm__("cld\n"
            "1:\tlodsb\n\t"
            "scasb\n\t"
            "jne 2f\n\t"
            "testb %%al,%%al\n\t"
            "jne 1b\n\t"
            "xorl %%eax,%%eax\n\t"
            "jmp 3f\n"
            "2:\tmovl $1,%%eax\n\t"
            "jl 3f\n\t"
            "negl %%eax\n"
            "3:"
            : "=a"(__res)
            : "D"(cs), "S"(ct)
            : "si", "di");
    return __res;
}

/**
 * 比较 cs, ct 的 count 个字节, 返回 `sign(cs-ct) * 1` */
extern inline int strncmp(const char *cs, const char *ct, int count)
{
    /**
     * ------------------------
     *     cld
     * 1:
     *     decl %ecx # count -= 1
     *     js 2f
     *     lodsb # %esi -> %al ~~~ %esi+=1
     *     scasb # %al == %edi ~~~ %edi+=1
     *     jne 3f
     *     testb %al, %al
     *     jne 1b
     * 2:
     *     xorl %eax, %eax
     *     jmp
     * 3:
     *     movl $1, %eax
     *     jl 4f
     *     negl %eax
     * 4:
     * ------------------------ */

    register int __res __asm__("ax");
    __asm__("cld\n"
            "1:\tdecl %3\n\t"
            "js 2f\n\t"
            "lodsb\n\t"
            "scasb\n\t"
            "jne 3f\n\t"
            "testb %%al,%%al\n\t"
            "jne 1b\n"
            "2:\txorl %%eax,%%eax\n\t"
            "jmp 4f\n"
            "3:\tmovl $1,%%eax\n\t"
            "jl 4f\n\t"
            "negl %%eax\n"
            "4:"
            : "=a"(__res)
            : "D"(cs), "S"(ct), "c"(count)
            : "si", "di", "cx");
    return __res;
}

/**
 * 在字符串 s 里面寻找 c
 * 返回 c 第一次出现的位置的指针, 如果没找到, 则返回 NULL */
extern inline char *strchr(const char *s, char c)
{
    /**
     * 约束 "0"(c) 表示参数 c 使用与第 0 个操作数相同的寄存器
     * ------------------------
     *     cld
     *     movb %al, %ah    # 这里实际上是把 c -> %ah
     * 1:
     *     lodsb            # (%esi) --> %al, %esi++
     *     cmpb %ah, %al    # 是否找到, 找到跳走, 找不到继续循环
     *     je 2f
     *     testb %al, %al   # 已经到结束位置还是没找到, 则 %esi = 1
     *     jne 1b
     *     movl $1, %esi
     * 2:
     *     movl %esi, %eax # 计算结果
     *        decl %eax # 回退一个位置, 此时指向的是 c 所在的位置(或者 NULL)
     * ------------------------ */
    register char *__res __asm__("ax");
    __asm__("cld\n\t"
            "movb %%al,%%ah\n"
            "1:\tlodsb\n\t"
            "cmpb %%ah,%%al\n\t"
            "je 2f\n\t"
            "testb %%al,%%al\n\t"
            "jne 1b\n\t"
            "movl $1,%1\n"
            "2:\tmovl %1,%0\n\t"
            "decl %0"
            : "=a"(__res)
            : "S"(s), "0"(c)
            : "si");
    return __res;
}

/**
 * 在字符串 s 里面从右往左寻找 c, 返回 c 第一次
 * 本函数实际上是从左往右扫描, 记录 c 最近一次出现的位置, 扫描结束后返回 */
extern inline char *strrchr(const char *s, char c)
{
    /**
     * 约束 "0"(0) 表示将 %0 占位指代的 %edx 置 0
     * ------------------------
     *     cld
     *     movb %al, %ah
     * 1:
     *     lodsb            # (%esi) -> %al, %esi++
     *     cmpb %ah, %al    # 是否找到了 c
     *     jne 2f
     *     mov %esi, %edx   # 找到了, 保存 c 的位置到结果 %edx
     *     decl %edx
     * 2:
     *     testb %al, %al   # 接着找直到遇到 s 的结束 NULL 标记
     *     jne 1b
     * ------------------------ */
    register char *__res __asm__("dx");
    __asm__("cld\n\t"
            "movb %%al,%%ah\n"
            "1:\tlodsb\n\t"
            "cmpb %%ah,%%al\n\t"
            "jne 2f\n\t"
            "movl %%esi,%0\n\t"
            "decl %0\n"
            "2:\ttestb %%al,%%al\n\t"
            "jne 1b"
            : "=d"(__res)
            : "0"(0), "S"(s), "a"(c)
            : "ax", "si");
    return __res;
}

/**
 * 返回 cs 中第一个不在字符串 ct 中出现的字符下标
 * strspn 是 string span 的缩写, span 强调对连续字符序列长度的计算,
 * 而非定位单个字符 */
extern inline int strspn(const char *cs, const char *ct)
{
    /**
     * ------------------------
     *     cld
     *
     *     # 入参列表已经设置过 %al = 0, 这里利用 repne 不断寻找, 直到找到 ~ct
     * 的结束符 # 找到后, %edi 指向这个结束符 movl ~ct, %edi repne scasb      #
     * 本步骤比较 %al 和 (%edi)
     *
     *        notl %ecx        # %ecx 取反, 得到的是刚刚 repne 循环的次数
     *        decl %ecx
     *        movl %ecx, %edx  # ct 长度记录到 %edx
     * 1:
     *     lodsb            # (%esi) -> %al
     *     testb %al, %al   # 找到结束符
     *     je 2f
     *     movl ~ct, %edi   # 重新把 ct 加载到 %edi
     *     movl %edx, %ecx  # 循环次数是 ct 的长度
     *     repne scasb      # 循环比较, 找到 %al, (%edi) 相等停止
     *     je 1b            # 如果是因为相等退出上一行循环, 就尝试 cs
     * 往后移一位, 继续比较 # 因此如果是因为不相等推出了, 那就实现了本函数的目的
     *     # 也即: 报告 cs 里面首个一个没有在 ct 里面出现的字符位置
     * 2:
     *     decl %esi
     * ------------------------ */
    register char *__res __asm__("si");
    __asm__("cld\n\t"
            "movl %4,%%edi\n\t"
            "repne\n\t"
            "scasb\n\t"
            "notl %%ecx\n\t"
            "decl %%ecx\n\t"
            "movl %%ecx,%%edx\n"
            "1:\tlodsb\n\t"
            "testb %%al,%%al\n\t"
            "je 2f\n\t"
            "movl %4,%%edi\n\t"
            "movl %%edx,%%ecx\n\t"
            "repne\n\t"
            "scasb\n\t"
            "je 1b\n"
            "2:\tdecl %0"
            : "=S"(__res)
            : "a"(0), "c"(0xffffffff), "0"(cs), "g"(ct)
            : "ax", "cx", "dx", "di");
    return __res - cs;
}

/**
 * 返回 cs 中第一个在字符串 ct 中出现的字符下标
 * strcspn 是 string complement span 的缩写 */
extern inline int strcspn(const char *cs, const char *ct)
{
    /**
     * ------------------------
     *     cld
     *     movl V(ct), %edi
     *     repne scasb      # %al == (%edi)
     *     notl %ecx
     *     decl %ecx
     *     movl %ecx, %edx  # 先计算好 ct 的长度
     * 1:
     *     lodsb            # (%esi) -> %al
     *     testb %al, %al
     *     je 2f
     *     movl V(ct), %edi # 接下来三行, 寻找 cs 里面的字符在 ct 里面的出现情况
     *     movl %edx, %ecx
     *     repne scasb      # 字符一直没有在 ct 出现, 就一直比较
     *     jne 1b           # 如果因为不相等退出上一行 rep 循环,
     * 跳转继续找是否还有更多不相等 # JNE 说明还没有找到全部的不属于 ct
     * 前缀字符集合, 因此要继续 #     否则, 说明当前的 %al 出现在了 ct 里面, 与
     * strcspn 目的不符, 因此就到了终止的时候 2: decl %0
     * ------------------------ */
    register char *__res __asm__("si");
    __asm__("cld\n\t"
            "movl %4,%%edi\n\t"
            "repne\n\t"
            "scasb\n\t"
            "notl %%ecx\n\t"
            "decl %%ecx\n\t"
            "movl %%ecx,%%edx\n"
            "1:\tlodsb\n\t"
            "testb %%al,%%al\n\t"
            "je 2f\n\t"
            "movl %4,%%edi\n\t"
            "movl %%edx,%%ecx\n\t"
            "repne\n\t"
            "scasb\n\t"
            "jne 1b\n"
            "2:\tdecl %0"
            : "=S"(__res)
            : "a"(0), "c"(0xffffffff), "0"(cs), "g"(ct)
            : "ax", "cx", "dx", "di");
    return __res - cs;
}

/**
 * 在 cs 中搜索​​首个出现​​在 ct 中的字符, 并返回该字符在
 * cs 中的指针 strpbrk = String Pointer Break 顺便说如果 ct 中有 cs 中的字符,
 * 这个函数实际上就是 res = cs + strspn(cs, ct) */
extern inline char *strpbrk(const char *cs, const char *ct)
{
    /**
     * ------------------------
     *     cld
     *     movl CT, %edi
     *     repne scasb
     *     notl %ecx
     *     decl %ecx
     *     movl %ecx, %edx  # EDX = ct length
     * 1:
     *     lodsb            # (%esi) -> %al
     *     testb %al, %al
     *     je 2f
     *     movl CT, %edi
     *     movl %edx, %ecx
     *     repne scasb      # cs 里面的字符 C 逐次和 ct 比较, 到 ct 出现 C 终止
     *     jne 1b           # 如果 C 没有在 ct 出现过, 就继续检测下一个 C 的情况
     *     decl %esi        # RES-=1, 如果 C 出现在 ct 里面, 把 %esi 往前指,
     * 这样 %esi 指向的 #         就是 cs 中前面的字符从来都不在 ct
     * 里面出现的那个位置
     * ############# 上面实际上就是 strcspn 功能的一部分
     *     jmp 3f
     * 2:
     *     xorl %0, %0      # 如果从来都没找到, %esi 就是 NULL
     * 3:
     * ------------------------ */
    register char *__res __asm__("si");
    __asm__("cld\n\t"
            "movl %4,%%edi\n\t"
            "repne\n\t"
            "scasb\n\t"
            "notl %%ecx\n\t"
            "decl %%ecx\n\t"
            "movl %%ecx,%%edx\n"
            "1:\tlodsb\n\t"
            "testb %%al,%%al\n\t"
            "je 2f\n\t"
            "movl %4,%%edi\n\t"
            "movl %%edx,%%ecx\n\t"
            "repne\n\t"
            "scasb\n\t"
            "jne 1b\n\t"
            "decl %0\n\t"
            "jmp 3f\n"
            "2:\txorl %0,%0\n"
            "3:"
            : "=S"(__res)
            : "a"(0), "c"(0xffffffff), "0"(cs), "g"(ct)
            : "ax", "cx", "dx", "di");
    return __res;
}

/**
 * 在 cs 中寻找 ct 子串, 返回 cs 中的位置, 如果找不到返回 NULL */
extern inline char *strstr(const char *cs, const char *ct)
{
    /**
     * ------------------------
     *     cld
     *     movl CT, %edi
     *     repne scasb
     *     notl %ecx
     *     decl %ecx        # NOTE! This also sets Z if searchstring=''
     *     movl %ecx, %edx  # EDX = len(ct)
     * 1:
     *     movl CT, %edi
     *     movl %esi, %eax
     *     movl %edx, %ecx
     *     repe cmpsb       # 比较字符串, 直到出现不相同
     *     je 2f            # 比较范围内全都相同(子串完全匹配), 跳到结束
     *     xchgl %eax, %esi # 交换 EAX, ESI
     *                      # 以第一轮循环为例, 原来的 EAX 是指向 cs[0] 的指针,
     * ESI 是 cs[x] != ct[x] 的位置 # 因此这里交换之后 ESI 指向本次循环的 cs
     * 初始位置, EAX 指向那个不同位置 incl %esi        # 现在直接把 ESI
     * 往后移动一位, 打算重新比较 cmpb $0, -1(%eax) # 比较一下 EAX
     * 指向的前个位置是不是 NULL, 是的话就是没有在 cs 发现 ct 子串, 程序结束 jne
     * 1b xorl %eax, %eax 2:
     * ------------------------ */
    register char *__res __asm__("ax");
    __asm__("cld\n\t"
            "movl %4,%%edi\n\t"
            "repne\n\t"
            "scasb\n\t"
            "notl %%ecx\n\t"
            "decl %%ecx\n\t" /* NOTE! This also sets Z if searchstring='' */
            "movl %%ecx,%%edx\n"
            "1:\tmovl %4,%%edi\n\t"
            "movl %%esi,%%eax\n\t"
            "movl %%edx,%%ecx\n\t"
            "repe\n\t"
            "cmpsb\n\t"
            "je 2f\n\t" /* also works for empty string, see above */
            "xchgl %%eax,%%esi\n\t"
            "incl %%esi\n\t"
            "cmpb $0,-1(%%eax)\n\t"
            "jne 1b\n\t"
            "xorl %%eax,%%eax\n\t"
            "2:"
            : "=a"(__res)
            : "0"(0), "c"(0xffffffff), "S"(cs), "g"(ct)
            : "cx", "dx", "di", "si");
    return __res;
}

/**
 * 获取 NULL 结尾的字符串长度 */
extern inline int strlen(const char *s)
{
    register int __res __asm__("cx");
    __asm__("cld\n\t"
            "repne\n\t"
            "scasb\n\t"
            "notl %0\n\t"
            "decl %0"
            : "=c"(__res)
            : "D"(s), "a"(0), "0"(0xffffffff)
            : "di");
    return __res;
}

// 因为 strtok 可以被多次调用, 需要一个地方存放原始字符串的扫描状态
// 这个全局变量就是用于临时存放指向被分析字符串 s 的指针的
extern char *___strtok;

/**
 * 利用 ct 中的字符将 s 分割成标记 tokern 序列
 *
 * 将 s 看作是包含零个或多个 token 的序列, 并由分割符字符串 ct
 * 中的一个或多个字符分开 第一次调用 strtok 时, 将返回指向 s 中第 1 个 token
 * 首字符的指针, 并在返回 token 时 将 null 字符写到分割符处, 后续使用 null 作为
 * s 的调用, 将用这种方法继续扫描 s, 直到没有 token 为止. 在不同的调用过程中,
 * 分割符串 ct 可以不同
 *
 * 返回字符串 s 中第 1 个 token, 如果没有找到 token, 则返回一个 null 指针
 * 后续使用字符串 s 指针为 null 的调用, 将在原字符串 s 中搜索下一个token */
extern inline char *strtok(char *s, const char *ct)
{
    /**
     * ------------------------
     *     testl %esi, %esi    # 检查输入字符串 s 是否为 NULL
     *     jne 1f              # s == NULL 的话, 再检查一下 __strtok 的情况
     *     testl %ebx, %ebx
     *     je 8f               # s == NULL && __strtok == NULL, 直接结束
     *     movl %ebx, %esi     # s == NULL && __strtok != NULL, 这说明 strtok
     * 不是第一次调用 # 这种情况就把之前保存的 __strtok 赋值给 %esi
     *
     * # 1 标号这段程序, 用来计算 CT 的长度
     * 1:
     *     xorl %ebx, %ebx     # %ebx 清零
     *     movl $-1, %ecx      # %ecx = -1, 其实就是给 %ecx = 0xFFFFFFFF
     *     xorl %eax, %eax     # %eax 清零
     *     cld
     *     movl CT, %edi
     *     repne scasb         # loop if (%edi) != %al
     *     notl %ecx
     *     decl %ecx           # %ecx = len(ct)
     *     je 7f               # je 检查 ZF, 这里 ZF=0 说明 ct 是空串
     *     movl %ecx, %edx     # EDX = len(ct)
     *
     * # 2 标号这段用来跳过 s 开头位置的 ct 字符集合, 避免空 token 出现
     * # 本段中 %esi 是待处理的字符串(来自 s 或者 __strtok)
     * 2:
     *     lodsb               # %(esi) -> %al
     *     testb %al, %al
     *     je 7f               # [(%al & %al) == 0], 已经扫描到末尾,
     * 则跳转到标号 7 结束, NULL Token movl CT, %edi movl %edx, %ecx repne scasb
     * # 注意这里 %al 还在字符串的开头位置 je 2b               # 如果 s[x] 等于
     * ct 里面的某个字符, 这些字符处于开头位置 # 拿到的实际上是一个空 token,
     * 丢弃, 重新找非空的 token
     *
     *     # 到这里我们的 s 最少能提供一个字符的非空 token
     *     decl %esi           # 指向这个 token 的字符, 也就是 s[x-1]
     *     cmpb $0, (%esi)     # 判断 s[x-1] 是否等于 NULL, 如果等于 NULL
     * 跳转到标号 7 结束, NULL Token je 7f movl %esi, %ebx     # 保存 s[x-1]
     * 位置到 __res
     *
     * # 标号 3 这部分, 尝试扩大 token 的长度
     * # 现在 %ebx 指向的位置, 就是 token 的开始位置
     * 3:
     *     lodsb
     *     testb %al, %al
     *     je 5f           # 已经扫描结束, 跳转到标号 5, 计算最终结果
     *     movl CT, %edi
     *     movl %edx, %ecx
     *     repne scasb
     *     jne 3b          # 如果 s[y] 不是分隔符, 继续看看 s[y+1] 的情况
     *     decl %esi       # 尤其要注意, 上面说的 s[y] 是通过 lodsb 加载到 %al
     * 里面的 # lodsb 之后, %esi 会指向 y+1, 于是这里需要 %esi-1, 才能让 %esi
     * 指向分隔符的位置 cmpb $0, (%esi) je 5f           # 分隔符 == NULL
     * 说明已经扫描结束, 跳转到标号 5, 计算最终结果 movb $0, (%esi) #
     * 把分隔符替换为 NULL - strtok 就可以愉快的返回那个 NULL 结尾的 token 了
     *     incl %esi       # 重新把 %esi 指向分隔符后面的那个位置
     *     jmp 6f
     *
     * # 标号 5 是 s 扫描结束之后跳过来的
     * 5:
     *     xorl %esi, %esi # ___strtok 置 NULL
     *
     * # 标号 6 是正常扫描到 Token 跳过来的
     * 6:
     *     cmpb $0, (%ebx) # %ebx 指向的是 Token 开始位置, 因此这里在检查 Token
     * 是不是 NULL Token jne 7f          # Token != NULL ? xorl %ebx, %ebx # res
     * = NULL
     *
     * # 标号 7 是扫到 token 之后跳过来的, token 可以是空或者非空
     * 7:
     *     testl %ebx, %ebx
     *     jne 8f          # res != NULL ?
     *     movl %ebx, %esi # ___strtok = NULL
     *
     * # 标号 8 正常结束
     * 8:
     * ------------------------ */
    register char *__res __asm__("si");
    __asm__("testl %1,%1\n\t"
            "jne 1f\n\t"
            "testl %0,%0\n\t"
            "je 8f\n\t"
            "movl %0,%1\n"
            "1:\txorl %0,%0\n\t"
            "movl $-1,%%ecx\n\t"
            "xorl %%eax,%%eax\n\t"
            "cld\n\t"
            "movl %4,%%edi\n\t"
            "repne\n\t"
            "scasb\n\t"
            "notl %%ecx\n\t"
            "decl %%ecx\n\t"
            "je 7f\n\t" /* empty delimeter-string */
            "movl %%ecx,%%edx\n"
            "2:\tlodsb\n\t"
            "testb %%al,%%al\n\t"
            "je 7f\n\t"
            "movl %4,%%edi\n\t"
            "movl %%edx,%%ecx\n\t"
            "repne\n\t"
            "scasb\n\t"
            "je 2b\n\t"
            "decl %1\n\t"
            "cmpb $0,(%1)\n\t"
            "je 7f\n\t"
            "movl %1,%0\n"
            "3:\tlodsb\n\t"
            "testb %%al,%%al\n\t"
            "je 5f\n\t"
            "movl %4,%%edi\n\t"
            "movl %%edx,%%ecx\n\t"
            "repne\n\t"
            "scasb\n\t"
            "jne 3b\n\t"
            "decl %1\n\t"
            "cmpb $0,(%1)\n\t"
            "je 5f\n\t"
            "movb $0,(%1)\n\t"
            "incl %1\n\t"
            "jmp 6f\n"
            "5:\txorl %1,%1\n"
            "6:\tcmpb $0,(%0)\n\t"
            "jne 7f\n\t"
            "xorl %0,%0\n"
            "7:\ttestl %0,%0\n\t"
            "jne 8f\n\t"
            "movl %0,%1\n"
            "8:"
            : "=b"(__res), "=S"(___strtok)
            : "0"(___strtok), "1"(s), "g"(ct)
            : "ax", "cx", "dx", "di");
    return __res;
}

/**
 * 从 src 拷贝 n 字节到 dest
 * 此函数不考虑内存重叠导致的错误情况 */
extern inline void *memcpy(void *dest, const void *src, int n)
{
    __asm__("cld\n\t"
            "rep\n\t"
            "movsb"
            :
            : "c"(n), "S"(src), "D"(dest)
            : "cx", "si", "di");
    return dest;
}

/**
 * 从 src 拷贝 n 字节到 dest
 * 这个函数相比 memcpy 考虑了内存重叠的情况, 比如下面的情况
 *
 *      char s[] = "12345";
 *      COPY(s + 2, s, 3);
 *
 * 如果使用 memcpy 拷贝, 则步骤如下:
 *
 *      s[2] = s[0]
 *      s[3] = s[1]
 *      s[4] = s[2] // s[2] 的值, 上面已经被改过了, 因此这个复制结果是错误的
 *
 * 如果使用 memmove, 因为 dest > src, 因此拷贝过程如下(走 std 分支):
 *
 *      s[4] = s[2]
 *      s[3] = s[1]
 *      s[2] = s[0] */
extern inline void *memmove(void *dest, const void *src, int n)
{
    if (dest < src)
        __asm__("cld\n\t"
                "rep\n\t"
                "movsb" ::"c"(n),
                "S"(src), "D"(dest)
                : "cx", "si", "di");
    else
        __asm__("std\n\t"
                "rep\n\t"
                "movsb" ::"c"(n),
                "S"(src + n - 1), "D"(dest + n - 1)
                : "cx", "si", "di");
    return dest;
}

/**
 * 比较 cs, ct 的 count 个字节, 如果相同返回 0, 否则返回 `1*sign(cs - ct)` */
extern inline int memcmp(const void *cs, const void *ct, int count)
{
    /**
     * ------------------------
     *     cld
     *     repe cmpsb
     *     je 1f
     *     movl $1, %eax
     *     jl 1f
     *     negl %eax
     * 1:
     * ------------------------ */
    register int __res __asm__("ax");
    __asm__("cld\n\t"
            "repe\n\t"
            "cmpsb\n\t"
            "je 1f\n\t"
            "movl $1,%%eax\n\t"
            "jl 1f\n\t"
            "negl %%eax\n"
            "1:"
            : "=a"(__res)
            : "0"(0), "D"(cs), "S"(ct), "c"(count)
            : "si", "di", "cx");
    return __res;
}

extern inline void *memchr(const void *cs, char c, int count)
{
    /**
     * ------------------------
     *     cld
     *     repne scasb # (%edi) == %al
     *     je 1f
     *     movl $1, %0
     * 1:
     * ------------------------ */
    register void *__res __asm__("di");
    if (!count)
        return NULL;
    __asm__("cld\n\t"
            "repne\n\t"
            "scasb\n\t"
            "je 1f\n\t"
            "movl $1,%0\n"
            "1:\tdecl %0"
            : "=D"(__res)
            : "a"(c), "D"(cs), "c"(count)
            : "cx");
    return __res;
}

extern inline void *memset(void *s, char c, int count)
{
    /**
     * ------------------------
     * cld
     * rep stosb
     * ------------------------ */
    __asm__("cld\n\t"
            "rep\n\t"
            "stosb" ::"a"(c),
            "D"(s), "c"(count)
            : "cx", "di");
    return s;
}

#endif
