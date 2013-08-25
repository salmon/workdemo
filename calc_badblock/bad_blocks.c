/*
 *
 * */

typedef int s32
typedef off64_t s64

#ifdef _LINUX_

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
	s32 fd;
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
	mdu_array_info_t array;
};

static void set_bit_range(char *bitmap, )
{

}

static s32 get_name_by_devno(struct devinfo *dinfo, char *name)
{
	FILE *fp;
	u8 buf[128];
	s32 maj, min, ret;
	__u64 size_kb;

	fp = fopen("/proc/partitions", "r");
	if (NULL == fp)
		return -1;

	while (!feof(fp)) {
		memset(buf, 0, sizeof(buf));
		if (fgets(buf, sizeof(buf), fp)) {
			ret = sscanf(buf, "%4d  %7d %10llu %s", &maj, &min, &size_kb, name);
			if (ret != 4)
				continue;

			if (maj == owner_maj && min == owner_min) {
				fclose(fp);
				return 0;
			}
		}
	}

	fclose(fp);
	return -1;
}

static s32 get_md_status(s32 fd, mdu_array_info_t *info)
{
	memset(info, 0, sizeof(mdu_array_info_t));
	if (ioctl(fd, GET_ARRAY_INFO, info) < 0)
		return -1;

	return 0;
}

