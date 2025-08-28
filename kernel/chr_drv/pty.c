/*
 *  linux/kernel/chr_drv/pty.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *    pty.c
 *
 * This module implements the pty functions
 *    void mpty_write(struct tty_struct * queue);
 *    void spty_write(struct tty_struct * queue);
 */

#include <asm/io.h>
#include <asm/system.h>
#include <linux/sched.h>
#include <linux/tty.h>

/**
 * @brief 从 from 的写队列拷贝数据到 to 的读队列
 *
 * 一直拷贝直到:
 *  1. from 的队列为空
 *  2. to 的辅助队列已满
 *  3. 当前任务收到非屏蔽信号
 *
 * @param from
 * @param to
 */
static inline void pty_copy(struct tty_struct *from, struct tty_struct *to)
{
    char c;

    while (!from->stopped && !EMPTY(from->write_q)) {
        if (FULL(to->read_q)) {
            if (FULL(to->secondary)) {
                break;
            }
            copy_to_cooked(to);
            continue;
        }

        GETCH(from->write_q, c);
        PUTCH(c, to->read_q);
        if (current->signal & ~current->blocked) {
            break;
        }
    }

    copy_to_cooked(to);
    wake_up(&from->write_q->proc_list);
}

/**
 * @brief 主终端写入
 *
 * This routine gets called when tty_write has put something into
 * the write_queue. It copies the input to the output-queue of it's
 * slave.
 *
 * @param tty
 */
void mpty_write(struct tty_struct *tty)
{
    int nr = tty - tty_table;

    /* 主伪终端, 编号从 128 开始, 因此它除以 64, 应该余 2 */
    if ((nr >> 6) != 2) {
        printk("bad mpty\n\r");
    } else {
        pty_copy(tty, tty + 64);
    }
}

/**
 * @brief 从终端写入
 *
 * @param tty
 */
void spty_write(struct tty_struct *tty)
{
    int nr = tty - tty_table;

    /* 从伪终端, 编号从 192 开始, 因此它除以 64, 应该余 3 */
    if ((nr >> 6) != 3) {
        printk("bad spty\n\r");
    } else {
        pty_copy(tty, tty - 64);
    }
}
