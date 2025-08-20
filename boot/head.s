/*
 *  linux/boot/head.s
 *
 *  (C) 1991  Linus Torvalds
 */

.code32

/*
 *  head.s contains the 32-bit startup code.
 *
 * NOTE!!! Startup happens at absolute address 0x00000000, which is also where
 * the page directory will exist. The startup code will be overwritten by
 * the page directory.
 */
.text
.globl _idt,_gdt,_pg_dir,_tmp_floppy_area
_pg_dir:
startup_32:
        movl $0x10, %eax # 数据段选择子, 使用 00010~0_00 = #2
        mov %ax, %ds
        mov %ax, %es
        mov %ax, %fs
        mov %ax, %gs
        # LSS 从内存中读取 ​​48 位数据​​(32 位偏移地址 + 16 位段选择符)
        # 分别加载到 通用寄存器和 SS 寄存器
        lss _stack_start, %esp
        call setup_idt
        call setup_gdt
        movl $0x10, %eax        # reload all the segment registers
        mov %ax, %ds        # after changing gdt. CS was already
        mov %ax, %es        # reloaded in 'setup_gdt'
        mov %ax, %fs
        mov %ax, %gs
        lss _stack_start, %esp

        # 测试A20地址线是否已经开启
        # 采用的方法是是向内存地址 0x00000 处写入任意一个数值, 然后看内存地址 0x100000(1M) 处是否也是这个数值.
        # 如果一直相同的话, 就一直比较下去, 也即死循环/死机. 表示地址 A20 线没有选通, 结果内核就不能使用 1MB 以上内存.
        # 原理是, 如果 A20 没有接通, 那寻址 0x100000 的时候, 因为地址会换的原因, 会导致寻址到 0x00000
        # 0x00000 放的是本文件第一条指令, 程序已经不会再到那个地方执行了, 因此可以覆盖
        xorl %eax, %eax
    1:
        incl %eax # check that A20 really IS enabled
        movl %eax, 0x00000 # loop forever if it isn't
        cmpl %eax, 0x100000
        je 1b

        /*
        * NOTE! 486 should set bit 16, to check for write-protect in supervisor
        * mode. Then it would be unnecessary with the "verify_area()"-calls.
        * 486 users probably want to set the NE (#5) bit also, so as to use
        * int 16 for math errors.
        *
        * 上面原注释中提到的 486 CPU 中 CR0 控制寄存器的 `位16` 是写保护标志 WP(Write-Protect),
        * 用于禁止超级用户级的程序向一般用户只读页面中进行写操作.
        * 该标志主要用于操作系统在创建新进程时实现写时复制(copy-on-write)方法。
        */

        /*
        * 下面这段用于检查数学协处理器芯片是否存在
        * 方法是修改控制寄存器 CR0, 在假设存在协处理器的情况下执行一个协处理器指令,
        * 如果出错的话则说明协处理器芯片不存在, 需要设置 CR0 中的协处理器仿真位 EM(位2),
        * 并复位协处理器存在标志MP(位1)
        */
        movl %cr0, %eax # check math chip
        andl $0x80000011, %eax # Save PG, PE, ET
    /* "orl $0x10020,%eax" here for 486 might be good */
        orl $2, %eax        # set MP
        movl %eax,%cr0
        call check_x87
        jmp after_page_tables

/*
 * We depend on ET to be correct. This checks for 287/387.
 * ------------------------------------------------------------
 * fninit 和 fstsw 是数学协处理器(80287/80387)的指令
 *
 * finit 向协处理器发出初始化命令, 它会把协处理器置于一个未受以前操作影响的已知状态, 设置
 * 其控制字为默认值, 清除状态字和所有浮点栈式寄存器 非等待形式的这条指令(fninit)还会让
 * 协处理器终止执行当前正在执行的任何先前的算术操作.
 *
 * fstsw 指令取协处理器的状态字. 如果系统中存在协处理器的话, 那么在执行了 fninit 指令后
 * 其状态字低字节肯定为 0
 * ------------------------------------------------------------
 * ​​EM=1: 强制所有浮点指令(如 FADD/FMUL)触发设备不存在异常(#NM)
 *       由操作系统内核的异常处理程序​​模拟执行​​浮点运算
 * ​​MP=1: 配合 EM=1, 确保任务切换时若任务切换发生(TS=1), 执行 WAIT 指令也会触发异常
 *       避免错误使用不存在的 FPU 上下文
 */
