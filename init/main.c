/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <time.h>
#include <unistd.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */

/* fork 函数复制了一份当前运行任务的副本
 * 然后更新属于新进程栈等等参数, 追加到任务队列里面, 等待调度 */
inline _syscall0(int, fork);
inline _syscall0(int, pause);
static inline _syscall1(int, setup, void *, BIOS);
inline _syscall0(int, sync);

#include <asm/io.h>
#include <asm/system.h>
#include <linux/head.h>
#include <linux/sched.h>
#include <linux/tty.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/fs.h>

#include <string.h>

static char printbuf[1024];

extern int vsprintf(char *buf, const char *fmt, va_list args);
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm *tm);

static int sprintf(char *str, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    i = vsprintf(str, fmt, args);
    va_end(args);
    return i;
}

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)                  // 超出 1M 部分的扩展内存
#define CON_ROWS ((*(unsigned short *)0x9000e) & 0xff)          // 屏幕的行数
#define CON_COLS (((*(unsigned short *)0x9000e) & 0xff00) >> 8) // 屏幕的列数
#define DRIVE_INFO (*(struct drive_info *)0x90080)              // 硬盘参数
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)              // 根文件系统所在设备号
#define ORIG_SWAP_DEV (*(unsigned short *)0x901FA)              // 交换分区所在设备号

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr)                                                                            \
    ({                                                                                             \
        outb_p(0x80 | addr, 0x70);                                                                 \
        inb_p(0x71);                                                                               \
    })

// BCD 码转正常的二进制数字
#define BCD_TO_BIN(val) ((val) = ((val) & 15) + ((val) >> 4) * 10)

/**
 * 从 CMOS 里面读取时间日期信息 */
static void time_init(void)
{
    struct tm time;

    do {
        time.tm_sec = CMOS_READ(0);
        time.tm_min = CMOS_READ(2);
        time.tm_hour = CMOS_READ(4);
        time.tm_mday = CMOS_READ(7);
        time.tm_mon = CMOS_READ(8);
        time.tm_year = CMOS_READ(9);
    } while (time.tm_sec != CMOS_READ(0));

    // CMOS 存储的是 BCD 码, 需要转化为二进制

    BCD_TO_BIN(time.tm_sec);
    BCD_TO_BIN(time.tm_min);
    BCD_TO_BIN(time.tm_hour);
    BCD_TO_BIN(time.tm_mday);
    BCD_TO_BIN(time.tm_mon);
    BCD_TO_BIN(time.tm_year);

    time.tm_mon--; // 注意这里给月份减了 1
    startup_time = kernel_mktime(&time);
}

static long memory_end = 0;        // 机器具有的物理内存容量
static long buffer_memory_end = 0; // 高速缓冲区末端地址
static long main_memory_start = 0; // 主内存(将用于分页)开始的位置
static char term[32];              // 终端设置字符串(环境参数)

// 读取并执行 /etc/rc 文件时所使用的命令行参数和环境参数
static char *argv_rc[] = {"/bin/sh", NULL};      // 调用执行程序时参数的字符串数组
static char *envp_rc[] = {"HOME=/", NULL, NULL}; // 调用执行程序时的环境字符串数组

// 运行登录 shell 时所使用的命令行参数和环境参数
// 第 122 行中 argv[0] 中的字符 `-` 是传递给 sh 的一个标志, 通过识别该标志,
// sh 程序会作为登录 shell 执行. 其执行过程与在 shell 提示符下执行 sh 不一样
static char *argv[] = {"-/bin/sh", NULL};
static char *envp[] = {"HOME=/usr/root", NULL, NULL};

struct drive_info {
    char dummy[32];
} drive_info; // 用于存放硬盘参数表信息

/**/

/**
 * @brief kernel C 语言入口函数
 *
 * The startup routine assumes (well, ...) this
 *
 * main 函数进来的时候, push 了一堆东西.
 * 从函数栈帧角度来看, 进到 main 执行的时候, 栈上应该是(左顶右底): IP, P1, P2, P3 ..
 * TODO: setup.s 里面网栈上面压了三个 0, 怎么说没有参数呢?
 *
 *         |--- This really IS void, no error here.
 *         V    */
