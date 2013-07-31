#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <linux/types.h>

#define STRIPE_SECTOR 8
#define BUF_SIZE 256
#define MAX_BBS 4096
#define MD_MAJOR 9
#define MAX_RDEV_NUM 64

struct bad_range {
	__u64 start_sector;
	int len;
};

struct lvm_bbs {
	int bb_cnt;

	struct bad_range bb_range[MAX_BBS];
};

static int get_sys_attr(const char *pathname, char *buf)
{
	int fd;
	ssize_t size;

	fd = open(pathname, O_RDONLY);
	if (fd < 0) {
		perror("open error");
		goto err;
	}

	size = read(fd, buf, BUF_SIZE);
	if (size < 0) {
		perror("read error");
		goto err;
	}

	close(fd);
	return 0;

err:
	close(fd);
	return -1;
}

static int get_name_by_devno(const int owner_maj, const int owner_min, char *name)
{
	FILE *fp;
	char buf[BUF_SIZE];
	int maj, min, ret;
	__u64 size_kb;

	fp = fopen("/proc/partitions", "r");
	if (NULL == fp)
		return -1;

	while (!feof(fp)) {
		memset(buf, 0, BUF_SIZE);
		fgets(buf, BUF_SIZE, fp);
		ret = sscanf(buf, "%4d  %7d %10llu %s", &maj, &min, &size_kb, name);
		if (ret != 4)
			continue;

		if (maj == owner_maj && min == owner_min) {
			fclose(fp);
			return 0;
		}
	}

	fclose(fp);
	return -1;
}

struct rdev_bbs_range {
	__u64 start_sector;
	int len;

	__u64 rdev_bitmap;
};

struct rdev_bbs {
	int bb_cnt;

	struct rdev_bbs_range bb_range[MAX_BBS];
};

static int count_one_bits(__u64 word)
{
	int i, ret = 0;
	int len = MAX_RDEV_NUM;

	for (i = 0; i < len; i ++) {
		if (word & (1LL << i))
			++ret;
	}

	return ret;
}

static int insert_range(struct rdev_bbs *rdev_bb, struct rdev_bbs_range *new, int pos)
{
	struct rdev_bbs_range *range = &rdev_bb->bb_range[pos];

	if (rdev_bb->bb_cnt > MAX_BBS - 1) {
		fprintf(stderr, "rdev no space to store badblocks\n");
		return -1;
	}

	if (rdev_bb->bb_cnt - pos > 0)
		memmove(range + 1, range, sizeof(struct rdev_bbs_range) * (rdev_bb->bb_cnt - pos));
	memcpy(range, new, sizeof(struct rdev_bbs_range));
	rdev_bb->bb_cnt ++;

	return 0;
}

static int is_last_range(struct rdev_bbs *rdev_bb, int pos)
{
	if (pos < rdev_bb->bb_cnt - 1)
		return 0;

	return 1;
}

