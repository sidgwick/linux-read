#
# SYS_SIZE is the number of clicks (16 bytes) to be loaded.
# 0x3000 is 0x30000 bytes = 196kB, more than enough for current
# versions of linux
#

#include <linux/config.h>
SYSSIZE = DEF_SYSSIZE

#
#	bootsect.s		(C) 1991 Linus Torvalds
#	modified by Drew Eckhardt
#
# bootsect.s is loaded at 0x7c00 by the bios-startup routines, and moves
# iself out of the way to address 0x90000, and jumps there.
#
# It then loads 'setup' directly after itself (0x90200), and the system
# at 0x10000, using BIOS interrupts.
#
# NOTE! currently system is at most 8*65536 bytes long. This should be no
# problem, even in the future. I want to keep it simple. This 512 kB
# kernel size should be enough, especially as this doesn't contain the
# buffer cache as in minix
#
# The loader has been made as simple as possible, and continuos
# read errors will result in a unbreakable loop. Reboot by hand. It
# loads pretty fast by getting whole sectors at a time whenever possible.

.code16

.globl begtext, begdata, begbss, endtext, enddata, endbss

.text
begtext:

.data
begdata:

.bss
begbss:

.text

SETUPLEN = 4				# nr of setup-sectors
BOOTSEG  = 0x07c0			# original address of boot-sector
INITSEG  = DEF_INITSEG			# we move boot here - out of the way
SETUPSEG = DEF_SETUPSEG			# setup starts here
SYSSEG   = DEF_SYSSEG			# system loaded at 0x10000 (65536).
ENDSEG   = SYSSEG + SYSSIZE		# where to stop loading

# ROOT_DEV & SWAP_DEV are now written by "build".
ROOT_DEV = 0
SWAP_DEV = 0

.globl start
start:
        movw $BOOTSEG, %ax
        movw %ax, %ds
        movw $INITSEG, %ax
        movw %ax, %es
        movw $256, %cx
        subw %si, %si
        subw %di, %di
        rep movsw
        ljmp $INITSEG, $go

go:
        movw %cs, %ax
        movw $0xfef4, %dx  # arbitrary value >>512 - disk parm size

        # 目前设置 DX 为 0xfef4, 此值通过计算得出:
        #   INITSEG 段末尾(0x9ffff) 减去 12 字节(用于保存寄存器状态), 即 0x9ff00 - 12 = 0x9fef4
        #   由于段基址 CS=0x9000, 偏移量 0xfef4 对应物理地址 (0x9000<<4 + 0xfef4) = 0x9fef4, 接近 0x9ffff
        # TODO: 为什么要设置这个值呢?

        movw %ax, %ds
        movw %ax, %es
        pushw %ax

        movw %ax, %ss # put stack at 0x9ff00 - 12.
        movw %dx, %sp

