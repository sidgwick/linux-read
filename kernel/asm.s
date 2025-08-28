/*
 *  linux/kernel/asm.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * asm.s contains the low-level code for most hardware faults.
 * page_exception is handled by the mm, so that isn't here. This
 * file also handles (hopefully) fpu-exceptions due to TS-bit, as
 * the fpu must be properly saved/resored. This hasn't been tested.
 */

#本代码文件主要涉及对 Intel 保留中断 int0 ~int16 的处理(int17 ~int31留作今后使用)
#以下是一些全局函数名的声明, 其原形在 traps.c 中说明

.globl _divide_error, _debug, _nmi, _int3, _overflow, _bounds, _invalid_op.globl _double_fault,
    _coprocessor_segment_overrun.globl _invalid_TSS, _segment_not_present,
    _stack_segment.globl _general_protection, _coprocessor_error, _irq13,
    _reserved.globl _alignment_check

#int0, 错误, 无出错号
#除零出错的情况
#在执行 DIV 或 IDIV 指令时, 若除数是 0, CPU 就会产生这个异常
#当 EAX(或 AX, AL) 容纳不了一个合法除操作的结果时, 也会产生这个异常
            _divide_error : pushl $_do_divide_error

#这里这段代码对没有出错号的中断是通用的
#正真的处理函数在跳转执行 no_error_code 之前, 先压栈了
#至此, 栈的情况为:
#FUNC, EIP, CS, EFLAGS, ESP *, SS *
#ESP, SS 是在发生特权级切换的时候才会有
                                no_error_code : xchgl %
        eax,
    (% esp) #栈顶是出错处理函数指针 pushl % ebx pushl % ecx pushl % edx pushl % edi pushl
        % esi pushl % ebp push % ds push % es push % fs pushl $0 #造一个 "error code" lea 44(% esp),
    % edx #EDX = addr(EIP),
          这里也是中断发生的时刻原始的栈顶(即当时 ESP 的值) pushl % edx #原始 ESP 入栈 movl $0x10,
          % edx #DS = ES = FS = 4GB 数据段 mov % dx, % ds mov % dx, % es mov % dx,
          % fs call * % eax #调用真正的处理函数(注意看栈上面 : EIP, 0) addl $8,
          % esp #恢复栈 pop % fs pop % es pop % ds popl % ebp popl % esi popl % edi popl % edx popl
              % ecx popl % ebx popl %
              eax iret

#int1, 错误 / 陷阱, 无错误号
#debug 调试中断入口点
#当 eflags 中 TF 标志置位时可引发此中断.当发现硬件断点(数据 : 陷阱, 代码 : 错误), 或者
#开启了指令跟踪陷阱或任务交换陷阱, 或者调试寄存器访问无效(错误), CPU 就会产生该异常
              _debug : pushl $_do_int3 #_do_debug jmp no_error_code

#int2, 陷阱, 无错误号
#非屏蔽中断调用入口点
#这是仅有的被赋予固定中断向量的硬件中断, 每当接收到一个 NMI 信号
#CPU 内部就会产生中断向量 2, 并执行标准中断应答周期, 因此很节省时间
#NMI 通常保留为极为重要的硬件事件使用, 当 CPU 收到一个 NMI 信号并
#且开始执行其中断处理过程时, 随后所有的硬件中断都将被忽略
                       _nmi : pushl $_do_nmi jmp no_error_code

#int3, 陷阱, 无错误号
#断点指令引起中断的入口点
#由 int 3 指令引发的中断, 与硬件中断无关.该指令通常由调式器插入被调式程序的代码中
                              _int3 : pushl $_do_int3 jmp no_error_code

#int4, 陷阱, 无错误号
#溢出出错处理中断入口点
#EFLAGS 中 OF 标志置位时 CPU 执行 INTO 指令就会引发该中断
#通常用于编译器跟踪算术计算溢出
                                      _overflow : pushl $_do_overflow jmp no_error_code

#int5, 错误, 无错误号
#边界检查出错中断入口点
#当操作数在有效范围以外时引发的中断, 当 BOUND 指令测试失败就会产生该中断
#BOUND 指令有 3 个操作数, 如果第 1 个不在另外两个之间, 就产生异常 5
                                                  _bounds : pushl $_do_bounds jmp no_error_code

#int6, 错误, 无错误号
#无效操作指令出错中断入口点
#CPU 执行机构检测到一个无效的操作码而引起的中断
                                                            _invalid_op
    : pushl $_do_invalid_op jmp no_error_code

