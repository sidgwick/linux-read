/*
 *  linux/kernel/rs_io.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *    rs_io.s
 *
 * This module implements the rs232 io interrupts.
 *
 * 该模块实现 rs232 输入输出中断处理程序
 */

.text
.globl rs1_interrupt, rs2_interrupt

size = 1024 /* must be power of two !
               and must match the value in tty_io.c!!!
               size 是输入缓冲区长度(字节数, 环形缓冲区) */

/* these are the offsets into the read/write buffer structures */
rs_addr = 0 # 队列的 data 字段放的是串行端口号
head = 4
tail = 8
proc_list = 12
buf = 16

/* 当一个写缓冲队列满后, 内核就会把要往写队列填字符的进程设置为等待状态.
 * 当写缓冲队列中还剩余最多 256 个字符时, 中断处理程序就可以唤醒这些等待
 * 进程继续往写队列中放字符 */
startup    = 256   /* chars left in write queue when we restart it */

/* These are the actual interrupt routines. They look where
 * the interrupt is coming from, and take appropriate action
 * 初始化时 rs1_interrupt 地址被放入中断描述符 0x24 中, 对应 8259A 的中断请求 IRQ4 引脚
 * 初始化时 rs2_interrupt 地址被放入中断描述符 0x23 中, 对应 8259A 的中断请求 IRQ3 引脚 */
.align 2
rs1_interrupt:
    pushl $table_list+8 # RS1 输入队列
    jmp rs_int

.align 2
rs2_interrupt:
    pushl $table_list+16 # RS2 输入队列

// 这段代码首先让段寄存器ds、es指向内核数据段, 然后从对应读写缓冲队列data字段取出
// 串行端口基地址. 该地址.
// 若位0 = 0, 表示有需
// 要处理的中断. 于是根据位2、位1使用指针跳转表调用相应中断源类型处理子程序. 在每
// 个子程序中会在处理完后复位UART 的相应中断源. 在子程序返回后这段代码会循环判断是
// 否还有其他中断源 (位0 = 0？) . 如果本次中断还有其他中断源, 则IIR的位0仍然是0.
// 于是中断处理程序会再调用相应中断源子程序继续处理. 直到引起本次中断的所有中断源都
// 被处理并复位, 此时UART会自动地设置IIR的位0 =1, 表示已无待处理的中断, 于是中断
// 处理程序即可退出.

#STACK : ds, es, eax, ebx, ecx, edx, Queue, eip, cs, eflags, esp, ss
rs_int:
    pushl %edx
    pushl %ecx
    pushl %ebx
    pushl %eax
    push %es
    push %ds        /* as this is an interrupt, we cannot */
    pushl $0x10        /* know that bs is ok. Load it */
    pop %ds
    pushl $0x10
    pop %es
    movl 24(%esp), %edx         # EDX = 输入队列地址(二级指针)
    movl (%edx), %edx           # 输入队列地址(一级指针)
    movl rs_addr(%edx), %edx    # 取出端口号(0x3F8)
    addl $2, %edx               # 0x3FA, interrupt ident. reg
    /* TODO: 串口端口值加 2 即是中断标识寄存器 IIR 的端口地址
     *       IIR 是 UART 芯片提供的, 搜索 NS8250 有详细介绍
     * IIR(Interrupt identification register), 中断处理程序以此判断此次中断是 4 种中的那一种
     *    [7:3] 全 0 保留不用
     *    [2:1] 11 线路状态改变中断, 优先级最高, 读线路状态可复位
     *          10 己接收到数据中断, 读接收数据可复位
     *          01 发送保持寄存器空中断, 读 IIR 或写发送保持寄存器可复位
     *          00 MODEM 状态改变中断, 读 MODEM 状态可复位
     *    [0]   0-有待处理的中断, 1-无中断
     */
rep_int:
    xorl %eax, %eax
    inb %dx, %al                # 读取 IIR 端口
    testb $1, %al
    jne end                     # IIR 最低位是 1, 表示没有待处理的中断, 直接跳转到结束
    cmpb $6, %al                # this shouldn't happen, but ...
    ja end                      # 只有低三位有效, 因此范围仅在 0~6 是合法的
    movl 24(%esp), %ecx         # 输入队列的二级指针
    pushl %edx                  # IIR 参数入栈
    subl $2, %edx               # Serial 端口号(0x3F8)
    call jmp_table(, %eax, 2)   # NOTE! not *4, bit0 is 0 already, [jmp_table + (eax >> 1)*4]
    popl %edx                   # 栈平衡
    jmp rep_int
end:
    movb $0x20, %al
    outb %al, $0x20  # 8259A, EOI

    pop %ds
    pop %es
    popl %eax
    popl %ebx
    popl %ecx
    popl %edx

    addl $4, %esp # jump over table_list entry
    iret

jmp_table:
    .long modem_status  # 00 MODEM 状态改变中断, 读 MODEM 状态可复位
    .long write_char    # 01 发送保持寄存器空中断, 读 IIR 或写发送保持寄存器可复位
    .long read_char     # 10 己接收到数据中断, 读接收数据可复位
    .long line_status   # 11 线路状态改变中断, 优先级最高, 读线路状态可复位

