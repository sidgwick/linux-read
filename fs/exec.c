/*
 *  linux/fs/exec.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * #!-checking implemented by tytso.
 */

/*
 * Demand-loading implemented 01.12.91 - no need to read anything but
 * the header into memory. The inode of the executable is put into
 * "current->executable", and page faults do the actual loading. Clean.
 *
 * Once more I can proudly say that linux stood up to being changed: it
 * was less than 2 hours work to get demand-loading completely implemented.
 */

/* 数据段的内存空间划分:
 * 普通数据内容 --- STACK --- ARG_PAGES --- LIBRARY */

#include <a.out.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>

#include <asm/segment.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>

extern int sys_exit(int exit_code);
extern int sys_close(int fd);

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES 32

/**
 * @brief 加载共享库(shared library)
 *
 * @param library 库文件路径
 * @return int 成功返回 0, 失败返回错误码
 */
int sys_uselib(const char *library)
{
    struct m_inode *inode;
    unsigned long base;

    /* 检查数据段的长度, 主要目的是判断当前进程是否普通进程 */
    if (get_limit(0x17) != TASK_SIZE) {
        return -EINVAL;
    }

    if (library) {
        if (!(inode = namei(library))) {
            /* get library inode */
            return -ENOENT;
        }
    } else {
        inode = NULL;
    }

    /* we should check filetypes (headers etc), but we don't
     * 下面先把原来的共享库资源释放掉
     * 然后将新共享库挂接到进程的 library 字段
     * TODO-DONE: library 是啥时候被加载到内存的?
     * 答: 在缺页中断中加载 */
    iput(current->library);
    current->library = NULL;
    base = get_base(current->ldt[2]);
    base += LIBRARY_OFFSET;
    free_page_tables(base, LIBRARY_SIZE);
    current->library = inode;
    return 0;
}

/**
 * @brief 初始化参数和环境变量表
 *
 * 假如有 argv = ["Hello", "world"], env=["a=A", "b=B", "c=C"]
 * 处理完成后栈的样子(假如原来的 stack pointer = 0xF00):
 *       0xF00 00 a=A
 *       0xEFC 04 b=B
 *       0xEF8 08 c=C
 *       0xEF4 0C 0
 *       0xEF0 10 Hello
 *       0xEEC 10 World
 *       0xEE8 14 0
 *       0xEE4 18 envp(0xF00)
 *       0xEE0 1C argv(0xEF0)
 *       0xEDC 20 argc(2)
 *       0xED8 24 -- sp --
 *
 * 这里就很容易看出来, 普通程序 `main(int argc, char **argv)` 里面的 argc 和 argv,
 * 就是这么来的, 你想的话, 甚至可以在 argv 后面在加上 envp 变量
 *
 * create_tables() parses the env- and arg-strings in new user
 * memory and creates the pointer tables from them, and puts their
 * addresses on the "stack", returning the new stack pointer value
 *
 * @param p
 * @param argc
 * @param envc
 * @return unsigned long* 最新的 stack pointer 值
 */
static unsigned long *create_tables(char *p, int argc, int envc)
{
    unsigned long *argv, *envp;
    unsigned long *sp;

    /* 将 stack pointer 按照 4 字节对齐, 因为 sp 向下生长, 因此这里直接切削即可 */
    sp = (unsigned long *)(0xfffffffc & (unsigned long)p);
    /* 预留出 envp 和 argv 所需的空间 */
    sp -= envc + 1;
    envp = sp;
    sp -= argc + 1;
    argv = sp;

    /* 将指向 env 的指针(envp), 指向 argv 的指针(argv), argc 放到栈上 */
    put_fs_long((unsigned long)envp, --sp);
    put_fs_long((unsigned long)argv, --sp);
    put_fs_long((unsigned long)argc, --sp);

    /* 将 argv 的内容放置到栈上面 */
    while (argc-- > 0) {
        put_fs_long((unsigned long)p, argv++);
        while (get_fs_byte(p++)) {
            /* 把 p 指向的内容都当做 NULL 结尾的字符串处理, 这里的循环是在先找到下一个字符串 */
        }
    }

    put_fs_long(0, argv);

    /* 把 env 塞到栈中 */
    while (envc-- > 0) {
        put_fs_long((unsigned long)p, envp++);
        while (get_fs_byte(p++)) /* nothing */
            ;
    }

    put_fs_long(0, envp);

    return sp;
}