#int9, 放弃, 无错误号
#协处理器段超出出错中断入口点
#该异常基本上等同于协处理器出错保护, 因为在浮点指令操作数太大时, 我们就有这个机会来
#加载或保存超出数据段的浮点值
      _coprocessor_segment_overrun : pushl $_do_coprocessor_segment_overrun jmp no_error_code

#int15
#其他 Intel 保留中断的入口点
                                     _reserved : pushl $_do_reserved jmp no_error_code

#int45
#(0x20 + 13) = 45, Linux 设置的数学协处理器硬件中断
#当协处理器执行完一个操作时就会发出 IRQ13 中断信号, 以通知 CPU 操作完成
# 80387 在执行计算时, CPU 会等待其操作完成.下面 `outb % al, $0xF0` 中
# 0xF0 是协处理端口, 用于清忙锁存器.通过写该端口, 本中断将消除 CPU 的 BUSY
#延续信号, 并重新激活 80387 的处理器扩展请求引脚 PEREQ, 该操作主要是为了确保
#在继续执行 80387 的任何指令之前, CPU 响应本中断
#TODO : 没太懂这块, 但是不影响理解内核工作流程
                                                 _irq13 : pushl
                                                          %
                                                          eax xorb % al,
          % al outb % al, $0xF0 movb $0x20, % al outb % al, $0x20 jmp 1f 1 : jmp 1f 1 : outb % al,
          $0xA0 popl %
              eax jmp _coprocessor_error

#int8, 放弃, 有错误码
#双出错故障
#通常当 CPU 在调用前一个异常的处理程序而又检测到一个新的异常时, 这两个
#异常会被串行地进行处理, 但也会碰到很少的情况, CPU 不能进行这样的串行
#处理操作, 此时就会引发该中断
              _double_fault : pushl $_do_double_fault

#这里这段代码对有出错号的中断是通用的
#正真的处理函数在跳转执行 error_code 之前, 先压栈了
#至此, 栈的情况为:
#FUNC, ERROR_CODE, EIP, CS, EFLAGS, ESP *, SS *
#ESP, SS 是在发生特权级切换的时候才会有
                              error_code : xchgl
                                           %
                                           eax,
          4(% esp) #error code <->% eax xchgl % ebx,
          (% esp) #&function <->% ebx pushl % ecx pushl % edx pushl % edi pushl % esi pushl
                                  % ebp push % ds push % es push % fs pushl
                                  % eax #error code lea 44(% esp),
          % eax #offset pushl % eax movl $0x10, % eax mov % ax, % ds mov % ax, % es mov % ax,
          % fs call * % ebx addl $8,
          % esp pop % fs pop % es pop % ds popl % ebp popl % esi popl % edi popl % edx popl
              % ecx popl % ebx popl %
              eax iret

#int10, 错误, 有出错码
#无效的任务状态段(TSS)
#CPU 企图切换到一个进程, 而该进程的 TSS 无效
              _invalid_TSS : pushl $_do_invalid_TSS jmp error_code

#int11, 错误, 有出错码
#段不存在
#被引用的段不在内存中, 段描述符中标志指明段不在内存中
                             _segment_not_present : pushl $_do_segment_not_present jmp error_code

#int12, 错误, 有出错码
#堆栈段错误
#指令操作试图超出堆栈段范围, 或者堆栈段不在内存中, 这是异常 11 和 13 的特例
#有些操作系统可以利用这个异常来确定什么时候应该为程序分配更多的栈空间
                                                    _stack_segment
    : pushl $_do_stack_segment jmp error_code

#int13, 错误, 有出错码
#一般保护性出错
#表明是不属于任何其他类的错误, 若一个异常产生时没有对应的处理向量(0 ~16),
#通常就会归到此类
      _general_protection : pushl $_do_general_protection jmp error_code

#int17
#边界对齐检查出错
#在启用了内存边界检查时, 若特权级 3(用户级)数据非边界对齐时会产生该异常
                            _alignment_check : pushl $_do_alignment_check jmp error_code

# ######其他中断情况
#int7-- 设备不存在(_device_not_available) 在kernel / sys_call.s
#int14-- 页错误(_page_fault) 在mm / page.s
#int16-- 协处理器错误(_coprocessor_error) 在kernel / sys_call.s
#时钟中断 int 0x20(_timer_interrupt)在kernel / sys_call.s
#系统调用 int 0x80(_system_call)在kernel / sys_call.s
