#ifndef __FIX_SECTOR_H_
#define __FIX_SECTOR_H_

#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <linux/types.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/time.h>

#include <syslog.h>
#include <signal.h>
#include <linux/hdreg.h>

int get_identify_data(int fd, __u16 *id);

#endif
