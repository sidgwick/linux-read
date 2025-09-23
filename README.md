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

GCC 版本是 13.3.0

```
# gcc --version
gcc (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
Copyright (C) 2023 Free Software Foundation, Inc.
This is free software; see the source for copying conditions.  There is NO
warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
```

## 第 1 步, 构建配置

```
make config
```

之后修正 Makefile, 在 64 bits 机器上编译 32 位系统

```patch
diff --git a/Makefile b/Makefile
index be8194b..e57cdae 100644
--- a/Makefile
+++ b/Makefile
@@ -51,13 +51,15 @@ SVGA_MODE=	-DSVGA_MODE=NORMAL_VGA
 #

 CFLAGS = -Wall -Wstrict-prototypes -O2 -fomit-frame-pointer -pipe
+CFLAGS += -m32 -g -nostdinc -fno-builtin -fno-stack-protector -fno-pie
+CFLAGS += -I/app/linux-read/include -I/app/xroot/usr/include/ -I/app/xroot/usr/lib/gcc-lib/i486-linux/2.7.2/include/

 ifdef CONFIG_CPP
 CFLAGS := $(CFLAGS) -x c++
 endif

 ifdef CONFIG_M486
-CFLAGS := $(CFLAGS) -m486
+CFLAGS := $(CFLAGS) -mtune=i486
 else
 CFLAGS := $(CFLAGS) -m386
 endif
@@ -72,12 +74,12 @@ endif
 AS86	=as86 -0 -a
 LD86	=ld86 -0

-AS	=as
-LD	=ld
+AS	=as -g --32
+LD	=ld -m elf_i386 -z noexecstack
 HOSTCC	=gcc
 CC	=gcc -D__KERNEL__
 MAKE	=make
-CPP	=$(CC) -E
+CPP	=$(CC) $(CFLAGS) -E
 AR	=ar
 STRIP	=strip


```

上一步会消除一些 include 找不到文件之类的报错
TODO: xroot 如何制作

## 第 2 步, 构建依赖

```
make dep
```

### 报错解决

问题

```text
serial.c:538:8: error: macro names must be identifiers
538 | #ifdef 0
    |        ^
```

解决:

```
find . -name "*.[ch]" | xargs -I {} sed 's/^#ifdef 0/#ifdef NO_COMPILE/g' -i {}
```

问题

```text
/app/linux-read/include/linux/sched.h:238:8: warning: extra tokens at end of #endif directive [-Wendif-labels]
  238 | #endif NEW_SWAP
```

解决:

```
find . -name "*.[ch]" | xargs -I {} sed 's|#endif \([A-Za-z0-9_]\+\)|#endif /* \1 */|g' -i {}
```

问题

```text
/app/linux-read/include/asm/io.h:60:1: error: impossible constraint in 'asm'
   60 | __asm__ __volatile__ ("in" #s " %" s2 "1,%" s1 "0"
      | ^~~~~~~
```

解决:

```
diff --git a/include/asm/io.h b/include/asm/io.h
index 09c494c..0b80fb3 100644
--- a/include/asm/io.h
+++ b/include/asm/io.h
@@ -49,9 +49,9 @@ __asm__ __volatile__ ("out" #s " %" s1 "0,%" s2 "1"

 #define __OUT(s,s1,x) \
 __OUT1(s,x) __OUT2(s,s1,"w") : : "a" (value), "d" (port)); } \
-__OUT1(s##c,x) __OUT2(s,s1,"") : : "a" (value), "i" (port)); } \
+__OUT1(s##c,x) __OUT2(s,s1,"") : : "a" (value), "d" (port)); } \
 __OUT1(s##_p,x) __OUT2(s,s1,"w") : : "a" (value), "d" (port)); SLOW_DOWN_IO; } \
-__OUT1(s##c_p,x) __OUT2(s,s1,"") : : "a" (value), "i" (port)); SLOW_DOWN_IO; }
+__OUT1(s##c_p,x) __OUT2(s,s1,"") : : "a" (value), "d" (port)); SLOW_DOWN_IO; }

 #define __IN1(s) \
 extern inline unsigned int __in##s(unsigned short port) { unsigned int _v;
@@ -61,9 +61,9 @@ __asm__ __volatile__ ("in" #s " %" s2 "1,%" s1 "0"

 #define __IN(s,s1,i...) \
 __IN1(s) __IN2(s,s1,"w") : "=a" (_v) : "d" (port) ,##i ); return _v; } \
-__IN1(s##c) __IN2(s,s1,"") : "=a" (_v) : "i" (port) ,##i ); return _v; } \
+__IN1(s##c) __IN2(s,s1,"") : "=a" (_v) : "d" (port) ,##i ); return _v; } \
 __IN1(s##_p) __IN2(s,s1,"w") : "=a" (_v) : "d" (port) ,##i ); SLOW_DOWN_IO; return _v; } \
-__IN1(s##c_p) __IN2(s,s1,"") : "=a" (_v) : "i" (port) ,##i ); SLOW_DOWN_IO; return _v; }
+__IN1(s##c_p) __IN2(s,s1,"") : "=a" (_v) : "d" (port) ,##i ); SLOW_DOWN_IO; return _v; }

 #define __INS(s) \
 extern inline void ins##s(unsigned short port, void * addr, unsigned long count) \

```