check_x87:
        fninit
        fstsw %ax
        cmpb $0, %al
        je 1f            /* JNE == no coprocessor: have to set bits */
        movl %cr0, %eax
        /* 注意是 XOR 6, 这里的本意是翻转 MP/EM 两位, 不影响 CR0 的其他位
         */
        xorl $6, %eax        /* reset MP-bit1, set EM-bit2 */
        movl %eax, %cr0
        ret

        /*
        *下面的两个字节值是 80287 协处理器指令 fsetpm 的机器码.
        * 其作用是把 80287 设置为保护模式, 80387 无需该指令, 并且将会把该指令看作是空操作
        */
    .align 4
    1:
        /* fsetpm for 287, ignored by 387 */
        .byte 0xDB, 0xE4

        ret

/*
 *  setup_idt
 *
 *  sets up a idt with 256 entries pointing to
 *  ignore_int, interrupt gates. It then loads
 *  idt. Everything that wants to install itself
 *  in the idt-table may do so themselves. Interrupts
 *  are enabled elsewhere, when we can be relatively
 *  sure everything is ok. This routine will be over-
 *  written by the page tables.
 */
setup_idt:
        # 这里实在组装 <中断门>, 中断门的结构如下:
        # OOOO-ABXX, A = p_dpl_s, B=TYPE=1110, XX=000_(ANY:5)
        # SSSS-OOOO
        # ABXX = 8E00 = 1000_1110_0000_0000
        lea ignore_int, %edx # OFFSET
        movl $0x00080000, %eax # SSSS=0008
        movw %dx, %ax        /* selector = 0x0008 = cs */
        movw $0x8E00, %dx    /* interrupt gate - dpl=0, present */

        # 中断门的低 32 位在 EAX
        # 中断门的高 32 位在 EDX

        lea _idt, %edi
        mov $256, %ecx
    rp_sidt:
        movl %eax, (%edi)
        movl %edx, 4(%edi)
        addl $8, %edi
        dec %ecx
        jne rp_sidt # 这里咋不 loop ?

        lidt idt_descr
        ret

/*
 *  setup_gdt
 *
 *  This routines sets up a new gdt and loads it.
 *  Only two entries are currently built, the same
 *  ones that were built in init.s. The routine
 *  is VERY complicated at two whole lines, so this
 *  rather long comment is certainly needed :-).
 *  This routine will be overwritten by the page tables.
 */
setup_gdt:
        lgdt gdt_descr
        ret

/*
 * I put the kernel page tables right after the page directory,
 * using 4 of them to span 16 Mb of physical memory. People with
 * more than 16MB will have to expand this.
 *
 * 一页有 1024 个条目, 分别是 4Kb
 * 因此 1 页可以表示 4Kb * 1Kb = 4M 的内存, PG0-PG3 就是 16M 内存
 * 顺便说一下, setup_paging 函数里面, 把 0x0000 ~ 0x0FFF 的空间, 重写成了页目录(Page Directory)
 */
.org 0x1000
pg0: // PG0 从 0x1000 ~ 0x1FFF

.org 0x2000
pg1: // PG1 从 0x2000 ~ 0x2FFF

.org 0x3000
pg2: // PG2 从 0x3000 ~ 0x3FFF

.org 0x4000
pg3: // PG3 从 0x4000 ~ 0x4FFF

.org 0x5000


/*
 * tmp_floppy_area is used by the floppy-driver when DMA cannot
 * reach to a buffer-block. It needs to be aligned, so that it isn't
 * on a 64kB border.
 */
_tmp_floppy_area:
    .fill 1024, 1, 0

after_page_tables:
        # 给 main 函数的参数
        pushl $0        # These are the parameters to main :-)
        pushl $0
        pushl $0
        pushl $L6        # return address for main, if it decides to.
        pushl $_main
        jmp setup_paging
    L6:
        jmp L6  # main should never return here, but
                # just in case, we know what happens.

/* This is the default interrupt "handler" :-)
 * 处理过程很简单, 就是简单的打印一行错误, 然后退出中断处理
 */
int_msg: .asciz "Unknown interrupt\n\r"

