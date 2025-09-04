.PHONY: rewrite

aa = setup0.S
ab = setup0.x.s
ac = setup0.o
ad = setup0.bin

ba = setup1.S
bb = setup1.x.s
bc = setup1.o
bd = setup1.bin

AS86 = as86 -0 -a
LD86 = ld86 -0

AS = as --32
LD = ld -m elf_i386
LDFLAGS	= -s -x -M
CC = gcc $(RAMDISK)
CFLAGS = -fno-builtin  -Wall -fno-stack-protector -m32 -g -fno-pie -fstrength-reduce -fomit-frame-pointer
CPP	= cpp -nostdinc -I./../include

rewrite:
	$(CPP) -traditional $(aa) -o $(ab)
	$(AS86) -o $(ac) $(ab)
	$(LD86) -s -d -o $(ad) $(ac)

	# as -o $(ac) $(ab)
	# ld -e start -z noexecstack --oformat=binary --Ttext=0 -o $(ad) $(ac)

	$(CPP) -traditional $(ba) -o $(bb)
	as -o $(bc) $(bb)
	ld -e start -z noexecstack --oformat=binary --Ttext=0 -o $(bd) $(bc)

	objdump -D -b binary -mi386 -Maddr16,data16 $(ad) | \
		sed 's/^[^:]*://g' > a

	objdump -D -b binary -mi386 -Maddr16,data16 $(bd) | \
		sed 's/^[^:]*://g' > b

	diff --color -u a b