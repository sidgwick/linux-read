/*
 *  linux/kernel/sys.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <asm/segment.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/tty.h>
#include <string.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/utsname.h>

/*
 * The timezone where the local system is located.  Used as a default by some
 * programs who obtain this value by using gettimeofday.
 *
 * 第 1 个字段 tz_minuteswest 表示距格林尼治标准时间 GMT 以西的分钟数
 * 第 2 个字段 tz_dsttime 是夏令时 DST(Daylight Savings Time) 调整类型
 * 该结构定义在include/sys/time.h中 */
struct timezone sys_tz = {0, 0};

/* 根据进程组号 pgrp 取得进程组所属会话(session)号
 * 该函数在 kernel/exit.c 中实现 */
extern int session_of_pgrp(int pgrp);

// 返回值是 -ENOSYS 的系统调用函数均表示在本版本内核中还未实现

// 返回日期和时间(ftime = Fetch time)
int sys_ftime()
{
    return -ENOSYS;
}

int sys_break()
{
    return -ENOSYS;
}

// 用于当前进程对子进程进行调试(debugging)
int sys_ptrace()
{
    return -ENOSYS;
}

// 改变并打印终端行设置
int sys_stty()
{
    return -ENOSYS;
}

// 取终端行设置信息
int sys_gtty()
{
    return -ENOSYS;
}

// 修改文件名
int sys_rename()
{
    return -ENOSYS;
}

int sys_prof()
{
    return -ENOSYS;
}

/*
 * This is done BSD-style, with no consideration of the saved gid, except
 * that if you set the effective gid, it sets the saved gid too.  This
 * makes it possible for a setgid program to completely drop its privileges,
 * which is often a useful assertion to make when you are doing a security
 * audit over a program.
 *
 * The general idea is that a program which uses just setregid() will be
 * 100% compatible with BSD.  A program which uses just setgid() will be
 * 100% compatible with POSIX w/ Saved ID's.
 *
 * setregid = SET Real and Effective Group ID
 *
 * 设置当前任务的实际以及/或者有效组ID(gid).
 * 如果任务没有超级用户特权, 那么只能互换其实际组 ID 和有效组 ID.
 * 如果任务具有超级用户特权, 就能任意设置有效的和实际的组 ID.
 * 保留的 gid(saved gid) 被设置成与有效gid. 实际组ID是指进程当前的gid.
 */
int sys_setregid(int rgid, int egid)
{
    if (rgid > 0) {
        if ((current->gid == rgid) || suser())
            current->gid = rgid;
        else
            return (-EPERM);
    }

    if (egid > 0) {
        if ((current->gid == egid) || (current->egid == egid) || suser()) {
            current->egid = egid;
            current->sgid = egid;
        } else
            return (-EPERM);
    }

    return 0;
}

/* setgid() is implemeneted like SysV w/ SAVED_IDS
 *
 * 设置进程组号(gid).
 *
 * 如果任务没有超级用户特权, 它可以使用 setgid 将其 effective gid 设置为成其
 saved gid 或其 real gid
 * 如果任务有超级用户特权, 则实际 gid, 有效 gid 和保留 gid 都被设置成参数指定的
 gid

 */
int sys_setgid(int gid)
{
    if (suser())
        current->gid = current->egid = current->sgid = gid;
    else if ((gid == current->gid) || (gid == current->sgid))
        current->egid = gid;
    else
        return -EPERM;
    return 0;
}

/* 打开或关闭进程计帐功能 */
int sys_acct()
{
    return -ENOSYS;
}

/* 映射任意物理内存到进程的虚拟地址空间 */
int sys_phys()
{
    return -ENOSYS;
}

int sys_lock()
{
    return -ENOSYS;
}

int sys_mpx()
{
    return -ENOSYS;
}

int sys_ulimit()
{
    return -ENOSYS;
}

/* 返回从 1970-01-01 00:00:00 GMT 开始计时的时间值(秒).
 *
 * 如果 tloc 不为 null, 则时间值也存储在那里
 * 由于参数是一个指针, 而其所指位置在用户空间, 因此需要使用函数 put_fs_long()
 * 来访问该值 在进入内核中运行时, 段寄存器 fs 被默认地指向当前用户数据空间.
 * 因此该函数就可利用 fs 来访问用户空间中的值 */
