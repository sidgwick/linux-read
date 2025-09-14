// 转到用户空间执行
// 参考 X86 汇编语言 -- 346 页: 当切换特权级别的中断发生的时候, 会依次入栈 ss,
// sp, eflags, cs, ip 下面几条 push 指令就是在手动构造中断出现时候栈状态
#define move_to_user_mode()                                                                        \
    __asm__("movl %%esp,%%eax\n\t"                                                                 \
            "pushl $0x17\n\t" /* SS */                                                             \
            "pushl %%eax\n\t" /* SP */                                                             \
            "pushfl\n\t"      /* EFLAGS */                                                         \
            "pushl $0x0f\n\t" /* CS */                                                             \
            "pushl $1f\n\t"   /* IP */                                                             \
            "iret\n"          /* 跳转到标号 1 执行, 但是特权级切换为=3 */                          \
            "1:\tmovl $0x17,%%eax\n\t"                                                             \
            "mov %%ax,%%ds\n\t"                                                                    \
            "mov %%ax,%%es\n\t"                                                                    \
            "mov %%ax,%%fs\n\t"                                                                    \
            "mov %%ax,%%gs" ::                                                                     \
                : "ax")

#define sti() __asm__("sti" ::) // 允许中断
#define cli() __asm__("cli" ::) // 不允许中断
#define nop() __asm__("nop" ::) // 空指令

#define iret() __asm__("iret" ::) // 中断返回

// 构造门描述符, 包含 32 位偏移地址, 16 位段选择子, 以及属性部分组成
// OOOO-ABCD
// SSSS-OOOO
// i 是 `立即数` 约束
// o 是 `内存地址偏移` 约束
#define _set_gate(gate_addr, type, dpl, addr)                                                      \
    __asm__("movw %%dx,%%ax\n\t" /* EAX = 0x0008_OOOO, SSSS=0x0008 = 1# 描述符,       \
                                GDT, dpl=0 */         \
            "movw %0,%%dx\n\t"   /* EDX = OOOO_ABCD */                                             \
            "movl %%eax,%1\n\t"  /* 装到 gate_addr */                                              \
            "movl %%edx,%2"      /* 装到 gate_addr */                                              \
            :                                                                                      \
            : "i"((short)(0x8000 + (dpl << 13) + (type << 8))), /* 构造属性部分 */                 \
              "o"(*((char *)(gate_addr))),                      /* 门描述符的低 4 字节地址 */      \
              "o"(*(4 + (char *)(gate_addr))),                  /* 门描述符的高 4 字节地址 */      \
              "d"((char *)(addr)), "a"(0x00080000))

/*

Intel 把中断描述符分三类：任务门、中断门、陷阱门，而Linux则分成五类：

- 中断门        Intel的中断门   DPL = 0     描述中断处理程序
- 系统中断门     Intel的中断门   DPL = 3     能够被用户进程访问的陷阱门
- 陷阱门        Intel陷阱门     DPL = 0     大部分的异常处理
- 系统门        Intel的陷阱门   DPL = 3     用于系统调用
- 任务门        Intel任务门     DPL = 0     对"Double fault"异常处理

*/

/* 在 idt 中为 N 号中断, 设置中断门描述符
 * TYPE = 14 = 1110, dpl=0  */
#define set_intr_gate(n, addr) _set_gate(&idt[n], 14, 0, addr)

/* 在 idt 中为 N 号中断, 设置陷阱门描述符
 * TYPE = 15 = 1111, dpl=0  */
#define set_trap_gate(n, addr) _set_gate(&idt[n], 15, 0, addr)

/* 在 idt 中为 N 号中断, 设置系统门描述符
 * TYPE = 15 = 1111, dpl=3
 * 系统门和陷阱门, 只有 DPL 的区别 */
#define set_system_gate(n, addr) _set_gate(&idt[n], 15, 3, addr)

/* 设置门描述符里面的段描述符选择子
 * 这里 Linus 写错了, 高 4 位和低 4 位弄反了 */
#define _set_seg_desc(gate_addr, type, dpl, base, limit)                                           \
    {                                                                                              \
        *(gate_addr) = ((base) & 0xff000000) | (((base) & 0x00ff0000) >> 16) |                     \
                       ((limit) & 0xf0000) | ((dpl) << 13) | (0x00408000) |                        \
                       ((type) << 8); /* 组装的是 seg_ddesc 的高 4 位 */                           \
        *((gate_addr) + 1) = (((base) & 0x0000ffff) << 16) | ((limit) & 0x0ffff);                  \
    }

/**
 * @brief 在全局表中设置任务状态段/局部表描述符
 *
 * 状态段和局部表描述符都被设置为 104 字节 TSS 描述符或者 LDT 描述符
 * 他们的组成如下: BBRL-RRBB BBBB-LLLL
 *
 * TODO-DONE: D/B 字段为啥没有置 1 呢?
 * 答: 可能是这里只考虑设置 TSS 或者 LDT 描述符, D/B 字段和这两个都没啥关系,
 *     D/B 字段控制的是
 *         1. 代码段: 操作数位数和有效地址长度
 *         2. 数据段: 栈操作的长度和栈的边界
 */
#define _set_tssldt_desc(n, addr, type)                                                                               \
    __asm__(                                                                                                          \
        "movw $104, %1\n\t"      /* TSS, LDT 的 limit 都被设置为 104 字节 */                                          \
        "movw %%ax, %2\n\t"      /* base 的低16位 */                                                                  \
        "rorl $16, %%eax\n\t"    /* AX = base 高 16 位 */                                                             \
        "movb %%al, %3\n\t"      /* base 的第三字节 */                                                                \
        "movb $" type ", %4\n\t" /* type 指定描述符 p_dpl_s_type 这几位 */                                            \
        "movb $0x00, %5\n\t" /* g_db_l_avl = 0000, L=0000 主要是考虑 limit 用低 16 位足够存储, 因此这一位 L 就是 0 */ \
        "movb %%ah,%6\n\t" /* base 的最高 1 字节 */                                                                   \
        "rorl $16,%%eax"   /* 恢复 EAX */                                                                             \
        :                                                                                                             \
        : "a"(addr), "m"(*(n)), "m"(*(n + 2)), "m"(*(n + 4)), "m"(*(n + 5)), "m"(*(n + 6)),                           \
          "m"(*(n + 7)))

// 在 GDT 里面设置任务 N 的 TSS
#define set_tss_desc(n, addr) _set_tssldt_desc(((char *)(n)), addr, "0x89")

// 在 GDT 里面设置任务 N 的 LDT
#define set_ldt_desc(n, addr) _set_tssldt_desc(((char *)(n)), addr, "0x82")
