/*
 *  linux/kernel/keyboard.S
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *    Thanks to Alfred Leung for US keyboard patches
 *        Wolfgang Thiel for German keyboard patches
 *        Marc Corsini for the French keyboard
 */

/* KBD_FINNISH for Finnish keyboards
 * KBD_US for US-type
 * KBD_GR for German keyboards
 * KBD_FR for Frech keyboard
 *
 * 定义使用的键盘类型, 用于后面选择采用的字符映射码表
 * 扫描码值可以参考: https://users.utcluj.ro/~baruch/sie/labor/PS2/Scan_Codes_Set_1.htm

# jne -> ZF=0(即 A!=B), ZF=0 -> mode 的第 7 位非 0 -> 之前曾经按下过 caps
# jne 就可以简单的视作, testb 对应的操作位是不是相等,
# 相等 ZF=1 不跳转, 不相等 ZF=0 跳转, 还挺符合直观
// je 要求 ZF=0, 操作数相等 ZF!=0, 操作数不相等 ZF=0, 反直觉

// je 结果为 0 则跳转
// jne 结果非 0 则跳转


 */
#define KBD_FINNISH

.text

/* 全局符号, 在 console.c 里面有使用 */
.globl keyboard_interrupt

/*
 * these are for the keyboard read functions
 */
size = 1024 /* must be a power of two !
               And MUST be the same as in tty_io.c !!!!
               size 是输入缓冲区长度(字节数, 环形缓冲区) */

/* 以下是键盘缓冲队列数据结构 tty_queue 中的偏移量(include/linux/tty.h) */
head = 4
tail = 8
proc_list = 12
buf = 16

/**
 * 在本程序中使用了 3 个标志字节:
 *  1. mode 是键盘特殊键(ctrl/alt/caps)的按下状态标志
 *  2. leds 是用于表示键盘指示灯的状态标志
 *  3. e0 是当收到扫描码 0xe0 或 0xe1 时设置的标志
 *
 * 每个字节标志中各位的含义见如下说明:
 * (1) mode是键盘特殊键的按下状态标志.
 *     表示大小写转换键(caps)、交换键(alt)、控制键(ctrl)和换档键(shift)的状态.
 *       - 位 7 ~ 0x80: caps 键按下
 *       - 位 6 ~ 0x40: caps 键的状态(应与 leds 中 caps-lock 标志位一样)
 *       - 位 5 ~ 0x20: 右 alt 键按下
 *       - 位 4 ~ 0x10: 左 alt 键按下
 *       - 位 3 ~ 0x08: 右 ctrl 键按下
 *       - 位 2 ~ 0x04: 左 ctrl 键按下
 *       - 位 1 ~ 0x02: 右 shift 键按下
 *       - 位 0 ~ 0x01: 左 shift 键按下
 * (2) leds 是用于表示键盘指示灯的状态标志.
 *     即表示数字锁定键(num-lock)/大小写转换键(caps-lock)/和
 *     滚动锁定键(scroll-lock) 的 LED 发光管状态
 *       - 位 7-3 ~ 0x00: 全0不用
 *       - 位   2 ~ 0x04: caps-lock
 *       - 位   1 ~ 0x02: num-lock(初始置1, 也即设置数字锁定键(num-lock)发光管为亮)
 *       - 位   0 ~ 0x01: scroll-lock
 * (3) 当扫描码是 0xe0 或 0xe1 时, 置该标志
 *     表示其后还跟随着 1 个或 2 个字符扫描码. 通常若收到扫描码 0xe0 则意味着还
 *     有一个字符跟随其后; 若收到扫描码 0xe1 则表示后面还跟随着2个字符
 *       - 位 1: 置 1 表示收到 0xe1 标志
 *       - 位 0: 置 1 表示收到 0xe0 标志
 */
mode:    .byte 0        /* caps, alt, ctrl and shift mode */
leds:    .byte 2        /* num-lock, caps, scroll-lock mode (nom-lock on) */
e0:    .byte 0