void main(void)
{
    /*
     * Interrupts are still disabled. Do necessary setups, then
     * enable them
     */
    ROOT_DEV = ORIG_ROOT_DEV;
    SWAP_DEV = ORIG_SWAP_DEV;
    sprintf(term, "TERM=con%dx%d", CON_COLS, CON_ROWS);
    envp[1] = term;
    envp_rc[1] = term;
    drive_info = DRIVE_INFO;

    /* 接着根据机器物理内存容量设置高速缓冲区和主内存区的位置和范围 */

    /* 内存大小=1Mb + 扩展内存(k)*1024字节 */
    memory_end = (1 << 20) + (EXT_MEM_K << 10);
    memory_end &= 0xfffff000; // 忽略掉高处没有 4K 对齐的部分
    if (memory_end > 16 * 1024 * 1024) {
        /* 最多支持 16M 内存 */
        memory_end = 16 * 1024 * 1024;
    }

    if (memory_end > 12 * 1024 * 1024) {
        /* 如果内存容量大于 12M, 给 buffer_memory_end 分配 4M */
        buffer_memory_end = 4 * 1024 * 1024;
    } else if (memory_end > 6 * 1024 * 1024) {
        /* 如果内存容量大于 6M, 给 buffer_memory_end 分配 2M */
        buffer_memory_end = 2 * 1024 * 1024;
    } else {
        /* 如果内存容量小于等于 6M, 给 buffer_memory_end 分配 1M */
        buffer_memory_end = 1 * 1024 * 1024;
    }

    main_memory_start = buffer_memory_end;

#ifdef RAMDISK
    /* 如果使用 RAMDISK, 在 buffer_memory_end 和 main_memory_start 之间,
     * 预留出给 RAMDISK 的空间 */
    main_memory_start += rd_init(main_memory_start, RAMDISK * 1024);
#endif

    /* 上面设置完成之后, 内存中的功能布局如下:
     *
     *    Kernel | buffer | RAMDISK | main memory
     */

    /* 以下是内核进行所有方面的初始化工作 */

    mem_init(main_memory_start, memory_end); /* 初始化主内存区域 */
    trap_init();                             /* 设置中断处理程序 */
    blk_dev_init();                          /* 初始化块设备请求数组 */
    chr_dev_init();                          /* 空 */
    tty_init();
    time_init(); /* 初始化开机时间 */
    sched_init();
    buffer_init(buffer_memory_end);
    hd_init();
    floppy_init();
    sti(); /* 开启中断 */
    move_to_user_mode();

    /* 上面必须打开中断, fork 依赖 `int 0x80` 没法运行
     *
     * fork 出一个任务, 然后在那个任务里面执行 init
     * kernel 自身跳到下面的 pause 执行 */
    if (!fork()) { /* we count on this going ok */
        init();
    }

    /*
     *   NOTE!!   For any other task 'pause()' would mean we have to get a
     * signal to awaken, but task0 is the sole exception (see 'schedule()')
     * as task 0 gets activated at every idle moment (when no other tasks
     * can run). For task0 'pause()' just means we go check if some other
     * task can run, and if not we return here.
     */
    for (;;)
        __asm__("int $0x80" : : "a"(__NR_pause));
}

int printf(const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    write(1, printbuf, i = vsprintf(printbuf, fmt, args));
    va_end(args);
    return i;
}

void init(void)
{
    int pid, i;

    setup((void *)&drive_info);

    /* 打开 tty1 文件, 这样 tty1 就和当前会话关联在一起了 */
    (void)open("/dev/tty1", O_RDWR, 0); /* STDIN */
    (void)dup(0);                       /* STDOUT */
    (void)dup(0);                       /* STDERR */

    printf("%d buffers = %d bytes buffer space\n\r", NR_BUFFERS, NR_BUFFERS * BLOCK_SIZE);
    printf("Free mem: %d bytes\n\r", memory_end - main_memory_start);

    /* fork 出来低一个进程, fork 父进程收到 pid, 子进程收到 0 */
    if (!(pid = fork())) {
        /* 子进程执行 */

        close(0); /* 关闭标准输入 */

        /* `/etc/rc` 是标准输入 */
        if (open("/etc/rc", O_RDONLY, 0)) {
            _exit(1);
        }

        /* 使用 sh 执行 `/etc/rc` 中的内容 */
        execve("/bin/sh", argv_rc, envp_rc);
        _exit(2);
    }

    /* 父进程等子进程结束 */
    if (pid > 0) {
        while (pid != wait(&i))
            /* nothing */;
    }

    /* init 进程是个死循环, 永不退出 */
    while (1) {
        if ((pid = fork()) < 0) {
            printf("Fork failed in init\r\n");
            continue;
        }

        if (!pid) {
            /* 子进程 */

            close(0);
            close(1);
            close(2);
            setsid();

            (void)open("/dev/tty1", O_RDWR, 0);
            (void)dup(0);
            (void)dup(0);

            _exit(execve("/bin/sh", argv, envp));
        }

        /* 父进程, 子进程没机会执行到这里 */
        while (1) {
            if (pid == wait(&i)) {
                break;
            }
        }

        printf("\n\rchild %d died with code %04x\n\r", pid, i);
        sync();
    }

    _exit(0); /* NOTE! _exit, not exit() */
}