int sys_time(long *tloc)
{
    int i;

    i = CURRENT_TIME;
    if (tloc) {
        verify_area(tloc, 4);
        put_fs_long(i, (unsigned long *)tloc);
    }
    return i;
}

/* Unprivileged users may change the real user id to the effective uid
 * or vice versa.  (BSD-style)
 *
 * When you set the effective uid, it sets the saved uid too.  This
 * makes it possible for a setuid program to completely drop its privileges,
 * which is often a useful assertion to make when you are doing a security
 * audit over a program.
 *
 * The general idea is that a program which uses just setreuid() will be
 * 100% compatible with BSD.  A program which uses just setuid() will be
 * 100% compatible with POSIX w/ Saved ID's.
 *
 * 设置任务的实际以及/或者有效的用户ID(uid)
 *
 * 如果任务没有超级用户特权, 那么只能互换其实际的 uid 和有效的 uid.
 * 如果任务具有超级用户特权, 就能任意设置有效的和实际的用户 ID.
 * 保存的 uid (saved uid) 被设置成与有效 uid 同值 */
int sys_setreuid(int ruid, int euid)
{
    int old_ruid = current->uid;

    if (ruid > 0) {
        if ((current->euid == ruid) || (old_ruid == ruid) || suser())
            current->uid = ruid;
        else
            return (-EPERM);
    }

    if (euid > 0) {
        if ((old_ruid == euid) || (current->euid == euid) || suser()) {
            current->euid = euid;
            current->suid = euid;
        } else {
            current->uid = old_ruid;
            return (-EPERM);
        }
    }

    return 0;
}

/*
 * setuid() is implemeneted like SysV w/ SAVED_IDS
 *
 * Note that SAVED_ID's is deficient in that a setuid root program
 * like sendmail, for example, cannot set its uid to be a normal
 * user and then switch back, because if you're root, setuid() sets
 * the saved uid too.  If you don't like this, blame the bright people
 * in the POSIX commmittee and/or USG.  Note that the BSD-style setreuid()
 * will allow a root program to temporarily drop privileges and be able to
 * regain them by swapping the real and effective uid.
 *
 * 设置任务用户 ID(uid)
 *
 * 如果任务没有超级用户特权, 它可以使用setuid()将其 effective uid 设置成其 saved
 * uid 或其 real uid 如果任务有超级用户特权,
 * 则实际的uid/有效的uid和保存的uid都会被设置成参数指定的uid */
int sys_setuid(int uid)
{
    if (suser())
        current->uid = current->euid = current->suid = uid;
    else if ((uid == current->uid) || (uid == current->suid))
        current->euid = uid;
    else
        return -EPERM;
    return (0);
}

/* 设置系统开机时间.
 * 参数 tptr 是从 1970-01-01 00:00:00 GMT 开始计时的时间值(秒)
 *
 * 调用进程必须具有超级用户权限. 其中 HZ=100, 是内核系统运行频率
 * 函数参数提供的当前时间值减去系统已经运行的时间秒值 `jiffies/HZ`
 * 即是开机时间秒值 */
int sys_stime(long *tptr)
{
    if (!suser())
        return -EPERM;
    startup_time = get_fs_long((unsigned long *)tptr) - jiffies / HZ;
    jiffies_offset = 0;
    return 0;
}

/* 获取当前任务运行时间统计值
 *
 * 在 tbuf 所指用户数据空间处返回 tms 结构的任务运行时间统计值.
 * tms 结构中包括进程用户运行时间, 内核(系统)时间, 子进程用户运行时间,
 * 子进程系统运行时间 函数返回值是系统运行到当前的嘀嗒数 */
int sys_times(struct tms *tbuf)
{
    if (tbuf) {
        verify_area(tbuf, sizeof(*tbuf));
        put_fs_long(current->utime, (unsigned long *)&tbuf->tms_utime);
        put_fs_long(current->stime, (unsigned long *)&tbuf->tms_stime);
        put_fs_long(current->cutime, (unsigned long *)&tbuf->tms_cutime);
        put_fs_long(current->cstime, (unsigned long *)&tbuf->tms_cstime);
    }
    return jiffies;
}

