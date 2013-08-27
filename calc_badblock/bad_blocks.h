typedef char u8;
typedef int s32;
typedef unsigned int u32;
typedef long long s64;
typedef unsigned long long u64;

#ifndef __BAD_BLOCKS_H_
#define __BAD_BLOCKS_H_

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <inttypes.h>

#define PROC_DEVICES "/proc/devices"

/* copy from md_u.h */
#define MD_MAJOR 9
#define GET_ARRAY_INFO _IOR (MD_MAJOR, 0x11, mdu_array_info_t)
#define ROUND_UP(x,y) (((x)+(y)-1)/(y))

typedef struct mdu_array_info_s {
	/*
	 * Generic constant information
	 */
	int major_version;
	int minor_version;
	int patch_version;
	int ctime;
	int level;
	int size;
	int nr_disks;
	int raid_disks;
	int md_minor;
	int not_persistent;

	/*
	 * Generic state information
	 */
	int utime;		/*  0 Superblock update time		      */
	int state;		/*  1 State bits (clean, ...)		      */
	int active_disks;	/*  2 Number of currently active disks	      */
	int working_disks;	/*  3 Number of working disks		      */
	int failed_disks;	/*  4 Number of failed disks		      */
	int spare_disks;	/*  5 Number of spare disks		      */

	/*
	 * Personality information
	 */
	int layout;		/*  0 the array's physical layout	      */
	int chunk_size;	/*  1 chunk size in bytes		      */

} mdu_array_info_t;

enum {
	TYPE_MD,
	TYPE_DM,
	TYPE_MDP,
	TYPE_INVALID,
};

struct devinfo {
	s32 rw;
	s64 start;
	s64 end;

	s32 type;
	s32 major;
	s32 minor;
};

struct md_devinfo {
	u8 name[128];
	s32 max_degraded;
	s32 stripe_len;
	mdu_array_info_t array_info;

	/* in stripe */
	s64 start_sect;
	s64 end_sect;
};

s32 is_badblock(s32 fd, s64 offset, s32 len, s32 rw);

#endif
