/*
 *  linux/lib/wait.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#define __LIBRARY__
#include <linux/types.h>
#include <linux/unistd.h>

_syscall3(pid_t, waitpid, pid_t, pid, int *, wait_stat, int, options)

    pid_t wait(int *wait_stat)
{
    return waitpid(-1, wait_stat, 0);
}
