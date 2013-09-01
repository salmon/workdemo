#include "bad_blocks.h"

int main(int args, char **argv)
{
	int len = 0, ret;

	if (args != 2) {
		printf("wrong arguments\n");
		exit(1);
	}

	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		printf("open error\n");
		exit(1);
	}

	ret = is_badblock(fd, 35*1024*1024, 37*1024*1024, 1);
	if (ret > 0) {
		printf("hit a badblock\n");
	} else if (ret == 0) {
		printf("not hit a badblock\n");
	} else
		printf("error\n");

	close(fd);

	return 0;
}
