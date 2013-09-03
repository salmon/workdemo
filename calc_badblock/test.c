#include "bad_blocks.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int main(int args, char **argv)
{
	int ret, len, rw;
	unsigned long long start_offset;

	if (args != 5) {
		printf("%s [device] [start_offset] [len] [rw]\n", argv[0]);
		exit(1);
	}

	start_offset = strtoull(argv[2], NULL, 10);
	len = atoi(argv[3]);
	rw = atoi(argv[4]);

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		printf("open error\n");
		exit(1);
	}

	ret = is_badblock(fd, start_offset, len, rw);
	if (ret > 0) {
		printf("%s %s start_offset %llu, end_offset %llu hit a badblock\n",
		      argv[1], rw ? "WRITE" : "READ", start_offset, start_offset + len);
	} else if (ret == 0) {
		printf("%s %s start_offset %llu, end_offset %llu not hit a badblock\n",
		      argv[1], rw ? "WRITE" : "READ", start_offset, start_offset + len);
	} else
		printf("error\n");

	close(fd);

	return 0;
}