/*
 *  con_int is the real interrupt routine that reads the
 *  keyboard scan-code and converts it into the appropriate
 *  ascii character(s).
 *
 * 键盘中断处理程序
 *
 * 当键盘控制器接收到用户的一个按键操作时, 就会向中断控制器发出一个键盘中断请求信号 IRQ1
 * 当 CPU 响应该请求时就会执行键盘中断处理程序, 中断处理程序会从键盘控制器相应端口(0x60)
 * 读入按键扫描码, 并调用对应的扫描码子程序进行处理
 */
keyboard_interrupt:
    pushl %eax
    pushl %ebx
    pushl %ecx
    pushl %edx
    push %ds
    push %es
    movl $0x10,%eax # 内核数据段
    mov %ax,%ds
    mov %ax,%es
    movl blankinterval, %eax # 屏幕黑屏间隔时间
    movl %eax, blankcount  # 预置黑屏时间计数值为 blankinterval (滴答数)
    xorl %eax,%eax        /* %eax is scan code, EAX 用来装键盘输入码值 */
    inb $0x60, %al # 读键盘输入, 得到键盘扫描码

    # 判断该扫描码是否是 0xe0 或 0xe1, 是的话就立刻对键盘控制器作出应答
    # 并向中断控制器发送 EOI 信号, 以允许键盘控制器能继续产生中断信号, 从而接收后续的字符
    cmpb $0xe0,%al # al = 0xe0
    je set_e0
    cmpb $0xe1,%al
    je set_e1

    # 不是 0xe0, 0xe1, 正常执行字符处理程序, 然后复位 e0 标志
    call key_table(, %eax, 4) # 字符处理程序 `key_table + EAX * 4`
    movb $0, e0
e0_e1:
    /* 下面这段代码针对使用 8255A 的 PC 标准键盘电路进行硬件复位处理. 端口 0x61
     * 是 8255A 输出口 B 的地址, 该输出端口的第 7 位(PB7)用于禁止和允许对键盘数据的处理
     * 这段程序用于对收到的扫描码做出应答, 方法是首先禁止键盘, 然后立刻重新允许键盘工作 */
    inb $0x61, %al # 取 PPI 端口 B 状态, 其位 7 用于 0-允许/1-禁止键盘
    jmp 1f
1:  jmp 1f
1:  orb $0x80,%al
    jmp 1f
1:  jmp 1f
1:  outb %al,$0x61 # 禁止键盘工作
    jmp 1f
1:  jmp 1f
1:  andb $0x7F,%al
    outb %al,$0x61 # 复位 PPI PB7, 恢复键盘工作

    movb $0x20,%al
    outb %al,$0x20 # 给 8259A 主片发送 EOI 信号

    pushl $0 # 没有指定 tty, 默认使用 fg_console
    call do_tty_interrupt # 将收到数据转换成规范模式并存放在辅助队列中
    addl $4,%esp
    pop %es
    pop %ds
    popl %edx
    popl %ecx
    popl %ebx
    popl %eax
    iret

set_e0:
    movb $1, e0
    jmp e0_e1

set_e1:
    movb $2, e0
    jmp e0_e1

/* This routine fills the buffer with max 8 bytes, taken from
 * %ebx:%eax. (%edx is high). The bytes are written in the
 * order %al,%ah,%eal,%eah,%bl,%bh ... until %eax is zero. */
put_queue:
    pushl %ecx
    pushl %edx

    # 找到 console 对应的 read queue
    movl table_list, %edx  # read-queue for console
    movl head(%edx), %ecx   # ECX 是缓冲区中数据头游标
1:
    movb %al,buf(%edx,%ecx) # 把 al 移动到 `[buf_start=(edx + buf) + ecx]` 的位置
    incl %ecx               # 游标加 1
    andl $size-1,%ecx       # 处理可能的 ecx 回绕
    cmpl tail(%edx), %ecx   # 看看 head 有没有追上 tail
    je 3f                   # buffer full - discard everything
    shrdl $8,%ebx,%eax      # 如果 buffer 不满, 把 ebx:eax 右移 8 位到 eax, ebx 不变
    je 2f                   # eax = 0? 说明没有字符了, 跳出循环
    shrl $8,%ebx            # EBX 也向右移动 8 位
    jmp 1b                  # 循环下一个字符入队列