/**
 * @brief 求出 argv 指向的参数集, 有多少参数
 *
 * count() counts the number of arguments/envelopes
 *
 * argv 的内容应该像这样: argv=["aaa", "bbb", NULL]
 *
 * @param argv
 * @return int 参数的个数
 */
static int count(char **argv)
{
    int i = 0;
    char **tmp;

    if ((tmp = argv)) {
        while (get_fs_long((unsigned long *)(tmp++))) {
            i++;
        }
    }

    return i;
}

/**
 * @brief 复制指定个数的参数字符串到参数和环境空间中
 *
 * 'copy_string()' copies argument/envelope strings from user
 * memory to free pages in kernel mem. These are in a format ready
 * to be put directly into the top of new user memory.
 *
 * Modified by TYT, 11/24/91 to add the from_kmem argument, which specifies
 * whether the string and the string array are from user or kernel segments:
 *
 * from_kmem     argv *        argv **
 *    0          user space    user space
 *    1          kernel space  user space
 *    2          kernel space  kernel space
 *
 * We do this by playing games with the fs segment register.  Since it
 * it is expensive to load a segment register, we try to avoid calling
 * set_fs() unless we absolutely have to.
 *
 * 在 do_execve 函数中, p 初始化为指向参数表(128kB)空间的最后一个长字处, 参数字符串
 * 是以堆栈操作方式逆向往其中复制存放的. 因此 p 会随着复制信息的增加而逐渐减小, 并始终
 * 指向参数字符串的头部.
 * 字符串来源标志 from_kmem 应该是 TYT 为了给 execve 增添执行脚本文件的功能而新加的
 * 参数, 当没有运行脚本文件的功能时, 所有参数字符串都在用户数据空间中
 *
 * from_kmem 把函数弄得一团糟.....
 *
 * @param argc 欲添加的参数个数
 * @param argv 参数指针数组
 * @param page 参数和环境空间页面指针数组
 * @param p 参数表空间中偏移指针, 始终指向已复制串的头部
 * @param from_kmem 字符串来源标志
 * @return unsigned long 参数和环境空间当前头部指针, 若出错则返回 0
 */
static unsigned long copy_strings(int argc, char **argv, unsigned long *page, unsigned long p,
                                  int from_kmem)
{
    char *tmp, *pag;
    int len, offset = 0;
    unsigned long old_fs, new_fs;

    if (!p) {
        return 0; /* bullet-proofing */
    }

    new_fs = get_ds(); /* 内核空间数据段 */
    old_fs = get_fs(); /* 用户空间数据段 */

    /* 内核空间互拷 */
    if (from_kmem == 2) {
        set_fs(new_fs);
    }

    while (argc-- > 0) {
        /* 内核空间到用户空间 */
        if (from_kmem == 1) {
            set_fs(new_fs);
        }

        /* 最后一个参数的地址 */
        if (!(tmp = (char *)get_fs_long(((unsigned long *)argv) + argc))) {
            panic("argc is wrong");
        }

        if (from_kmem == 1) {
            set_fs(old_fs);
        }

        /* 参数字符串的长度 */
        len = 0; /* remember zero-padding */
        do {
            len++;
        } while (get_fs_byte(tmp++));

        /* 合法性检查, p 指向的是 ARG PAGE 内的指针,
         * 如果这里小于 0 了, 说明 ARG PAGE 放不下全部数据 */
        if (p - len < 0) { /* this shouldn't happen - 128kB */
            set_fs(old_fs);
            return 0;
        }

        /* 倒着拷贝字符串 */
        while (len) {
            --p;
            --tmp;
            --len;
            if (--offset < 0) {
                /* 这里说明当前页, 被完全用光了, 或根本就不存在当前页, 需要分配新页面 */
                offset = p % PAGE_SIZE;
                if (from_kmem == 2) {
                    set_fs(old_fs);
                }

                /* p 指向的是 arg pages 里面的相对(arg page 开始)的地址, 使用除法就可以算出,
                 * 正在分配的页面位于 arg page 数组的第几个上
                 * if 条件里面, 先确保这个 page 确实不存在, 然后分配这个 page */
                if (!(pag = (char *)page[p / PAGE_SIZE]) &&
                    !(pag = (char *)(page[p / PAGE_SIZE] = get_free_page()))) {
                    return 0;
                }

                if (from_kmem == 2) {
                    set_fs(new_fs);
                }
            }

            *(pag + offset) = get_fs_byte(tmp);
        }
    }

    if (from_kmem == 2) {
        set_fs(old_fs);
    }

    return p;
}

