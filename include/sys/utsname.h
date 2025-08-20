#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#include <sys/param.h>
#include <sys/types.h>

struct utsname {
    char sysname[9];
    char nodename[MAXHOSTNAMELEN + 1];
    char release[9];
    char version[9];
    char machine[9];
};

extern int uname(struct utsname *utsbuf);

#endif