/*
 *	Many BIOS's default disk parameter tables will not
 *	recognize multi-sector reads beyond the maximum sector number
 *	specified in the default diskette parameter tables - this may
 *	mean 7 sectors in some cases.
 *
 *  许多 BIOS 默认磁盘参数表不支持超出最大扇区数的多扇区读取(某些情况下仅支持 7 扇区)
 *
 *	Since single sector reads are slow and out of the question,
 *	we must take care of this by creating new parameter tables
 *	(for the first disk) in RAM.  We will set the maximum sector
 *	count to 18 - the most we will encounter on an HD 1.44.
 *
 *  单扇区读取效率低下, 因此需在 RAM 中创建新参数表(针对第一块磁盘),
 *  将最大扇区数设为 18(适用于 1.44MB 软盘)
 *
 *	High doesn't hurt.  Low does.
 *
 *  过高无妨, 过低则有害
 *
 *	Segments are as follows: ds=es=ss=cs - INITSEG,
 *		fs = 0, gs = parameter table segment
 */

        ### -------- 拷贝磁盘参数表
        pushw $0
        popw %fs
        mov $0x78, %bx # fs:bx is parameter table address
        # seg 并非独立指令, 而是​​段超越前缀​​(Segment Override Prefix), 它强制指定下一条指令使用的段寄存器, 覆盖默认的段寄存器设置
        # lgs 用于​​加载远指针(48 位地址)到 GS 段寄存器和指定的通用寄存器
        # // seg %fs
        # // lgs	si,(bx)			! gs:si is source
        lgs %fs:(%bx), %si # 从内存(0:0x78位置)加载 段和偏移量, 执行完成后 GS:SI 指向的就是这个参数表指向的位置

        movw %dx, %di # DX 之前被设置到内存位置稍高的区域了(0xfef4)
        mov $6, %cx # 拷贝 6*2 = 12 字节
        cld
        rep movsw %gs:(%si), %es:(%di)

        movw %dx, %di
        movb $18, 4(%di) # patch sector count --> 把磁盘参数表里面的最大扇区, 调整成 18

        # BX=0x78 --- 让中断向量 0x1E, 0x20 的值指向新表
        # TODO: 好像没啥作用?
        movw %di, %fs:(%bx)
        movw %es, %fs:2(%bx)

        ### -------- 拷贝磁盘参数表(结束)

        popw %ax
        mov %ax, %fs
        mov %ax, %gs

        xor %ah, %ah # reset FDC(Floppy Disk Controller)
        xor %dl, %dl # 1st floppy disk
        int $0x13

    # load the setup-sectors directly after the bootblock.
    # Note that 'es' is already set up.

    # 加载 setup 程序, 这个程序放在 0 磁盘 0 柱面 0 磁头 2 扇区
    # 目标内存位置是 ES:0x0200 开始, 这个位置紧挨着 MBR 在内存里面的位置
    # BIOS 0x13 - AH = 0x02, Read Sectors From Drive
    # ------- INPUTS:
    #       AH	02h
    #       AL	Sectors To Read Count
    #       CH	Cylinder
    #       CL	Sector
    #       DH	Head
    #       DL	Drive
    #       ES:BX	Buffer Address Pointer
    # ------- OUTPUTS:
    #       CF  Set On Error, Clear If No Error
    #       AH  Return Code
    #       AL  Actual Sectors Read Count
    load_setup:
        xor	%dx, %dx # drive 0, head 0
        mov	$0x0002, %cx # sector 2, track 0
        mov	$0x0200, %bx # address = 512, in INITSEG
        mov	$(0x0200+SETUPLEN), %ax	# service 2, nr of sectors
        int	$0x13 # read it
        jnc	ok_load_setup # ok - continue, TODO: 编译出来是 jne 指令?

        # 读取出错的情况
        pushw %ax			# dump error code
        call print_nl
        mov	%sp, %bp
        call print_hex
        pop	%ax

        # 重新读
        xor %ah, %ah # reset FDC
        xor %dl, %dl
        int $0x13
        jmp load_setup

    ok_load_setup:
        # Get disk drive parameters, specifically nr of sectors/track

        # BIOS 0x13 - AH=08h: Read Drive Parameters
        # -------- INPUT
        #   AH	08h = function number for read_drive_parameters
        #   DL	drive index (e.g. 1st HDD = 80h)
        #   ES:DI	set to 0000h:0000h to work around some buggy BIOS
        # -------- OUTPUT
        #   CF	Set On Error, Clear If No Error
        #   AH	Return Code
        #   DL	number of hard disk drives
        #   DH	logical last index of heads = number_of - 1 (because index starts with 0)
        #   CX	[7:6][15:8] logical last index of cylinders = number_of - 1 (because index starts with 0)
        #       [5:0] logical last index of sectors per track = number_of (because index starts with 1)
        #   BL	drive type (only AT/PS2 floppies)
        #   ES:DI	pointer to drive parameter table (only for floppies)


        xor %dl, %dl
        movb $0x08, %ah # AH=8 is get drive parameters
        int $0x13

        # 下句保存每磁道扇区数, 对于软盘来说(dl=0), 其最大磁道号不会超过 256
        # ch 已经足够表示它, 因此 cl 的位 6-7 肯定为 0. 只需要设置ch=0, 此时 cx 中就是每磁道扇区数
        xor %ch, %ch
        movw %cx, %cs:sectors
        movw $INITSEG, %ax
        movw %ax, %es

        # Print some inane message
        # BIOS 0x10
        # -------- INPUT
        #   AH=03h Get cursor position and shape
        #   BH = Page Number
        # -------- OUTPUT
        #   AX = 0
        #   CH = Start scan line
        #   CL = End scan line
        #   DH = Row
        #   DL = Column
        mov $0x03, %ah # read cursor pos
        xor %bh, %bh
        int $0x10


        # 注意这里已经有 ES = CS
        # BIOS 0x10
        # -------- INPUT
        #   AH=13h --- Write string (EGA+, meaning PC AT minimum)
        #   AL = Write mode
        #   BH = Page Number
        #   BL = Color
        #   CX = Number of characters in string
        #   DH = Row
        #   DL = Column
        #   ES:BP = Offset of string
        movw $9, %cx
        movw $0x0007, %bx # page 0, attribute 7 (normal)
        mov $msg1, %bp
        mov 0x1301, %ax # write string, move cursor
        int $0x10

        # ok, we've written the message, now
        # we want to load the system (at 0x10000)

        mov $SYSSEG, %ax
        mov %ax, %es # segment of 0x010000
        call read_it
        call kill_motor
        call print_nl

        # After that we check which root-device to use. If the device is
        # defined (!= 0), nothing is done and the given device is used.
        # Otherwise, either /dev/PS0 (2,28) or /dev/at0 (2,8), depending
        # on the number of sectors that the BIOS reports currently.

        mov %cs:root_dev, %ax
        or %ax, %ax
        jne root_defined # OR 操作影响 ZF 标志位, 如果 %ax != 0, 说明 root_dev 有定义

        mov $0x0208, %ax # /dev/ps0 - 1.2Mb
        mov %cs:sectors, %bx
        cmp $15, %bx
        je root_defined
        mov $0x021c, %ax # /dev/PS0 - 1.44Mb
        cmp $18, %bx
        je root_defined
    undef_root:
        jmp undef_root
    root_defined:
        mov	%ax, %cs:root_dev

        # after that (everyting loaded), we jump to
        # the setup-routine loaded directly after
        # the bootblock:

        ljmp $SETUPSEG, $0

    # This routine loads the system at address 0x10000, making sure
    # no 64kB boundaries are crossed. We try to load it as fast as
    # possible, loading whole tracks whenever we can.
    #
    # in:	es - starting address segment (normally 0x1000)
