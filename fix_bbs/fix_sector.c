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

#include "md_u.h"
#include "md_p.h"

#define BUF_SIZE (1024 * 1024)
#define RETRY 3
#define SECTOR_SIZE 512
#define MD_SB_BYTES 4096
#define MD_SB_MAGIC 0xa92b4efc
#define INTERVAL 2
#define PROC_NAME "fix_sector"

struct device_info {
	char *name;

	__u64 total_sectors;
	__u32 sector_size;
	__u32 phy_sector_size;

	off64_t start_offset;
	struct timeval stime;

	int raid_uuid[4];
	int data_disks;
	int chunk_size;
	int role;
	int data_offset;
	off64_t data_size;
};

struct device_info dinfo;
char *buf;

static void usage()
{
	printf("Version: v0.1\n");
	printf("Usage:\n");
	printf("\t-f [dev_name] [start_offset]: fix disk bad sector\n");
	printf("\t-s [dev_name]: query disk current status\n");
	printf("\t-x [dev_name]: stop fixing disk\n");
	exit(1);
}

int load_super0(int fd)
{
	off64_t dsize;
	off64_t offset;
	mdp_super_t super;

	if (ioctl(fd, BLKGETSIZE64, &dsize) != 0) {
		return -1;
	}
	offset = MD_NEW_SIZE_SECTORS(dsize >> 9);
	offset *= 512;
	ioctl(fd, BLKFLSBUF, 0);

	if (lseek64(fd, offset, 0) < 0LL) {
		perror("lseek64 error");
		return -1;
	}

	memset(buf, 0, sizeof(super));
	if (read(fd, buf, sizeof(super)) != MD_SB_BYTES) {
		perror("read superblock error");
		return -1;
	}
	memcpy(&super, buf, sizeof(super));

	if (super.md_magic != MD_SB_MAGIC) {
		perror("Not metadata 0.9");
		return -2;
	}

	dinfo.raid_uuid[0] = super.set_uuid0;
	if (super.minor_version >= 90) {
		dinfo.raid_uuid[1] = super.set_uuid1;
		dinfo.raid_uuid[2] = super.set_uuid2;
		dinfo.raid_uuid[3] = super.set_uuid3;
	}

	if (super.level == 5) {
		dinfo.data_disks = super.raid_disks - 1;
	} else if (super.level == 6) {
		dinfo.data_disks = super.raid_disks - 2;
	} else {
		perror("Unsupport raid level");
		return -2;
	}

	dinfo.chunk_size = super.chunk_size;
	dinfo.role = super.this_disk.raid_disk;
	dinfo.data_offset = 0;
	dinfo.data_size = (off64_t)super.size * 1024;

	return 0;
}

int load_super1(int fd)
{
	return -1;
}

static int get_raidinfo(int fd)
{
	int ret;

	ret = load_super0(fd);
	if (ret)
		return ret;

	return 0;
}

static int get_devinfo(int fd)
{
	struct stat stat_buf;

	memset(&stat_buf, 0, sizeof(stat_buf));
	if (fstat(fd, &stat_buf) < 0 ) {
		perror("Failed to get device status");
		return -1;
	}

	if (! S_ISBLK(stat_buf.st_mode)) {
		perror("device is not a block device");
		return -1;
	}

	if (ioctl(fd, BLKGETSIZE, &dinfo.total_sectors) < 0) {
		perror("get total sectors error");
		return -1;
	}

	if (ioctl(fd, BLKSSZGET, &dinfo.sector_size) < 0) {
		perror("get logical block size error");
		return -1;
	}

	dinfo.phy_sector_size = 512;

	return get_raidinfo(fd);
}

/**********/

