file tools/system.elf
target remote localhost:1234
set listsize 30
set disassembly-flavor att
set disassemble-next-line on
#layout regs

b recal_interrupt

# b floppy_interrupt
# b do_fd_request
# b output_byte
b result
# b bad_flp_intr
# b recal_interrupt
# b unexpected_floppy_interrupt
