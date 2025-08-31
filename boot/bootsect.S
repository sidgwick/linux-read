#
# SYS_SIZE is the number of clicks (16 bytes) to be loaded.
# 0x3000 is 0x30000 bytes = 196kB, more than enough for current
# versions of linux
#

#include <linux/config.h>
SYSSIZE = DEF_SYSSIZE

# BIOS 中断的更多信息, 可以参考这里:
# https://mirror.math.princeton.edu/pub/oldlinux/Linux.old/docs/interrupts/int-html/int.htm

#
# bootsect.s  (C) 1991 Linus Torvalds
# modified by Drew Eckhardt
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

SETUPLEN = 4    # nr of setup-sectors
BOOTSEG  = 0x07c0   # original address of boot-sector
INITSEG  = DEF_INITSEG   # we move boot here - out of the way
SETUPSEG = DEF_SETUPSEG   # setup starts here
SYSSEG   = DEF_SYSSEG   # system loaded at 0x10000 (65536).
ENDSEG   = SYSSEG + SYSSIZE  # where to stop loading

# ROOT DEV:
#     0x000 - same type of floppy as boot.
#             根文件系统设备使用与引导时同样的软驱设备;
#     0x301 - first partition on first drive etc
#             根文件系统设备在第一个硬盘的第一个分区上,等等;
#
# 设备号 0x306 指定根文件系统设备是第 2 个硬盘的第 1 个分区.
# 当年 Linus 是在第 2 个硬盘上安装了 Linux 0.11 系统, 所以这里 ROOT_DEV 被设置为 0x306
# 在编译这个内核时你可以根据自己根文件系统所在设备位置修改这个设备号, 这个设备号是 Linux
# 系统老式的硬盘设备号命名方式, 硬盘设备号具体值的含义如下:
#
# 设备号 = 主设备号*256+次设备号(也即 dev_no = (major << 8) + minor)
#
# 主设备号:
#     1-内存
#     2-磁盘
#     3-硬盘
#     4-ttyx
#     5-tty
#     6-并行口
#     7-非命名管道
#
# 0x300 /dev/hd0 代表整个第 1 个硬盘
# 0x301 /dev/hd1 第 1 个盘的第 1 个分区
# 0x304 /dev/hd4 第 1 个盘的第 4 个分区
# .....
# 0x305 /dev/hd5 代表整个第 2 个硬盘
# 0x306 /dev/hd6 第 2 个盘的第 1 个分区
# 0x309 /dev/hd9 第 2 个盘的第 4 个分区
#
# 从 Linux 内核 0.95 版后就已经使用与现在内核相同的命名方法了

# ROOT_DEV & SWAP_DEV are now written by "build".
ROOT_DEV = 0
SWAP_DEV = 0

.globl start
start:
        mov $BOOTSEG, %ax
        mov %ax, %ds
        mov $INITSEG, %ax
        mov %ax, %es
        mov $256, %cx
        sub %si, %si
        sub %di, %di
        rep movsw
        ljmp $INITSEG, $go

go:
        mov %cs, %ax
        mov $0xfef4, %dx  # arbitrary value >>512 - disk parm size

        # 目前设置 DX 为 0xfef4, 此值通过计算得出:
        #   INITSEG 段末尾(0x9ffff) 减去 12 字节(用于保存寄存器状态), 即 0x9ff00 - 12 = 0x9fef4
        #   由于段基址 CS=0x9000, 偏移量 0xfef4 对应物理地址 (0x9000<<4 + 0xfef4) = 0x9fef4, 接近 0x9ffff
        #
        # 猜测当年 Linus 可能规划的是, 让 bootsect 工作在 0x90000 ~ 0x9ff00 之间
        # 然后他把 0x9fef4 ~ 0x9ff00 这 12 个字节, 用来存放磁盘参数表
        #        0x9fef4 之下的空间当做栈

        mov %ax, %ds
        mov %ax, %es
        push %ax

        mov %ax, %ss # put stack at 0x9ff00 - 12.
        mov %dx, %sp

/*
 * Many BIOS's default disk parameter tables will not
 * recognize multi-sector reads beyond the maximum sector number
 * specified in the default diskette parameter tables - this may
 * mean 7 sectors in some cases.
 *
 *  许多 BIOS 默认磁盘参数表不支持超出最大扇区数的多扇区读取(某些情况下仅支持 7 扇区)
 *
 * Since single sector reads are slow and out of the question,
 * we must take care of this by creating new parameter tables
 * (for the first disk) in RAM.  We will set the maximum sector
 * count to 18 - the most we will encounter on an HD 1.44.
 *
 *  单扇区读取效率低下, 因此需在 RAM 中创建新参数表(针对第一块磁盘),
 *  将最大扇区数设为 18(适用于 1.44MB 软盘)
 *
 * High doesn't hurt.  Low does.
 *
 *  过高无妨, 过低则有害
 *
 * Segments are as follows: ds=es=ss=cs - INITSEG,
 *  fs = 0, gs = parameter table segment
 */

        ### -------- 拷贝磁盘参数表
        push $0
        pop %fs # FS=0
        mov $0x78, %bx # fs:bx is parameter table address

        # seg 并非独立指令, 而是​​段超越前缀​​(Segment Override Prefix),
        #     它强制指定下一条指令使用的段寄存器, 覆盖默认的段寄存器设置
        # lgs 用于​​加载远指针(48 位地址)到 GS 段寄存器和指定的通用寄存器
        # // seg %fs
        # // lgs si,(bx)   ! gs:si is source
        lgs %fs:(%bx), %si # 从内存(0:0x78位置)加载 段和偏移量
                           # 执行完成后 GS:SI 指向的就是这个参数表指向的位置

        # DX 之前被设置到内存位置稍高的区域了(0xfef4)
        # 拷贝 6*2 = 12 字节
        mov %dx, %di
        mov $6, %cx
        cld
        rep movsw %gs:(%si), %es:(%di)

        # 把磁盘参数表里面的最大扇区, 调整成 18
        # 注意这里 ES = DS = CS
        mov %dx, %di
        movb $18, 4(%di) # patch sector count

        # BX=0x78 --- 让中断向量 0x1E 的值指向新表
        # 0x1E 这个中断向量表位置里面放的不是中断处理函数, 而是指向软盘模式表（包含关于软驱的大量信息）的指针
        mov %di, %fs:(%bx)  # 低 2 byte -- 段内偏移
        mov %es, %fs:2(%bx) # 高 2 byte -- 段基址

        ### -------- 拷贝磁盘参数表(结束)

        pop %ax # AX=CS
        mov %ax, %fs
        mov %ax, %gs

        # BIOS INT 13h AH=00h: Reset Disk System
        # ------- INPUT:
        #   AH    00h
        #   DL    Drive (bit 7 set means reset both hard and floppy disks)
        # ------- OUTPUT:
        #   CF    Set on error
        #   AH    Return Code
        xor %ah, %ah # reset FDC ~ Floppy Disk Controller
        xor %dl, %dl # 1st floppy disk
        int $0x13

    # load the setup-sectors directly after the bootblock.
    # Note that 'es' is already set up.

    # 注意扇区是从 1 开始计数
    # 加载 setup 程序, 这个程序放在 0 磁盘 0 柱面 0 磁头 2 扇区
    # 目标内存位置是 ES:0x0200 开始, 这个位置紧挨着 MBR 在内存里面的位置

    # BIOS INT 13h AH=02h: Read Sectors From Drive
    # ------- INPUT:
    #     AH      02h
    #     AL      Sectors To Read Count
    #     CH      Cylinder
    #     CL      Sector
    #     DH      Head
    #     DL      Drive
    #     ES:BX   Buffer Address Pointer
    # ------- OUTPUT:
    #     CF      Set On Error, Clear If No Error
    #     AH      Return Code
    #     AL      Actual Sectors Read Count
    load_setup:
        xor %dx, %dx # drive 0, head 0
        mov $0x0002, %cx # sector 2, track 0
        mov $0x0200, %bx # address = 512, in INITSEG
        mov $(0x0200+SETUPLEN), %ax # service 2, nr of sectors
        int $0x13 # read it
        jnc ok_load_setup # ok - continue

        # 读取出错的情况, 打印换行然后打印错误码
        push %ax   # dump error code
        call print_nl
        mov %sp, %bp
        call print_hex # HEX 形式打印 SS:BP 地址的字, 这里实际上就是打印栈顶
        pop %ax

        # 重置磁盘
        xor %ah, %ah # reset FDC
        xor %dl, %dl
        int $0x13

        # 重新读
        jmp load_setup

    ok_load_setup:
        # Get disk drive parameters, specifically nr of sectors/track

        # BIOS INT 13h AH=08h: Read Drive Parameters
        # ------- INPUT:
        #     AH     08h = function number for read_drive_parameters
        #     DL     drive index (e.g. 1st HDD = 80h)
        #     ES:DI  set to 0000h:0000h to work around some buggy BIOS
        # ------- OUTPUT:
        #     CF     Set On Error, Clear If No Error
        #     AH     Return Code
        #     DL     number of hard disk drives
        #     DH     logical last index of heads = number_of - 1 (because index starts with 0)
        #     CX     [7:6][15:8] logical last index of cylinders = number_of - 1 (because index starts with 0)
        #            [5:0] logical last index of sectors per track = number_of (because index starts with 1)
        #     BL     drive type (only AT/PS2 floppies)
        #     ES:DI  pointer to drive parameter table (only for floppies)
        # ------- Remarks:
        #  Logical values of function 08h may/should differ from physical CHS values of function 48h.
        #  Result register CX contains both cylinders and sector/track values, see remark of function 02h.
        xor %dl, %dl
        mov $0x08, %ah # AH=8 is get drive parameters
        int $0x13

        # 下句保存每磁道扇区数, 对于软盘来说(dl=0), 其最大磁道号不会超过 256
        # ch 已经足够表示它, 因此 cl 的位 6-7 肯定为 0. 只需要设置ch=0, 此时 cx 中就是每磁道扇区数
        xor %ch, %ch
        mov %cx, %cs:sectors
        mov $INITSEG, %ax
        mov %ax, %es

        # Print some inane message
        # BIOS INT 10h AH=03h: Get cursor position and shape
        # -------- INPUT
        #     BH    Page Number
        # -------- OUTPUT
        #     AX    0
        #     CH    Start scan line
        #     CL    End scan line
        #     DH    Row
        #     DL    Column
        mov $0x03, %ah # read cursor pos
        xor %bh, %bh
        int $0x10

        # 注意这里已经有 ES = CS = INITSEG
        # BIOS INT 10h AH=13h: Write string (EGA+, meaning PC AT minimum)
        # -------- INPUT
        #     AL       Write mode
        #     BH       Page Number
        #     BL       Color
        #     CX       Number of characters in string
        #     DH       Row
        #     DL       Column
        #     ES:BP    Offset of string
        mov $9, %cx
        mov $0x0007, %bx # page 0, attribute 7 (normal)
        mov $msg1, %bp # 字符串首字符地址入 BP
        mov $0x1301, %ax # write string, move cursor
        int $0x10

        ###### 下面开始把系统映像加载进内存里面
        #      系统映像也在当前 BIOS 启动之后的这块磁盘上

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

        # 设备号 = 主设备号*256+次设备号(也即 dev_no = (major << 8) + minor)
        #
        # 次设备号=type*4+nr
        # 其中:
        #     nr 为 0-3 分别对应软驱 A/B/C/D
        #     type 是软驱的类型(2 表示 1.2MB 或 7 表示 1.44MB等)
        #
        # ------------------------------
        #
        # 在 Linux 中软驱的主设备号是 2 (参见第 43 行的注释)
        #
        # 因为 7*4+0=28, 所以 /dev/PS0 (2, 28) 指的是 1.44MB A 驱动器, 其设备号是 0x021c
        # 同理 2*4+0=8, 所以 /dev/at0 (2, 8) 指的是 1.2MB A 驱动器, 其设备号是 0x0208
        #
        # 下面 root_dev 定义在引导扇区 508, 509 字节处, 指根文件系系统所在设备号
        #
        # 0x0306 指第 2 个硬盘第 1 个分区, 这里默认为 0x0306 是因为当时 Linus 开发 Linux 系统时
        # 是在第 2 个硬盘第 1 个分区中存放根文件系统
        #
        # 这个值需要根据你自己根文件系统所在硬盘和分区进行修改. 例如, 如果你的根文件系统在第 1
        # 个硬盘的第 1 个分区上, 那么该值应该为0x0301, 即 (0x01, 0x03).
        #
        # 如果根文件系统是在第 2 个 Bochs 软盘上, 那么该值应该为 0x021D, 即 (0x1D, 0x02)
        #
        # 当编译内核时, 你可以在 Makefile 文件中另行指定你自己的值, 内核映像文件 Image 的创建
        # 程序 tools/build 会使用你指定的值来设置你的根文件系统所在设备号
        #
        # 决定启动设备是什么
        # 如果已经指定就不再重复计算
        # 如果未指定, 根据磁盘的一个磁道扇区数量来决定设备类型:
        #   1.2MB 软盘
        #   1.44MB 软盘
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
        jmp undef_root # 找不到启动设备, 罢工死循环
    root_defined:
        mov %ax, %cs:root_dev

        # after that (everyting loaded), we jump to
        # the setup-routine loaded directly after
        # the bootblock:

        # 跳转到 setup 程序执行
        ljmp $SETUPSEG, $0

# This routine loads the system at address 0x10000, making sure
# no 64kB boundaries are crossed. We try to load it as fast as
# possible, loading whole tracks whenever we can.
#
# in: es - starting address segment (normally 0x1000)
#
# 这个函数从磁盘读数据
# 系统的大小是 SYSSIZE 字节, 这个函数从磁盘上读这么多, 到 %es 指定的内存段
read_it:
        # 缓冲区段必须对齐 64Kb, 据说是以后要用
        mov %es, %ax
        test $0xfff, %ax
    die:
        jne die # es must be at 64kB boundary

        xor %bx, %bx # bx is starting address within segment
    rp_read:
        mov %es, %ax
        cmp $ENDSEG, %ax # have we loaded all yet?
        jb ok1_read # if ax < ENDSEG
        ret

        # 下面开始正式读取系统映像数据
    ok1_read:
        mov %cs:sectors, %ax # 每个磁道扇区数, 上面费劲算好的
        sub sread, %ax # sread 是当前磁道已经读取的数量
        mov %ax, %cx # cx = 当前未读扇区数量
        shl $9, %cx # 乘上 512, 这是未读取的字节数量
        add %bx, %cx # cx 现在是本次读取完成之后, 段内共计字节数量
                     # bx 本身在这里是当做缓冲区游标使用的, 它指示的事现在段内没有被使用内存的开始位置

        # 如果没有超过, 或者刚好要读取 64K, 就正常读
        # 否则, 空间放不下, 就算算还能读多少, 都读进来

        # JNC, JE 是 %cx 的状态有没有进位, cx 是 16 bits 的, 也就是最多读 64Kb
        jnc ok2_read
        je ok2_read

        # 若加上此次读, 磁道上所有未读扇区时会超过 64Kb, 则计算算此时最多能读入的字节数:
        # 使用 (64Kb - 段内读偏移位置), 再转换成需读取的扇区数, 其中 0 减某数就是取该数 64Kb 的补值
        # AX = 0 - BX == 0x10000 - BX, 这就是段内还剩余的空间容量
        # 这里太妙了, 假如 BX = 0001B, 寄存器只有 16bits, 那么(丢掉不存在的第 16bit):
        #   0x0_0000-BX = 1111B
        #   0x1_0000-BX = 1111B
        # 可见他们在内存中的表示是一样的, 我们只要强行将 -BX 解释称正数就行了
        # 这里 0x10000 就刚好是 64Kb
        xor %ax, %ax
        sub %bx, %ax
        shr $9, %ax # 转化为可读取的扇区数量

        # 读当前磁道上指定开始扇区(cl)和需读扇区数(al)的数据到 es:bx 开始处, 然后统计当前磁道
        # 上已经读取的扇区数并与磁道最大扇区数sectors作比较, 如果小于sectors说明当前磁道上的还
        # 有扇区未读, 于是跳转到 ok3_read 处继续操作
    ok2_read:
        call read_track # 读取一个磁道, ax 是期望读取的扇区数量
        mov %ax, %cx # ax 是读取的扇区数量
        add sread, %ax # ax 现在是历次读取扇区总数量
        cmp %cs:sectors, %ax # 比较一圈磁道扇区数量和已经读到的扇区数量
        jne ok3_read # 如果还没有读够一圈磁道, 就到 ok3_read 继续读
        mov $1, %ax # 若该磁道的当前磁头面所有扇区已经读取, 则读该磁道的下一磁头面(1号磁头)上的数据
        sub head, %ax # 判断当前磁头号
        jne ok4_read # 如果是 0 磁头, 则再去读 1 磁头面上的扇区数据
        incw track # 否则去读下一磁道
    ok4_read:
        mov %ax, head # 记录当前正在读的磁头
        xor %ax, %ax # 换了盘面之后, 读取到的扇区数量应该清零
    ok3_read:
        # 如果当前磁道上的还有未读扇区, 则首先保存当前磁道已读扇区数, 然后调整存放数据处的开始
        # 位置, 若小于 64Kb 边界值, 则跳转到 rp_read 处继续读数据
        # ax 表示当前磁道已读扇区数
        # cx 是上次已读扇区数
        mov %ax, sread
        shl $9, %cx # 上次已读扇区占用的空间大小
        add %cx, %bx # 调整当前段内空闲区域数据游标位置
        jnc rp_read # 小于 64Kb, 继续读取
        mov %es, %ax # 调整 ES 到新段, 往这个新的段里面读数据
        add $0x10, %ah # es 增大 0x1000, 作为段, 实际上是增加了 0x10000, 也就是 64Kb
        mov %ax, %es
        xor %bx, %bx
        jmp rp_read

read_track:
        pusha # ax, cx, dx, bx, sp, bp, si, di 入栈
        pusha

        # 在屏幕上打印 `loading...` 里面的 `.` 号, message 2e = .
        # 搜索 `Teletype output`, 已经有关于这个调用功能的解释
        mov $0x0e2e, %ax
        mov $0x0007, %bx
        int $0x10

        popa

        mov track, %dx # 当前磁道号, 磁道号不会超过 255
        mov sread, %cx # 当前已经读取的扇区数量
        inc %cx # (cl=Sector) 要读的是接下来的扇区, 因此加一
        mov %dl, %ch # (ch=Cylinder), CX, DX 都不会超过 255
        mov head, %dx
        mov %dl, %dh # (dh=Head)
        and $0x0100, %dx # (dl=Drive) 磁头号不会大于 1, 因此只留第 4bit
        mov $2, %ah # 0x13 中断的 2 号功能, 搜索 `Read Sectors From Drive` 看注解

        push %dx # save for error dump
        push %cx
        push %bx
        push %ax

        # AL = Sectors To Read Count
        # AL 是调用者算出来的, 调用者尝试每次都读取一个磁道全部的剩余扇区数量

        int $0x13 # ES:BX Buffer Address Pointer
        jc bad_rt

        add $8, %sp # 恢复栈

        popa
        ret

    bad_rt:
        push %ax # save error code
        call print_all # ah = error, al = read sectors

        xor %ah, %ah # reset FDC
        xor %dl, %dl
        int $0x13

        add $10, %sp
        popa

        jmp read_track

/*
 * print_all is for debugging purposes.
 * It will print out all of the registers.  The assumption is that this is
 * called from a routine, with a stack frame like
 * dx
 * cx
 * bx
 * ax
 * error
 * ret <- sp
 */
print_all:
        mov $5, %cx # error code + 4 registers
        mov %sp, %bp # 栈被破坏了???
        # 主要是外面有 pusha 保护, 因此这里不需要特别保护 bp
        # 而且 bootsect 这里的调用关系比较简单, 没有系统的维护函数栈帧

    print_loop:
        push %cx # save count left
        call print_nl # nl for readability
        jae no_reg # see if register name is needed

        # jae 指令依赖 CF, 如果 CF=0 (相当于 OP1 - OP2 > 0 没有进位)就会执行跳转
        # 因为 print_all 是在读取磁盘之后用的, 磁盘读取出错就会设置 CF

        # 打印出来的是 @/A/B/C/D 这 5 个字符
        # cl = 5, 4, 3, 2, 1
        # --> al = 5 + 'A' - 1 - cl = A/B/C/D
        mov $(0x0e05 + 0x41 - 1), %ax # ah=0x0e, al=(5 + 'A' - 1)
        sub %cl, %al
        int $0x10

        mov $0x58, %al # X
        int $0x10

        mov $0x3a, %al #  :
        int $0x10

    no_reg:
        add $2, %bp # next register
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
        mov $0xe0d, %ax # CR
        int $0x10
        mov $0xa, %al # LF
        int $0x10
        ret

/*
 * print_hex is for debugging purposes, and prints the word
 * pointed to by ss:bp in hexadecmial.
 */
print_hex:
        mov $4, %cx # 4 hex digits
        mov (%bp), %dx # load word into dx, `bp` 默认就是配合 SS 使用
    print_digit:
        # rol 把高 4 位转到低 4 位, 然后打印
        rol $4, %dx # rotate so that lowest 4 bits are used
        mov $0x0e, %ah
        mov %dl, %al #  mask off so we have only next nibble
        and $0x0f, %al
        add $0x30, %al # convert to 0 based digit, '0'
        cmp $0x39, %al # check for overflow
        jbe good_digit # JMP below equal, 上面处理的是数字, 下面处理的是字母
        add $(0x41 - 0x30 - 0xa), %al # 'A' - '0' - 0xa

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
        xor %al, %al # A驱动器, 关闭FDC, 禁止DMA和中断请求, 关闭马达
        outb %al, (%dx)
        pop %dx
        ret

sread: .word 1+SETUPLEN # sectors read of current track - 当前已经读取的扇区数量
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
    .word SWAP_DEV # 0x1FA
root_dev:
    .word ROOT_DEV # 0x1FC
boot_flag:
    .word 0xAA55

.text
endtext:

.data
enddata:

.bss
endbss:
