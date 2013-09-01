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
	s64 data_offset, bad_blocks, stripe;
	s32 i, bad_len, chunk_sector, md_start_bit, md_end_bit;
	s32 cross = 0, tmp1, tmp2;
	FILE *fp;

	sprintf(buf, "/sys/block/%s/md/rd%d", md_info->name, idx);
	if (access(buf, R_OK))
		return 0;

	sprintf(buf, "/sys/block/%s/md/rd%d/offset", md_info->name, idx);
	if (get_sys_data_offset(buf, &data_offset))
		return 0;

	chunk_sector = md_info->array_info.chunk_size >> 9;
	stripe = md_info->start_sect / md_info->stripe_sect;
	md_start_bit = (md_info->start_sect % chunk_sector) / 8;
	md_end_bit = md_start_bit + ROUND_UP((md_info->end_sect - md_info->start_sect), 8);
	if (md_end_bit > (chunk_sector / 8)) {
		tmp1 = md_end_bit / (chunk_sector / 8);
		tmp2 = md_end_bit % (chunk_sector / 8);
		if (tmp1 > 1 || tmp2 >= md_start_bit) {
			cross = 1;
			md_start_bit = tmp % (chunk_sector / 8);
			md_end_bit = md_start_bit;
		} else {
			md_start_bit = 0;
			md_end_bit = chunk_sector / 8;
		}
	}

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
			s64 failed_stripe;
			s32 start_bit, end_bit, offset;

			memset(buf, 0, sizeof(buf));
			if (fgets(buf, sizeof(buf), fp)) {
				switch(sscanf(buf, "%"PRId64" %d", &bad_blocks, &bad_len)) {
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

				failed_stripe = bad_blocks / chunk_sector;
				if (stripe != failed_stripe)
					continue;

				offset = (bad_blocks % chunk_sector);
				start_bit = offset / 8;
				if (offset + bad_len >= chunk_sector)
					end_bit = chunk_sector / 8;
				else
					end_bit = ROUND_UP(offset + bad_len, 8);

				if (md_start_bit > end_bit || md_end_bit < start_bit)
					continue;

				fprintf(stderr, "start_bit %d, end_bit %d\n", start_bit, end_bit);
				set_bit_range(bitmap, start_bit, end_bit);
			}
		}
	}

	return 0;
}

static s32 find_next_bit(s32 start_bit, u32 value)
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

	start_offset = dinfo->start_sect;
	stripe_offset = start_offset % md_info->stripe_sect;
	len = dinfo->end_sect - dinfo->start_sect;
	fprintf(stderr, "stripe size %d, stripe offset %d\n", md_info->stripe_sect, stripe_offset);

	while (len > 0) {
		if (len > md_info->stripe_sect - stripe_offset) {
			process = md_info->stripe_sect - stripe_offset;
			len -= process;
		} else {
			process = len;
			len = 0;
		}

		md_info->start_sect = start_offset + stripe_offset + i * md_info->stripe_sect;
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
	md_info.stripe_sect = (md_info.array_info.raid_disks - max_degraded)
			             * (md_info.array_info.chunk_size >> 9);

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

	dm_start = dinfo->start_sect;

	while (!feof(fp)) {
		memset(buf, 0, sizeof(buf));
		if (fgets(buf, sizeof(buf), fp)) {
			if ((strstr(buf, "linear")) == NULL)
				continue;

			ret = sscanf(buf, "%"PRId64" %"PRId64" linear %d:%d %"PRId64"",
				          &start, &end, &maj, &min, &offset);
			if (ret != 5)
				continue;

			if (dm_start > end || dinfo->end_sect < start)
				continue;

			memset(&md_device, 0, sizeof(struct devinfo));
			md_device.rw = dinfo->rw;
			md_device.start_sect = dm_start - start + offset;
			if (dinfo->end_sect > end) {
				dm_start = end;
				md_device.end_sect = dinfo->end_sect - start + offset;
			} else
				md_device.end_sect = dinfo->end_sect;
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
	dinfo.start_sect = offset / SECTOR_SIZE;
	dinfo.end_sect = ROUND_UP((offset + len), SECTOR_SIZE);

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