static void merge_or_split(struct rdev_bbs *rdev_bb, int idx, __u64 bad_sector, int len)
{
	int i, count;
	__u64 start_sector, end_sector, tmp_sector;
	struct rdev_bbs_range *range;
	struct rdev_bbs_range tmp;

	memset(&tmp, 0, sizeof(struct rdev_bbs_range));
	tmp.start_sector = bad_sector;
	tmp.len = len;
	tmp.rdev_bitmap |= (1 << idx);

	count = rdev_bb->bb_cnt;
	if (0 == count) {
		insert_range(rdev_bb, &tmp, 0);
		return;
	}

	for (i = 0; i < count; i ++) {
		range = &rdev_bb->bb_range[i];
		start_sector = range->start_sector;
		end_sector = range->len + start_sector - 1;

		if (rdev_bb->bb_range[i].start_sector > tmp.start_sector)
			break;

		if (start_sector <= (tmp.start_sector + len) && end_sector >= tmp.start_sector)
			break;
	}

	while (1) {
		if (start_sector > (tmp.start_sector + tmp.len) || end_sector < tmp.start_sector) {
			insert_range(rdev_bb, &tmp, i);
			break;
		}

		/* overlap */
		if (range->rdev_bitmap == (1 << idx)) {
			if (start_sector > tmp.start_sector) {
				range->start_sector = tmp.start_sector;
				range->len += start_sector - tmp.start_sector;
			}

			tmp_sector = tmp.start_sector + tmp.len - 1;
			if (end_sector >= tmp_sector)
				return;

			if (is_last_range(rdev_bb, i)) {
				range->len += tmp_sector - end_sector;
				return;
			}

			tmp_sector = rdev_bb->bb_range[i + 1].start_sector;
			if (tmp.start_sector + tmp.len > tmp_sector) {
				range->len = tmp_sector - range->start_sector;

				tmp.len = tmp.start_sector + tmp.len - tmp_sector;
				tmp.start_sector = tmp_sector;

				range = &rdev_bb->bb_range[++i];
				start_sector = range->start_sector;
				end_sector = range->len + start_sector - 1;
			} else {
				range->len += tmp.start_sector + tmp.len - 1 - end_sector;
				return;
			}
		} else {
			struct rdev_bbs_range split_range;
			int is_split = 0;

			if (start_sector > tmp.start_sector) {
				split_range.start_sector = tmp.start_sector;
				split_range.len = start_sector - tmp.start_sector;
				split_range.rdev_bitmap = tmp.rdev_bitmap;
				if (insert_range(rdev_bb, &split_range, i))
					return;

				tmp.len = tmp.start_sector + tmp.len - start_sector;
				tmp.start_sector = start_sector;
				is_split = 1;
			} else if (start_sector < tmp.start_sector) {
				range->len = range->start_sector + range->len - tmp.start_sector;
				range->start_sector = tmp.start_sector;

				split_range.start_sector = start_sector;
				split_range.len = tmp.start_sector - start_sector;
				split_range.rdev_bitmap = range->rdev_bitmap;
				if (insert_range(rdev_bb, &split_range, i))
					return;
				is_split = 1;
			}

			if (is_split) {
				range = &rdev_bb->bb_range[++i];
				start_sector = range->start_sector;
				end_sector = range->len + start_sector - 1;
			}

			tmp_sector = tmp.start_sector + tmp.len - 1;
			if (tmp_sector > end_sector) {
				tmp.start_sector = end_sector + 1;
				tmp.len = tmp_sector - end_sector;

				range->rdev_bitmap |= (1 << idx);
				//printf("start_sector %llu, len %d\n", tmp.start_sector, tmp.len);

				if (! is_last_range(rdev_bb, i)) {
					range = &rdev_bb->bb_range[++i];
					start_sector = range->start_sector;
					end_sector = range->len + start_sector - 1;
				}

				continue;
			} else if (tmp_sector < end_sector) {
				split_range.start_sector = tmp_sector + 1;
				split_range.len = end_sector - tmp_sector;
				split_range.rdev_bitmap = range->rdev_bitmap;

				if (insert_range(rdev_bb, &split_range, i + 1))
					return;

				range->len = tmp_sector - range->start_sector + 1;
				range->rdev_bitmap |= (1 << idx);

				break;
			} else {
				range->rdev_bitmap |= (1 << idx);
				break;
			}
		}
	}
}

static void align_with_stripe(__u64 *sector, int *len)
{
	__u64 tmp_sector = *sector;
	int tmp_len = *len;
	int tmp;

	tmp = tmp_sector % STRIPE_SECTOR;
	if (tmp) {
		tmp_sector = (tmp_sector / STRIPE_SECTOR) * STRIPE_SECTOR;
		tmp_len += tmp;
	}

	if (tmp_len % STRIPE_SECTOR)
		tmp_len = (tmp_len / STRIPE_SECTOR + 1) * STRIPE_SECTOR;

	*sector = tmp_sector;
	*len = tmp_len;
}