/* 当参数 end_data_seg 数值合理, 并且系统确实有足够的内存,
 * 而且进程没有超越其最大数据段大小时, 该函数设置数据段末尾为 end_data_seg
 * 指定的值. 该值必须大于代码结尾并且要小于堆栈结尾 16384=16KB.
 * 返回值是数据段的新结尾值(如果返回值与要求值不同, 则表明有错误发生)
 * 该函数并不被用户直接调用, 而由 libc 库函数进行包装, 并且返回值也不一样 */
int sys_brk(unsigned long end_data_seg)
{
    if (end_data_seg >= current->end_code && end_data_seg < current->start_stack - 16384)
        current->brk = end_data_seg;
    return current->brk; /* 返回进程当前的数据段结尾值 */
}

/*
 * This needs some heave checking ...
 * I just haven't get the stomach for it. I also don't fully
 * understand sessions/pgrp etc. Let somebody who does explain it.
 *
 * OK, I think I have the protection semantics right.... this is really
 * only important on a multi-user system anyway, to make sure one user
 * can't send a signal to a process owned by another.  -TYT, 12/12/91
 *
 * 设置指定进程 pid 的进程组号为 pgid
 *
 * 参数:
 *      - pid 是指定进程的进程号. 如果它为 0, 则让它等于当前进程的进程号.
 *      - pgid 是指定进程的进程组号. 如果它为 0, 则让它等于当前进程的进程号.
 *
 * 如果该函数用于将进程从一个进程组移到另一个进程组,
 * 则这两个进程组必须属于同一个 会话(session). 在这种情况下, 参数 pgid
 * 指定了要加入的现有进程组 ID, 此时该 组的会话 ID 必须与将要加入进程的相同 */
int sys_setpgid(int pid, int pgid)
{
    int i;

    /* 如果参数 pid 为 0, 则 pid 取值为当前进程的进程号 pid */
    if (!pid)
        pid = current->pid;

    /* 如果参数 pgid 为 0, 则 pgid 也取值为当前进程的 pid (??
     * 这里与POSIX标准的描述有出入) */
    if (!pgid)
        pgid = current->pid;

    /* 若 pgid 小于 0, 则返回无效错误码 */
    if (pgid < 0)
        return -EINVAL;

    /* 扫描任务数组, 查找指定进程号 pid 的任务 */
    for (i = 0; i < NR_TASKS; i++)
        /* 如果找到了进程号是 pid 的进程,
         * 并且该进程的父进程就是当前进程或者该进程 就是当前进程. 这就是说,
         * 自己的父进程或者自己才可以给自己设置 pgid */
        if (task[i] && (task[i]->pid == pid) &&
            ((task[i]->p_pptr == current) || (task[i] == current))) {

            /* 那么若该任务已经是会话首领, 则出错返回 */
            if (task[i]->leader)
                return -EPERM;

            /* 若该任务的会话号(session)与当前进程的不同
             * 或者指定的进程组号 pgid 与 pid 不同并且 pgid
             * 进程组所属会话号与当前进程所属会话号不同 */
            if ((task[i]->session != current->session) ||
                ((pgid != pid) && (session_of_pgrp(pgid) != current->session)))
                return -EPERM;

            /* 把查找到的进程的 pgrp 设置为 pgid, 并返回 0 */
            task[i]->pgrp = pgid;
            return 0;
        }

    /* 若没有找到指定 pid 的进程, 则返回进程不存在出错码 */
    return -ESRCH;
}

/* 返回当前进程的进程组号, 与 getpgid(0) 等同 */
int sys_getpgrp(void)
{
    return current->pgrp;
}

/* 创建一个会话(session)(即设置其leader=1), 并且设置其会话号=其组号=其进程号
 *
 * 如果当前进程已是会话首领并且不是超级用户, 则出错返回.
 * 否则设置当前进程为新会话首领(leader = 1), 并且设置当前进程会话号 session 和
 * 组号 pgrp 都等于进程号 pid, 而且设置当前进程没有控制终端
 *
 * 最后系统调用返回会话号 */
