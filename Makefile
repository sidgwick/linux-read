#
# if you want the ram-disk device, define this to be the
# size in blocks.
#

RAMDISK = #-DRAMDISK=512

AS86	=as86 -0 -a
LD86	=ld86 -0

# AS = as --32
# LD = ld -m elf_i386

AS	=as -g --32
LD	=ld -m elf_i386
LDFLAGS = -M -x -Ttext 0 -e startup_32 -z noexecstack
CC	=gcc $(RAMDISK)
CFLAGS	= -fno-builtin -Wall -fno-stack-protector -m32 -g -fno-pie -fstrength-reduce -fomit-frame-pointer
CPP	=cpp -nostdinc -Iinclude

#
# ROOT_DEV specifies the default root-device when making the image.
# This can be either FLOPPY, /dev/xxxx or empty, in which case the
# default of /dev/hd6 is used by 'build'.
#
ROOT_DEV=FLOPPY
SWAP_DEV=

OBJS = boot/head.o init/main.o \
       lib/_exit.o lib/malloc.o lib/setsid.o \
       lib/wait.o lib/open.o lib/close.o \
       lib/write.o lib/dup.o lib/ctype.o \
	   lib/execve.o lib/errno.o lib/string.o \
       mm/page.o mm/swap.o mm/memory.o \
       fs/ioctl.o fs/inode.o fs/truncate.o fs/read_write.o \
       fs/block_dev.o fs/namei.o fs/fcntl.o fs/stat.o \
       fs/open.o fs/char_dev.o fs/buffer.o fs/file_table.o \
       fs/super.o fs/pipe.o fs/exec.o fs/bitmap.o \
       fs/file_dev.o fs/select.o \
       kernel/signal.o kernel/asm.o kernel/fork.o kernel/mktime.o \
	   kernel/sched.o kernel/sys.o kernel/printk.o kernel/exit.o \
	   kernel/panic.o kernel/sys_call.o kernel/traps.o kernel/vsprintf.o \
       kernel/math/error.o kernel/math/ea.o kernel/math/add.o \
       kernel/math/mul.o kernel/math/compare.o kernel/math/get_put.o \
       kernel/math/math_emulate.o kernel/math/convert.o kernel/math/div.o \
       kernel/chr_drv/keyboard.o kernel/chr_drv/rs_io.o kernel/chr_drv/console.o \
	   kernel/chr_drv/pty.o kernel/chr_drv/tty_io.o kernel/chr_drv/serial.o \
	   kernel/chr_drv/tty_ioctl.o \
       kernel/blk_drv/ll_rw_blk.o kernel/blk_drv/floppy.o \
       kernel/blk_drv/hd.o kernel/blk_drv/ramdisk.o

.c.s:
	$(CC) $(CFLAGS) -nostdinc -Iinclude -S -o $*.s $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -nostdinc -Iinclude -c -o $*.o $<

all: $(SUBDIRS) Image

Image: boot/bootsect boot/setup tools/system tools/build
	tools/build boot/bootsect boot/setup tools/system > Image

tools/build: tools/build.c
	$(CC) -Wall -o tools/build tools/build.c

boot/head.o: boot/head.s

tools/system: $(OBJS)
	$(LD) -M -x --Ttext=0 -e startup_32 -z noexecstack $(OBJS) -o tools/system.elf > System.map
	objcopy -R .pdr -R .comment -R .note -S -O binary tools/system.elf tools/system

# --------- OK ---------
boot/setup: boot/setup.s
	$(AS) -o boot/setup.o boot/setup.s
	$(LD) -e start --Ttext=0 -s -o boot/setup.elf boot/setup.o
	objcopy -R .pdr -R .comment -R .note -S -O binary boot/setup.elf boot/setup

boot/bootsect:	boot/bootsect.s
	$(AS) -o boot/bootsect.o boot/bootsect.s
	$(LD) -e start -z noexecstack --Ttext=0 -o boot/bootsect.elf boot/bootsect.o
	objcopy -R .pdr -R .comment -R .note -S -O binary boot/bootsect.elf boot/bootsect

boot/setup.s:	boot/setup.S include/linux/config.h
	$(CPP) -traditional boot/setup.S -o boot/setup.s

boot/bootsect.s:	boot/bootsect.S include/linux/config.h
	$(CPP) -traditional boot/bootsect.S -o boot/bootsect.s

kernel/chr_drv/keyboard.s: kernel/chr_drv/keyboard.S
	$(CPP) -traditional $^ -o kernel/chr_drv/keyboard.s

clean:
	rm -f Image System.map tmp_make core boot/bootsect boot/setup boot/bootsect.elf boot/setup.elf \
		boot/bootsect.s boot/setup.s boot/bootsect0.s boot/bootsect1.s
	rm -f tools/system tools/system.elf tools/build $(OBJS)
	rm -f kernel/chr_drv/keyboard.s
