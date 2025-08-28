#
# setup.s  (C) 1991 Linus Torvalds
#
# setup.s is responsible for getting the system data from the BIOS,
# and putting them into the appropriate places in system memory.
# both setup.s and system has been loaded by the bootblock.
#
# This code asks the bios for memory/disk/other parameters, and
# puts them in a "safe" place: 0x90000-0x901FF, ie where the
# boot-block used to be. It is then up to the protected mode
# system to read them from there before the area is overwritten
# for buffer-blocks.
#
#
# NOTE! These had better be the same as in bootsect.s!

#include <linux/config.h>

INITSEG  = DEF_INITSEG # we move boot here - out of the way
SYSSEG   = DEF_SYSSEG # system loaded at 0x10000 (65536).
SETUPSEG = DEF_SETUPSEG # this is the current segment

.code16

.globl start, begtext, begdata, begbss, endtext, enddata, endbss

.text

start:
        # ok, the read went well so we get current cursor position and save it for
        # posterity.

        mov $INITSEG, %ax
        mov %ax, %ds

        # BIOS INT 15h AH=88h: Get Extended Memory Size
        # 返回值: AX = number of contiguous KB above 1M - 注意是超过 1M 的扩展内存大小
        #        若出错则 CF 置位, ax=错误码

        # Get memory size (extended mem, kB)
        mov $0x88, %ah
        int $0x15
        mov %ax, 0x02 # 保存到 INITSEG:0x02 位置

        # 参考: https://mirror.math.princeton.edu/pub/oldlinux/Linux.old/docs/interrupts/int-html/rb-0162.htm
        # BIOS INT 10h AH=12h: detect EGA/VGA
        # 检查显示方式(EGA/VGA)并取参数。
        #
        # 返回:
        #    AH = ​​开关设置状态​​(如视频模式, 显示组合码)
        #    AL = ​​视频模式状态​​(如当前模式是否有效)
        #    BH = 显示状态: 0x00-彩色模式,I/0端口=0x3dX:0x01-单色模式,I/0端口=0x3bX
        #    BL = 安装的显示内存: 0x00-64k;0x01-128k;0x02 - 192k; 0x03 = 256k。
        #    CX = 显示卡特性参数

        # check for EGA/VGA and some config parameters
        mov $0x12, %ah
        mov $0x10, %bl
        int $0x10

        mov %ax, 0x08
        mov %bx, 0x0a # 小端序, 因此 0b=内存容量, 0a=显示状态
        mov %cx, 0x0c

        mov $0x5019, %ax # 默认是 0x50=80 列, 0x19=25 行
        cmp $0x10, %bl # ​​基础 EGA 适配器​​(无 VGA 扩展功能)
        je novga
        call chsvga
    novga:
        mov %ax, 0x0e
        mov $0x03, %ah # read cursor pos
        xor %bh, %bh
        int $0x10

        # save it in known place, con_init fetches it from 0x90000
        mov %dx, 0 # 光标位置

        # BIOS INT 0x10, AH = 0xF -- get video page and mode
        #
        # 下面这段用于取显示卡当前显示模式
        # 返回:
        #    ah=字符列数
        #    al=显示模式
        #    bh=当前显示页
        #
        #    0x90004(1字)存放当前页
        #    0x90006存放显示模式
        #    0x90007存放字符列数

        # Get video-card data:
        mov $0x0f, %ah
        int $0x10
        mov %bx, 0x04 # bh = display page
        mov %ax, 0x06 # al = video mode, ah = window width

        # BIOS 41, 46 号中断的位置存的不是 handler, 而是:
        # INT 41h -- 地址指针: 硬盘参数表(第一硬盘)
        # INT 46h -- 地址指针: 硬盘参数表(第二硬盘)

        # Get hd0 data
        mov $0x0000, %ax
        mov %ax, %ds
        # lds 用于​​加载远指针(48 位地址)到 DS 段寄存器和指定的通用寄存器
        lds (4*0x41), %si
        mov $INITSEG, %ax
        mov %ax, %es
        mov $0x0080, %di
        mov $0x10, %cx # copy 16 bytes
        rep movsb

        # Get hd1 data
        mov $0x0000, %ax
        mov %ax, %ds
        lds (4*0x46), %si
        mov $INITSEG, %ax
        mov %ax, %es
        mov $0x0090, %di
        mov $0x10, %cx
        rep movsb

        # BIOS INT 13h AH=15h: Reset Drive Type
        # ------- INPUT:
        #   DL    Drive (0x80表示第一块硬盘hd0, 0x81表示第二块硬盘hd1)
        # ------- OUTPUT:
        #   CF    Set on error
        #   AH    ah=00-没有这个盘, CF置位
        #            01-是软软驱, 没有change-line支持
        #            02-是软驱(或其他可移动设备), 有change-line支持
        #            03-是硬盘

        # Check that there IS a hd1 :-)
        mov $0x1500, %ax
        mov $0x81, %dl
        int $0x13
        jc no_disk1
        cmp $3, %ah
        je is_disk1
    no_disk1:
        # 擦掉刚刚复制的 HD1 参数表信息
        mov $INITSEG, %ax
        mov %ax, %es
        mov $0x0090, %di
        mov $0x10, %cx
        mov $0x00, %ax
        # The stos (Store String) instruction copies the value in AL, AX or EAX into
        #   the location pointed to by ES:DI or ES:EDI (depending on the address-size
        #   attribute of the instruction, 32 or 16, respectively). (E)DI is then
        #   incremented (if the direction flag is cleared) or decremented (if the
        #   direction flag is set), in preparation for storing AX in the next location.
        # ---------------------------------------------------------------------------
        # stosb stores a byte from the AL register into the destination operand.
        # stosw stores a word from the AX register into the destination operand.
        # stosd stores a doubleword from the EAX register into the destination operand.
        rep stosb
    is_disk1:

        # now we want to move to protected mode ...

        cli   # no interrupts allowed !

        # first we move the system to it's rightful place
        # 把 setup 程序, 从 XXX 移动到 0x0 地址, 每次拷贝 4Kb, 一共拷贝 9 次共计 36Kb

        # bootsect 引导程序是将 system 模块载入到 0x10000 (64Kb) 开始的位置
        # 由于当时假设 system 模块最大长度不会超过 0x80000 (512KB), 即其末端不会超过内存地址 0x90000,
        # 所以 bootsect 会把自己移动到 0x90000 开始的地方, 并把 setup 加载到它的后面
        # 下面这段程序的用途是再把整个 system 模块移动到 0x000000 位置, 即把从 0x10000 到 0x8ffff
        # 的内存数据块(512KB)整块地向内存低端移动了0x10000 (64KB)的位置

        mov $0x0000, %ax
        cld   # 'direction'=0, movs moves forward
    do_move:
        mov %ax, %es # destination segment
        add $0x1000, %ax # 4096
        cmp $0x9000, %ax
        jz end_move
        mov %ax, %ds # source segment
        sub %di, %di
        sub %si, %si
        mov  $0x8000, %cx # 32K 次 * 2byte = 64Kb
        rep movsw
        jmp do_move

        # 比如低一个循环开始的时候
        #   DS = AX + 0x1000, SI=0
        #   ES = AX, DI=0
        # 所以移动的内存是从 (0x1000 << 4 + 0) --> 0, 这块区域大小是 64Kb
        # 循环完成之后, 一共移动了 64*8 = 512Bb

    # 从下面开始, 系统映像放在了 0x00 开始的 512Kb 字节内存中
    # 也就是从地址范围为 0x00000 ~ 0x80000 的内存空间中
    # 要注意 setup 还在 0x90200 开始的内存空间

        # then we load the segment descriptors
    end_move:
        mov $SETUPSEG, %ax # right, forgot this at first. didn't work :-)
        mov %ax, %ds

        # 加载中断描述符表和全局描述符表
        lidt idt_48 # load idt with 0, 0
        lgdt gdt_48 # load gdt with whatever appropriate

        # that was painless, now we enable A20
        call empty_8042
        mov $0xD1, %al # command write
        out %al, $0x64
        call empty_8042
        mov $0xDF, %al # A20 on
        out %al, $0x60
        call empty_8042

        /*
        ; 或者这样打开:
        in al, 0x92         ; 南桥芯片内的端口
        or al, 0000_0010B
        out 0x92, al       ; 打开A20
        */

        # 下面是设置 8259A PIC, 调整它的中断号, 避免和处理器内置中断冲突
        # ------------------------
        # well, that went ok, I hope. Now we have to reprogram the interrupts :-(
        # we put them right after the intel-reserved hardware interrupts, at
        # int 0x20-0x2F. There they won't mess up anything. Sadly IBM really
        # messed this up with the original PC, and they haven't been able to
        # rectify it afterwards. Thus the bios puts interrupts at 0x08-0x0f,
        # which is used for the internal hardware interrupts as well. We just
        # have to reprogram the 8259's, and it isn't fun.

        # 关于 .word 0x00eb, 0x00eb 说明
        # EB 00 对应的汇编是 jmp $+2, 即跳转到 IP + 2 执行, 这事实上相当于什么都不做
        # 主要是早期硬件比较慢, 起一个延时作用
        mov $0x11, %al #  主片 ICW1 = 需要 ICW4, 级联模式, 边沿触发
        out %al, $0x20 #  send it to 8259A-1
        .word 0x00eb, 0x00eb
        out %al, $0xA0  # and to 8259A-2, 从片 ICW1
        .word 0x00eb, 0x00eb
        mov $0x20, %al # start of hardware int's (0x20), 主片 ICW2 = 指定中断号为 0x20~27
        out %al, $0x21
        .word 0x00eb, 0x00eb
        mov $0x28, %al #  start of hardware int's 2 (0x28), 从片 ICW2 = 指定中断号为 0x28~2F
        out %al, $0xA1
        .word 0x00eb, 0x00eb
        mov $0x04, %al #  8259-1 is master, 主片 ICW3 = 0000_0100, 从片接在 IRQ2 引脚
        out %al, $0x21
        .word 0x00eb, 0x00eb
        mov $0x02, %al #  8259-2 is slave, 从片 ICW3 = 0x02, 表示从片接在主片的 IRQ2 引脚
        out %al, $0xA1
        .word 0x00eb, 0x00eb
        mov $0x01, %al #  8086 mode for both, ICW4 = 非缓冲方式, 非自动结束中断(AEOI=0), SFNM=0
        out %al, $0x21
        .word 0x00eb, 0x00eb
        out %al, $0xA1
        .word 0x00eb, 0x00eb
        mov $0xFF, %al # mask off all interrupts for now. OCW1 = 设置中断屏蔽掩码, 暂时全部中断都不允许
        out %al, $0x21
        .word 0x00eb, 0x00eb
        out %al, $0xA1

        # well, that certainly wasn't fun :-(. Hopefully it works, and we don't
        # need no steenking BIOS anyway (except for the initial loading :-).
        # The BIOS-routine wants lots of unnecessary data, and it's less
        # "interesting" anyway. This is how REAL programmers do it.
        #
        # Well, now's the time to actually move into protected mode. To make
        # things as simple as possible, we do no register set-up or anything,
        # we let the gnu-compiled 32-bit programs do that. We just jump to
        # absolute address 0x00000, in 32-bit protected mode.

        mov $0x0001, %ax # protected mode (PE) bit
        lmsw %ax # This is it!
        ljmp $8, $0 # jmp offset 0 of segment 8 (cs)

        # LMSW = <Load Machine Status Word>
        # 作用是将 AX的值加载到 ​​CR0 寄存器​​（控制寄存器0）的低16位（称为“机器状态字”)

        # 到这里 setup 其实已经结束了, 因为它跳转到了 system 的开始位置去执行

/*

struct info {
    uint16_t cursor;         // 0x00, 光标位置
    uint16_t extended_mem;   // 0x02, 扩展内存大小
    uint16_t display_page;   // 0x04, display page
    uint8_t display_mode;    // 0x06, video mode
    uint8_t display_columns; // 0x07, window width
    uint16_t display_???;    // 0x08, 显示配置信息
    uint8_t display_memory;  // 0x0a, 显示内存, 0x00-64k, 0x01-128k, 0x02-192k, 0x03-256k
    uint8_t display_status;  // 0x0b, 显示状态, 0x00-彩色模式(I/0端口=0x3dX), 0x01-单色模式(I/0端口=0x3bX)
    uint16_t display_card;   // 0x0c, 显示卡特性参数
    uint8_t ;                // 0x0e, 屏幕行行数
    uint8_t ;                // 0x0f, 屏幕行列数
    ....
    16bytes ;                // 0x80, HD0 参数 - 16 字节
    16bytes ;                // 0x90, HD1 参数 - 16 字节
}

*/




# This routine checks that the keyboard command queue is empty
# No timeout is used - if this hangs there is something wrong with
# the machine, and we probably couldn't proceed anyway.
empty_8042:
        .word 0x00eb, 0x00eb
        in $0x64, %al # 8042 status port
        test $2, %al  # is input buffer full?
        jnz empty_8042 # yes - loop
        ret