static int get_rdev_badblocks(const char *raid_name, int idx, struct rdev_bbs *rdev_badblocks)
{
	char pathname[BUF_SIZE];
	char buf[BUF_SIZE];
	int data_offset, is_bad = 0, i, len;
	FILE *fp;
	__u64 bad_block;

	sprintf(pathname, "/sys/block/%s/md/rd%d", raid_name, idx);
	/* rdev may be faulty */
	if (access(pathname, R_OK))
		return 0;

	sprintf(pathname, "/sys/block/%s/md/rd%d/offset", raid_name, idx);
	if (get_sys_attr(pathname, buf) ||
				(sscanf(buf, "%d", &data_offset)) != 1)
		return -1;


	for (i = 0; i < 2; i ++) {
		if (0 == i)
			sprintf(pathname, "/sys/block/%s/md/rd%d/bad_blocks", raid_name, idx);
		else
			sprintf(pathname, "/sys/block/%s/md/rd%d/unacknowledged_bad_blocks", raid_name, idx);

		fp = fopen(pathname, "r");
		if (NULL == fp) {
			//if (errno == ENOENT)
			return 0;
		}

		while (!feof(fp)) {
			memset(buf, 0, BUF_SIZE);
			fgets(buf, BUF_SIZE, fp);
			switch(sscanf(buf, "%llu %d", &bad_block, &len)) {
			case 2:
				if (len <= 0)
					is_bad = 1;
				break;
			default:
				is_bad = 1;
			}
			if (is_bad)
				break;

			if (bad_block + len < data_offset)
				continue;

			if (bad_block < data_offset) {
				bad_block = 0;
				len = bad_block + len - data_offset;
			} else
				bad_block -= data_offset;

			align_with_stripe(&bad_block, &len);
			/* split or merge */
			merge_or_split(rdev_badblocks, idx, bad_block, len);
			/*
			int m;
			for (m = 0; m < rdev_badblocks->bb_cnt; m ++) {
				struct rdev_bbs_range *range = &rdev_badblocks->bb_range[m];
				printf("range start_sector %llu, len %d, %lx\n", range->start_sector, range->len, range->rdev_bitmap);
			}
			*/
		}
	}

	fclose(fp);

	return 0;
}

