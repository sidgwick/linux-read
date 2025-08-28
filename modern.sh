#!/bin/bash

# 参考:
#   - https://www.isvee.com/archives/5411
#   - https://github.com/JackeyLea/Linux-0.12
#   - https://blog.jackeylea.com/linux/compile-linux-kernel-0.12/
#   - https://github.com/dibingfa/flash-linux0.11-talk

# 安装一些必要的软件包
#
# apt install gcc-multilib bin86
#
# ---- 安装较低版本的 GCC, 使用 3.4 版本
# apt-get install software-properties-common
# add-apt-repository ppa:ubuntu-toolchain-r/test
# apt-get update

find -name "Makefile" -exec sed -i "s/gas/as/g;
                                    s/gld/ld/g;
                                    s/gar/ar/g;
                                    s/as$/as --32/g;
                                    s/\bld$/ld -m elf_i386/g;
                                    s/-O/-g -m32/g;
                                    s/-Wall/-Wall -fno-stack-protector/g;
                                    s/-fcombine-regs//g;
                                    s/^CFLAGS\s\+=/& -fno-builtin /g;
                                    s/-mstring-insns//g;" {} \;


sed -i 's/^LDFLAGS\s\+=.*$/LDFLAGS = -M -x -Ttext 0 -e startup_32 -z noexecstack/g' Makefile
sed -i 's#ROOT_DEV=/dev/hd6#ROOT_DEV=FLOPPY#g' Makefile
sed -i 's#SWAP_DEV=/dev/hd2#SWAP_DEV=#g' Makefile

# sed '14s/-O //' -i kernel/chr_drv/Makefile

sed 's/align 2/align 4 # 2^2/g;
     s/align 3/align 8 # 2^3/g;
     15s/$/,startup_32/' -i boot/head.s

sed 's/static inline _syscall0/inline _syscall0/g;
     s/static int printf/int printf/g' -i init/main.c

# 删掉内联汇编里面可能被改动的寄存器列表
find -type f -exec sed -i 's/:\"\w\{2\}\"\(,\"\w\{2\}\"\)*)/:)/g' {} \;

sed  's/extern inline/static inline/g' -i include/asm/segment.h include/string.h include/linux/mm.h

sed 's/\b_\(\w\+\)/\1/g' -i boot/head.s kernel/sys_call.s kernel/asm.s kernel/chr_drv/rs_io.s kernel/chr_drv/keyboard.S mm/page.s

sed '161s/pag = (char \*) //;
     162s/pag = (char \*) //;
     164aelse { pag = (char *) page[p/PAGE_SIZE]; }' -i fs/exec.c

sed '156c cp = get_free_page(); bdesc->page = (void *)cp; bdesc->freeptr = (void *) cp;' -i lib/malloc.c

sed '33a#ifndef MAJOR \
	#define MAJOR(a) (((unsigned)(a))>>8) \
#endif \
#ifndef MINOR \
	#define MINOR(a) ((a)&0xff) \
#endif' -i tools/build.c

sed '192,195s#^#//#' -i tools/build.c


sed 's/void inline invalidate_buffers(int dev)/static void inline invalidate_buffers(int dev)/g' -i fs/buffer.c
sed 's/inline void setup_rw_floppy(void)/static inline void setup_rw_floppy(void)/g' -i kernel/blk_drv/floppy.c
sed 's/extern inline/static inline/g' -i kernel/blk_drv/blk.h

sed 's/movl _video_num_columns/movl video_num_columns/g' -i kernel/chr_drv/console.c

sed '229s/_last_task_used_math/last_task_used_math/' -i include/linux/sched.h

sed 's/ecx,_current/ecx,current/g' -i include/linux/sched.h