.align 2
modem_status:
#clear intr by reading modem status reg
    addl $6, %edx   # (0x3F8+6)=0x3FE ~ modem 状态寄存器
    inb %dx, %al
    ret

.align 2
line_status:
#clear intr by reading line status reg
    addl $5, %edx   # (0x3F8+5)=0x3FD ~ 线路状态寄存器
    inb %dx, %al
    ret

/* 由于 UART 芯片接收到字符而引起这次中断, 对接收缓冲寄存器执行读操作可复位该中断源
 * 这个子程序将接收到的字符放到读缓冲队列 read_q 头指针(head)处, 并且让该指针前移一
 * 个字符位置. 若 head 指针已经到达缓冲区末端, 则让其折返到缓冲区开始处
 * 最后调用 C 函数 do_tty_interrupt, 把读入的字符经过处理放入规范模式缓
 * 冲队列(辅助缓冲队列 secondary)中 */
.align 2
read_char:
    inb %dx, %al                # EDX=0x3F8, 读取 `读接受缓冲寄存器(RBR)` 寄存器
    movl %ecx, %edx             # 输入队列的二级指针
    subl $table_list, %edx     # 输入队列的二级指针相对于 table_list 的偏移量
    shrl $3, %edx               # 除以 8, 得到的是 RS 端口的索引值(1/2号端口)
    movl (%ecx), %ecx            # read-queue, 输入队列的1级指针
    movl head(%ecx), %ebx       # 缓冲区的 head 游标
    movb %al, buf(%ecx, %ebx)   # 将字符放到 [ecx + buf + ebx(head)], 相当于写入队列里
    incl %ebx                   # head++
    andl $size-1, %ebx          # 防止溢出回绕
    cmpl tail(%ecx), %ebx       # 判断 tail 追上了 head 没有
    je 1f                       # 追上说明缓冲区就满了, 丢掉数据, 直接退出
    movl %ebx, head(%ecx)       # 更新 head 游标
1:
    addl $63, %edx              # 串口号转换成 tty 号(63或64)并作为参数入栈, 和 tty_table 分配有关系
    pushl %edx
    call do_tty_interrupt      # 使用这个 C 函数, 把字符拷贝到辅助序列
    addl $4, %esp
    ret

/* 由于设置了发送保持寄存器允许中断标志而引起此次中断. 说明对应串行终端的写字符缓冲队
 * 列中有字符需要发送. 于是计算出写队列中当前所含字符数, 若字符数已小于 256 个, 则唤醒
 * 等待写操作进程. 然后从写缓冲队列尾部取出一个字符发送, 并调整和保存尾指针. 如果写缓
 * 冲队列已空, 则跳转到 write_buffer_empty 处理写缓冲队列空的情况 */
.align 2
write_char:
    movl 4(%ecx), %ecx        # write-queue, 写队列1级指针
    movl head(%ecx), %ebx   # 写队列游标头
    subl tail(%ecx), %ebx   # 减去游标尾
    andl $size-1, %ebx        # 得到的就是现在队列里面还有多少空间. nr chars in queue
    je write_buffer_empty   # 头指针 == 尾指针, 说明写队列空, 跳转处理

    cmpl $startup, %ebx         # 剩余空间和 startup=256 比较
    ja 1f                       # 空间还很多, 跳转到 1f 处理
    movl proc_list(%ecx), %ebx    # 字符数量已经小于 startup 的时候, 开始唤醒任务
    testl %ebx, %ebx            # wake up sleeping process, is there any?
    je 1f                       # 没有等待任务
    movl $0, (%ebx)             # 有等待任务, 唤醒它
1:
    movl tail(%ecx), %ebx       # 拿到末尾游标
    movb buf(%ecx, %ebx), %al   # 从队列的末尾取数据 [(ecx+buf) + ebx(tail)]
    outb %al, %dx               # 写到 0x3F8, `发送保持寄存器(THR)`
    incl %ebx                   # 尾游标增加
    andl $size-1, %ebx          # 防止地址回绕
    movl %ebx, tail(%ecx)       # 更新尾游标
    cmpl head(%ecx), %ebx       # 队列空了吗?
    je write_buffer_empty
    ret

#处理写缓冲队列 write_q 已空的情况
.align 2
write_buffer_empty:
#写队列现在空了, 就通知那些等待的任务, 可以开始写出了
    movl proc_list(%ecx), %ebx    # wake up sleeping process
    testl %ebx, %ebx            # is there any?
    je 1f
    movl $0, (%ebx)
1:
    incl %edx
    inb %dx, %al
    jmp 1f
1:  jmp 1f
1:
#屏蔽发送保持寄存器空中断, 不让发送保持寄存器空时产生中断
#当再有字符被放入写缓冲队列中时, serial.c 中的 rs_write 函数会
#再次允许发送保持寄存器空时产生中断, 因此 UART 就又会 “自动” 地来
#取写缓冲队列中的字符, 并发送出去
    andb $0xd, %al        /* disable transmit interrupt (bit1) */
    outb %al, %dx
    ret