问题

```text
/app/linux-read/include/linux/mm.h:98:17: error: 'asm' operand has impossible constraints
   98 |                 __asm__ __volatile__("rep ; stosl"
      |                 ^~~~~~~
```

解决:

```
find . -name "*.[ch]" | xargs -I {} sed '/asm.*(/,/);$/{s/:\s*"[ "abcdxsdiSDImeory,]\+"\s*)/)/g}' -i {}
```

问题

```text
In file included from sched.c:35:
/app/linux-read/include/linux/timex.h:120:32: error: conflicting type qualifiers for 'xtime'
  120 | extern volatile struct timeval xtime;           /* The current time */
      |                                ^~~~~
```

解决:

```
sed 's/extern struct timeval xtime;/extern volatile struct timeval xtime;/g' -i include/linux/sched.h kernel/time.c
```

问题

```text
ld: traps.o: in function `tas':
/app/linux-read/kernel/traps.c:43: multiple definition of `tas'; sched.o:/app/linux-read/include/asm/system.h:40: first defined here
ld: traps.o: in function `verify_area':
/app/linux-read/include/linux/mm.h:15: multiple definition of `verify_area'; sched.o:/app/linux-read/include/linux/mm.h:15: first defined here
ld: traps.o: in function `get_free_page':
/app/linux-read/include/linux/mm.h:93: multiple definition of `get_free_page'; sched.o:/app/linux-read/include/linux/mm.h:93: first defined here
ld: traps.o: in function `add_wait_queue':
```

解决:

```
diff --git a/include/asm/io.h b/include/asm/io.h
index 0b80fb3..835ae6a 100644
--- a/include/asm/io.h
+++ b/include/asm/io.h
@@ -42,7 +42,7 @@
  */
 
 #define __OUT1(s,x) \
-extern inline void __out##s(unsigned x value, unsigned short port) {
+static inline void __out##s(unsigned x value, unsigned short port) {
 
 #define __OUT2(s,s1,s2) \
 __asm__ __volatile__ ("out" #s " %" s1 "0,%" s2 "1"
@@ -54,7 +54,7 @@ __OUT1(s##_p,x) __OUT2(s,s1,"w") : : "a" (value), "d" (port)); SLOW_DOWN_IO; } \
 __OUT1(s##c_p,x) __OUT2(s,s1,"") : : "a" (value), "d" (port)); SLOW_DOWN_IO; }
 
 #define __IN1(s) \
-extern inline unsigned int __in##s(unsigned short port) { unsigned int _v;
+static inline unsigned int __in##s(unsigned short port) { unsigned int _v;
 
 #define __IN2(s,s1,s2) \
 __asm__ __volatile__ ("in" #s " %" s2 "1,%" s1 "0"
@@ -66,12 +66,12 @@ __IN1(s##_p) __IN2(s,s1,"w") : "=a" (_v) : "d" (port) ,##i ); SLOW_DOWN_IO; retu
 __IN1(s##c_p) __IN2(s,s1,"") : "=a" (_v) : "d" (port) ,##i ); SLOW_DOWN_IO; return _v; }
 
 #define __INS(s) \
-extern inline void ins##s(unsigned short port, void * addr, unsigned long count) \
+static inline void ins##s(unsigned short port, void * addr, unsigned long count) \
 { __asm__ __volatile__ ("cld ; rep ; ins" #s \
 : "=D" (addr), "=c" (count) : "d" (port),"0" (addr),"1" (count)); }
 
 #define __OUTS(s) \
-extern inline void outs##s(unsigned short port, const void * addr, unsigned long count) \
+static inline void outs##s(unsigned short port, const void * addr, unsigned long count) \
 { __asm__ __volatile__ ("cld ; rep ; outs" #s \
 : "=S" (addr), "=c" (count) : "d" (port),"0" (addr),"1" (count)); }
 
diff --git a/include/asm/system.h b/include/asm/system.h
index fb7550c..5296a3e 100644
--- a/include/asm/system.h
+++ b/include/asm/system.h
@@ -36,7 +36,7 @@ __asm__ __volatile__ ( \
 	)
 
 
-extern inline int tas(char * m)
+static inline int tas(char * m)
 {
 	char res;
 
diff --git a/include/linux/interrupt.h b/include/linux/interrupt.h
index 2a7d91f..acafea0 100644
--- a/include/linux/interrupt.h
+++ b/include/linux/interrupt.h
@@ -22,17 +22,17 @@ enum {
 	KEYBOARD_BH
 };
 
-extern inline void mark_bh(int nr)
+static inline void mark_bh(int nr)
 {
 	__asm__ __volatile__("orl %1,%0":"=m" (bh_active):"ir" (1<<nr));
 }
 
-extern inline void disable_bh(int nr)
+static inline void disable_bh(int nr)
 {
 	__asm__ __volatile__("andl %1,%0":"=m" (bh_mask):"ir" (~(1<<nr)));
 }
 
-extern inline void enable_bh(int nr)
+static inline void enable_bh(int nr)
 {
 	__asm__ __volatile__("orl %1,%0":"=m" (bh_mask):"ir" (1<<nr));
 }
diff --git a/include/linux/mm.h b/include/linux/mm.h
index 9aa6e29..7615cb5 100644
--- a/include/linux/mm.h
+++ b/include/linux/mm.h
@@ -11,7 +11,7 @@
 
 int __verify_write(unsigned long addr, unsigned long count);
 
-extern inline int verify_area(int type, const void * addr, unsigned long size)
+static inline int verify_area(int type, const void * addr, unsigned long size)
 {
 	if (TASK_SIZE <= (unsigned long) addr)
 		return -EFAULT;
@@ -89,7 +89,7 @@ extern unsigned long secondary_page_list;
  * overhead, just use __get_free_page() directly..
  */
 extern unsigned long __get_free_page(int priority);
-extern inline unsigned long get_free_page(int priority)
+static inline unsigned long get_free_page(int priority)
 {
 	unsigned long page;
 
diff --git a/include/linux/sched.h b/include/linux/sched.h
index 930e5d2..25f8455 100644
--- a/include/linux/sched.h
+++ b/include/linux/sched.h
@@ -403,7 +403,7 @@ __asm__("movw %%dx,%0\n\t" \
  * to keep them correct. Use only these two functions to add/remove
  * entries in the queues.
  */
-extern inline void add_wait_queue(struct wait_queue ** p, struct wait_queue * wait)
+static inline void add_wait_queue(struct wait_queue ** p, struct wait_queue * wait)
 {
 	unsigned long flags;
 
@@ -427,7 +427,7 @@ extern inline void add_wait_queue(struct wait_queue ** p, struct wait_queue * wa
 	restore_flags(flags);
 }
 
-extern inline void remove_wait_queue(struct wait_queue ** p, struct wait_queue * wait)
+static inline void remove_wait_queue(struct wait_queue ** p, struct wait_queue * wait)
 {
 	unsigned long flags;
 	struct wait_queue * tmp;
@@ -466,7 +466,7 @@ extern inline void remove_wait_queue(struct wait_queue ** p, struct wait_queue *
 #endif
 }
 
-extern inline void select_wait(struct wait_queue ** wait_address, select_table * p)
+static inline void select_wait(struct wait_queue ** wait_address, select_table * p)
 {
 	struct select_table_entry * entry;
 
@@ -484,14 +484,14 @@ extern inline void select_wait(struct wait_queue ** wait_address, select_table *
 
 extern void __down(struct semaphore * sem);
 
-extern inline void down(struct semaphore * sem)
+static inline void down(struct semaphore * sem)
 {
 	if (sem->count <= 0)
 		__down(sem);
 	sem->count--;
 }
 
-extern inline void up(struct semaphore * sem)
+static inline void up(struct semaphore * sem)
 {
 	sem->count++;
 	wake_up(&sem->wait);
diff --git a/include/linux/string.h b/include/linux/string.h
index f27d68e..cc096a2 100644
--- a/include/linux/string.h
+++ b/include/linux/string.h
@@ -19,7 +19,7 @@
  *		Copyright (C) 1991, 1992 Linus Torvalds
  */
  
-extern inline char * strcpy(char * dest,const char *src)
+static inline char * strcpy(char * dest,const char *src)
 {
 __asm__("cld\n"
 	"1:\tlodsb\n\t"
@@ -31,7 +31,7 @@ __asm__("cld\n"
 return dest;
 }
 
-extern inline char * strncpy(char * dest,const char *src,size_t count)
+static inline char * strncpy(char * dest,const char *src,size_t count)
 {
 __asm__("cld\n"
 	"1:\tdecl %2\n\t"
@@ -48,7 +48,7 @@ __asm__("cld\n"
 return dest;
 }
 
-extern inline char * strcat(char * dest,const char * src)
+static inline char * strcat(char * dest,const char * src)
 {
 __asm__("cld\n\t"
 	"repne\n\t"
@@ -63,7 +63,7 @@ __asm__("cld\n\t"
 return dest;
 }
 
-extern inline char * strncat(char * dest,const char * src,size_t count)
+static inline char * strncat(char * dest,const char * src,size_t count)
 {
 __asm__("cld\n\t"
 	"repne\n\t"
@@ -84,7 +84,7 @@ __asm__("cld\n\t"
 return dest;
 }
 
-extern inline int strcmp(const char * cs,const char * ct)
+static inline int strcmp(const char * cs,const char * ct)
 {
 register int __res __asm__("ax");
 __asm__("cld\n"
@@ -103,7 +103,7 @@ __asm__("cld\n"
 return __res;
 }
 
-extern inline int strncmp(const char * cs,const char * ct,size_t count)
+static inline int strncmp(const char * cs,const char * ct,size_t count)
 {
 register int __res __asm__("ax");
 __asm__("cld\n"
@@ -124,7 +124,7 @@ __asm__("cld\n"
 return __res;
 }
 
-extern inline char * strchr(const char * s,char c)
+static inline char * strchr(const char * s,char c)
 {
 register char * __res __asm__("ax");
 __asm__("cld\n\t"
@@ -141,7 +141,7 @@ __asm__("cld\n\t"
 return __res;
 }
 
-extern inline char * strrchr(const char * s,char c)
+static inline char * strrchr(const char * s,char c)
 {
 register char * __res __asm__("dx");
 __asm__("cld\n\t"
@@ -157,7 +157,7 @@ __asm__("cld\n\t"
 return __res;
 }
 
-extern inline size_t strspn(const char * cs, const char * ct)
+static inline size_t strspn(const char * cs, const char * ct)
 {
 register char * __res __asm__("si");
 __asm__("cld\n\t"
@@ -181,7 +181,7 @@ __asm__("cld\n\t"
 return __res-cs;
 }
 
-extern inline size_t strcspn(const char * cs, const char * ct)
+static inline size_t strcspn(const char * cs, const char * ct)
 {
 register char * __res __asm__("si");
 __asm__("cld\n\t"
@@ -205,7 +205,7 @@ __asm__("cld\n\t"
 return __res-cs;
 }
 
-extern inline char * strpbrk(const char * cs,const char * ct)
+static inline char * strpbrk(const char * cs,const char * ct)
 {
 register char * __res __asm__("si");
 __asm__("cld\n\t"
@@ -232,7 +232,7 @@ __asm__("cld\n\t"
 return __res;
 }
 
-extern inline char * strstr(const char * cs,const char * ct)
+static inline char * strstr(const char * cs,const char * ct)
 {
 register char * __res __asm__("ax");
 __asm__("cld\n\t" \
@@ -259,7 +259,7 @@ __asm__("cld\n\t" \
 return __res;
 }
 
-extern inline size_t strlen(const char * s)
+static inline size_t strlen(const char * s)
 {
 register int __res __asm__("cx");
 __asm__("cld\n\t"
@@ -273,7 +273,7 @@ return __res;
 
 extern char * ___strtok;
 
-extern inline char * strtok(char * s,const char * ct)
+static inline char * strtok(char * s,const char * ct)
 {
 register char * __res;
 __asm__("testl %1,%1\n\t"
@@ -332,7 +332,7 @@ __asm__("testl %1,%1\n\t"
 return __res;
 }
 
-extern inline void * memcpy(void * to, const void * from, size_t n)
+static inline void * memcpy(void * to, const void * from, size_t n)
 {
 __asm__("cld\n\t"
 	"movl %%edx, %%ecx\n\t"
@@ -351,7 +351,7 @@ __asm__("cld\n\t"
 return (to);
 }
 
-extern inline void * memmove(void * dest,const void * src, size_t n)
+static inline void * memmove(void * dest,const void * src, size_t n)
 {
 if (dest<src)
 __asm__("cld\n\t"
@@ -373,7 +373,7 @@ __asm__("std\n\t"
 return dest;
 }
 
-extern inline int memcmp(const void * cs,const void * ct,size_t count)
+static inline int memcmp(const void * cs,const void * ct,size_t count)
 {
 register int __res __asm__("ax");
 __asm__("cld\n\t"
@@ -389,7 +389,7 @@ __asm__("cld\n\t"
 return __res;
 }
 
-extern inline void * memchr(const void * cs,char c,size_t count)
+static inline void * memchr(const void * cs,char c,size_t count)
 {
 register void * __res __asm__("di");
 if (!count)
@@ -405,7 +405,7 @@ __asm__("cld\n\t"
 return __res;
 }
 
-extern inline void * memset(void * s,char c,size_t count)
+static inline void * memset(void * s,char c,size_t count)
 {
 __asm__("cld\n\t"
 	"rep\n\t"

```

问题

```text
fpu_entry.c:473:24: error: lvalue required as left operand of assignment
  473 |       FPU_data_address = 0;
      |                        ^
```

解决:

```
sed '/^#define FPU_data_address/c#define FPU_data_address        ((I387.soft.twd))' -i drivers/FPU-emu/fpu_system.h
```



问题

```text
div_small.S: Assembler messages:
div_small.S:25: Error: invalid instruction suffix for `push'
div_small.S:28: Error: invalid instruction suffix for `push'
div_small.S:46: Error: invalid instruction suffix for `pop'
```

解决:

```
sed 's/-D__ASSEMBLER__/-D__ASSEMBLER__ $(CFLAGS)/g' -i drivers/FPU-emu/Makefile
```

问题

```text
fpu_trig.c:748:17: error: missing terminating " character
  748 |   asm volatile ("movl %2,%%eax; mull %4; subl %%eax,%0; sbbl %%edx,%1;
      |                 ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
fpu_trig.c:749:18: error: expected string literal before 'movl'
  749 |                  movl %3,%%eax; mull %4; subl %%eax,%1;
      |                  ^~~~
fpu_trig.c:750:56: warning: missing terminating " character
  750 |                  movl %2,%%eax; mull %5; subl %%eax,%1;"
      |                                                        ^
fpu_trig.c:750:56: error: missing terminating " character
```

解决:

```
diff --git a/README.md b/README.md
index 6b2e1df..50efcb8 100644
--- a/README.md
+++ b/README.md
@@ -588,6 +588,16 @@ sed 's/-D__ASSEMBLER__/-D__ASSEMBLER__ $(CFLAGS)/g' -i drivers/FPU-emu/Makefile
 问题
 
 ```text
+fpu_trig.c:748:17: error: missing terminating " character
+  748 |   asm volatile ("movl %2,%%eax; mull %4; subl %%eax,%0; sbbl %%edx,%1;
+      |                 ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
+fpu_trig.c:749:18: error: expected string literal before 'movl'
+  749 |                  movl %3,%%eax; mull %4; subl %%eax,%1;
+      |                  ^~~~
+fpu_trig.c:750:56: warning: missing terminating " character
+  750 |                  movl %2,%%eax; mull %5; subl %%eax,%1;"
+      |                                                        ^
+fpu_trig.c:750:56: error: missing terminating " character
 ```
 
 解决:
diff --git a/drivers/FPU-emu/fpu_trig.c b/drivers/FPU-emu/fpu_trig.c
index d26074a..8e64866 100644
--- a/drivers/FPU-emu/fpu_trig.c
+++ b/drivers/FPU-emu/fpu_trig.c
@@ -745,9 +745,16 @@ static void rem_kernel(unsigned long long st0, unsigned long long *y,
   x = st0 << n;
 
   /* Do the required multiplication and subtraction in the one operation */
-  asm volatile ("movl %2,%%eax; mull %4; subl %%eax,%0; sbbl %%edx,%1;
-                 movl %3,%%eax; mull %4; subl %%eax,%1;
-                 movl %2,%%eax; mull %5; subl %%eax,%1;"
+  asm volatile ("movl %2,%%eax\n\t"
+                "mull %4\n\t"
+                "subl %%eax,%0\n\t"
+                "sbbl %%edx,%1\n\t"
+                "movl %3,%%eax\n\t"
+                "mull %4\n\t"
+                "subl %%eax,%1\n\t"
+                "movl %2,%%eax\n\t"
+                "mull %5\n\t"
+                "subl %%eax,%1"
 		:"=m" (x), "=m" (((unsigned *)&x)[1])
 		:"m" (st1),"m" (((unsigned *)&st1)[1]),
 		 "m" (q),"m" (((unsigned *)&q)[1])

```

问题

```text
reg_div.S: Assembler messages:
reg_div.S:194: Error: `%ax' not allowed with `movb'
reg_div.S:195: Error: `%ax' not allowed with `movb'
```

解决:

```
diff --git a/drivers/FPU-emu/reg_div.S b/drivers/FPU-emu/reg_div.S
index 2727cfa..530d81a 100644
--- a/drivers/FPU-emu/reg_div.S
+++ b/drivers/FPU-emu/reg_div.S
@@ -191,8 +191,8 @@ L_arg2_not_inf:
 #endif DENORM_OPERAND
 
 L_copy_arg1:
-	movb	TAG(%esi),%ax
-	movb	%ax,TAG(%edi)
+	mov	TAG(%esi),%ax
+	mov	%ax,TAG(%edi)
 	movl	EXP(%esi),%eax
 	movl	%eax,EXP(%edi)
 	movl	SIGL(%esi),%eax
diff --git a/drivers/FPU-emu/reg_round.S b/drivers/FPU-emu/reg_round.S
index 88a6bbd..e597f7a 100644
--- a/drivers/FPU-emu/reg_round.S
+++ b/drivers/FPU-emu/reg_round.S
@@ -368,7 +368,7 @@ LRound_nearest_64:
 	jne	LDo_64_round_up
 
 	/* Now test for round-to-even */
-	testb	$1,%ebx
+	test	$1,%ebx
 	jz	LCheck_truncate_64
 
 LDo_64_round_up:

```

问题

```text
exec.c: In function 'copy_strings':
exec.c:380:72: error: lvalue required as left operand of assignment
  380 |                                     !(pag = (char *) page[p/PAGE_SIZE] =
      |                                                                        ^
exec.c: In function 'change_ldt':
```

解决:

```
diff --git a/fs/exec.c b/fs/exec.c
index db46a74..036ae20 100644
--- a/fs/exec.c
+++ b/fs/exec.c
@@ -376,10 +376,16 @@ unsigned long copy_strings(int argc,char ** argv,unsigned long *page,
 				offset = p % PAGE_SIZE;
 				if (from_kmem==2)
 					set_fs(old_fs);
-				if (!(pag = (char *) page[p/PAGE_SIZE]) &&
-				    !(pag = (char *) page[p/PAGE_SIZE] =
-				      (unsigned long *) get_free_page(GFP_USER))) 
-					return 0;
+
+                pag = (char *) page[p/PAGE_SIZE];
+                if (!pag) {
+                    page[p/PAGE_SIZE] = (unsigned long *) get_free_page(GFP_USER);
+                    pag = (char *) page[p/PAGE_SIZE];
+                    if (!pag) {
+                        return 0;
+                    }
+                }
+
 				if (from_kmem==2)
 					set_fs(new_fs);
 

```
问题

```text
ld: super.o: in function `wait_on_buffer':
/app/linux-read/include/linux/locks.h:11: multiple definition of `wait_on_buffer'; buffer.o:/app/linux-read/include/linux/locks.h:11: first defined here
ld: super.o: in function `lock_buffer':
/app/linux-read/include/linux/locks.h:17: multiple definition of `lock_buffer'; buffer.o:/app/linux-read/include/linux/locks.h:17: first defined here
ld: super.o: in function `unlock_buffer':
/app/linux-read/include/linux/locks.h:24: multiple definition of `unlock_buffer'; buffer.o:/app/linux-read/include/linux/locks.h:24: first defined here
ld: super.o: in function `unlock_super':
```

解决:

```
diff --git a/include/asm/bitops.h b/include/asm/bitops.h
index 4a18616..1fc5510 100644
--- a/include/asm/bitops.h
+++ b/include/asm/bitops.h
@@ -19,7 +19,7 @@
 struct __dummy { unsigned long a[100]; };
 #define ADDR (*(struct __dummy *) addr)
 
-extern __inline__ int set_bit(int nr, void * addr)
+static inline int set_bit(int nr, void * addr)
 {
 	int oldbit;
 
@@ -29,7 +29,7 @@ extern __inline__ int set_bit(int nr, void * addr)
 	return oldbit;
 }
 
-extern __inline__ int clear_bit(int nr, void * addr)
+static inline int clear_bit(int nr, void * addr)
 {
 	int oldbit;
 
@@ -43,7 +43,7 @@ extern __inline__ int clear_bit(int nr, void * addr)
  * This routine doesn't need to be atomic, but it's faster to code it
  * this way.
  */
-extern __inline__ int test_bit(int nr, void * addr)
+static inline int test_bit(int nr, void * addr)
 {
 	int oldbit;
 
@@ -69,7 +69,7 @@ extern __inline__ int test_bit(int nr, void * addr)
  * C language equivalents written by Theodore Ts'o, 9/26/92
  */
 
-extern __inline__ int set_bit(int nr,int * addr)
+static inline int set_bit(int nr,int * addr)
 {
 	int	mask, retval;
 
@@ -82,7 +82,7 @@ extern __inline__ int set_bit(int nr,int * addr)
 	return retval;
 }
 
-extern __inline__ int clear_bit(int nr, int * addr)
+static inline int clear_bit(int nr, int * addr)
 {
 	int	mask, retval;
 
@@ -95,7 +95,7 @@ extern __inline__ int clear_bit(int nr, int * addr)
 	return retval;
 }
 
-extern __inline__ int test_bit(int nr, int * addr)
+static inline int test_bit(int nr, int * addr)
 {
 	int	mask;
 
diff --git a/include/linux/locks.h b/include/linux/locks.h
index ac9b290..7c234ee 100644
--- a/include/linux/locks.h
+++ b/include/linux/locks.h
@@ -7,20 +7,20 @@
  */
 extern void __wait_on_buffer(struct buffer_head *);
 
-extern inline void wait_on_buffer(struct buffer_head * bh)
+static inline void wait_on_buffer(struct buffer_head * bh)
 {
 	if (bh->b_lock)
 		__wait_on_buffer(bh);
 }
 
-extern inline void lock_buffer(struct buffer_head * bh)
+static inline void lock_buffer(struct buffer_head * bh)
 {
 	if (bh->b_lock)
 		__wait_on_buffer(bh);
 	bh->b_lock = 1;
 }
 
-extern inline void unlock_buffer(struct buffer_head * bh)
+static inline void unlock_buffer(struct buffer_head * bh)
 {
 	bh->b_lock = 0;
 	wake_up(&bh->b_wait);
@@ -33,20 +33,20 @@ extern inline void unlock_buffer(struct buffer_head * bh)
  */
 extern void __wait_on_super(struct super_block *);
 
-extern inline void wait_on_super(struct super_block * sb)
+static inline void wait_on_super(struct super_block * sb)
 {
 	if (sb->s_lock)
 		__wait_on_super(sb);
 }
 
-extern inline void lock_super(struct super_block * sb)
+static inline void lock_super(struct super_block * sb)
 {
 	if (sb->s_lock)
 		__wait_on_super(sb);
 	sb->s_lock = 1;
 }
 
-extern inline void unlock_super(struct super_block * sb)
+static inline void unlock_super(struct super_block * sb)
 {
 	sb->s_lock = 0;
 	wake_up(&sb->s_wait);

```

问题

```text
balloc.c:51:17: error: missing terminating " character
balloc.c:52:17: error: expected string literal before 'cld'
   52 |                 cld
      |                 ^~~
balloc.c:55:20: error: invalid suffix "f" on integer constant
   55 |                 je 1f
      |                    ^~
balloc.c:60:21: error: invalid suffix "f" on integer constant
   60 |                 jmp 2f
      |                     ^~
```

解决:

```
diff --git a/fs/ext2/balloc.c b/fs/ext2/balloc.c
index a531d0e..366ef20 100644
--- a/fs/ext2/balloc.c
+++ b/fs/ext2/balloc.c
@@ -48,20 +48,20 @@ static inline int find_first_zero_bit (unsigned long * addr, unsigned size)
 
 	if (!size)
 		return 0;
-	__asm__("
-		cld
-		movl $-1,%%eax
-		repe; scasl
-		je 1f
-		subl $4,%%edi
-		movl (%%edi),%%eax
-		notl %%eax
-		bsfl %%eax,%%edx
-		jmp 2f
-1:		xorl %%edx,%%edx
-2:		subl %%ebx,%%edi
-		shll $3,%%edi
-		addl %%edi,%%edx"
+	__asm__(
+"		cld\n\t"
+"		movl $-1,%%eax\n\t"
+"		repe; scasl\n\t"
+"		je 1f\n\t"
+"		subl $4,%%edi\n\t"
+"		movl (%%edi),%%eax\n\t"
+"		notl %%eax\n\t"
+"		bsfl %%eax,%%edx\n\t"
+"		jmp 2f\n\t"
+"1:		xorl %%edx,%%edx\n\t"
+"2:		subl %%ebx,%%edi\n\t"
+"		shll $3,%%edi\n\t"
+"		addl %%edi,%%edx\n\t"
 		:"=d" (res)
 		:"c" ((size + 31) >> 5), "D" (addr), "b" (addr)
 		);
@@ -78,11 +78,11 @@ static inline int find_next_zero_bit (unsigned long * addr, int size,
 		/*
 		 * Look for zero in first byte
 		 */
-		__asm__("
-			bsfl %1,%0
-			jne 1f
-			movl $32, %0
-1:			"
+		__asm__(
+"			bsfl %1,%0\n\t"
+"			jne 1f\n\t"
+"			movl $32, %0\n\t"
+"1:			"
 			: "=r" (set)
 			: "r" (~(*p >> bit)));
 		if (set < (32 - bit))
@@ -103,13 +103,13 @@ static inline char * find_first_zero_byte (char * addr, int size)
 
 	if (!size)
 		return 0;
-	__asm__("
-		cld
-		mov $0,%%eax
-		repnz; scasb
-		jnz 1f
-		dec %%edi
-1:		"
+	__asm__(
+"		cld\n\t"
+"		mov $0,%%eax\n\t"
+"		repnz; scasb\n\t"
+"		jnz 1f\n\t"
+"		dec %%edi\n\t"
+"1:		"
 		: "=D" (res)
 		: "0" (addr), "c" (size)
 		);
diff --git a/fs/ext2/ialloc.c b/fs/ext2/ialloc.c
index 113114d..ca1b318 100644
--- a/fs/ext2/ialloc.c
+++ b/fs/ext2/ialloc.c
@@ -39,20 +39,20 @@ static inline int find_first_zero_bit (unsigned long * addr, unsigned size)
 
 	if (!size)
 		return 0;
-	__asm__("
-		cld
-		movl $-1,%%eax
-		repe; scasl
-		je 1f
-		subl $4,%%edi
-		movl (%%edi),%%eax
-		notl %%eax
-		bsfl %%eax,%%edx
-		jmp 2f
-1:		xorl %%edx,%%edx
-2:		subl %%ebx,%%edi
-		shll $3,%%edi
-		addl %%edi,%%edx"
+	__asm__(
+"		cld\n\t"
+"		movl $-1,%%eax\n\t"
+"		repe; scasl\n\t"
+"		je 1f\n\t"
+"		subl $4,%%edi\n\t"
+"		movl (%%edi),%%eax\n\t"
+"		notl %%eax\n\t"
+"		bsfl %%eax,%%edx\n\t"
+"		jmp 2f\n\t"
+"1:		xorl %%edx,%%edx\n\t"
+"2:		subl %%ebx,%%edi\n\t"
+"		shll $3,%%edi\n\t"
+"		addl %%edi,%%edx\n\t"
 		: "=d" (res)
 		: "c" ((size + 31) >> 5), "D" (addr), "b" (addr)
 		);

```

问题

```text
sock.c: In function 'unix_proto_create':
sock.c:331:17: error: lvalue required as left operand of assignment
  331 |   UN_DATA(sock) = upd;
      |                 ^
sock.c: In function 'unix_proto_release':
sock.c:363:17: error: lvalue required as left operand of assignment
  363 |   UN_DATA(sock) = NULL;
      |                 ^
```

解决:

```
diff --git a/net/unix/sock.c b/net/unix/sock.c
index ae264e3..024d01a 100644
--- a/net/unix/sock.c
+++ b/net/unix/sock.c
@@ -328,7 +328,7 @@ unix_proto_create(struct socket *sock, int protocol)
   }
   upd->protocol = protocol;
   upd->socket = sock;
-  UN_DATA(sock) = upd;
+  UN_DATA_FIX(sock) = upd;
   upd->refcnt = 1;	/* Now its complete - bgm */
   dprintf(1, "UNIX: create: allocated data 0x%x\n", upd);
   return(0);
@@ -360,7 +360,7 @@ unix_proto_release(struct socket *sock, struct socket *peer)
 	iput(upd->inode);
 	upd->inode = NULL;
   }
-  UN_DATA(sock) = NULL;
+  UN_DATA_FIX(sock) = NULL;
   upd->socket = NULL;
   if (upd->peerupd) unix_data_deref(upd->peerupd);
   unix_data_deref(upd);
diff --git a/net/unix/unix.h b/net/unix/unix.h
index 4ad032a..7ab33b9 100644
--- a/net/unix/unix.h
+++ b/net/unix/unix.h
@@ -50,6 +50,7 @@ extern struct unix_proto_data unix_datas[NSOCKETS];
 
 
 #define UN_DATA(SOCK) 		((struct unix_proto_data *)(SOCK)->data)
+#define UN_DATA_FIX(SOCK) 		((SOCK)->data)
 #define UN_PATH_OFFSET		((unsigned long)((struct sockaddr_un *)0) \
 							->sun_path)
 

```

问题

```text
arp.c:126:27: error: conflicting type qualifiers for 'arp_q'
  126 | struct sk_buff * volatile arp_q = NULL;
      |                           ^~~~~
```

解决:

```
sed 's/extern struct sk_buff \*arp_q/extern struct sk_buff * volatile arp_q/g' -i net/inet/arp.h
```

问题

```text
ld: kernel/kernel.o: in function `__delay':
/app/linux-read/include/linux/delay.h:13: multiple definition of `__delay'; init/main.o:/app/linux-read/include/linux/delay.h:14: first defined here
ld: kernel/kernel.o: in function `udelay':
/app/linux-read/include/linux/delay.h:28: multiple definition of `udelay'; init/main.o:/app/linux-read/include/linux/delay.h:28: first defined here
ld: mm/mm.o: in function `__delay':
/app/linux-read/mm/kmalloc.c:14: multiple definition of `__delay'; init/main.o:/app/linux-read/include/linux/delay.h:14: first defined here
ld: mm/mm.o: in function `udelay':
```

解决:

```

find . -name "*.[sS]" | xargs -I {} sed 's/\b_\([a-zA-Z0-9_]\+\)\b/\1/g' -i {}

```

问题

```text
```

解决:

```
```

问题

```text
```

解决:

```
```
问题

```text
```

解决:

```
```
问题

```text
```

解决:

```
```
问题

```text
```

解决:

```
```
问题

```text
```

解决:

```
```





---

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