static s32 get_sys_data_offset(const char *pathname, s32 *data_offset)
{
	s32 fd, size;
	u8 buf[128];

	fd = open(pathname, O_RDONLY);
	if (fd < 0)
		return -1;

	size = read(fd, buf, sizeof(buf));
	if (size < 0) {
		close(fd);
		return -1;
	}

	if (sscanf(buf, "%d", data_offset) != 1) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static s32 sys_fetch_bb(md_devinfo *md_info, s64 stripe, u8 *bitmap, s32 idx)
{
	u8 buf[256];
	s64 data_offset, bad_blocks;
	s64 stripe_start_sect, stripe_end_sect;
	s32 i, bad_len, data_disks, chunk_sector = md_info->array_info.chunk_size >> 9;
	FILE *fp;

	sprintf(buf, "/sys/block/%s/md/rd%d", md_info->name, idx);
	if (access(buf, R_OK))
		return 0;

	sprintf(buf, "/sys/block/%s/md/rd%d/offset", md_info->name, idx);
	if (get_sys_data_offset(buf, &data_offset))
		return 0;

	data_disks = md_info->array_info.raid_disks - md_info.max_degraded;
	stripe_start_sect = stripe * (chunk_sector * data_disks);
	stripe_end_sect = (stripe + 1) * (chunk_sector * data_disks);

	for (i = 0; i < 2; i ++) {
		if (0 == i)
			sprintf(buf, "/sys/block/%s/md/rd%d/unacknowledged_bad_blocks",
						md_info->name, idx);
		else
			sprintf(buf, "/sys/block/%s/md/rd%d/bad_blocks",
						md_info->name, idx);
        fp = fopen(buf, "r");
        if (NULL == fp) {
			return 0;
		}

		while (!feof(fp)) {
			s32 invalid = 0;
			s64 bad_block_end;
			s32 start_bit, end_bit;

			memset(buf, 0, BUF_SIZE);
			if (fgets(buf, BUF_SIZE, fp)) {
				switch(sscanf(buf, "%llu %d", &bad_blocks, &bad_len)) {
				case 2:
					if (len <= 0)
						invalid = 1;
					break;
				default:
					invalid = 1;
				}

				if (invalid)
					continue;

				if (bad_blocks + bad_len < data_offset)
					continue;

				if (bad_blocks < data_offset) {
					bad_blocks = 0;
					bad_len = bad_blocks + bad_len - data_offset;
				} else
					bad_blocks -= data_offset;

				bad_block_end = bad_blocks + bad_len;
				if (stripe_start_sect > bad_block_end &&
					bad_block < stripe_end_sect)
					continue;

				start_bit = (stripe_start_sect > bad_block ?
							(stripe_start_sect - bad_block) : 0) / 8;
				end_bit = ROUND_UP((stripe_end_sect > bad_block_end ?
							bad_block_end : bad_block_end - stripe_end_sect), 8);
				set_bit_range(bitmap, start_bit, end_bit);
			}
		}
	}

	return 0;
}

static s32 is_hit_badblock(md_devinfo *md_info, s64 stripe)
{
	s32 raid_disks = md_info->array_info.raid_disks;
	s32 chunck_page = md_info->array_info.chunk_size >> 12;
	s32 capacity = ROUND_UP(chunk_page, 8);
	s32 i;
	u8 bitmap[raid_disks][capacity];

	memset(bitmap, 0, sizeof(bitmap));
	for (i = 0; i < raid_disks; i ++) {
		sys_fetch_bb(md_info, stripe, bitmap[i], i);
	}

	return 0;
}

static s32 process_badblock(struct devinfo *dinfo, md_devinfo *md_info)
{
	s64 start_offset, stripe;
	s32 stripe_offset, len, stripe_len, finish = 0;
	s32 process;

	start_offset = dinfo.start;
	len = dinfo.end - dinfo.start;

	while (len > 0) {
		stripe = start_offset / md_info->stripe_len;
		stripe_offset = start_offset % md_info->stripe_len;

		if (len > stripe_len - stripe_offset) {
			process = md_info->stripe_len - stripe_offset;
			len -= process;
		} else {
			process = len;
			len = 0;
		}
	}

	return 0;
}

static s32 process_md_badblk(struct devinfo *dinfo)
{
	s32 fd, ret, degraded, max_degraded;
	md_devinfo md_info;
	u32 devname[64];

	ret = get_name_by_devno(dinfo, md_info.name);
	if (ret)
		return ret;

	sprintf(devname, "/dev/%s", md_info.name);
	fd = open(devname, O_RDONLY);
	if (fd < 0)
		return -1;
	ret = get_md_status(fd, &md_info.array_info);
	close(fd);
	if (ret)
		return ret;

	switch (md_info.array_info.level) {
	case 1:
		max_degrade = md_info.array_info.raid_disks - 1;
		break;
	case 4:
	case 5:
		max_degrade = 1;
		break;
	case 6:
		max_degrade = 2;
		break;
	/* case 10: */
	default:
		return -1;
	}

	degraded = md_info.array_info.raid_disks - md_info.array_info.active_disks;
	if (degraded > max_degraded) {
		/* raid is failed */
		return -1;
	}

	md_info.max_degraded = max_degraded;
	md_info.stripe_len = (md_info.array_info.raid_disks -
							max_degraded) * md_info.array_info.chunk_size;

	ret = process_badblock(dinfo, &md_info);

	return ret;
}

/*
 * May rewrite by ioctl
 * */
static s32 process_dmlinear_badblk(struct devinfo *dinfo)
{
	FILE *fp;
	char buf[256];
	s32 maj, min;
	s64 start, end, offset;
	struct devinfo mdinfo;

	sprintf(buf, "dmsetup table -j %d -m %d", dinfo->major, dinfo.minor);
	fp = popen(buf, "r");
	if (NULL == fp)
		return -1;

	while (!feof(fp)) {
		memset(buf, 0, sizeof(buf));
		if (fgets(buf, sizeof(buf), fp)) {
			if ((strstr(buf, "linear")) == NULL)
				continue;

			ret = sscanf(buf, "%lld %lld linear %d:%d %llu",
						&start, &end, &maj, &min, &offset);
			if (ret != 5)
				continue;

			/* not finish */
		}
	}

	pclose(fp);

	return 0;
}

static s32 get_valid_major(s32 *dm_major, s32 *mdp_major)
{
	char buf[512];
	char driver_name[64];
	s32 ret, major;
	FILE *fp = fopen(PROC_DEVICES, "r");

	if (NULL == fp)
		return -1;

	while (!feof(fp)) {
		major = -1;
		memset(driver_name, 0, sizeof(driver_name));
		memset(buf, 0, sizeof(buf));
		if (fgets(buf, sizeof(buf), fp)) {
			ret = sscanf("%d %s", &major, driver_name);
			if (ret != 2)
				continue;
			if (0 == strncmp(driver_name, "device-mapper", sizeof(driver_name))) {
				*dm_major = major;
			} else if (0 == strncmp(driver_name, "mdp", sizeof(driver_name))) {
				*mdp_major = major;
			}
		}
	}

	fclose(fp);

	return 0;
}

static s32 init_device_info(struct devinfo *dinfo)
{
	struct stat sbuf;
	s32 dm_major = -1, mdp_major = -1;

	memset(&sbuf, 0, sizeof(struct stat));
	if (fstat(dinfo.fd, &sbuf) < 0) {
		return -1;
	}

	if (! S_ISBLK(stat_buf.st_mode))
		return -1;

	dinfo->major = major(sbuf.st_rdev);
	dinfo->minor = minor(sbuf.st_rdev);

	if (get_valid_major(&dm_major, &mdp_major))
		return -1

	switch (major) {
	case MD_MAJOR:
		dinfo->type = TYPE_MD;
	case dm_major:
		dinfo->type = TYPE_DM;
	case mdp_major:
		dinfo->type = TYPE_MDP;
	default:
		dinfo->type = TYPE_INVALID;
	}

	return 0;
}

s32 is_badblock(s32 fd, s64 offset, s32 len, s32 rw)
{
	s32 type, ret;
	struct devinfo dinfo;

	memset(&dinfo, 0, sizeof(struct devinfo));
	dinfo.fd = fd;
	dinfo.rw = rw;
	dinfo.start = offset;
	dinfo.end = offset + len;

	if (init_device_info(&dinfo))
		return -1;

	switch (dinfo.type) {
	case TYPE_DM:
		
	case TYPE_MDP:

	case TYPE_MD:

	defalut:
		return -1;
	}
}

#else

s32 is_badblock(s32 fd, s64 offset, s32 len, s32 rw)
{
	return 0;
}

#endif
