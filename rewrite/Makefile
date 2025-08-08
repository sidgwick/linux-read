#
# if you want the ram-disk device, define this to be the
# size in blocks.
#
AS = as --32
LD = ld -m elf_i386
CPP	=cpp -nostdinc -I.


all:
	$(CPP) -traditional bootsect.S -o bootsect.s
	$(AS) -o bootsect.o bootsect.s
	$(LD) -e start -z noexecstack --oformat=binary --Ttext=0 -o bootsect.bin bootsect.o

	$(CPP) -traditional setup.S -o setup.s
	$(AS) -o setup.o setup.s
	$(LD) -e start -z noexecstack --oformat=binary --Ttext=0 -o setup.bin setup.o

	$(AS) -o head.o head.s
	$(LD) -e start -z noexecstack --oformat=binary --Ttext=0 -o head.bin head.o

	dd if=/dev/zero of=a.img bs=512 count=2880

	# bootsect 占用 1 扇区
	# setup 占用 4 扇区(代码里面指定的, 不管大小够不够, 都认为它是 4 个扇区)
	# head 占用从 5 扇区开始的地方
	dd if=bootsect.bin of=a.img bs=512 count=1 conv=notrunc
	dd if=setup.bin of=a.img seek=1 bs=512 conv=notrunc
	dd if=head.bin of=a.img seek=5 bs=512 conv=notrunc

	bochs -debugger -f bochsrc.txt

clean:
	rm -f a.img *.bin *.o bootsect.s setup.s