int sys_setsid(void)
{
    if (current->leader && !suser())
        return -EPERM;

    current->leader = 1;
    current->session = current->pgrp = current->pid;
    current->tty = -1;

    return current->pgrp;
}

/* Supplementary group ID's
 * 进程的其他用户组号
 *
 * 取当前进程其他辅助用户组号
 *
 * 任务数据结构中 groups 数组保存着进程同时所属的多个用户组号, 该数组共 NGROUPS
 * 个项, 若某项的值是 NOGROUP, 则表示从该项开始以后所有项都空闲,
 * 否则数组项中保存的是用户组号.
 *
 * 参数
 *      - gidsetsize 是获取的用户组号个数
 *      - grouplist 是存储这些用户组号的用户空间缓存
 * 返回实际含有的用户组号个数 */
int sys_getgroups(int gidsetsize, gid_t *grouplist)
{
    int i;

    if (gidsetsize)
        verify_area(grouplist, sizeof(gid_t) * gidsetsize);

    for (i = 0; (i < NGROUPS) && (current->groups[i] != NOGROUP); i++, grouplist++) {
        if (gidsetsize) {
            if (i >= gidsetsize)
                return -EINVAL;
            put_fs_word(current->groups[i], (short *)grouplist);
        }
    }

    return (i);
}

/* 设置当前进程同时所属的其他辅助用户组号
 *
 * 参数:
 *      - gidsetsize 是将设置的用户组号个数
 *      - grouplist 是含有用户组号的用户空间缓存
 * 正常处理返回 0, 否则返回出错码 */
int sys_setgroups(int gidsetsize, gid_t *grouplist)
{
    int i;

    if (!suser())
        return -EPERM;

    if (gidsetsize > NGROUPS)
        return -EINVAL;

    for (i = 0; i < gidsetsize; i++, grouplist++) {
        current->groups[i] = get_fs_word((unsigned short *)grouplist);
    }

    if (i < NGROUPS)
        current->groups[i] = NOGROUP;

    return 0;
}

/* 判断 grp 在不在当前进程的用户组里面
 * 是则返回 1, 否则返回 0 */
int in_group_p(gid_t grp)
{
    int i;

    /* 如果当前进程的有效组号就是 grp, 则表示进程属于 grp 进程组. 函数返回 1
     * 否则就在进程的辅助用户组数组中扫描是否有 grp 进程组号. 若有则函数也返回 1
     * 若扫描到值为 NOGROUP 的项, 表示已扫描完全部有效项而没有发现匹配的组号,
     * 函数返回 0 */
    if (grp == current->egid)
        return 1;

    for (i = 0; i < NGROUPS; i++) {
        if (current->groups[i] == NOGROUP)
            break;
        if (current->groups[i] == grp)
            return 1;
    }
    return 0;
}

/* utsname 结构含有一些字符串字段, 用于保存系统的名称.
 * 其中包含 5 个字段, 分别是：
 *      - 当前操作系统的名称
 *      - 网络节点名称(主机名)
 *      - 当前操作系统发行级别
 *      - 操作系统版本号
 *      - 系统运行的硬件类型名称 */
static struct utsname thisname = {UTS_SYSNAME, UTS_NODENAME, UTS_RELEASE, UTS_VERSION, UTS_MACHINE};

/* 获取系统名称等信息 */
int sys_uname(struct utsname *name)
{
    int i;

    if (!name)
        return -ERROR;
    verify_area(name, sizeof *name);
    for (i = 0; i < sizeof *name; i++)
        put_fs_byte(((char *)&thisname)[i], i + (char *)name);
    return 0;
}

/* Only sethostname; gethostname can be implemented by calling uname()
 *
 * 设置系统主机名(系统的网络节点名)
 *
 * 参数:
 *      - name 指针指向用户数据区中含有主机名字符串的缓冲区
 *      - len 是主机名字符串长度
 * 正常处理返回 0, 否则返回出错码 */