read_it:
        mov %es, %ax
        test %ax, 0x0FFF
    die:
        jne die # es must be at 64kB boundary

        xor %bx, %bx # bx is starting address within segment
    rp_read:
        mov %es, %ax
        cmp $ENDSEG, %ax # have we loaded all yet?
        jb ok1_read
        ret

    ok1_read:
        mov %cs:sectors, %ax # 每个磁道扇区数, 上面费劲算好的
        sub $sread, %ax # sread 是当前磁道已经读取的数量
        mov %ax, %cx # cx = 当前未读扇区数量
        shl $9, %cx # 乘上 512, 这是未读取的字节数量
        add %bx, %cx # bx 保存的是本次读取完成之后, 段内共计字节数量

        # JNC, JE 是 %cx 的状态有没有进位, cx 是 16 bits 的, 也就是最多读 64Kb
        jnc ok2_read
        je ok2_read

        # 若加上此次读, 磁道上所有未读扇区时会超过 64Kb, 则计算算此时最多能读入的字节数:
        # 使用 (64Kb - 段内读偏移位置), 再转换成需读取的扇区数, 其中 0 减某数就是取该数 64Kb 的补值
        # AX = 0 - BX == 0x10000 - BX, 这就是段内还剩余的空间容量
        # 这里太秒了, 假如 BX = 0001B, 寄存器只有 16bits, 那么(丢掉不存在的第 16bit):
        #   0x0_0000-BX = 1111B
        #   0x1_0000-BX = 1111B
        # 可见他们在内存中的表示是一样的, 我们只要强行将 -BX 解释称正数就行了
        # 这里 0x10000 就刚好是 64Kb
        xor %ax, %ax
        sub %bx, %ax
        shr $9, %ax # 转化为可读取的扇区数量
    ok2_read:
        call read_track # 读取一个磁道
        mov %ax, %cx # AX 是读取的扇区数量
        add $sread, %ax
        cmp %cs:sectors, %ax
        jne ok3_read
        mov $1, %ax
        sub $head, %ax
        jne ok4_read
        incw track
    ok4_read:
        mov %ax, head
        xor %ax, %ax
    ok3_read:
        mov %ax, sread
        shl $9, %cx
        add %cx, %bx
        jnc rp_read
        mov %es, %ax
        add $0x10, %ah
        mov %ax, %es
        xor %bx, %bx
        jmp rp_read

read_track:
        pusha
        pusha

        # 在屏幕上打印 `loading...` 里面的 `.` 号, message 2e = .
        # 搜索 `Teletype output`, 上面已经有关于这个调用功能的解释
        mov $0x0e2e, %ax
        mov $0x0007, %bx
        int $0x10

        popa

        mov $track, %dx # 当前磁道号, 磁道号不会超过 255
        mov $sread, %cx # 当前已经读取的扇区数量
        inc %cx
        mov %dl, %ch # CX, DX 都不会超过 255
        mov $head, %dx # 当前磁头号
        mov %dl, %dh
        and $0x0100, %dx # 磁头号不会大于 1, 因此只留第 4bit, DL 是驱动器号
        mov $2, %ah # 0x13 中断的 2 号功能, 搜索 `Read Sectors From Drive` 看注解

        push %dx # save for error dump
        push %cx
        push %bx
        push %ax

        int $0x13 # ES:BX Buffer Address Pointer
        jc bad_rt

        add $8, %sp # 恢复栈

        popa
        ret

    bad_rt:
        push %ax # save error code
        call print_all # ah = error, al = read

        xor %ah, %ah # reset FDC
        xor %dl, %dl
        int $0x13

        add $10, %sp
        popa

        jmp read_track