2:
    movl %ecx, head(%edx)       # 更新缓冲数据头游标
    movl proc_list(%edx), %ecx  # 准备检查是不是有正在等待的进程
    testl %ecx, %ecx
    je 3f                       # 没有正在等待进程, 直接结束
    movl $0,(%ecx)              # 把进程唤醒
    # TODO : 为啥不适用 wake_up 调用 ? 这里少了一些判断
3:
    popl %edx
    popl %ecx
    ret

/* 从这里开始是键跳转表 key_table 中指针对应的各个按键(或松键)处理子程序 */

# ctrl 按键处理程序
ctrl:
    movb $0x04, %al # 0x04 是 mode 中左 ctrl 键对应的比特位(位2)
    jmp 1f

# alt 按键处理程序
alt:
    movb $0x10, %al # 0x10 是 mode 中左 alt 键对应的比特位(位4)
1:
    cmpb $0, e0     # 如果 e0 置位, 说明按下的是右边的 ctrl/alt 键
    je 2f           # 没有置位, 跳转
    addb %al, %al   # 置位, 则改成置相应右键标志位(位3或位5)
    # 好牛逼位运算 : 以 ctrl 为例:
    # (左, 右) =(0x04, 0x08) --> `0x04 + 0x04 = 0x08`
2:
    # TODO-DONE: mode 这样 or, 不怕最后混淆吗 ? 研究一下它在什么地方被 clear 了
    # 答 : 在 unctrl / unalt 处理程序里面, 有清理操作
    orb %al, mode # 记录 ctrl 或者 alt 的按压结果
    ret

/* 处理 ctrl 或 alt 键松开时的扫描码, 复位模式标志 mode 中的对应比特位 */
unctrl:
    movb $0x04, %al
    jmp 1f

unalt:
    movb $0x10, %al
1:
    cmpb $0, e0
    je 2f
    addb %al,%al
2:
    notb %al
    andb %al, mode
    ret

lshift:
    orb $0x01, mode
    ret

unlshift:
    andb $0xfe, mode
    ret

rshift:
    orb $0x02, mode
    ret

unrshift:
    andb $0xfd, mode
    ret

# caps 键按下之后的处理
caps:
    testb $0x80, mode
    jne 1f              # 先判断 caps 是否正在按下状态, 如实跳转到 1f 执行, 否则继续执行
    xorb $0x04, leds    # 翻转 leds 标志中 caps-lock 比特位
    xorb $0x40, mode    # 翻转 mode 标志中 caps 键按下的比特位
    orb $0x80, mode     # 设置 mode 标志中 caps 键已按下标志位

# 根据 leds 标志, 开启或关闭 LED 指示器
# 所以键盘上的 LED 指示灯不是单纯的硬件行为, 是软件在控制的
set_leds:
    call kb_wait        # 等待键盘控制器输入缓冲为空
    movb $0xed, %al        # set leds command
    outb %al, $0x60     # 发送键盘命令 0xed 到 0x60 端口
    call kb_wait        # 等待键盘控制器输入缓冲为空
    movb leds, %al      # leds 状态作为参数
    outb %al, $0x60     # 发送参数到 0x60 端口
    ret

uncaps:
    andb $0x7f, mode
    ret

scroll:
    testb $0x03, mode   # 左右 shift 任一按下
    je 1f               # 没有按下跳转, 按下继续执行
    call show_mem      # 显示内存使用信息
    jmp 2f
1:
    call show_state    # 显示进程状态信息
2:
    xorb $1, leds       # 翻转 leds 中 scroll 对应的位
    jmp set_leds
num:
    xorb $2, leds       # 翻转 leds 中 num 对应的位
    jmp set_leds

/* curosr-key/numeric keypad cursor keys are handled here.
 *  checking for numeric keypad etc.
 *
 * 这里处理方向键/数字小键盘方向键, 检测数字小键盘等 */
cursor:
    subb $0x47, %al # 扫描码是数字键盘上的键(扫描码>=0x47)发出
    jb 1f           # 不是数字小键盘区, 直接退出
    cmpb $12, %al   # 如果扫描码大于 0x53, 也不处理
    ja 1f

    /* 若等于 12, 说明 del 键已被按下(keypad 上面的 dot 键),
     * 则继续判断 ctrl 和 alt 是否也被同时按下 */
    jne cur2            # check for ctrl-alt-del
    testb $0x0c, mode   # ctrl 键是否被按下
    je cur2             # 没有被按下跳转到 cur2
    testb $0x30, mode   # alt 键是否被按下
    jne reboot          # 如果按下, 执行 reboot