.align 4
ignore_int:
        pushl %eax
        pushl %ecx
        pushl %edx
        push %ds
        push %es
        push %fs
        movl $0x10, %eax
        mov %ax, %ds
        mov %ax, %es
        mov %ax, %fs
        pushl $int_msg
        call _printk
        popl %eax
        pop %fs
        pop %es
        pop %ds
        popl %edx
        popl %ecx
        popl %eax
        iret

/*
 * Setup_paging
 *
 * This routine sets up paging by setting the page bit
 * in cr0. The page tables are set up, identity-mapping
 * the first 16MB. The pager assumes that no illegal
 * addresses are produced (ie >4Mb on a 4Mb machine).
 *
 * NOTE! Although all physical memory should be identity
 * mapped by this routine, only the kernel page functions
 * use the >1Mb addresses directly. All "normal" functions
 * use just the lower 1Mb, or the local data space, which
 * will be mapped to some other place - mm keeps track of
 * that.
 *
 * For those with more memory than 16 Mb - tough luck. I've
 * not got it, why should you :-) The source is here. Change
 * it. (Seriously - it shouldn't be too difficult. Mostly
 * change some constants etc. I left it at 16Mb, as my machine
 * even cannot be extended past that (ok, but it was cheap :-)
 * I've tried to show which constants to change by having
 * some kind of marker at them (search for "16Mb"), but I
 * won't guarantee that's all :-( )
 */
.align 4
setup_paging:
        /* 使用 stosl store string 指令填充内存区域
         * EAX = 0, ECX = 1024*5, 每次填充 4 字节
         * 因此一共填充 1024 * 5 * 4 = 4Kb * 5 字节 */
        movl $(1024*5), %ecx        /* 5 pages - pg_dir+4 page tables */
        xorl %eax, %eax
        xorl %edi, %edi            /* pg_dir is at 0x000 */
        cld
        rep stosl

        # +7 实际上实在设置低 12 位包含的属性 bits, 这里的设置是 P=1, RW=1, US=1
        movl $pg0+7, _pg_dir        /* set present bit/user r/w */
        movl $pg1+7, _pg_dir+4        /*  --------- " " --------- */
        movl $pg2+7, _pg_dir+8        /*  --------- " " --------- */
        movl $pg3+7, _pg_dir+12        /*  --------- " " --------- */

        # 下面这段, 从 PG3 的 4092 项开始, 倒序逐个填充 4Byte 的页表项
        # 0xFFF007 = 11_11_1111_1111~0000_0000_0111
        # 这意思是, PG3 的 4092 项, 指向的是 0xFFF000 所在的那个物理内存帧
        # 此后, stosl 操作 %edi 每次减去 4, %eax 每次减去 0x1000 指向上一个物理内存帧
        movl $pg3+4092, %edi
        movl $0xfff007, %eax        /*  16Mb - 4096 + 7 (r/w user,p) */
        std # 倒序 - %edi 逐次递减
    1:
        stosl /* fill pages backwards - more efficient :-) */
        subl $0x1000, %eax
        jge 1b

        # 开启分页
        xorl %eax, %eax        /* pg_dir is at 0x0000 */
        movl %eax, %cr3        /* cr3 - page directory start */
        movl %cr0, %eax
        orl $0x80000000, %eax
        movl %eax, %cr0        /* set paging (PG) bit */
        ret            /* this also flushes prefetch-queue */

        /* 最后这个 ret 转到了进入 setup_paging 之前
         * 精心设计的栈顶指示的 CS:IP 位置 - 也就是 init/main 函数入口
         * 多说一句, 栈上的设计, 如果在 main 里面继续 return, 就会到 L6 那个位置死循环下去 */

.align 4
.word 0
idt_descr:
    .word (256*8-1) # idt contains 256 entries
    .long _idt

.align 4
.word 0
gdt_descr:
    .word 256*8-1 # so does gdt (not that that's any
    .long _gdt # magic number, but it works for me :^)

.align 8
_idt:
    .fill 256, 8, 0        # idt is uninitialized

_gdt:
    .quad 0x0000000000000000    /* NULL descriptor */
    .quad 0x00c09a0000000fff    /* 16Mb, 全局代码段 */
    .quad 0x00c0920000000fff    /* 16Mb, 全局数据段 */
    .quad 0x0000000000000000    /* TEMPORARY - don't use */
    .fill 252, 8, 0            /* space for LDT's and TSS's etc */