# Routine trying to recognize type of SVGA-board present (if any)
# and if it recognize one gives the choices of resolution it offers.
# If one is found the resolution chosen is given by al,ah (rows,cols).
# 这块都是和 VGA 相关的操作, 不是本次学习的重点, 先不研究了
# 可以参考 https://www.oldlinux.org/Book-Lite/code-list/pdf/P6-2.pdf
chsvga:
        cld

        push %ds
        push %cs
        pop %ds
        mov $0xc000, %ax
        mov %ax, %es
        lea msg1, %si
        call prtstr
    nokey:
        in $0x60, %al
        cmp $0x82, %al
        jb nokey
        cmp $0xe0, %al
        ja nokey
        cmp $0x9c, %al
        je svga
        mov $0x5019, %ax
        pop %ds
        ret

    svga:
        lea idati, %si # Check ATI 'clues'
        mov $0x31, %di
        mov $0x09, %cx
        repe cmpsb
        jne noati
        lea dscati, %si
        lea moati, %di
        lea selmod, %cx
        jmp *%cx

    noati:
        mov $0x200f, %ax # Check Ahead 'clues'
        mov $0x3ce, %dx
        out %ax, %dx
        inc %dx
        in %dx, %al
        cmp $0x20, %al
        je isahed
        cmp $0x21, %al
        jne noahed
    isahed:
        lea dscahead, %si
        lea moahead, %di
        lea selmod, %cx
        jmp *%cx
    noahed:
        mov $0x3c3, %dx # Check Chips & Tech. 'clues'
        in %dx, %al
        or $0x10, %al
        out %al, %dx
        mov $0x104, %dx
        in %dx, %al
        mov %al, %bl
        mov $0x3c3, %dx
        in %dx, %al
        and $0xef, %al
        out %al, %dx
        cmp idcandt, %bl
        jne nocant
        lea dsccandt, %si
        lea mocandt, %di
        lea selmod, %cx
        jmp *%cx
    nocant:
        mov $0x3d4, %dx  # Check Cirrus 'clues'
        mov $0x0c, %al
        out %al, %dx
        inc %dx
        in %dx, %al
        mov %al, %bl
        xor %al, %al
        out %al, %dx
        dec %dx
        mov $0x1f, %al
        out %al, %dx
        inc %dx
        in %dx, %al
        mov %al, %bh
        xor %ah, %ah
        shl $4, %al
        mov %ax, %cx
        mov %bh, %al
        shr $4, %al
        add %ax, %cx
        shl $8, %cx
        add $6, %cx
        mov %cx, %ax
        mov $0x3c4, %dx
        out %ax, %dx
        inc %dx
        in %dx, %al
        and %al, %al
        jnz nocirr
        mov %bh, %al
        out %al, %dx
        in %dx, %al
        cmp $0x01, %al
        jne nocirr
        call rst3d4
        lea dsccirrus, %si
        lea mocirrus, %di
        lea selmod, %cx
        jmp *%cx
    rst3d4:
        mov $0x3d4, %dx
        mov %bl, %al
        xor %ah, %ah
        shl $8, %ax
        add $0x0c, %ax
        out %ax, %dx
        ret
    nocirr:
        call rst3d4 # Check Everex 'clues'
        mov $0x7000, %ax
        xor %bx, %bx
        int $0x10
        cmp $0x70, %al
        jne noevrx
        shr $4, %dx
        cmp $0x678, %dx
        je istrid
        cmp $0x236, %dx
        je istrid
        lea dsceverex, %si
        lea moeverex, %di
        lea selmod, %cx
        jmp *%cx
    istrid:
        lea ev2tri, %cx
        jmp *%cx
    noevrx:
        lea idgenoa, %si # Check Genoa 'clues'
        xor  %ax, %ax
        mov %es:0x37, %al
        mov %ax, %di
        mov $0x04, %cx
        dec %si
        dec %di
    l1:
        inc %si
        inc %di
        mov (%si), %al
        and %es:(%di), %al
        cmp (%si), %al
        loope  l1
        cmp $0x00, %cx
        jne nogen
        lea dscgenoa, %si
        lea mogenoa, %di
        lea selmod, %cx
        jmp *%cx
    nogen:
        lea idparadise, %si  # Check Paradise 'clues'
        mov $0x7d, %di
        mov $0x04, %cx
        repe cmpsb
        jne nopara
        lea dscparadise, %si
        lea moparadise, %di
        lea selmod, %cx
        jmp *%cx
    nopara:
        mov $0x3c4, %dx  # Check Trident 'clues'
        mov $0x0e, %al
        out %al, %dx
        inc %dx
        in %dx, %al
        xchg %al, %ah
        mov $0x00, %al
        out %al, %dx
        in %dx, %al
        xchg %ah, %al
        mov %al, %bl  # Strange thing ... in the book this wasn't
        and $0x02, %bl # necessary but it worked on my card which
        jz setb2  # is a trident. Without it the screen goes
        and $0xfd, %al # blurred ...
        jmp clrb2  #
    setb2:
        or $0x02, %al #
    clrb2:
        out %al, %dx
        and $0x0f, %ah
        cmp $0x02, %ah
        jne notrid
    ev2tri:
        lea dsctrident, %si
        lea motrident, %di
        lea selmod, %cx
        jmp *%cx
    notrid:
        mov $0x3cd, %dx  # Check Tseng 'clues'
        in %dx, %al   # Could things be this simple ! :-)
        mov %al, %bl
        mov $0x55, %al
        out %al, %dx
        in %dx, %al
        mov %al, %ah
        mov %bl, %al
        out %al, %dx
        cmp $0x55, %ah
        jne notsen
        lea dsctseng, %si
        lea motseng, %di
        lea selmod, %cx
        jmp *%cx
    notsen:
        mov $0x3cc, %dx  # Check Video7 'clues'
        in %dx, %al
        mov $0x3b4, %dx
        and $0x01, %al
        jz even7
        mov $0x3d4, %dx
    even7:
        mov $0x0c, %al
        out %al, %dx
        inc %dx
        in %dx, %al
        mov %al, %bl
        mov $0x55, %al
        out %al, %dx
        in %dx, %al
        dec %dx
        mov $0x1f, %al
        out %al, %dx
        inc %dx
        in %dx, %al
        mov %al, %bh
        dec %dx
        mov $0x0c, %al
        out %al, %dx
        inc %dx
        mov %bl, %al
        out %al, %dx
        mov $0x55, %al
        xor $0xea, %al
        cmp %bh, %al
        jne novid7
        lea dscvideo7, %si
        lea movideo7, %di
    selmod:
        push %si
        lea msg2, %si
        call prtstr
        xor %cx, %cx
        mov (%di), %cl
        pop %si
        push %si
        push %cx
    tbl:
        pop %bx
        push %bx
        mov %bl, %al
        sub %cl, %al
        call dprnt
        call spcing
        lodsw
        xchg %ah, %al
        call dprnt
        xchg %al, %ah
        push %ax
        mov $0x78, %al
        call prnt1
        pop %ax
        call dprnt
        call docr
        loop tbl
        pop %cx
        call docr
        lea msg3, %si
        call prtstr
        pop %si
        add $0x80, %cl
    nonum:
        in $0x60, %al # Quick and dirty...
        cmp $0x82, %al
        jb nonum
        cmp $0x8b, %al
        je zero
        cmp %cl, %al
        ja nonum
        jmp nozero
    zero:
        sub $0x0a, %al
    nozero:
        sub $0x80, %al
        dec %al
        xor %ah, %ah
        add %ax, %di
        inc %di
        push %ax
        mov (%di), %al
        int $0x10
        pop %ax
        shl $1, %ax
        add %ax, %si
        lodsw
        pop %ds
        ret

    novid7:
        pop %ds # Here could be code to support standard 80x50,80x30
        mov $0x5019, %ax
        ret