/**
 * @brief 修改任务的局部描述符表内容
 *
 * 修改 LDT 中描述符的段基址和段限长, 并将参数和环境空间页面放置在数据段末端
 *
 * @param text_size 执行文件头部中 a_text 字段给出的代码段长度值
 * @param page 参数和环境空间页面指针数组
 * @return unsigned long 数据段限长值(64MB)
 */
static unsigned long change_ldt(unsigned long text_size, unsigned long *page)
{
    unsigned long code_limit, data_limit, code_base, data_base;
    int i;

    code_limit = TASK_SIZE;
    data_limit = TASK_SIZE;
    code_base = get_base(current->ldt[1]);
    data_base = code_base;
    set_base(current->ldt[1], code_base);
    set_limit(current->ldt[1], code_limit);
    set_base(current->ldt[2], data_base);
    set_limit(current->ldt[2], data_limit);
    /* make sure fs points to the NEW data segment */
    __asm__("pushl $0x17\n\tpop %%fs" ::);

    /* data_base 指向了数据段结尾前面 LIBRARY_SIZE 的地方
     * 然后倒着在数据段空间(逻辑内存空间)分配 MAX_ARG_PAGES 个页面, 页面对应的物理内存在 page[i] 里面 */
    data_base += data_limit - LIBRARY_SIZE;
    for (i = MAX_ARG_PAGES - 1; i >= 0; i--) {
        data_base -= PAGE_SIZE;
        if (page[i]) {
            put_dirty_page(page[i], data_base);
        }
    }

    return data_limit;
}

/**
 * @brief 执行新程序(execve 系统中断调用函数)
 *
 * 'do_execve()' executes a new program.
 *
 * NOTE! We leave 4MB free at the top of the data-area for a loadable
 * library.
 *
 * 调用本函数的时候, 栈的状态如下:
 * stack = (EIP, EIP0, EIP, EBX, ECX, EDX, ORIGI_EAX, FS, ES, DS, EIP, CS, EFLAGS, ESP, SS, ...)
 *          ^    ^          ^                                     ^
 *          |    |          sys_call --------------------------<| int0x80
 *          |    sys_execve
 *          do_execve
 *
 * @param eip 调用系统中断(int0x80)时的程序代码指针
 * @param tmp 系统中断中在调用 sys_execve 时的返回地址, 此处不关注
 * @param filename 被执行程序文件名指针
 * @param argv 命令行参数指针数组的指针
 * @param envp 环境变量指针数组的指针
 * @return int
 */