int sys_sethostname(char *name, int len)
{
    int i;

    if (!suser())
        return -EPERM;

    if (len > MAXHOSTNAMELEN)
        return -EINVAL;

    for (i = 0; i < len; i++) {
        if ((thisname.nodename[i] = get_fs_byte(name + i)) == 0)
            break;
    }

    if (thisname.nodename[i]) {
        thisname.nodename[i > MAXHOSTNAMELEN ? MAXHOSTNAMELEN : i] = 0;
    }

    return 0;
}

/* 取当前进程指定资源的界限值.
 *
 * 进程的任务结构中定义有一个 rlim[RLIM_NLIMITS] 数组,
 * 用于控制进程使用系统资源的界限, 数组每个项是一个 rlimit 结构,
 * 其中包含两个字段: 一个说明进程对指定资源的当前限制 界限(soft limit,
 * 即软限制), 另一个说明系统对指定资源的最大限制界限(hard limit, 即硬限制)
 *
 * rlim 数组的每一项对应系统对当前进程一种资源的界限信息. Linux 0.12
 * 系统共对6种资源规定了界限, 即 RLIM_NLIMITS=6
 *
 * 参数:
 *      - resource 指定我们咨询的资源名称, 实际上它是任务结构中 rlim
 * 数组的索引值
 *      - rlim 是指向 rlimit 结构的用户缓冲区指针, 用于存放取得的资源界限信息
 * 正常处理返回 0, 否则返回出错码 */
int sys_getrlimit(int resource, struct rlimit *rlim)
{
    if (resource >= RLIM_NLIMITS)
        return -EINVAL;

    verify_area(rlim, sizeof *rlim);

    put_fs_long(current->rlim[resource].rlim_cur, (unsigned long *)rlim);
    put_fs_long(current->rlim[resource].rlim_max, ((unsigned long *)rlim) + 1);
    return 0;
}

/* 设置当前进程指定资源的界限值
 * 参数:
 *      - resource 指定我们设置界限的资源名称, 实际上它是任务结构中 rlim
 * 数组的索引值
 *      - rlim 是指向 rlimit 结构的用户缓冲区指针, 用于内核读取新的资源界限信息
 */
int sys_setrlimit(int resource, struct rlimit *rlim)
{
    struct rlimit new, *old;

    if (resource >= RLIM_NLIMITS)
        return -EINVAL;

    old = current->rlim + resource;
    new.rlim_cur = get_fs_long((unsigned long *)rlim);
    new.rlim_max = get_fs_long(((unsigned long *)rlim) + 1);

    /* 超级用户可以调整 rlim_max */
    if (((new.rlim_cur > old->rlim_max) || (new.rlim_max > old->rlim_max)) && !suser())
        return -EPERM;

    *old = new;
    return 0;
}

/* It would make sense to put struct rusuage in the task_struct,
 * except that would make the task_struct be *really big*.  After
 * task_struct gets moved into malloc'ed memory, it would
 * make sense to do this.  It will make moving the rest of the information
 * a lot simpler!  (Which we're not doing right now because we're not
 * measuring them yet).
 *
 * 获取指定进程的资源利用信息,
 * 本系统调用提供当前进程或其已终止的和等待着的子进程资源使用情况
 *
 * 如果参数 who 等于 RUSAGE_SELF, 则返回当前进程的资源利用信息
 * 如果指定进程 who 是 RUSAGE_CHILDREN,
 * 则返回当前进程的已终止和等待着的子进程资源利用信息 */
