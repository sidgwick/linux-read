
HEAD = head.o
SYSTEM = ../tools/zSystem
#LD = gcc
#TEST = -DTEST_DRIVER

AS	=as -g --32
LD	=ld -m elf_i386 -z noexecstack


zOBJECTS = $(HEAD) inflate.o unzip.o misc.o

CFLAGS = -O2 -DSTDC_HEADERS $(TEST) -Wall -Wstrict-prototypes -fomit-frame-pointer -pipe
CFLAGS += -m32 -g -nostdinc -fno-builtin -fno-stack-protector -fno-pie
CFLAGS += -I/app/linux-read/include -I/app/xroot/usr/include/ -I/app/xroot/usr/lib/gcc-lib/i486-linux/2.7.2/include/

.c.s:
	$(CC) $(CFLAGS) -S -o $*.s $<
.s.o:
	$(AS) -c -o $*.o $<
.c.o:
	$(CC) $(CFLAGS) -c -o $*.o $<

all:	zSystem

zSystem:	piggy.o $(zOBJECTS)
		$(LD) $(LDFLAGS) -o zSystem -Ttext 1000 -e startup_32 $(zOBJECTS) piggy.o

head.o:	head.s

head.s: head.S ../include/linux/tasks.h
	$(CC) $(CFLAGS) -E -traditional head.S -o head.s

piggy.o:	$(SYSTEM) xtract piggyback
	./xtract $(SYSTEM) | gzip -9 | ./piggyback > piggy.o

xtract: xtract.c
	$(CC) -I/app/linux-read/include -o $@ $<

piggyback: piggyback.c
	$(CC) -I/app/linux-read/include -o $@ $<

$(SYSTEM):
		$(MAKE) -C .. tools/zSystem