# Routine that 'tabs' to next col.
spcing:
        mov $0x2e, %al
        call prnt1
        mov $0x20, %al
        call prnt1
        mov $0x20, %al
        call prnt1
        mov $0x20, %al
        call prnt1
        mov $0x20, %al
        call prnt1
        ret

# Routine to print asciiz-string at DS:SI
prtstr:
        lodsb
        and %al, %al
        jz fin
        call prnt1
        jmp prtstr
    fin:
        ret

# Routine to print a decimal value on screen, the value to be
# printed is put in al (i.e 0-255).
dprnt:
        push %ax
        push %cx
        mov $0x00, %ah
        mov $0x0a, %cl
        idiv %cl
        cmp $0x09, %al
        jbe lt100
        call dprnt
        jmp skip10
    lt100:
        add $0x30, %al
        call prnt1
    skip10:
        mov %ah, %al
        add $0x30, %al
        call prnt1
        pop %cx
        pop %ax
        ret

# Part of above routine, this one just prints ascii al
prnt1:
        push %ax
        push %cx
        mov $0x00, %bh
        mov $0x01, %cx
        mov $0x0e, %ah
        int $0x10
        pop %cx
        pop %ax
        ret

# Prints <CR> + <LF>
docr:
        push %ax
        push %cx
        mov $0x00, %bh
        mov $0x0e, %ah
        mov $0x0a, %al
        mov $0x01, %cx
        int $0x10
        mov $0x0d, %al
        int $0x10
        pop %cx
        pop %ax
        ret