cur2:
    cmpb $0x01, e0        # e0 forces cursor movement
    je cur              # 有 e0 前缀, 跳转
    testb $0x02, leds    # not num-lock forces cursor
    je cur              # 没有 leds 标记(num led 不亮), 数字键当做方向键, 跳转
    testb $0x03, mode    # shift forces cursor
    jne cur             # 有 shift 按下, 跳转

    # 后面要把字符放入缓冲队列中, ebx 清零是 put_queue 的要求
    xorl %ebx, %ebx

    movb num_table(%eax), %al # 取得对应按键的字符值
    jmp put_queue       # 将字符放到队列里面去
1:
    ret

cur:
    movb cur_table(%eax), %al

    # 若字符 <= '9'(也即 5 / 6 / 2 / 3)
    # 说明是上一页 / 下一页 / 插入 / 删除键, 则功能字符序列中要添入字符 '~'
    cmpb $'9, %al
    ja ok_cur
    movb $'~, %ah
ok_cur:
    shll $16, %eax      # 将 ax 中内容移到 eax 高字中
    movw $0x5b1b, %ax   # 把 `ESC + [` 放到 ax, 与 EAX 组合成移动序列
    xorl %ebx, %ebx     # 清零 EBX, 准备调用 put_queue
    jmp put_queue

#if defined(KBD_FR)
num_table:
    .ascii "789 456 1230."
#else
num_table:
    .ascii "789 456 1230,"
#endif

# 小键盘区对应的非数字功能(移动 / 翻页 / 删除等)
cur_table:
    .ascii "HA5 DGC YB623"

/* this routine handles function keys
 * 下面子程序处理功能键 */
func:
    subb $0x3B, %al # F1 的扫描码是 0x3B, 因此 al 中是功能键索引号
    jb end_func     # 值非法, 直接退出
    cmpb $9, %al
    jbe ok_func     # F1~F10 跳转到 ok_func
    subb $18, %al   # F11=0x57, F12=0x58, (18+3B)=0x4D, (0x4D+10)=0x57
    cmpb $10, %al
    jb end_func     # 不可能是 F11/F12
    cmpb $11, %al
    ja end_func     # 不可能是 F11/F12
ok_func:
    testb $0x10, mode   # 左 alt 检测
    jne alt_func        # 执行 alt 对应的 func
    cmpl $4, %ecx       # check that there is enough room, TODO: ECX 在哪里设置的?
    jl end_func
    movl func_table(,%eax,4), %eax  # EAX 里面原来是功能号, mov 之后是功能号对应的字符序列
    xorl %ebx, %ebx
    jmp put_queue
alt_func:
    pushl %eax
    call change_console # 使用 alt+FX 切换前台 console
    popl %eax
end_func:
    ret

/*
 * function keys send F1:'esc [ [ A' F2:'esc [ [ B' etc.
 */
func_table:
    .long 0x415b5b1b,0x425b5b1b,0x435b5b1b,0x445b5b1b
    .long 0x455b5b1b,0x465b5b1b,0x475b5b1b,0x485b5b1b
    .long 0x495b5b1b,0x4a5b5b1b,0x4b5b5b1b,0x4c5b5b1b

# 定义 扫描码 - ASCII字符映射表
#if defined(KBD_FINNISH)
key_map:
    .byte 0,27
    .ascii "1234567890+'"
    .byte 127,9
    .ascii "qwertyuiop}"
    .byte 0,13,0
    .ascii "asdfghjkl|{"
    .byte 0,0
    .ascii "'zxcvbnm,.-"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '<
    .fill 10,1,0

shift_map:
    .byte 0,27
    .ascii "!\"#$%&/()=?`"
    .byte 127,9
    .ascii "QWERTYUIOP]^"
    .byte 13,0
    .ascii "ASDFGHJKL\\["
    .byte 0,0
    .ascii "*ZXCVBNM;:_"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '>
    .fill 10,1,0

