/*
 *  linux/fs/file_table.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <linux/fs.h>

/**
 * @brief 文件表数组
 *
 * 这个数组里面的对象 (f_count == 0) 就认为是空闲的
 */
struct file file_table[NR_FILE];