int sys_getrusage(int who, struct rusage *ru)
{
    struct rusage r;
    unsigned long *lp, *lpend, *dest;

    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN)
        return -EINVAL;

    verify_area(ru, sizeof *ru);
    memset((char *)&r, 0, sizeof(r));

    if (who == RUSAGE_SELF) {
        r.ru_utime.tv_sec = CT_TO_SECS(current->utime);
        r.ru_utime.tv_usec = CT_TO_USECS(current->utime);
        r.ru_stime.tv_sec = CT_TO_SECS(current->stime);
        r.ru_stime.tv_usec = CT_TO_USECS(current->stime);
    } else {
        r.ru_utime.tv_sec = CT_TO_SECS(current->cutime);
        r.ru_utime.tv_usec = CT_TO_USECS(current->cutime);
        r.ru_stime.tv_sec = CT_TO_SECS(current->cstime);
        r.ru_stime.tv_usec = CT_TO_USECS(current->cstime);
    }

    lp = (unsigned long *)&r;
    lpend = (unsigned long *)(&r + 1);
    dest = (unsigned long *)ru;

    for (; lp < lpend; lp++, dest++)
        put_fs_long(*lp, dest);

    return (0);
}

// 取得系统当前时间, 并用指定格式返回
// timeval 结构含有秒和微秒(tv_sec和tv_usec)两个字段
// timezone
// 结构含有本地距格林尼治标准时间以西的分钟数(tz_minuteswest)和夏令时间调整类型(tz_dsttime)两个字段.
int sys_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    if (tv) {
        verify_area(tv, sizeof *tv);
        put_fs_long(startup_time + CT_TO_SECS(jiffies + jiffies_offset), (unsigned long *)tv);
        put_fs_long(CT_TO_USECS(jiffies + jiffies_offset), ((unsigned long *)tv) + 1);
    }

    if (tz) {
        verify_area(tz, sizeof *tz);
        put_fs_long(sys_tz.tz_minuteswest, (unsigned long *)tz);
        put_fs_long(sys_tz.tz_dsttime, ((unsigned long *)tz) + 1);
    }

    return 0;
}

/* The first time we set the timezone, we will warp the clock so that
 * it is ticking GMT time instead of local time.  Presumably,
 * if someone is setting the timezone then we are running in an
 * environment where the programs understand about timezones.
 * This should be done at boot time in the /etc/rc script, as
 * soon as possible, so that the clock can be set right.  Otherwise,
 * various programs will get confused when the clock gets warped.
 *
 * 设置系统当前时间 */
int sys_settimeofday(struct timeval *tv, struct timezone *tz)
{
    static int firsttime = 1;
    void adjust_clock();

    if (!suser())
        return -EPERM;

    if (tz) {
        sys_tz.tz_minuteswest = get_fs_long((unsigned long *)tz);
        sys_tz.tz_dsttime = get_fs_long(((unsigned long *)tz) + 1);
        if (firsttime) {
            firsttime = 0;
            if (!tv)
                adjust_clock();
        }
    }

    if (tv) {
        int sec, usec;

        sec = get_fs_long((unsigned long *)tv);
        usec = get_fs_long(((unsigned long *)tv) + 1);

        /* 开机时间的设置, 注意看我们不调整 jiffies */
        startup_time = sec - jiffies / HZ;

        /* usec 对应的滴答数 - 已经经过的滴答数 */
        jiffies_offset = usec * HZ / 1000000 - jiffies % HZ;
    }

    return 0;
}

/*
 * Adjust the time obtained from the CMOS to be GMT time instead of
 * local time.
 *
 * This is ugly, but preferable to the alternatives.  Otherwise we
 * would either need to write a program to do it in /etc/rc (and risk
 * confusion if the program gets run more than once; it would also be
 * hard to make the program warp the clock precisely n hours)  or
 * compile in the timezone information into the kernel.  Bad, bad....
 *
 * XXX Currently does not adjust for daylight savings time.  May not
 * need to do anything, depending on how smart (dumb?) the BIOS
 * is.  Blast it all.... the best thing to do not depend on the CMOS
 * clock at all, but get the time via NTP or timed if you're on a
 * network....                - TYT, 1/1/92
 *
 * 把系统启动时间调整为以 GMT 为标准的时间
 * startup_time 是秒值, 因此这里需要把时区分钟值乘上 60 */
void adjust_clock()
{
    startup_time += sys_tz.tz_minuteswest * 60;
}

/* 设置当前进程创建文件属性屏蔽码为 mask & 0777, 并返回原屏蔽码 */
int sys_umask(int mask)
{
    int old = current->umask;

    current->umask = mask & 0777;
    return (old);
}
