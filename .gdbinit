file tools/system.elf
target remote localhost:1234
set disassembly-flavor att
set disassemble-next-line on
#layout regs
b main