# 下面是 3 个描述符
gdt:
    # 0 号描述符是空
    .word 0,0,0,0 # dummy

    # 1 号描述符全局代码段
    # 从高位向低位排开描述符如下(上高下低, 左高右低):
    # 00C0_9A00 ~ BBRL_RRBB
    # 0000_07FF ~ BBBB_LLLL
    # base=0x00, limit=0x7FF, 4K, 32bits, P=1, dpl=0, S=1, XCRA=1010=可读可执行代码段
    .word 0x07FF  #  8Mb - limit=2047 (2048*4096=8Mb)
    .word 0x0000  #  base address=0
    .word 0x9A00  #  code read/exec
    .word 0x00C0  #  granularity=4096, 386

    # 2 号描述符全局数据段
    # 00C0_9200 ~ BBRL_RRBB
    # 0000_07FF ~ BBBB_LLLL
    # base=0x00, limit=0x7FF, 4K, 32bits, P=1, dpl=0, S=1, XEWA=0010=可读可写数据段
    .word 0x07FF  #  8Mb - limit=2047 (2048*4096=8Mb)
    .word 0x0000  #  base address=0
    .word 0x9200  #  data read/write
    .word 0x00C0  #  granularity=4096, 386

idt_48:
    .word 0   # idt limit=0
    .word 0,0   # idt base=0L

