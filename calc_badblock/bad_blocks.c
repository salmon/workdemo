/*
 *
 * */
#include "bad_blocks.h"

#ifdef _LINUX_

static void set_bit_range(u32 *bitmap, s32 start_bit, s32 end_bit)
{
	s32 i;
	s32 start_pos = start_bit / 32;
	s32 start_off = start_bit % 32;
	s32 len = end_bit - start_bit;
	u32 *pos = bitmap + start_pos;

	for (i = 0; i < len; i ++) {
		*pos |= (1 << start_off++);
		if (! start_off % 32) {
			pos ++;
			start_off = 0;
		}
	}
}

static s32 get_name_by_devno(struct devinfo *dinfo, char *name)
{
	FILE *fp;
	u8 buf[128];
	s32 maj, min, ret;
	u64 size_kb;

	fp = fopen("/proc/partitions", "r");
	if (NULL == fp)
		return -1;

	while (!feof(fp)) {
		memset(buf, 0, sizeof(buf));
		if (fgets(buf, sizeof(buf), fp)) {
			ret = sscanf(buf, "%4d  %7d %10"PRIu64" %s", &maj, &min, &size_kb, name);
			if (ret != 4)
				continue;

			if (maj == dinfo->major && min == dinfo->minor) {
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

static s32 get_sys_data_offset(const u8 *pathname, s64 *data_offset)
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

	if (sscanf(buf, "%lu", data_offset) != 1) {
		close(fd);
		return -1;
	}

	close(fd);
	return 0;
}

static s32 sys_fetch_bb(struct md_devinfo *md_info, u32 *bitmap, s32 idx)
{
	u8 buf[256];
	s64 data_offset, bad_blocks;
	s32 i, bad_len, data_disks;
	FILE *fp;

	sprintf(buf, "/sys/block/%s/md/rd%d", md_info->name, idx);
	if (access(buf, R_OK))
		return 0;

	sprintf(buf, "/sys/block/%s/md/rd%d/offset", md_info->name, idx);
	if (get_sys_data_offset(buf, &data_offset))
		return 0;

	data_disks = md_info->array_info.raid_disks - md_info->max_degraded;

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

			memset(buf, 0, sizeof(buf));
			if (fgets(buf, sizeof(buf), fp)) {
				switch(sscanf(buf, "%"PdId64" %d", &bad_blocks, &bad_len)) {
				case 2:
					if (bad_len <= 0)
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
				if (md_info->start_sect > bad_block_end ||
					bad_blocks < md_info->end_sect)
					continue;

				start_bit = (md_info->start_sect > bad_blocks ?
							(md_info->start_sect - bad_blocks) : 0) / 8;
				end_bit = ROUND_UP((md_info->end_sect > bad_block_end ?
							bad_block_end : bad_block_end - md_info->end_sect), 8);
				set_bit_range(bitmap, start_bit, end_bit);
			}
		}
	}

	return 0;
}

s32 find_next_bit(s32 start_bit, u32 value)
{
	if (start_bit >= 32)
		return 0;

	value &= ~((1 >> start_bit) - 1);

	return ffs(value);
}

static s32 is_hit_badblock(struct md_devinfo *md_info)
{
	s32 raid_disks = md_info->array_info.raid_disks;
	s32 chunk_page = md_info->array_info.chunk_size >> 12;
	s32 capacity = ROUND_UP(chunk_page, 32);
	s32 i, j, bad_cnt, can_degraded, pos;
	u32 bitmap[raid_disks][capacity], tmp;

	can_degraded = md_info->max_degraded - (md_info->array_info.raid_disks
						- md_info->array_info.active_disks);
	memset(bitmap, 0, sizeof(bitmap));
	for (i = 0; i < raid_disks; i ++) {
		sys_fetch_bb(md_info, bitmap[i], i);
	}

	for (i = 0; i < capacity; i ++) {
		tmp = 0, bad_cnt = 0, pos = 0;
		for (j = 0; j < raid_disks; j ++) {
			tmp |= bitmap[j][i];
		}
		while ((pos = find_next_bit(pos, tmp)) != 0) {
			for (j = 0; j < raid_disks; j ++) {
				if (bitmap[j][i] & (1 << pos))
					bad_cnt ++;
			}

			if (bad_cnt > can_degraded)
				return 1;
		}
	}

	return 0;
}

static s32 process_badblock(struct devinfo *dinfo, struct md_devinfo *md_info)
{
	s64 start_offset;
	s32 stripe_offset, len;
	s32 process, i = 0;

	start_offset = dinfo->start;
	stripe_offset = start_offset % md_info->stripe_len;
	len = dinfo->end - dinfo->start;

	while (len > 0) {
		if (len > md_info->stripe_len - stripe_offset) {
			process = md_info->stripe_len - stripe_offset;
			len -= process;
		} else {
			process = len;
			len = 0;
		}

		md_info->start_sect = start_offset + stripe_offset + i * md_info->stripe_len;
		md_info->end_sect = md_info->start_sect + process;
		if (is_hit_badblock(md_info))
			return 1;

		stripe_offset = 0;
		i ++;
	}

	return 0;
}

static s32 process_md_badblk(struct devinfo *dinfo)
{
	s32 fd, ret, degraded, max_degraded;
	struct md_devinfo md_info;
	u8 devname[64];

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
		max_degraded = md_info.array_info.raid_disks - 1;
		break;
	case 4:
	case 5:
		max_degraded = 1;
		break;
	case 6:
		max_degraded = 2;
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
	s32 maj, min, ret;
	s64 start, end, offset, dm_start;
	struct devinfo md_device;

	sprintf(buf, "dmsetup table -j %d -m %d", dinfo->major, dinfo->minor);
	fp = popen(buf, "r");
	if (NULL == fp)
		return -1;

	dm_start = dinfo->start;

	while (!feof(fp)) {
		memset(buf, 0, sizeof(buf));
		if (fgets(buf, sizeof(buf), fp)) {
			if ((strstr(buf, "linear")) == NULL)
				continue;

			ret = sscanf(buf, "%"PRId64" %"PRId64" linear %d:%d %llu",
						&start, &end, &maj, &min, &offset);
			if (ret != 5)
				continue;

			if (dm_start > end || dinfo->end < start)
				continue;

			memset(&md_device, 0, sizeof(struct devinfo));
			md_device.rw = dinfo->rw;
			md_device.start = dm_start - start + offset;
			if (dinfo->end > end) {
				dm_start = end;
				md_device.end = dinfo->end - start + offset;
			} else
				md_device.end = dinfo->end;
			md_device.type = TYPE_MD;
			md_device.major = maj;
			md_device.minor = min;
			ret = process_md_badblk(&md_device);
			if (ret == 1)
				return ret;
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
			ret = sscanf(buf, "%d %s", &major, driver_name);
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

static s32 init_device_info(struct devinfo *dinfo, int fd)
{
	struct stat sbuf;
	s32 dm_major = -1, mdp_major = -1;

	memset(&sbuf, 0, sizeof(struct stat));
	if (fstat(fd, &sbuf) < 0) {
		return -1;
	}

	if (! S_ISBLK(sbuf.st_mode))
		return -1;

	dinfo->major = major(sbuf.st_rdev);
	dinfo->minor = minor(sbuf.st_rdev);

	if (get_valid_major(&dm_major, &mdp_major))
		return -1;

	if (dinfo->major == MD_MAJOR)
		dinfo->type = TYPE_MD;
	else if (dinfo->major == dm_major)
		dinfo->type = TYPE_DM;
	else if (dinfo->major == mdp_major)
		dinfo->type = TYPE_MDP;
	else
		dinfo->type = TYPE_INVALID;

	return 0;
}

s32 is_badblock(s32 fd, s64 offset, s32 len, s32 rw)
{
	s32 ret;
	struct devinfo dinfo;

	memset(&dinfo, 0, sizeof(struct devinfo));
	dinfo.rw = rw;
	dinfo.start = offset;
	dinfo.end = offset + len;

	if (init_device_info(&dinfo, fd))
		return -1;

	switch (dinfo.type) {
	case TYPE_DM:
		ret = process_dmlinear_badblk(&dinfo);
		break;
	case TYPE_MDP:
		ret = -1;
		break;
	case TYPE_MD:
		ret = process_md_badblk(&dinfo);
		break;
	default:
		return -1;
	}

	return ret;
}

#else

s32 is_badblock(s32 fd, s64 offset, s32 len, s32 rw)
{
	return 0;
}

#endif
