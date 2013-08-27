#include "bad_blocks.h"

int main()
{
	int fd = -1;

	is_badblock(fd, 0, 1, 1);

	return 0;
}