static int open_shm_file(int create)
{
	char filename[128];
	char devname[16];
	int fd;

	memset(devname, 0, sizeof(devname));
	sscanf(dinfo.name, "/dev/%s", devname);
	sprintf(filename, "/dev/shm/fix_%s_%x_%x_%x_%x_%d", devname, dinfo.raid_uuid[0],
			dinfo.raid_uuid[1], dinfo.raid_uuid[2], dinfo.raid_uuid[3], dinfo.role);

	if (create) {
		fd = open(filename, O_RDWR | O_SYNC | O_CREAT | O_TRUNC);
		if (fd < 0) {
			perror("create shmfile error");
			return -1;
		}
	} else {
		fd = open(filename, O_RDWR | O_SYNC);
		if (fd < 0) {
			if (errno == ENOENT)
				return -2;
			else
				return -1;
		}
	}

	return fd;
}

static int clear_status()
{
	char filename[128];
	char devname[16];

	memset(devname, 0, sizeof(devname));
	sscanf(dinfo.name, "/dev/%s", devname);
	sprintf(filename, "/dev/shm/fix_%s_%x_%x_%x_%x_%d", devname, dinfo.raid_uuid[0],
			dinfo.raid_uuid[1], dinfo.raid_uuid[2], dinfo.raid_uuid[3], dinfo.role);

	unlink(filename);

	return 0;
}

static int write_status(int fd, off64_t offset, int status)
{
	struct timeval ctime;
	char shm_buf[512];

	gettimeofday(&ctime, NULL);

	lseek(fd, 0, SEEK_SET);
	memset(shm_buf, 0, sizeof(shm_buf));
	sprintf(shm_buf, "%d-%lu-%lu-%"PRId64"-%"PRId64"\n", status, ctime.tv_sec,
				dinfo.stime.tv_sec, dinfo.start_offset, offset);

	write(fd, shm_buf, strlen(shm_buf) + 1);

	return 0;
}

static int print_status()
{
	int shm_fd, ret;
	char shm_buf[512];
	time_t start_time, cur_time, finish_time = 3600 * 2;
	off64_t start_offset, offset;
	int status, percent = 0;
	double avg_spd, remained;

	shm_fd = open_shm_file(0);
	if (shm_fd < 0) {
		if (shm_fd == -2) {
			printf("%s is not fixing\n", dinfo.name);
			return 0;
		} else {
			perror("open shm file error");
			return -1;
		}
	}

	read(shm_fd, shm_buf, sizeof(shm_buf));
	ret = sscanf(shm_buf, "%d-%lu-%lu-%"PRId64"-%"PRId64"\n", &status, &cur_time,
		&start_time, &start_offset, &offset);

	if (ret != 5) {
		perror("invalid format");
		return -1;
	}

	if (status == 2) {
		printf("fix badsector failed\n");
		return 0;
	} else if (status == 1) {
		printf("fix badsector finished successfull\n");
		return 0;
	} else if (status != 0) {
		perror("invalid format");
		return -1;
	}

	if (offset < start_offset || offset == 0)
		avg_spd = 0;
	else {
		avg_spd = (double)(offset - start_offset) / (cur_time - start_time);
		if (dinfo.data_size < offset) {
			finish_time = 0;
			percent = 100;
		} else {
			finish_time = (dinfo.data_size - offset) / avg_spd;
			remained = (double)offset / dinfo.data_size;
			percent = remained * 100;
		}
	}

	printf("avg_spd %.2f, finish percent %d%%, remain %lu seconds\n", avg_spd, percent, finish_time);

	close(shm_fd);

	return 0;
}

/**********/