alt_map:
    .byte 0,0
    .ascii "\0@\0$\0\0{[]}\\\0"
    .byte 0,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte '~,13,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0,0,0        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte 0,0,0,0,0        /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '|
    .fill 10,1,0

#elif defined(KBD_US)

key_map:
    .byte 0,27
    .ascii "1234567890-="
    .byte 127,9
    .ascii "qwertyuiop[]"
    .byte 13,0
    .ascii "asdfghjkl;'"
    .byte '`,0
    .ascii "\\zxcvbnm,./"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '<
    .fill 10,1,0


shift_map:
    .byte 0,27
    .ascii "!@#$%^&*()_+"
    .byte 127,9
    .ascii "QWERTYUIOP{}"
    .byte 13,0
    .ascii "ASDFGHJKL:\""
    .byte '~,0
    .ascii "|ZXCVBNM<>?"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '>
    .fill 10,1,0

alt_map:
    .byte 0,0
    .ascii "\0@\0$\0\0{[]}\\\0"
    .byte 0,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte '~,13,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0,0,0        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte 0,0,0,0,0        /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '|
    .fill 10,1,0

#elif defined(KBD_GR)

key_map:
    .byte 0,27
    .ascii "1234567890\\'"
    .byte 127,9
    .ascii "qwertzuiop@+"
    .byte 13,0
    .ascii "asdfghjkl[]^"
    .byte 0,'#
    .ascii "yxcvbnm,.-"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '<
    .fill 10,1,0


shift_map:
    .byte 0,27
    .ascii "!\"#$%&/()=?`"
    .byte 127,9
    .ascii "QWERTZUIOP\\*"
    .byte 13,0
    .ascii "ASDFGHJKL{}~"
    .byte 0,''
    .ascii "YXCVBNM;:_"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '>
    .fill 10,1,0

alt_map:
    .byte 0,0
    .ascii "\0@\0$\0\0{[]}\\\0"
    .byte 0,0
    .byte '@,0,0,0,0,0,0,0,0,0,0
    .byte '~,13,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0,0,0        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte 0,0,0,0,0        /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '|
    .fill 10,1,0

#elif defined(KBD_FR)

key_map:
    .byte 0,27
    .ascii "&{\"'(-}_/@)="
    .byte 127,9
    .ascii "azertyuiop^$"
    .byte 13,0
    .ascii "qsdfghjklm|"
    .byte '`,0,42        /* coin sup gauche, don't know, [*|mu] */
    .ascii "wxcvbn,;:!"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '<
    .fill 10,1,0

shift_map:
    .byte 0,27
    .ascii "1234567890]+"
    .byte 127,9
    .ascii "AZERTYUIOP<>"
    .byte 13,0
    .ascii "QSDFGHJKLM%"
    .byte '~,0,'#
    .ascii "WXCVBN?./\\"
    .byte 0,'*,0,32        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte '-,0,0,0,'+    /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '>
    .fill 10,1,0

alt_map:
    .byte 0,0
    .ascii "\0~#{[|`\\^@]}"
    .byte 0,0
    .byte '@,0,0,0,0,0,0,0,0,0,0
    .byte '~,13,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0
    .byte 0,0,0,0,0,0,0,0,0,0,0
    .byte 0,0,0,0        /* 36-39 */
    .fill 16,1,0        /* 3A-49 */
    .byte 0,0,0,0,0        /* 4A-4E */
    .byte 0,0,0,0,0,0,0    /* 4F-55 */
    .byte '|
    .fill 10,1,0

#else
#error "KBD-type not defined"
#endif
/*
 * do_self handles "normal" keys, ie keys that don't change meaning
 * and which have just one character returns.
 */
do_self:
    lea alt_map, %ebx   # 使用 alt_map 取映射字符
    testb $0x20,mode    # alt-gr
    jne 1f              # 是 alt 按下, 跳转到 1f
    lea shift_map, %ebx # 试试是不是 shift
    testb $0x03, mode
    jne 1f
    lea key_map, %ebx   # 不是 alt/shift, 使用普通字符映射