static int fill_lvm_bbs(const char *raid_name, __u64 start, __u64 end,
						__u64 lvm_offset, struct lvm_bbs *lvm_badblocks)
{
	char pathname[BUF_SIZE];
	char buf[BUF_SIZE];
	int chunk_sector, raid_disks, level, ret, i;
	struct rdev_bbs *rdev_badblocks;
	int degraded, max_degraded;

	__u64 failed_stripe, raid_failed_stripe, raid_len;
	int chunk_offset;

	/* get raid attr */
	sprintf(pathname, "/sys/block/%s/md/chunk_size", raid_name);
	if (get_sys_attr(pathname, buf) ||
				(sscanf(buf, "%d", &chunk_sector) != 1))
		goto err;
	chunk_sector = chunk_sector >> 9;

	sprintf(pathname, "/sys/block/%s/md/raid_disks", raid_name);
	if (get_sys_attr(pathname, buf) ||
				(sscanf(buf, "%d", &raid_disks) != 1))
		goto err;

	sprintf(pathname, "/sys/block/%s/md/degraded", raid_name);
	if (get_sys_attr(pathname, buf) ||
				(sscanf(buf, "%d", &degraded)) != 1)
		goto err;

	sprintf(pathname, "/sys/block/%s/md/level", raid_name);
	if (get_sys_attr(pathname, buf) ||
				(sscanf(buf, "raid%d", &level)) != 1)
		goto err;
	if (6 == level)
		max_degraded = 2;
	else if (5 == level)
		max_degraded = 1;
	else
		goto err;

	/* get rdev badblocks */
	rdev_badblocks = malloc(sizeof(struct rdev_bbs));
	if (NULL == rdev_badblocks)
		goto err;

	memset(rdev_badblocks, 0, sizeof(struct rdev_bbs));

	for (i = 0; i < raid_disks; i ++) {
		ret = get_rdev_badblocks(raid_name, i, rdev_badblocks);
		if (ret) {
			free(rdev_badblocks);
			return -1;
		}
	}

	/* calculate raid badblocks */
	for (i = 0; i < rdev_badblocks->bb_cnt; i ++) {
		int j;
		struct rdev_bbs_range *range;

		range = &rdev_badblocks->bb_range[i];
		if (degraded > max_degraded) {
			fprintf(stderr, "raid is inactive\n");
			free(rdev_badblocks);
			return -1;
		}

		if ((count_one_bits(range->rdev_bitmap) + degraded) <= max_degraded)
			continue;

		failed_stripe = range->start_sector / chunk_sector;
		chunk_offset = range->start_sector % chunk_sector;
		//printf("failed stripe %llu, chunk offset %d\n", failed_stripe, chunk_offset);
		for (j = 0; j < raid_disks; j ++) {
			raid_failed_stripe = (failed_stripe * (raid_disks - max_degraded) + j)
							* chunk_sector + chunk_offset;
			raid_len = range->len;
			//printf("raid %llu, len %d\n", raid_failed_stripe, raid_len);

			/* calculate lvm badblocks */
			if ((lvm_offset > raid_failed_stripe + raid_len) ||
					(lvm_offset + end < raid_failed_stripe))
				continue;

			if (raid_failed_stripe >= lvm_offset)
				lvm_badblocks->bb_range[lvm_badblocks->bb_cnt].start_sector =
							start + raid_failed_stripe - lvm_offset;
			else
				lvm_badblocks->bb_range[lvm_badblocks->bb_cnt].start_sector = start;

			if ((lvm_offset + end) >= (raid_failed_stripe + raid_len))
				lvm_badblocks->bb_range[lvm_badblocks->bb_cnt].len = range->len;
			else {
				int m;
				m = raid_failed_stripe + raid_len - lvm_offset - end;
				lvm_badblocks->bb_range[lvm_badblocks->bb_cnt].len =
						range->len - m;
			}

			lvm_badblocks->bb_cnt ++;
			if (lvm_badblocks->bb_cnt == MAX_BBS) {
				fprintf(stderr, "No space to store lvm badblocks\n");
				free(rdev_badblocks);
				return -1;
			}
		}
	}

	free(rdev_badblocks);

	return 0;

err:
	return -1;
}

int get_lvm_bbs(const char *lvm_name, struct lvm_bbs *lvm_badblocks)
{
	FILE *fp;
	char buf[BUF_SIZE];
	int maj, min, ret;
	__u64 start, end, offset;

	/* clear badblocks table everytime */
	lvm_badblocks->bb_cnt = 0;

	sprintf(buf, "dmsetup table %s", lvm_name);
	fp = popen(buf, "r");
	if (NULL == fp)
		return -1;

	while (!feof(fp)) {
		memset(buf, 0, BUF_SIZE);
		fgets(buf, BUF_SIZE, fp);
		if ((strstr(buf, "linear")) == NULL)
			continue;

		ret = sscanf(buf, "%llu %llu linear %d:%d %llu",
					&start, &end, &maj, &min, &offset);
		if (ret != 5)
			continue;

		memset(buf, 0, BUF_SIZE);
		if (get_name_by_devno(maj, min, buf)) {
			fprintf(stderr, "can't find %d:%d devname\n", maj, min);
			continue;
		}

		ret = fill_lvm_bbs(buf, start, end, offset, lvm_badblocks);
		if (ret) {
			fprintf(stderr, "raid %s is inactive\n", buf);
			pclose(fp);
			return -1;
		}
	}

	pclose(fp);

	return 0;
}

int main(int argc, char **argv)
{
	int i, ret;
	struct lvm_bbs bad_blocks;

	if (argc != 2)
		exit(1);

	ret = get_lvm_bbs(argv[1], &bad_blocks);
	if (ret) {
		fprintf(stderr, "get lvm badblocks error\n");
		exit(1);
	}

	printf("%s has %d bad sectors:\n", argv[1], bad_blocks.bb_cnt);
	for (i = 0; i < bad_blocks.bb_cnt; i ++) {
		printf("start %llu len %d\n", bad_blocks.bb_range[i].start_sector, bad_blocks.bb_range[i].len);
	}

	return 0;
}