static int fix_pending_sector(int fd, const __u64 rd_offset, const size_t size)
{
	off64_t offset, scaned_size = 0;
	int i, ret, fixed = 0;

	while (1) {
		offset = rd_offset + scaned_size;
		if (lseek64(fd, offset, SEEK_SET) < 0LL) {
			if (EOVERFLOW != errno) {
				perror("seek error");
				return -1;
			}
		}
		ret = read(fd, buf, dinfo.phy_sector_size);

		if (ret < 0) {
			if (EIO != errno)
				return -1;

			fixed = 0;
			for (i = 0; i < RETRY; i ++) {
				if (lseek64(fd, offset, SEEK_SET) < 0LL) {
					perror("seek error");
					return -1;
				}
				memset(buf, 0, dinfo.phy_sector_size);
				ret = write(fd, buf, dinfo.phy_sector_size);
				if (ret != dinfo.phy_sector_size) {
					perror("write badsector error");
					continue;
				}

				if (lseek64(fd, offset, SEEK_SET) < 0LL) {
					perror("seek error");
					return -1;
				}
				ret = read(fd, buf, dinfo.phy_sector_size);
				if (ret < 0) {
					perror("reread error");
					continue;
				}

				fixed = 1;
				syslog(LOG_WARNING, "%s uuid: %x role %d: fixed %"PRId64"-%d\n",
						dinfo.name, dinfo.raid_uuid[0], dinfo.role,
						offset / SECTOR_SIZE, dinfo.phy_sector_size / SECTOR_SIZE);
				break;
			}

			if (!fixed) {
				syslog(LOG_WARNING, "%s uuid: %x role %d: can't fix %"PRId64"-%d\n",
						dinfo.name, dinfo.raid_uuid[0], dinfo.role,
						offset / SECTOR_SIZE, dinfo.phy_sector_size / SECTOR_SIZE);
				return -1;
			}
		}

		if (scaned_size > size)
			break;

		scaned_size += dinfo.phy_sector_size;
	}

	return 0;
}

static int fix_bad_sector(int fd, off64_t start_offset)
{
	off64_t offset;
	__u64 scaned_size = 0;
	ssize_t size;
	int ret, shm_fd, last = 0;
	struct timeval ctime;
	time_t last_sec;

	start_offset = (start_offset * 2 / dinfo.data_disks / dinfo.chunk_size) * dinfo.chunk_size;
	start_offset += dinfo.data_offset;
	dinfo.start_offset = start_offset;

	gettimeofday(&dinfo.stime, NULL);
	last_sec = dinfo.stime.tv_sec;

	openlog("fix_bad_sector", LOG_CONS | LOG_PID, LOG_USER);

	shm_fd = open_shm_file(1);
	if (shm_fd < 0)
		return 1;

	write_status(shm_fd, 0, 0);

	syslog(LOG_INFO, "start_offset %"PRId64 " uuid %X:%X:%X:%X role %d\n", start_offset, dinfo.raid_uuid[0],
			dinfo.raid_uuid[1], dinfo.raid_uuid[2], dinfo.raid_uuid[3], dinfo.role);

	if (start_offset > dinfo.data_size) {
		write_status(shm_fd, dinfo.data_size, 1);
		return 0;
	}

	while (1) {
		offset = start_offset + scaned_size;
		if (lseek64(fd, offset, SEEK_SET) < 0LL) {
			if (EOVERFLOW != errno) {
				perror("seek error");
				write_status(shm_fd, offset, 2);
				return 0;
			}
			break;
		}
		if (dinfo.data_size - offset > BUF_SIZE) {
			size = BUF_SIZE;
		} else {
			size = dinfo.data_size - offset;
			last = 1;
		}

		ret = read(fd, buf, size);
		if (ret < 0) {
			if (EIO == errno) {
				ret = fix_pending_sector(fd, offset, size);
				if (ret) {
					perror("Can't fix pending sector");
					write_status(shm_fd, offset, 2);
					return 0;
				}
			} else {
				perror("Other error happened");
				write_status(shm_fd, offset, 2);
				return 0;
			}
		}

		scaned_size += size;

		if (last)
			break;

		gettimeofday(&ctime, NULL);
		if (ctime.tv_sec > last_sec + INTERVAL) {
			last_sec = ctime.tv_sec;
			write_status(shm_fd, offset, 0);
		}
	}

	offset = start_offset + scaned_size;
	write_status(shm_fd, offset, 1);
	close(shm_fd);
	closelog();

	return 0;
}