1:
    movb (%ebx,%eax), %al   # al=[ebx+eax], 把相应的字符保存到 al 里面
    orb %al,%al
    je none                 # al == 0 ? 跳转到 none 啥都不做

    # 接下来若 ctrl 键已按下或 caps 键锁定, 并且字符在 'a ~ }' 范围内, 则将其转成大写字符
    testb $0x4c, mode   # ctrl or caps
    je 2f
    cmpb $'a, %al
    jb 2f
    cmpb $'}, %al
    ja 2f
    subb $32, %al # 转大写
2:
    testb $0x0c, mode   # ctrl
    je 3f
    cmpb $64, %al       # '@' = A-1
    jb 3f
    cmpb $64+32, %al     # '`' = a-1
    jae 3f
    subb $64, %al       # ctrl - X 对应的控制字符
3:
    testb $0x10, mode   # left alt
    je 4f               # 没有按下 Alt(left)

    # 按下了 Alt(left), 将 code 转为对应的扩展字符
    # 即此时生成值大于 0x7f 的扩展字符集中的字符
    orb $0x80, %al
4:
    # 将 al 中字符放到读缓冲区
    andl $0xff, %eax
    xorl %ebx, %ebx
    call put_queue
none:
    ret

/*
 * minus has a routine of it's own, as a 'E0h' before
 * the scan code for minus means that the numeric keypad
 * slash was pushed.
 *
 * 减号有它自己的处理子程序, 因为在减号扫描码之前的0xe0
 * 意味着按下了数字小键盘上的斜杠键 */
minus:
    cmpb $1, e0
    jne do_self     # 没有 e0 前缀
    movl $'/, %eax  # 有 e0 前缀, 使用 `/` 替代 `-`
    xorl %ebx, %ebx
    jmp put_queue

