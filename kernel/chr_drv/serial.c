/*
 *  linux/kernel/serial.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *    serial.c
 *
 * This module implements the rs232 io functions
 *    void rs_write(struct tty_struct * queue);
 *    void rs_init(void);
 * and all interrupts pertaining to serial IO.
 */

#include <asm/io.h>
#include <asm/system.h>
#include <linux/sched.h>
#include <linux/tty.h>

#define WAKEUP_CHARS (TTY_BUF_SIZE / 4)

extern void rs1_interrupt(void);
extern void rs2_interrupt(void);

/**
 * @brief 初始化串行端口
 *
 * @param port 串行端口基地址, 串口1=0x3F8; 串口2=0x2F8
 */
static void init(int port)
{
    outb_p(0x80, port + 3); /* set DLAB of line control reg */
    outb_p(0x30, port);     /* LS of divisor (48 -> 2400 bps */
    outb_p(0x00, port + 1); /* MS of divisor */
    outb_p(0x03, port + 3); /* reset DLAB */
    outb_p(0x0b, port + 4); /* set DTR,RTS, OUT_2 */
    outb_p(0x0d, port + 1); /* enable all intrs but writes */
    (void)inb(port);        /* read data port to reset things (?) */
}

/**
 * @brief 初始化串行端口
 */
void rs_init(void)
{
    set_intr_gate(0x24, rs1_interrupt);
    set_intr_gate(0x23, rs2_interrupt);
    init(tty_table[64].read_q->data);
    init(tty_table[65].read_q->data);

    outb(inb_p(0x21) & 0xE7, 0x21); /* 打开 UART 中断 */
}

/**
 * @brief 写串行端口
 *
 * This routine gets called when tty_write has put something into
 * the write_queue. It must check wheter the queue is empty, and
 * set the interrupt register accordingly
 *
 *    void _rs_write(struct tty_struct * tty);
 *
 * 该函数实际上只是开启发送保持寄存器已空中断标志, 此后当发送保持寄存器空时, UART 就会
 * 产生中断请求. 而在该串行中断处理过程中, 程序会取出写队列尾指针处的字符, 并输出到发
 * 送保持寄存器中. 一旦 UART 把该字符发送了出去, 发送保持寄存器又会变空而引发中断请求.
 * 于是只要写队列中还有字符, 系统就会重复这个处理过程, 把字符一个一个地发送出去. 当写
 * 队列中所有字符都发送了出去, 写队列变空了, 中断处理程序就会把中断允许寄存器中的发送
 * 保持寄存器中断允许标志复位掉, 从而再次禁止发送保持寄存器空引发中断请求. 此次“循环”
 * 发送操作也随之结束
 *
 * @param tty
 */
void rs_write(struct tty_struct *tty)
{
    cli();

    if (!EMPTY(tty->write_q)) {
        /* 发送保持寄存器中断允许标志(位1)置位 */
        outb(inb_p(tty->write_q->data + 1) | 0x02, tty->write_q->data + 1);
    }

    sti();
}