static int open_excl(const char *devname)
{
	int fd;

	fd = open(devname, O_RDWR | O_DIRECT);
	if (fd < 0) {
		perror("open error");
		exit(1);
	}

	return fd;
}

static int open_ro(const char *devname)
{
	int fd;

	fd = open(devname, O_RDONLY | O_DIRECT);
	if (fd < 0) {
		perror("open error");
		exit(1);
	}

	return fd;
}

/**************************************/

static int check()
{
	char cmd[64];
	char buf[128];
	int count = 0;
	FILE *fp;

	sprintf(cmd, "ps -ef|grep \"%s -f %s \"| grep -v grep | wc -l", PROC_NAME, dinfo.name);
	fp = popen(cmd, "r");
	if (fp != NULL) {
		memset(buf, 0, sizeof(buf));
		fgets(buf, sizeof(buf), fp);
		count = atoi(buf);
	}

	if (count > 1) {
		printf("more than one is running\n");
		return -1;
	} else
		return 0;
}

static int stop_fixing()
{
	char cmd[64];
	char buf[128];
	int kpid = 0;
	FILE *fp;

	sprintf(cmd, "ps -ef|grep \"%s -f %s \"| grep -v grep | awk '{print $2}'", PROC_NAME, dinfo.name);

	fp = popen(cmd, "r");
	if (fp != NULL) {
		memset(buf, 0, sizeof(buf));
		fgets(buf, sizeof(buf), fp);
		kpid = atoi(buf);
	}

	kill(kpid, SIGINT);

	return 0;
}

static int deamon_init()
{
	int fd;

	switch (fork()) {
	case -1:
		return -1;
	case 0:
		break;
	default:
		_exit(EXIT_SUCCESS);
	}

	if (setsid() == -1)
		return -1;

	if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
		if(dup2(fd, STDIN_FILENO) < 0) {
			perror("dup2 stdin");
			return -1;
		}
		if(dup2(fd, STDOUT_FILENO) < 0) {
			perror("dup2 stdout");
			return -1;
		}
		if(dup2(fd, STDERR_FILENO) < 0) {
			perror("dup2 stderr");
			return -1;
		}

		if (fd > STDERR_FILENO) {
			if(close(fd) < 0) {
				perror("close");
				return -1;
			}
		}
	}

	return 0;
}

/**************************************/

int main(int argc, char **argv)
{
	int fd, ret, vaild_opt = 0;
	off64_t start_offset = 0;
	static const char *option_string = "x:f:s:";
	int option = 0, tmp = 0;

	memset(&dinfo, 0, sizeof(struct device_info));

	buf = valloc(BUF_SIZE);
	if (NULL == buf) {
		perror("alloc error");
		return 1;
	}

	while ((option = getopt(argc, argv, option_string)) != EOF) {
		switch (option) {
		case 'f':
			if (optind + 1 != argc)
				usage();

			fd = open_excl(optarg);
			start_offset = atoll(argv[optind]);
			tmp = 1;
			vaild_opt = 1;

			break;
		case 'x':
		case 's':
			if (optind != argc)
				usage();

			fd = open_ro(optarg);
			tmp = 1;
			vaild_opt = 1;

			break;
		default:
			usage();
		}
		if (tmp)
			break;
	}

	if (! vaild_opt) {
		usage();
		return 1;
	}

	dinfo.name = optarg;
	ret = get_devinfo(fd);
	if (ret == -2) {
		printf("not a valid raid device");
		return 1;
	} else if (ret != 0)
		return 1;

	switch (option) {
	case 'f':
		ret = check(argv[0]);
		if (ret)
			return 0;
		ret = deamon_init();
		if (ret)
			return 0;
		fix_bad_sector(fd, start_offset);
		break;
	case 's':
		print_status();
		break;
	case 'x':
		stop_fixing();
		clear_status();
		break;
	}

	close(fd);

	return 0;
}
