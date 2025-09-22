清理空格

```bash
find . -name "*.[chsS]" -exec sed 's/\s\+$//g' -i {} \;

cat 001.txt | sed '/^$/d' | sed -f abc.sed | pbcopy
```

```sed
s/）/) /g
s/（/ (/g
s/。/. /g
s/；/; /g
s/，/, /g
s/：/: /g
```

# compile bochs from source

```console
brew install --build-from-source --formula bochs.rb
```

## TODO

1. 下面这个函数, 是怎么返回 tm 结构的? 这里面不涉及到栈上内存释放/堆上内存泄漏吗?

```c
struct tm *gmtime(const time_t *tp);
```

---

# COMPILE

下面我们开始编译内核的工作,首先将从网上下载的内核放到 CentOS 6.3 的 /usr/src 目录下,然后执行下面的操作:

## 第 0 步, 解压

```
cd /usr/src
tar xzvf linux-1.0.tar.gz

cd /usr/include
mv linux linux-CentOS
mv asm asm-CentOS
ln -s /usr/src/linux/include/linux .
ln -s /usr/src/linux/include/asm .
```

## 第 1 步, 构建配置

```
make config
```

## 第 2 步, 构建依赖

```
make dep
```

```
[root@localhost linux]#
gcc -D__KERNEL__ -E -M tty_io.c console.c keyboard.c serial.c tty_ioctl.c pty.c vt.c mem.c defkeymap.c psaux.c mouse.c > .depend
serial.c:538:8: 错误：宏名必须是标识符

解决办法：
cd /usr/src/linux/drivers/char
vi serial.c
第538行,修改为这样
//#ifdef 0
#if 0

[root@localhost linux]# make dep
make[2]: 进入目录“/usr/src/linux/drivers/net”
gcc -D__KERNEL__ -E -I../../net/inet -M *.c > .depend
/usr/include/bits/socket.h:381:24: 致命错误：asm/socket.h：没有那个文件或目录
编译中断。

解决办法:
cd /usr/src/linux/include/asm
ls
可以看到此目录下没有socket.h文件

cd /usr/include/bits
cp socket.h socket.h.bak-fedor15
vi socket.h
第381行,注释掉
//#include <asm/socket.h>



[root@localhost linux]# make dep
make[1]: 进入目录“/usr/src/linux/fs”
gcc -D__KERNEL__ -E -M *.c > .depend
buffer.c:108:8: 错误：宏名必须是标识符

解决办法：
cd /usr/src/linux/fs
vi buffer.c
第108行,修改
//#ifdef 0 /* Disable bad-block debugging code */
#if 0 /* Disable bad-block debugging code */



[root@localhost linux]# make dep
......
gcc -D__KERNEL__ -E -M *.c > .depend
make[1]: 离开目录“/usr/src/linux/lib”
rm -f tools/version.h
mv .tmpdepend .depend

这步执行成功!



------------------------------------------------------------------------------------------------------------------
第三步
[root@localhost linux]# make zImage
gcc -D__KERNEL__ -E -traditional -DSVGA_MODE=NORMAL_VGA  boot/bootsect.S -o boot/bootsect.s
as86 -0 -a -o boot/bootsect.o boot/bootsect.s
make: as86：命令未找到
make: *** [boot/bootsect.o] 错误 127

解决办法:
yum install dev86* (请确定网络是通的)



[root@localhost linux]# make zImage
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -m486  -c -o init/main.o init/main.c
cc1: 错误：unrecognized command line option ‘-m486’
make: *** [init/main.o] 错误 1

解决办法:
cd /usr/src/linux
vi Makefile
注释掉
#ifdef CONFIG_M486
#CFLAGS := $(CFLAGS) -m486
#else
#CFLAGS := $(CFLAGS) -m386
#endif
或者修改为
ifdef CONFIG_M486
CFLAGS := $(CFLAGS) -march=i486
else
CFLAGS := $(CFLAGS) -march=i386
endif



[root@localhost linux]# make zImage
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe  -c -o init/main.o init/main.c
init/main.c: 在函数‘get_options’中:
/usr/include/linux/string.h:130:1: 错误：can’t find a register in class ‘SIREG’ while reloading ‘asm’
/usr/include/linux/string.h:130:1: 错误：‘asm’操作数中有不可能的约束
make: *** [init/main.o] 错误 1

解决办法:
vi /usr/include/linux/string.h
第130行的 strchr函数,修改为这样
extern inline char * strchr(const char * s,char c)
{
register char * __res __asm__("ax");
__asm__("cld\n\t"
        "movb %%al,%%ah\n"
        "1:\tlodsb\n\t"
        "cmpb %%ah,%%al\n\t"
        "je 2f\n\t"
        "testb %%al,%%al\n\t"
        "jne 1b\n\t"
        "movl $1,%1\n"
        "2:\tmovl %1,%0\n\t"
        "decl %0"
        :"=a" (__res):"S" (s),"0" (c));
        //:"=a" (__res):"S" (s),"0" (c):"si");
return __res;
}

这类错误是由于gcc的进化,导致现在版本的gcc已经不需要指定如最后一个:"si" 这样的寄存器了.

[root@localhost linux]# make zImage
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe  -c -o init/main.o init/main.c
init/main.c: 在函数‘checksetup’中:
/usr/include/linux/string.h:266:1: 错误：can’t find a register in class ‘DIREG’ while reloading ‘asm’
/usr/include/linux/string.h:109:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/linux/string.h:266:1: 错误：‘asm’操作数中有不可能的约束
/usr/include/linux/string.h:109:1: 错误：‘asm’操作数中有不可能的约束
make: *** [init/main.o] 错误 1

解决办法:
vi /usr/include/linux/string.h
第109行的 strncmp; 第266行的strlen



[root@localhost linux]# make zImage
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe  -c -o init/main.o init/main.c
init/main.c:357:2: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
/usr/include/linux/string.h:382:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/linux/string.h:90:1: 错误：can’t find a register in class ‘SIREG’ while reloading ‘asm’
/usr/include/linux/delay.h:14:2: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
init/main.c:238:4: 错误：can’t find a register in class ‘DREG’ while reloading ‘asm’
init/main.c:357:2: 错误：‘asm’操作数中有不可能的约束

解决办法:
vi /usr/include/linux/string.h
第90行的 strcmp,修改; 第382行的 memcmp,修改
vi /usr/include/linux/delay.h
第14行的 memcmp,修改



init/main.c:357:2: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
init/main.c:238:4: 错误：can’t find a register in class ‘DREG’ while reloading ‘asm’
解决办法:
vi /usr/src/linux/init/main.c
第238行,修改
vi /usr/src/linux/include/asm/system.h
第55行的_set_gate,修改



[root@localhost linux]# make zImage
make[1]: 进入目录“/usr/src/linux/kernel”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe  -fno-omit-frame-pointer -c sched.c
In file included from sched.c:35:0:
/usr/include/linux/timex.h:120:32: 错误：‘xtime’的类型限定冲突
/usr/include/linux/sched.h:308:23: 附注：‘xtime’的上一个声明在此
sched.c:41:25: 错误：‘xtime’的类型限定冲突
/usr/include/linux/sched.h:308:23: 附注：‘xtime’的上一个声明在此

gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe  -fno-omit-frame-pointer -c sched.c
In file included from sched.c:35:0:
/usr/include/linux/timex.h:120:32: error: conflicting type qualifiers for 'xtime'
/usr/include/linux/sched.h:308:23: note: previous declaration of 'xtime' was here
sched.c:41:25: error: conflicting type qualifiers for 'xtime'
/usr/include/linux/sched.h:308:23: note: previous declaration of 'xtime' was here

解决办法:
vi /usr/include/linux/sched.h
第308行的_set_gate,修改为这样
//extern struct timeval xtime;
extern volatile struct timeval xtime;
保持和/usr/include/linux/timex.h中的xtime声明一致



[root@localhost linux]# make zImage
make[1]: 进入目录“/usr/src/linux/kernel”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe  -fno-omit-frame-pointer -c sched.c
sched.c: 在函数‘schedule’中:
sched.c:285:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
sched.c:285:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/sched.h
第357行的switch_to,修改



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c sys.c
sys.c: 在函数‘save_v86_state’中:
/usr/include/asm/segment.h:108:4: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/asm/segment.h:108:4: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/include/asm/segment.h
第90行的COMMON,修改



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c sys.c
sys.c: 在函数‘sys_vm86’中:
/usr/include/asm/segment.h:169:4: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/asm/segment.h:169:4: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/include/asm/segment.h
第158行的COMMON,修改



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c sys.c
sys.c: 在函数‘getrusage’中:
/usr/include/linux/string.h:422:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/linux/string.h:422:1: 错误：‘asm’操作数中有不可能的约束

解决办法:
vi /usr/include/linux/string.h
第422行的memset



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c module.c
module.c: 在函数‘sys_get_kernel_syms’中:
/usr/include/linux/string.h:36:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/asm/segment.h:109:4: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第36行的strncpy



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c module.c
module.c: 在函数‘sys_init_module’中:
/usr/include/asm/segment.h:126:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
解决办法:
vi /usr/src/linux/include/asm/segment.h
第126行的__generic_memcpy_fromfs



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c ldt.c
ldt.c: 在函数‘sys_modify_ldt’中:
/usr/include/asm/segment.h:57:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/asm/segment.h:57:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/include/asm/segment.h
第57行的__generic_memcpy_tofs



make[1]: 进入目录“/usr/src/linux/kernel”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c time.c
time.c:33:23: 错误：‘xtime’的类型限定冲突
/usr/include/linux/timex.h:120:32: 附注：‘xtime’的上一个声明在此
/usr/include/linux/timex.h:120:32: error: conflicting type qualifiers for 'xtime'
/usr/include/linux/sched.h:308:23: note: previous declaration of 'xtime' was here
sched.c:41:25: error: conflicting type qualifiers for 'xtime'
解决办法:
vi /usr/src/linux/kernel/time.c
第33行,修改为这样
//extern struct timeval xtime;
extern volatile struct timeval xtime;
保持和/usr/include/linux/timex.h中的xtime声明一致



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c floppy.c
floppy.c: 在函数‘setup_DMA’中:
floppy.c:440:16: 警告：variable ‘dma_code’ set but not used [-Wunused-but-set-variable]
floppy.c:460:4: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
floppy.c:460:4: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/drivers/block/floppy.c
第431行的copy_buffer


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c tty_io.c
tty_io.c: 在函数‘tty_open’中:
/usr/include/linux/mm.h:98:3: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/linux/mm.h:98:3: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
解决办法:
vi /usr/include/linux/mm.h
第98行的get_free_page


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c console.c
console.c:567:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
console.c:567:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/drivers/char/console.c
第567行


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c console.c
console.c: 在函数‘scrdown.part.0’中:
console.c:479:2: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
console.c:479:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/drivers/char/console.c
第479行scrdown


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c console.c
console.c: 在函数‘scrup.part.2’中:
console.c:460:3: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
解决办法:
vi /usr/src/linux/drivers/char/console.c
第433,449,460行的scrup


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c console.c
console.c: 在函数‘blank_screen’中:
console.c:1340:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/asm/io.h:82:387: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/drivers/char/console.c
第1340行memsetw



console.c: 在函数‘con_write’中:
console.c:603:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
console.c:603:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/drivers/char/console.c
第603行



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -c vt.c
vt.c: 在函数‘vt_ioctl’中:
/usr/include/linux/string.h:375:1: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/linux/string.h:271:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第375行



make[2]: 进入目录“/usr/src/linux/drivers/FPU-emu”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -DPARANOID  -fno-builtin  -c fpu_entry.c
In file included from fpu_entry.c:30:0:
fpu_proto.h:68:13: 附注：需要类型‘long unsigned int *’，但实参的类型为‘long int *’
fpu_entry.c:473:48: 错误：赋值运算的左操作数必须是左值
fpu_entry.c:473:48: error: lvalue required as left operand of assignment
解决办法:
vi /usr/src/linux/drivers/FPU-emu/fpu_system.h
第64行,修改为这样
//#define FPU_data_address        ((void *)(I387.soft.twd))
#define FPU_data_address        ((I387.soft.twd))
vi /usr/include/linux/sched.h
第375行的,修改为这样
struct i387_soft_struct {
        long    cwd;
        long    swd;
        //long  twd;
        void*   twd;
        long    fip;
        long    fcs;
        long    foo;
        long    fos;
        long    top;
        struct fpu_reg  regs[8];        /* 8*16 bytes for each FP-reg = 128 bytes */
        unsigned char   lookahead;
        struct info     *info;
        unsigned long   entry_eip;
};



fpu_trig.c: 在函数‘rem_kernel’中:
fpu_trig.c:748:3: 错误：缺少结尾的 " 字符
fpu_trig.c:749:18: 错误：expected string literal before ‘movl’
fpu_trig.c:750:18: 错误：缺少结尾的 " 字符
解决办法:
vi /usr/src/linux/drivers/FPU-emu/fpu_trig.c
第748行,
  /* Do the required multiplication and subtraction in the one operation */
  asm volatile ("movl %2,%%eax; mull %4; subl %%eax,%0; sbbl %%edx,%1;
                 movl %3,%%eax; mull %4; subl %%eax,%1;
                 movl %2,%%eax; mull %5; subl %%eax,%1;"
                :"=m" (x), "=m" (((unsigned *)&x)[1])
                :"m" (st1),"m" (((unsigned *)&st1)[1]),
                 "m" (q),"m" (((unsigned *)&q)[1])
                :"%ax","%dx");
  修改为这样
  asm volatile ("movl %2,%%eax; mull %4; subl %%eax,%0; sbbl %%edx,%1;"
                "movl %3,%%eax; mull %4; subl %%eax,%1;"
                "movl %2,%%eax; mull %5; subl %%eax,%1;"
                :"=m" (x), "=m" (((unsigned *)&x)[1])
                :"m" (st1),"m" (((unsigned *)&st1)[1]),
                 "m" (q),"m" (((unsigned *)&q)[1])
                :"%ax","%dx");


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c memory.c
memory.c: 在函数‘zeromap_page_range’中:
memory.c:967:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
memory.c:967:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/mm/memory.c
第967行的__zero_page



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c memory.c
memory.c: 在函数‘put_page’中:
memory.c:955:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
memory.c:955:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/mm/memory.c
第943行的__bad_pagetable; 第955行的__bad_page



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c memory.c
memory.c: 在函数‘do_wp_page’中:
memory.c:580:4: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
memory.c:580:4: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/mm/memory.c
第63行的copy_page,修改为这样
#define copy_page(from,to) \
__asm__("cld ; rep ; movsl": :"S" (from),"D" (to),"c" (1024))
//__asm__("cld ; rep ; movsl": :"S" (from),"D" (to),"c" (1024):"cx","di","si")



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c buffer.c
buffer.c: 在函数‘bread_page’中:
buffer.c:854:5: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
buffer.c:854:5: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/buffer.c
第819行的COPYBLK,修改为这样
#define COPYBLK(size,from,to) \
__asm__ __volatile__("rep ; movsl": \
 :"c" (((unsigned long) size) >> 2),"S" (from),"D" (to) \
 )
 //:"cx","di","si")



make[1]: 进入目录“/usr/src/linux/fs”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c exec.c
exec.c: 在函数‘copy_strings’中:
exec.c:380:57: 错误：赋值运算的左操作数必须是左值
解决办法:
vi /usr/src/linux/fs/exec.c
第380行,修改为这样
if (!(pag = (char *) page[p/PAGE_SIZE]) &&
    !(pag = (char *) (page[p/PAGE_SIZE] = (unsigned long *) get_free_page(GFP_USER))))
    //!(pag = (char *) page[p/PAGE_SIZE] = (unsigned long *) get_free_page(GFP_USER)))



make[1]: 进入目录“/usr/src/linux/fs”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c exec.c
exec.c: 在函数‘flush_old_exec’中:
exec.c:528:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
exec.c:528:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/types.h
第113行的__FD_ZERO,修改为这样
#define __FD_ZERO(fdsetp) \
  __asm__ __volatile__("cld ; rep ; stosl" \
   :"=m" (*(fd_set *) (fdsetp)) \
   :"a" (0), "c" (__FDSET_LONGS), \
   "D" ((fd_set *) (fdsetp)) )
   //"D" ((fd_set *) (fdsetp)) :"cx","di")



make[2]: 进入目录“/usr/src/linux/fs/minix”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c bitmap.c
bitmap.c: 在函数‘minix_new_block’中:
bitmap.c:113:25: 错误：can’t find a register in class ‘SIREG’ while reloading ‘asm’
bitmap.c:130:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
bitmap.c:113:25: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/minix/bitmap.c
第24行的find_first_zero; 第17行的clear_block



make[2]: 进入目录“/usr/src/linux/fs/minix”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c namei.c
namei.c: 在函数‘minix_find_entry’中:
namei.c:28:3: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
namei.c:28:3: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/minix/namei.c
第25行,修改为这样
__asm__("repe ; cmpsb ; setz %0"
   :"=q" (same)
   :"S" ((long) name),"D" ((long) buffer),"c" (len)
   );
   //:"cx","di","si");



make[2]: 进入目录“/usr/src/linux/fs/ext”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c freelists.c
freelists.c: 在函数‘ext_new_block’中:
freelists.c:136:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
freelists.c:136:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/ext/freelists.c
第40行的clear_block



make[2]: 进入目录“/usr/src/linux/fs/ext”
namei.c: 在函数‘ext_find_entry’中:
namei.c:67:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
namei.c:67:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/ext/namei.c
第67行,修改为这样
__asm__("cld\n\t"
  "repe ; cmpsb\n\t"
  "setz %%al"
  :"=a" (same)
  :"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
  );
  //:"cx","di","si");



make[2]: 进入目录“/usr/src/linux/fs/ext2”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c balloc.c
balloc.c: 在函数‘find_first_zero_bit’中:
balloc.c:51:2: 错误：缺少结尾的 " 字符
balloc.c:52:3: 错误：expected string literal before ‘cld’
balloc.c:55:6: 错误：整数常量的“f”后缀无效
balloc.c:60:7: 错误：整数常量的“f”后缀无效
balloc.c:64:3: 错误：缺少结尾的 " 字符
balloc.c: 在函数‘find_next_zero_bit’中:
balloc.c:81:3: 错误：缺少结尾的 " 字符
balloc.c:82:4: 错误：expected string literal before ‘bsfl’
balloc.c:83:8: 错误：整数常量的“f”后缀无效
balloc.c:85:1: 错误：缺少结尾的 " 字符
balloc.c: 在函数‘find_first_zero_byte’中:
balloc.c:106:2: 错误：缺少结尾的 " 字符
balloc.c:107:3: 错误：expected string literal before ‘cld’
balloc.c:110:7: 错误：整数常量的“f”后缀无效
balloc.c:112:1: 错误：缺少结尾的 " 字符
balloc.c: 在函数‘ext2_check_blocks_bitmap’中:
balloc.c:595:19: 附注：需要类型‘unsigned char *’，但实参的类型为‘char *’
解决办法:
vi /usr/src/linux/fs/ext2/balloc.c
第51行,修改为这样
        __asm__("cld\n\t" \
                "movl $-1,%%eax\n\t" \
                "repe;\n\t" \
                "scasl\n\t" \
                "je 1f\n\t" \
                "subl $4,%%edi\n\t" \
                "movl (%%edi),%%eax\n\t" \
                "notl %%eax\n\t" \
                "bsfl %%eax,%%edx\n\t" \
                "jmp 2f\n\t" \
"1:             xorl %%edx,%%edx\n\t" \
"2:             subl %%ebx,%%edi\n\t" \
                "shll $3,%%edi\n\t" \
                "addl %%edi,%%edx" \
                :"=d" (res)
                :"c" ((size + 31) >> 5), "D" (addr), "b" (addr)
                );
                //:"ax", "bx", "cx", "di");
第81行,修改为这样
  __asm__("bsfl %1,%0\n\t" \
            "jne 1f\n\t" \
            "movl $32, %0\n\t" \
            "1:                     " \
            : "=r" (set)
            : "r" (~(*p >> bit)));
第106行,修改为这样
        __asm__("cld\n\t" \
                "mov $0,%%eax\n\t" \
                "repnz; scasb\n\t" \
                "jnz 1f\n\t" \
                "dec %%edi\n\t" \
                "1:             " \
                : "=D" (res)
                : "0" (addr), "c" (size)
                );
                //: "ax");



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c balloc.c
balloc.c: 在函数‘ext2_new_block’中:
balloc.c:544:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
/usr/include/asm/bitops.h:50:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/ext2/balloc.c
第35行的clear_block,修改为这样
#define clear_block(addr,size) \
 __asm__("cld\n\t" \
  "rep\n\t" \
  "stosl" \
  : \
  :"a" (0), "c" (size / 4), "D" ((long) (addr)) \
  )
  //:"cx", "di")


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c ialloc.c
ialloc.c: 在函数‘find_first_zero_bit’中:
ialloc.c:42:10: 警告：缺少结尾的 " 字符 [enabled by default]
ialloc.c:42:2: 错误：缺少结尾的 " 字符
ialloc.c:43:3: 错误：expected string literal before ‘cld’
ialloc.c:46:6: 错误：整数常量的“f”后缀无效
ialloc.c:51:7: 错误：整数常量的“f”后缀无效
ialloc.c:55:3: 错误：缺少结尾的 " 字符
解决办法:
vi /usr/src/linux/fs/ext2/ialloc.c
第42行,修改为这样
 __asm__("cld\n\t" \
  "movl $-1,%%eax\n\t" \
  "repe; scasl\n\t" \
  "je 1f\n\t" \
  "subl $4,%%edi\n\t" \
  "movl (%%edi),%%eax\n\t" \
  "notl %%eax\n\t" \
  "bsfl %%eax,%%edx\n\t" \
  "jmp 2f\n\t" \
"1:  xorl %%edx,%%edx\n\t" \
"2:  subl %%ebx,%%edi\n\t" \
  "shll $3,%%edi\n\t" \
  "addl %%edi,%%edx" \
  : "=d" (res)
  : "c" ((size + 31) >> 5), "D" (addr), "b" (addr)
  );
  //: "ax", "bx", "cx", "di");


gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c inode.c
inode.c: 在函数‘ext2_alloc_block’中:
inode.c:110:3: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’

解决办法:
vi /usr/src/linux/fs/ext2/inode.c
第28行



make[2]: 进入目录“/usr/src/linux/fs/ext2”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c namei.c
namei.c: 在函数‘ext2_match’中:
namei.c:58:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
namei.c:58:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/ext2/namei.c
第58行,修改为这样
 __asm__("cld\n\t"
  "repe ; cmpsb\n\t"
  "setz %0"
  :"=q" (same)
  :"S" ((long) name), "D" ((long) de->name), "c" (len)
  );
  //:"cx", "di", "si");



make[2]: 进入目录“/usr/src/linux/fs/ext2”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c truncate.c
truncate.c: 在函数‘trunc_indirect’中:
truncate.c:167:4: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
truncate.c:167:4: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/ext2/truncate.c
第28行



make[2]: 进入目录“/usr/src/linux/fs/proc”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c base.c
base.c: 在函数‘proc_match’中:
base.c:82:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
base.c:82:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/fs/proc/base.c
第82行,修改为这样
 __asm__("cld\n\t"
  "repe ; cmpsb\n\t"
  "setz %%al"
  :"=a" (same)
  :"0" (0),"S" ((long) name),"D" ((long) de->name),"c" (len)
  );
  //:"cx","di","si");



make[2]: 进入目录“/usr/src/linux/fs/proc”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c array.c
array.c: 在函数‘array_read’中:
/usr/include/linux/string.h:24:1: 错误：can’t find a register in class ‘SIREG’ while reloading ‘asm’
/usr/include/linux/string.h:24:1: 错误：‘asm’操作数中有不可能的约束
/usr/include/asm/segment.h:57:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第24行的strcpy



make[2]: 进入目录“/usr/src/linux/net/unix”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 \
-c -o sock.o sock.c
sock.c: 在函数‘unix_proto_create’中:
sock.c:331:44: 错误：赋值运算的左操作数必须是左值
sock.c: 在函数‘unix_proto_release’中:
sock.c:363:44: 错误：赋值运算的左操作数必须是左值
解决办法:
vi /usr/src/linux/net/unix/sock.c
第331行,修改为这样
//UN_DATA(sock) = upd;
sock->data = upd;
第363行,修改为这样
//UN_DATA(sock) = NULL;
sock->data = NULL;



make[2]: 进入目录“/usr/src/linux/net/inet”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c -o arp.o arp.c
arp.c:126:27: 错误：‘arp_q’的类型限定冲突
arp.h:48:24: 附注：‘arp_q’的上一个声明在此
解决办法:
vi /usr/src/linux/net/inet/arp.h
第48行,修改为这样
//extern struct sk_buff *arp_q;
extern struct sk_buff * volatile arp_q;
保持和arp.c中的定义一致



make[2]: 进入目录“/usr/src/linux/net/inet”
ip.c: 在函数‘ip_compute_csum’中:
ip.c:472:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
ip.c:487:2: 错误：can’t find a register in class ‘BREG’ while reloading ‘asm’
ip.c:472:2: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/net/inet/ip.c
第472行, 第487行, 第495行



make[2]: 进入目录“/usr/src/linux/net/inet”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c -o tcp.o tcp.c
tcp.c: 在函数‘tcp_check’中:
tcp.c:530:3: 错误：can’t find a register in class ‘DREG’ while reloading ‘asm’
tcp.c:538:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
tcp.c:550:3: 错误：can’t find a register in class ‘BREG’ while reloading ‘asm’
tcp.c:530:3: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/net/inet/ip.c
第530行



make[2]: 进入目录“/usr/src/linux/net/inet”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c -o udp.o udp.c
udp.c: 在函数‘udp_check’中:
udp.c:149:3: 错误：can’t find a register in class ‘DREG’ while reloading ‘asm’
udp.c:157:2: 错误：can’t find a register in class ‘CREG’ while reloading ‘asm’
udp.c:169:3: 错误：can’t find a register in class ‘BREG’ while reloading ‘asm’
udp.c:149:3: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/src/linux/net/inet/udp.c
第149行



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c string.c
In file included from string.c:16:0:
/usr/include/linux/string.h: 在函数‘strcat’中:
/usr/include/linux/string.h:55:1: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
/usr/include/linux/string.h:55:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第55行strcat



make[1]: 进入目录“/usr/src/linux/lib”
gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c string.c
In file included from string.c:16:0:
/usr/include/linux/string.h: 在函数‘strncat’中:
/usr/include/linux/string.h:71:1: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
/usr/include/linux/string.h:71:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第71行strncat



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c string.c
In file included from string.c:16:0:
/usr/include/linux/string.h: 在函数‘strrchr’中:
/usr/include/linux/string.h:154:1: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
/usr/include/linux/string.h:154:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第154行strrchr



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c string.c
In file included from string.c:16:0:
/usr/include/linux/string.h: 在函数‘strspn’中:
/usr/include/linux/string.h:171:1: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
/usr/include/linux/string.h:171:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第171行strspn



gcc -D__KERNEL__ -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -march=i386 -c string.c
In file included from string.c:16:0:
/usr/include/linux/string.h: 在函数‘strcspn’中:
/usr/include/linux/string.h:196:1: 错误：can’t find a register in class ‘AREG’ while reloading ‘asm’
/usr/include/linux/string.h:196:1: 错误：‘asm’操作数中有不可能的约束
解决办法:
vi /usr/include/linux/string.h
第196行strcspn



irq.c:(.text+0x1ddd): undefined reference to `_cache_21'
irq.c:(.text+0x1de3): undefined reference to `_cache_21'
irq.c:(.text+0x1deb): undefined reference to `_intr_count'
kernel/kernel.o: In function `_fast_IRQ0_interrupt':
irq.c:(.text+0x1e0b): undefined reference to `_cache_21'
... ...'
kernel/kernel.o: In function `free_irq':
(.text+0x316b): undefined reference to `idt'
kernel/kernel.o:(.text+0x3172): more undefined references to `idt' follow
......
kernel/kernel.o: In function `_symbol_table':
(.data+0xf00): undefined reference to `_wp_works_ok'
make: *** [tools/zSystem] 错误 1
解决办法:

vi boot/head.S
.globl _idt,_gdt,
.globl _swapper_pg_dir,_pg0
.globl _empty_bad_page
.globl _empty_bad_page_table
.globl _empty_zero_page
.globl _tmp_floppy_area,_floppy_track_buffer
修改为
.globl idt,gdt,
.globl swapper_pg_dir,pg0
.globl empty_bad_page
.globl empty_bad_page_table
.globl empty_zero_page
.globl tmp_floppy_area,floppy_track_buffer

并且把标志为由"_"下划线开头的去掉下划线, 这是由as汇编器进化导致的问题.

vi kernel/sys_call.S
vi kernel/ksyms.S
vi kernel/ksyms.sh
vi drivers/FPU-emu/div_small.S
vi drivers/FPU-emu/polynomial.S
vi drivers/FPU-emu/poly_div.S
vi drivers/FPU-emu/poly_mul64.S
vi drivers/FPU-emu/reg_div.S
vi drivers/FPU-emu/reg_norm.S
vi drivers/FPU-emu/reg_round.S
vi drivers/FPU-emu/reg_u_add.S
vi drivers/FPU-emu/reg_u_div.S
vi drivers/FPU-emu/reg_u_mul.S
vi drivers/FPU-emu/reg_u_sub.S
vi drivers/FPU-emu/wm_shrx.S
vi drivers/FPU-emu/wm_sqrt.S
vi net/inet/loopback.c loopback_xmit里相关__asm__的内容
vi drivers/char/keyboard.c  hard_reset_now里相关__asm__的内容
vi drivers/char/console.c  scrup里相关__asm__的内容; scrdown里相关__asm__的内容
vi usr/include/linux/sched.h switch_to里相关__asm__的内容_current修改为current
vi usr/src/linux/include/asm/irq.h 里相关__asm__的内容
vi drivers/FPU-emu/fpu_asm.h
#define EXCEPTION _exception
修改为
#define EXCEPTION exception



make[1]: 离开目录“/usr/src/linux/lib”
ld  -Ttext 100000 boot/head.o init/main.o tools/version.o \
 kernel/kernel.o mm/mm.o fs/fs.o net/net.o ipc/ipc.o \
 fs/filesystems.a \
 drivers/block/block.a drivers/char/char.a drivers/net/net.a ibcs/ibcs.o drivers/FPU-emu/math.a \
 lib/lib.a \
 -o tools/zSystem
ld: warning: cannot find entry symbol _start; defaulting to 0000000000100000
kernel/kernel.o: In function `symbol_table':
(.data+0xef0): undefined reference to `verify_write'
make: *** [tools/zSystem] 错误 1
解决办法:
vi usr/include/linux/mm.h
修改
//int __verify_write(unsigned long addr, unsigned long count);
extern int verify_write(unsigned long addr, unsigned long count);

extern inline int verify_area(int type, const void * addr, unsigned long size)
{
 if (TASK_SIZE <= (unsigned long) addr)
  return -EFAULT;
 if (size > TASK_SIZE - (unsigned long) addr)
  return -EFAULT;
 if (wp_works_ok || type == VERIFY_READ || !size)
  return 0;
 //return __verify_write((unsigned long) addr,size);
 return verify_write((unsigned long) addr,size);
}

vi /usr/src/linux/mm/memcpy.c
第654行,修改为这样
//int __verify_write(unsigned long start, unsigned long size)
int verify_write(unsigned long start, unsigned long size)
{
 size--;
 size += start & ~PAGE_MASK;
 size >>= PAGE_SHIFT;
 start &= PAGE_MASK;
 do {
  do_wp_page(1,start,current,0);
  start += PAGE_SIZE;
 } while (size--);
 return 0;
}



make[1]: 进入目录“/usr/src/linux/zBoot”
gcc -D__KERNEL__ -O2 -DSTDC_HEADERS  -c -o misc.o misc.c
misc.c:81:7: 错误：与‘malloc’类型冲突
misc.c:81:7: error: conflicting types for 'malloc'
解决办法:
vi /usr/src/linux/zBoot/misc.c
第81行,修改为这样
//void *malloc(int size)
void *malloc(size_t size)



make[1]: 进入目录“/usr/src/linux/zBoot”
gcc -D__KERNEL__ -O2 -DSTDC_HEADERS  -c -o misc.o misc.c
misc.o: In function `fill_inbuf':
misc.c:(.text+0x352): undefined reference to `input_len'
misc.c:(.text+0x384): undefined reference to `input_data'
解决办法:
vi /usr/src/linux/zBoot/misc.c
第53行,修改为这样
//extern char input_data[];
char input_data[];
//extern int input_len;
int input_len;



make[1]: 离开目录“/usr/src/linux/zBoot”
tools/build boot/bootsect boot/setup zBoot/zSystem CURRENT > zImage
Root device is (-3, 1)
Boot sector 512 bytes.
Setup is 1980 bytes.
Non-GCC header of 'system'
make: *** [zImage] 错误 1
解决办法:
vi /usr/src/linux/tools/build.c
第191行,修改为这样
//if (N_MAGIC(*ex) != ZMAGIC)
//      die("Non-GCC header of 'system'");

vi /usr/src/linux/zBoot/xtract.c

//if (N_MAGIC(*ex) != ZMAGIC)
//      die("Non-GCC header of 'system'");

make[1]: Leaving directory `/usr/src/linux/zBoot'
tools/build boot/bootsect boot/setup zBoot/zSystem CURRENT > zImage
Root device is (-3, 1)
Boot sector 512 bytes.
Setup is 1980 bytes.
System is 64 kB (64 kB code, 0 kB data and 0 kB bss)
Unexpected EOF
Can't read 'system'
make: *** [zImage] Error 1
解决办法:
vi /usr/src/linux/tools/build.c
第208行,修改为这样
n=read(id, buf, l);
if (n != l) {
        if( n < 0)
        {
                perror(argv[1]);
                fprintf(stderr, "Unexpected EOF\n");
                die("Can't read 'system'");
        }
        else if( n == 0)
                break;
}
if (write(1, buf, n) != n)
        die("Write failed");
sz -= n;



*******************************************************************************************************************
编译压缩内核镜象zImage成功
make[1]: Leaving directory `/usr/src/linux/zBoot'
gcc -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe -o tools/build tools/build.c
tools/build.c: In function 'main':
tools/build.c:125:2: warning: dereferencing type-punned pointer will break strict-aliasing rules [-Wstrict-aliasing]
tools/build.c:154:2: warning: dereferencing type-punned pointer will break strict-aliasing rules [-Wstrict-aliasing]
tools/build boot/bootsect boot/setup zBoot/zSystem CURRENT > zImage
Root device is (-3, 1)
Boot sector 512 bytes.
Setup is 1980 bytes.
System is 64 kB (64 kB code, 0 kB data and 0 kB bss)
argv[3]zBoot/zSystem sz:68953
sync



[root@localhost linux]# ls
boot       config.old  CREDITS  ibcs     ipc     Makefile    net     zBoot
CHANGES    Configure   drivers  include  kernel  makever.sh  README  zImage
config.in  COPYING     fs       init     lib     mm          tools   zSystem.map

下一步完成内核运行。
```