gdt_48:
    .word 0x800  # gdt limit=2048, 256 GDT entries
    .long (SETUPSEG<<4)+gdt # gdt base = 0X9xxxx

msg1: .ascii "Press <RETURN> to see SVGA-modes available or any other key to continue."
      .byte 0x0d, 0x0a, 0x0a, 0x00
msg2: .ascii "Mode:  COLSxROWS:"
      .byte 0x0d, 0x0a, 0x0a, 0x00
msg3: .ascii "Choose mode by pressing the corresponding number."
      .byte 0x0d, 0x0a, 0x00

idati: .ascii "761295520"
idcandt: .byte 0xa5
idgenoa: .byte 0x77, 0x00, 0x66, 0x99
idparadise: .ascii "VGA="

# Manufacturer:   Numofmodes: Mode:
moati:  .byte 0x02, 0x23, 0x33
moahead: .byte 0x05, 0x22, 0x23, 0x24, 0x2f, 0x34
mocandt: .byte 0x02, 0x60, 0x61
mocirrus: .byte 0x04, 0x1f, 0x20, 0x22, 0x31
moeverex: .byte 0x0a, 0x03, 0x04, 0x07, 0x08, 0x0a, 0x0b, 0x16, 0x18, 0x21, 0x40
mogenoa: .byte 0x0a, 0x58, 0x5a, 0x60, 0x61, 0x62, 0x63, 0x64, 0x72, 0x74, 0x78
moparadise: .byte 0x02, 0x55, 0x54
motrident: .byte 0x07, 0x50, 0x51, 0x52, 0x57, 0x58, 0x59, 0x5a
motseng: .byte 0x05, 0x26, 0x2a, 0x23, 0x24, 0x22
movideo7: .byte 0x06, 0x40, 0x43, 0x44, 0x41, 0x42, 0x45

# msb = Cols lsb = Rows:
dscati: .word 0x8419, 0x842c
dscahead: .word 0x842c, 0x8419, 0x841c, 0xa032, 0x5042
dsccandt: .word 0x8419, 0x8432
dsccirrus: .word 0x8419, 0x842c, 0x841e, 0x6425
dsceverex: .word 0x5022, 0x503c, 0x642b, 0x644b, 0x8419, 0x842c, 0x501e, 0x641b, 0xa040, 0x841e
dscgenoa: .word 0x5020, 0x642a, 0x8419, 0x841d, 0x8420, 0x842c, 0x843c, 0x503c, 0x5042, 0x644b
dscparadise: .word 0x8419, 0x842b
dsctrident: .word  0x501e, 0x502b, 0x503c, 0x8419, 0x841e, 0x842b, 0x843c
dsctseng: .word 0x503c, 0x6428, 0x8419, 0x841c, 0x842c
dscvideo7: .word 0x502b, 0x503c, 0x643c, 0x8419, 0x842c, 0x841c

.text
endtext:

.data
enddata:

.bss
endbss:

