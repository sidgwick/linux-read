#ifndef _SYS_STAT_H
#define _SYS_STAT_H

/**
 * 文件状态头文件
 */

#include <sys/types.h>

// 这个结构包含了文件信息
struct stat {
    dev_t st_dev;     // 含有文件的设备号
    ino_t st_ino;     // 文件 i 节点号
    umode_t st_mode;  // 文件类型和属性(见下面)
    nlink_t st_nlink; // 指定文件的连接数
    uid_t st_uid;     // 文件的用户(标识)号
    gid_t st_gid;     // 文件的组号
    dev_t st_rdev;    // 设备号(如果文件是特殊的字符文件或块文件)
    off_t st_size;    // 文件大小(字节数)(如果文件是常规文件)
    time_t st_atime;  // 上次(最后)访问时间
    time_t st_mtime;  // 最后修改时间
    time_t st_ctime;  // 最后节点修改时间
};

// 下面是为 st_mode 字段所用的值定义的符号名称, 这些值均用八进制表示
// 参见第12章文件系统中 图12-5(i 节点属性字段内容)
// 为便于记忆, 这些符号名称均为一些英文单词的首字母或缩写组合而成, 例如
// 名称 S_IFMT 的每个字母分别代表单词 State/Inode/File/Mask/Type
// 而名称 S_IFREG 则是 State/Inode/File/REGular 几个大写字母的组合
// 名称 S_IRWXU 是 State/Inode/Read/Write/eXecute/User 的组合
// 其它名称可以此类推

/**
 * 总结一下, st_mode 字段使用一个 16 bits 整形来表示含义, 16 bits 记作
 * FEDC_BA9_876_543_210, 作用如下:
 *
 * FEDCB 用来记录文件的类型:
 *     0001 ~ 1=FIFO
 *     0010 ~ 2=字符设备
 *     0100 ~ 4=目录
 *     0110 ~ 6=块设备
 *     1000 ~ 8=常规文件
 *     1010 ~ 10=符号链接
 *
 * BA9 三位记录文件属性:
 *     100 ~ 4=set-user-ID
 *     010 ~ 2=set-group-ID
 *     001 ~ 1=受限删除标志
 *
 * 876, 543, 210 三组三个位控制用户权限, 三个组分别表示 自己/同组其他用户/其他人
 *     001 ~ 1=执行/搜索权限
 *     010 ~ 2=写权限
 *     100 ~ 4=读权限
 */

#define S_IFMT 00170000 // 1111_0000_0000_0000 = 0xF000 文件类型比特位屏蔽码(8进制表示)
#define S_IFLNK 0120000 // 1010_0000_0000_0000 = 0xA000 符号链接
#define S_IFREG 0100000 // 1000_0000_0000_0000 = 0x8000 常规文件
#define S_IFBLK 0060000 // 0110_0000_0000_0000 = 0x6000 块特殊(设备)文件, 如磁盘 /dev/fd0
#define S_IFDIR 0040000 // 0100_0000_0000_0000 = 0x4000 目录
#define S_IFCHR 0020000 // 0010_0000_0000_0000 = 0x2000 字符设备文件
#define S_IFIFO 0010000 // 0001_0000_0000_0000 = 0x1000 FIFO特殊文件

// S_ISUID 用于测试文件的 set-user-ID 标志是否置位.
// 若该标志置位, 则当执行该文件时, 进程的有效用户 ID 将被设置为该文件宿主的用户
// ID S_ISGID 则是针对组 ID 进行相同处理
#define S_ISUID 0004000 // 执行时设置用户 ID(set-user-ID)
#define S_ISGID 0002000 // 执行时设置组 ID(set-group-ID)
#define S_ISVTX 0001000 // 对于目录, 受限删除标志

#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)  // 测试是否符号链接文件
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)  // 测试是否常规文件
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)  // 是否目录文件
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)  // 是否字符设备文件
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)  // 是否块设备文件
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO) // 是否FIFO特殊文件

#define S_IRWXU 00700 // 宿主可以读/写/执行/搜索(名称最后字母代表User)
#define S_IRUSR 00400 // 宿主读许可
#define S_IWUSR 00200 // 宿主写许可
#define S_IXUSR 00100 // 宿主执行/搜索许可

#define S_IRWXG 00070 // 组成员可以读/写/执行/搜索(名称最后字母代表Group)
#define S_IRGRP 00040 // 组成员读许可
#define S_IWGRP 00020 // 组成员写许可
#define S_IXGRP 00010 // 组成员执行/搜索许可

#define S_IRWXO 00007 // 其他人读/写/执行/搜索许可(名称最后字母O代表Other)
#define S_IROTH 00004 // 其他人读许可(最后3个字母代表Other)
#define S_IWOTH 00002 // 其他人写许可
#define S_IXOTH 00001 // 其他人执行/搜索许可

// 修改文件属性
extern int chmod(const char *_path, mode_t mode);
// 取指定文件句柄的文件状态信息
extern int fstat(int fildes, struct stat *stat_buf);
// 创建目录
extern int mkdir(const char *_path, mode_t mode);
// 创建管道文件
extern int mkfifo(const char *_path, mode_t mode);
// 取指定文件名的文件状态信息
extern int stat(const char *filename, struct stat *stat_buf);
// 设置属性屏蔽码
extern mode_t umask(mode_t mask);

#endif