int do_execve(unsigned long *eip, long tmp, char *filename, char **argv, char **envp)
{
    struct m_inode *inode;
    struct buffer_head *bh;
    struct exec ex; /* 即将执行的可执行程序指针 */

    /* 存放参数和环境变量的页面, 真实的内存分配在 copy_strings 里面按需分配 */
    unsigned long page[MAX_ARG_PAGES];

    int i, argc, envc;
    int e_uid, e_gid;
    int retval;
    int sh_bang = 0;

    /* p 指向 ARG PAGE 最后可用指针的位置
     * TODO: DEBUG: 这里为啥要减去 4, 在 copy_strings 里面, 是 p-- 处理的 */
    unsigned long p = PAGE_SIZE * MAX_ARG_PAGES - 4;

    /* eip[1] 实际上是 CS 的值, 如果我们是从 ring3 来的, 那么 CS 应该是 0x0F=0000_1_1_11
     * 这个地方就是检查一下 CS 的情况, 确保只有 ring3 才能执行 do_execve
     * 必须是 ring3 的原因是内核代码是常驻内存而不能被替换掉的 */
    if ((0xffff & eip[1]) != 0x000f) {
        panic("execve called from supervisor mode");
    }

    /* ARGS page 清空 */
    for (i = 0; i < MAX_ARG_PAGES; i++) { /* clear page-table */
        page[i] = 0;
    }

    /* 找到可执行文件的 inode */
    if (!(inode = namei(filename))) { /* get executables inode */
        return -ENOENT;
    }

    /* 拿到参数数量和环境变量的数量 */
    argc = count(argv);
    envc = count(envp);

/* restart interpreter */
restart_interp:
    if (!S_ISREG(inode->i_mode)) { /* must be regular file */
        retval = -EACCES;
        goto exec_error2;
    }

    i = inode->i_mode;

    /* 检查权限 */
    e_uid = (i & S_ISUID) ? inode->i_uid : current->euid;
    e_gid = (i & S_ISGID) ? inode->i_gid : current->egid;

    if (current->euid == inode->i_uid) {
        i >>= 6;
    } else if (in_group_p(inode->i_gid)) {
        i >>= 3;
    }

    /* 拆解一下这个表达式 `!X && !(A && B)` 为 `!(X || (A && B))`, 于是原式变成了
     *
     * `!((i & 1) || ((inode->i_mode & 0111) && super()))`
     *
     * 解读为满足以下任意一个, 就可以执行, 否则报错:
     *  1. 有执行权限
     *  2. 文件本身有执行权限, 且用户是超级用户
     */
    if (!(i & 1) && !((inode->i_mode & 0111) && suser())) {
        retval = -ENOEXEC;
        goto exec_error2;
    }

    /* 读取文件的第一个数据块
     * TODO-DONE: 更多的数据块是怎么加载进来的???
     * 答: 在缺页异常的处理过程, 有具体的加载逻辑 */
    if (!(bh = bread(inode->i_dev, inode->i_zone[0]))) {
        retval = -EACCES;
        goto exec_error2;
    }

    ex = *((struct exec *)bh->b_data); /* read exec-header */

    /* 如果是脚本文件, 进到 if 里面去处理 */
    if ((bh->b_data[0] == '#') && (bh->b_data[1] == '!') && (!sh_bang)) {
        /*
         * This section does the #! interpretation.
         * Sorta complicated, but hopefully it will work.  -TYT
         */

        char buf[128], *cp, *interp, *i_name, *i_arg;
        unsigned long old_fs;

        /* 从缓冲区里面拷贝 127 字节
         * TODO: 权且认为这里面已经完整读取了第一行 */
        strncpy(buf, bh->b_data + 2, 127);
        brelse(bh);
        iput(inode);
        buf[127] = '\0';

        /* 只截取第一行的内容处理 */
        if ((cp = strchr(buf, '\n'))) {
            *cp = '\0';
            for (cp = buf; (*cp == ' ') || (*cp == '\t'); cp++)
                /* 跳过 sh bang 之后的空格和制表符 */;
        }

        /* 找不到解释器脚本 */
        if (!cp || *cp == '\0') {
            retval = -ENOEXEC; /* No interpreter name found */
            goto exec_error1;
        }

        /* 解析解释器程序路径以及解析器程序名字 */
        interp = i_name = cp;
        i_arg = 0;
        for (; *cp && (*cp != ' ') && (*cp != '\t'); cp++) {
            if (*cp == '/') {
                i_name = cp + 1;
            }
        }

        /* 下面调整 i_arg 指向参数(如有) */
        if (*cp) {
            *cp++ = '\0';
            i_arg = cp;
        }

        /*
         * OK, we've parsed out the interpreter name and
         * (optional) argument.
         *
         * 这部分是 execve 的原始入参, 只拷贝一遍就行
         */
        if (sh_bang++ == 0) {
            p = copy_strings(envc, envp, page, p, 0);
            p = copy_strings(--argc, argv + 1, page, p, 0);
        }

        /* 下面是解析得到的 sh bang 指定的参数, 将他们都复制到 ARG PAGE 里面
         *
         * Splice in (1) the interpreter's name for argv[0]
         *           (2) (optional) argument to interpreter
         *           (3) filename of shell script
         *
         * This is done in reverse order, because of how the
         * user environment and arguments are stored.
         */
        p = copy_strings(1, &filename, page, p, 1);
        argc++;
        if (i_arg) {
            p = copy_strings(1, &i_arg, page, p, 2);
            argc++;
        }

        p = copy_strings(1, &i_name, page, p, 2);
        argc++;
        if (!p) {
            retval = -ENOMEM;
            goto exec_error1;
        }

        /* 上面一通拷贝下来, 程序参数变成了这样子:
         * i_name i_args... script_filename, script_args..., envs... */

        /*
         * OK, now restart the process with the interpreter's inode.
         *
         * 因为 interp 是内核空间数据, 因此要先把 fs 设置为内核数据段, namei 才能正常工作
         */
        old_fs = get_fs();
        set_fs(get_ds());
        if (!(inode = namei(interp))) { /* get executables inode */
            set_fs(old_fs);
            retval = -ENOENT;
            goto exec_error1;
        }

        set_fs(old_fs);
        goto restart_interp;
    }

    /* 接下来解析可执行文件的头部 */
    brelse(bh);

    /* 对于 Linux 0.12 内核来说, 它仅支持 ZMAGIC 执行文件格式, 并且执行文件代码都从逻辑
     * 地址 0 开始执行, 因此不支持含有代码或数据重定位信息的执行文件, 另外如果执行文件太大
     * 或者执行文件残缺不全, 也不能运行它
     *
     * 因此对于下列情况将不执行程序:
     *  1. 如果执行文件不是需求页可执行文件(ZMAGIC)
     *  2. 代码和数据重定位部分长度不等于 0
     *  3. (代码段+数据段+堆)的长度超过 50MB
     *  4. 执行文件长度小于(代码段+数据段+符号表长度+执行头部分)长度的总和 */
    if (N_MAGIC(ex) != ZMAGIC || ex.a_trsize || ex.a_drsize ||
        ex.a_text + ex.a_data + ex.a_bss > 0x3000000 ||
        inode->i_size < ex.a_text + ex.a_data + ex.a_syms + N_TXTOFF(ex)) {
        retval = -ENOEXEC;
        goto exec_error2;
    }

    /* 如果执行文件中代码开始处没有位于 1 个页面(1024字节)边界处, 则也不能执行.
     * 因为需求页(Demand paging)技术要求加载执行文件内容时以页面为单位, 因此要求执行
     * 文件映像中代码和数据都从页面边界处开始. */
    if (N_TXTOFF(ex) != BLOCK_SIZE) {
        printk("%s: N_TXTOFF != BLOCK_SIZE. See a.out.h.", filename);
        retval = -ENOEXEC;
        goto exec_error2;
    }

    /* sh_bang 标志已经设置, 在脚本解释程序部分环境变量页面已经复制, 无须再复制 */
    if (!sh_bang) {
        /* 指针 p 随着复制信息增加而逐渐向小地址方向移动, 因此这两个复制串函数执行完后, 环境
         * 参数串信息块位于程序参数串信息块的上方, 并且 p 指向程序的第 1 个参数串
         * 事实上, p 是 128KB 参数和环境空间中的偏移值
         * 因此如果 p=0, 则表示环境变量与参数空间页面已经被占满, 容纳不下了 */
        p = copy_strings(envc, envp, page, p, 0);
        p = copy_strings(argc, argv, page, p, 0);
        if (!p) {
            retval = -ENOMEM;
            goto exec_error2;
        }
    }

    /* OK, This is the point of no return */
    /* note that current->library stays unchanged by an exec */
    /* 原来的那个 executable 文件引用, 减去 1 */
    if (current->executable) {
        iput(current->executable);
    }

    /* 替换可执行文件节点 */
    current->executable = inode;

    /* 信号处理, 只继承那些 SIG_IGN 的, 其他的一律设置为 NULL */
    current->signal = 0;
    for (i = 0; i < 32; i++) {
        current->sigaction[i].sa_mask = 0;
        current->sigaction[i].sa_flags = 0;
        if (current->sigaction[i].sa_handler != SIG_IGN) {
            current->sigaction[i].sa_handler = NULL;
        }
    }

    /* 如果设置了 close_on_exec 标记, 关闭对应的文件描述符
     * TODO-DONE: 了解 close_on_exec 的更多用途
     * 答: 主要是防止文件描述符泄露, 当一个进程执行 exec 系统调用加载新程序时,
     *     如果不使用 close_on_exec, 所有打开的文件描述符都会被新程序继承. 这可能导致:
     *     - 安全风险: 敏感文件被新程序意外访问
     *     - 资源泄露: 不必要的文件描述符占用系统资源
     *     - 意外行为: 新程序可能误操作父进程打开的文件 */
    for (i = 0; i < NR_OPEN; i++) {
        if ((current->close_on_exec >> i) & 1) {
            sys_close(i);
        }
    }

    current->close_on_exec = 0;

    /* 释放掉原来程序占用的页面, 本程序如有需要, 会引起缺页异常, 到时候单独在申请即可
     *
     * 注意: 此时新执行文件并没有占用主内存区任何页面, 因此在处理器真正运行新执行文件
     * 代码时就会引起缺页异常中断, 此时内存管理程序即会执行缺页处理, 进而为新执行文件
     * 申请内存页面和设置相关页表项, 并且把相关执行文件页面读入内存中 */
    free_page_tables(get_base(current->ldt[1]), get_limit(0x0f));
    free_page_tables(get_base(current->ldt[2]), get_limit(0x17));

    /* 如果上次任务使用了协处理器指向的是当前进程, 则将其置空, 并复位使用了协处理器的标志 */
    if (last_task_used_math == current) {
        last_task_used_math = NULL;
    }

    current->used_math = 0;

    /* 到 create_tables 调用, p 整体的计算是这样的:
     *
     * p = MAX_ARG_PAGES * PAGE_SIZE - 4 - len(ARGS) + data_lim - (LIBRARY_SIZE + MAX_ARG_PAGES * PAGE_SIZE)
     *   = data_lim - LIBRARY_SIZE - len(ARGS)  - 4
     *
     * 式子里面 len(ARGS) 是在 copy_strings 操作中, 拷贝的字符串总长度
     *
     * 因此, p 指向的是 ARGS 之前的那个低一个可用字长处(有可能在 ARG PAGES 里面, 因为 args 占不满这些页面)
     * 但是要注意, p 是从数据段(实际上也是代码段)开始位置算起的, 属于一个进程中的偏移量, 不是相对 4GB 空间的
     */

    /* 把含有入参和环境变量参数的 page 插到线性地址空间里面 */
    p += change_ldt(ex.a_text, page);
    /* 把 p 指向 ARG PAGE 开始位置, 这个位置也是栈底 */
    p -= LIBRARY_SIZE + MAX_ARG_PAGES * PAGE_SIZE;
    /* 在栈上记录参数和环境变量的信息, 这样用户程序 main 函数就能拿到这些值了 */
    p = (unsigned long)create_tables((char *)p, argc, envc);

    /* brk 用于指明进程当前数据段(包括未初始化数据部分)末端位置, 供内核为进程分配内存时指定分配开始位置
     * TODO-DONE: start_stack 的设置方法 argc/argv/enpv 还能取到???
     * 答: start_stack 只是设置了栈底位置, 栈顶还是 p. 但是这个栈底似乎没什么作用, 没有地方会使用到它
     * TODO: 确认 brk 是堆开始的位置 */

    current->end_code = ex.a_text;                     /* 代码段结束位置 */
    current->end_data = ex.a_data + current->end_code; /* 数据段结束位置 */
    current->brk = ex.a_bss + current->end_data;       /* 进程堆结束位置 */
    current->start_stack = p & 0xfffff000;             /* 栈底设置(似乎没啥用) */
    current->suid = current->euid = e_uid;
    current->sgid = current->egid = e_gid;
    eip[0] = ex.a_entry; /* eip, magic happens :-) */
    eip[3] = p;          /* stack pointer, 栈顶 */
    return 0;

exec_error2:
    iput(inode);

exec_error1:
    for (i = 0; i < MAX_ARG_PAGES; i++) {
        free_page(page[i]);
    }

    return (retval);
}