/*
 *	print_all is for debugging purposes.
 *	It will print out all of the registers.  The assumption is that this is
 *	called from a routine, with a stack frame like
 *	dx
 *	cx
 *	bx
 *	ax
 *	error
 *	ret <- sp
 *
 */
print_all:
        mov $5, %cx # error code + 4 registers
        mov %sp, %bp # TODO: why not push %bp

    print_loop:
        push %cx # save count left
        call print_nl # nl for readability
        jae no_reg #see if register name is needed

        mov $(0xe05 + 0x41 - 1), %ax
        sub %cl, %al
        int $0x10

        mov $0x58, %al # X
        int $0x10

        mov $0x3a, %al #  :
        int $0x10

    no_reg:
        add	$2, %bp # next register
        call print_hex # print it
        pop %cx
        loop print_loop
        ret

# BIOS 0x10 号中断 -- AH=0Eh:
# Teletype output
#   AL=Character
#   BH=Page Number
#   BL=Color (only in graphic mode)
print_nl:
        movw $0xe0d, %ax # CR
        int $0x10
        movb $0xa, %al # LF
        int $0x10
        ret

/*
 * print_hex is for debugging purposes, and prints the word
 * pointed to by ss:bp in hexadecmial.
 */
print_hex:
        movw $4, %cx # 4 hex digits
        movw (%bp), %dx # load word into dx, `bp` 默认就是配合 SS 使用
    print_digit:
        rolw $4, %dx # rotate so that lowest 4 bits are used
        movb $0x0e, %ah
        movb %dl, %al #  mask off so we have only next nibble
        andb $0x0f, %al
        addb $0x30, %al # convert to 0 based digit, '0'
        cmpb $0x39, %al # check for overflow
        jbe good_digit # JMP below equal, 上面处理的是数字, 下面处理的是字母
        addb $(0x41 - 0x30 - 0xa), %al # 'A' - '0' - 0xa

    good_digit:
        int $0x10
        loop print_digit
        ret

/*
 * This procedure turns off the floppy drive motor, so
 * that we enter the kernel in a known state, and
 * don't have to worry about it later.
 *
 *   参考:
 *     - https://blog.csdn.net/longintchar/article/details/106912096
 *     - http://wiki.osdev.org/Floppy_Disk_Controller
 *
 *   Mnemonic   bit number   value    meaning/usage
 *   MOTD       7            0x80     Set to turn drive 3's motor ON
 *   MOTC       6            0x40     Set to turn drive 2's motor ON
 *   MOTB       5            0x20     Set to turn drive 1's motor ON
 *   MOTA       4            0x10     Set to turn drive 0's motor ON
 *   IRQ        3            8        Set to enable IRQs and DMA
 *   RESET      2            4        Clear = enter reset mode, Set = normal operation
 *   DSEL       0,1          3        "Select" drive number for next access
 */
kill_motor:
        push %dx
        mov $0x3f2, %dx # 软驱控制卡的数字输出寄存器(DOR)端口, 只写
        movb $0b00000000, %al # A驱动器, 关闭FDC, 禁止DMA和中断请求, 关闭马达
        outb %al, (%dx)
        pop %dx
        ret

sread: .word 1+SETUPLEN	# sectors read of current track - 当前已经读取的扇区数量
head: .word 0 # current head - 当前磁头号
track: .word 0 # current track - 当前磁道号
sectors: .word 0 # 磁盘一个磁道有多少扇区

msg1:
    .byte 13, 10
    .ascii "Loading"

# .org指令(Origin Directive)是一种伪指令(Pseudo-instruction), 用于​​指定后续代码或数据在内存中的起始偏移地址
# 它不会生成任何机器码, 仅用于指导汇编器在编译时如何布局程序和数据
.org 506

swap_dev:
    .word SWAP_DEV
root_dev:
    .word ROOT_DEV
boot_flag:
    .word 0xAA55

.text
endtext:
.data
enddata:
.bss
endbss:
