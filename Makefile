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

ARCHIVES=kernel/kernel.o mm/mm.o fs/fs.o
DRIVERS =kernel/blk_drv/blk_drv.o kernel/chr_drv/chr_drv.o
MATH	=kernel/math/math.o
LIBS	=lib/lib.o

.c.s:
	$(CC) $(CFLAGS) \
	-nostdinc -Iinclude -S -o $*.s $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) \
	-nostdinc -Iinclude -c -o $*.o $<

all:	Image

Image: boot/bootsect boot/setup tools/system tools/build
	tools/build boot/bootsect boot/setup tools/system $(ROOT_DEV) \
		$(SWAP_DEV) > Image
	sync

disk: Image
	dd bs=8192 if=Image of=/dev/PS0

tools/build: tools/build.c
	$(CC) -Wall -o tools/build tools/build.c

boot/head.o: boot/head.s

tools/system:	boot/head.o init/main.o \
				$(ARCHIVES) $(DRIVERS) $(MATH) $(LIBS)
	# $(LD) $(LDFLAGS) boot/head.o init/main.o \
	# 	$(ARCHIVES) \
	# 	$(DRIVERS) \
	# 	$(MATH) \
	# 	$(LIBS) \
	# 	--oformat=binary --Ttext=0 -o tools/system > System.map

	$(LD) $(LDFLAGS) boot/head.o init/main.o \
		$(ARCHIVES) \
		$(DRIVERS) \
		$(MATH) \
		$(LIBS) \
		-o tools/system.elf > System.map

	objcopy -R .pdr -R .comment -R .note -S -O binary tools/system.elf tools/system
	# objcopy -O binary tools/system kernel.bin
	# mv kernel.bin tools/system

kernel/math/math.o:
	(cd kernel/math; make)

kernel/blk_drv/blk_drv.o:
	(cd kernel/blk_drv; make)

kernel/chr_drv/chr_drv.o:
	(cd kernel/chr_drv; make)

kernel/kernel.o:
	(cd kernel; make)

mm/mm.o:
	(cd mm; make)

fs/fs.o:
	(cd fs; make)

lib/lib.o:
	(cd lib; make)

# --------- OK ---------
boot/setup: boot/setup.s
	$(AS) -o boot/setup.o boot/setup.s
	$(LD) -e start --oformat=binary --Ttext=0 -s -o boot/setup boot/setup.o

boot/setup.s:	boot/setup.S include/linux/config.h
	$(CPP) -traditional boot/setup.S -o boot/setup.s

boot/bootsect.s:	boot/bootsect.S include/linux/config.h
	$(CPP) -traditional boot/bootsect.S -o boot/bootsect.s

boot/bootsect:	boot/bootsect.s
	$(AS) -o boot/bootsect.o boot/bootsect.s
	$(LD) -e start -z noexecstack --oformat=binary --Ttext=0 -o boot/bootsect boot/bootsect.o

clean:
	rm -f Image System.map tmp_make core boot/bootsect boot/setup \
		boot/bootsect.s boot/setup.s boot/bootsect0.s boot/bootsect1.s
	rm -f init/*.o tools/system tools/system.elf tools/build boot/*.o boot/*.bin
	(cd mm;make clean)
	(cd fs;make clean)
	(cd kernel;make clean)
	(cd lib;make clean)

backup: clean
	(cd .. ; tar cf - linux | compress - > backup.Z)
	sync

dep:
	sed '/\#\#\# Dependencies/q' < Makefile > tmp_make
	(for i in init/*.c;do echo -n "init/";$(CPP) -M $$i;done) >> tmp_make
	cp tmp_make Makefile
	(cd fs; make dep)
	(cd kernel; make dep)
	(cd mm; make dep)

### Dependencies:
init/main.o : init/main.c include/unistd.h include/sys/stat.h \
  include/sys/types.h include/sys/time.h include/time.h include/sys/times.h \
  include/sys/utsname.h include/sys/param.h include/sys/resource.h \
  include/utime.h include/linux/tty.h include/termios.h include/linux/sched.h \
  include/linux/head.h include/linux/fs.h include/linux/mm.h \
  include/linux/kernel.h include/signal.h include/asm/system.h \
  include/asm/io.h include/stddef.h include/stdarg.h include/fcntl.h \
  include/string.h 
