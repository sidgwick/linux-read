file tools/system.elf
target remote localhost:1234
set listsize 30
set disassembly-flavor att
set disassemble-next-line on
#layout regs

break page_fault
ignore 1 100000

b main.c:258