/*
 * This table decides which routine to call when a scan-code has been
 * gotten. Most routines just call do_self, or none, depending if
 * they are make or break.
 *
 * 下面是一张子程序地址跳转表. 当取得扫描码后就根据此表调用相应的扫描码
 * 处理子程序. 大多数调用的子程序是 do_self, 或者是 none, 这起决于是按键
 * (make)还是释放键(break).
 * 下面给出所有按键可能的处理程序列表:
 *   - alt/unalt:
 *   - caps/uncaps:
 *   - ctrl/unctrl:
 *   - cursor:
 *   - do_self:
 *   - func:
 *   - lshift/unlshift:
 *   - minus:
 *   - none:
 *   - num:
 *   - rshift/unrshift:
 *   - scroll:
*/
key_table:
    .long none,do_self,do_self,do_self    /* 00-03 s0 esc 1 2 */
    .long do_self,do_self,do_self,do_self    /* 04-07 3 4 5 6 */
    .long do_self,do_self,do_self,do_self    /* 08-0B 7 8 9 0 */
    .long do_self,do_self,do_self,do_self    /* 0C-0F + ' bs tab */
    .long do_self,do_self,do_self,do_self    /* 10-13 q w e r */
    .long do_self,do_self,do_self,do_self    /* 14-17 t y u i */
    .long do_self,do_self,do_self,do_self    /* 18-1B o p } ^ */
    .long do_self,ctrl,do_self,do_self    /* 1C-1F enter ctrl a s */
    .long do_self,do_self,do_self,do_self    /* 20-23 d f g h */
    .long do_self,do_self,do_self,do_self    /* 24-27 j k l | */
    .long do_self,do_self,lshift,do_self    /* 28-2B { para lshift , */
    .long do_self,do_self,do_self,do_self    /* 2C-2F z x c v */
    .long do_self,do_self,do_self,do_self    /* 30-33 b n m , */
    .long do_self,minus,rshift,do_self    /* 34-37 . - rshift * */
    .long alt,do_self,caps,func        /* 38-3B alt sp caps f1 */
    .long func,func,func,func        /* 3C-3F f2 f3 f4 f5 */
    .long func,func,func,func        /* 40-43 f6 f7 f8 f9 */
    .long func,num,scroll,cursor        /* 44-47 f10 num scr home */
    .long cursor,cursor,do_self,cursor    /* 48-4B up pgup - left */
    .long cursor,cursor,do_self,cursor    /* 4C-4F n5 right + end */
    .long cursor,cursor,cursor,cursor    /* 50-53 dn pgdn ins del */
    .long none,none,do_self,func        /* 54-57 sysreq ? < f11 */
    .long func,none,none,none        /* 58-5B f12 ? ? ? */
    .long none,none,none,none        /* 5C-5F ? ? ? ? */
    .long none,none,none,none        /* 60-63 ? ? ? ? */
    .long none,none,none,none        /* 64-67 ? ? ? ? */
    .long none,none,none,none        /* 68-6B ? ? ? ? */
    .long none,none,none,none        /* 6C-6F ? ? ? ? */
    .long none,none,none,none        /* 70-73 ? ? ? ? */
    .long none,none,none,none        /* 74-77 ? ? ? ? */
    .long none,none,none,none        /* 78-7B ? ? ? ? */
    .long none,none,none,none        /* 7C-7F ? ? ? ? */
    .long none,none,none,none        /* 80-83 ? br br br */
    .long none,none,none,none        /* 84-87 br br br br */
    .long none,none,none,none        /* 88-8B br br br br */
    .long none,none,none,none        /* 8C-8F br br br br */
    .long none,none,none,none        /* 90-93 br br br br */
    .long none,none,none,none        /* 94-97 br br br br */
    .long none,none,none,none        /* 98-9B br br br br */
    .long none,unctrl,none,none        /* 9C-9F br unctrl br br */
    .long none,none,none,none        /* A0-A3 br br br br */
    .long none,none,none,none        /* A4-A7 br br br br */
    .long none,none,unlshift,none        /* A8-AB br br unlshift br */
    .long none,none,none,none        /* AC-AF br br br br */
    .long none,none,none,none        /* B0-B3 br br br br */
    .long none,none,unrshift,none        /* B4-B7 br br unrshift br */
    .long unalt,none,uncaps,none        /* B8-BB unalt br uncaps br */
    .long none,none,none,none        /* BC-BF br br br br */
    .long none,none,none,none        /* C0-C3 br br br br */
    .long none,none,none,none        /* C4-C7 br br br br */
    .long none,none,none,none        /* C8-CB br br br br */
    .long none,none,none,none        /* CC-CF br br br br */
    .long none,none,none,none        /* D0-D3 br br br br */
    .long none,none,none,none        /* D4-D7 br br br br */
    .long none,none,none,none        /* D8-DB br ? ? ? */
    .long none,none,none,none        /* DC-DF ? ? ? ? */
    .long none,none,none,none        /* E0-E3 e0 e1 ? ? */
    .long none,none,none,none        /* E4-E7 ? ? ? ? */
    .long none,none,none,none        /* E8-EB ? ? ? ? */
    .long none,none,none,none        /* EC-EF ? ? ? ? */
    .long none,none,none,none        /* F0-F3 ? ? ? ? */
    .long none,none,none,none        /* F4-F7 ? ? ? ? */
    .long none,none,none,none        /* F8-FB ? ? ? ? */
    .long none,none,none,none        /* FC-FF ? ? ? ? */

/* kb_wait waits for the keyboard controller buffer to empty.
 * there is no timeout - if the buffer doesn't empty, we hang. */
kb_wait:
    pushl %eax
1:
    inb $0x64, %al      # 读键盘控制器状态
    testb $0x02, %al    # 测试输入缓冲器是否为空(等于0)
    jne 1b              # 若不空, 则跳转循环等待
    popl %eax
    ret

/* This routine reboots the machine by asking the keyboard
 * controller to pulse the reset-line low.
 *
 * 地址 0x472 的作用: 位于 BIOS 数据区(0040:0072)存储启动标志字(Boot Flag Word)
 * 值 0x1234 的含义
 *  - 高字节 0x12 表示热启动(跳过 POST 过程)
 *  - 低字节 0x34 向 BIOS 传递'跳过内存检查'的信号
 *
 * 然后对键盘控制器端口 0x64 写命令, 触发重启
 *   Bit 0       复位CPU(低电平有效)
 *   Bit 1       禁用A20地址线
 *   Bit 2-7     保留(置1)
 */
reboot:
    call kb_wait
    movw $0x1234, 0x472 /* don't do memory check */
    movb $0xfc, %al     /* pulse reset and A20 low */
    outb %al, $0x64
die:
    jmp die
