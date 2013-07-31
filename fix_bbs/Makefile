CC ?= gcc
FIX_SECTOR_SOURCE := fix_sector.c
CFLAGS := -Wall -g
FIX_SECTOR_OBJS = $(FIX_SECTOR_SOURCE:.c=.o)

all: fix_sector

fix_sector: $(FIX_SECTOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $(FIX_SECTOR_OBJS)

clean:
	-rm -f $(FIX_SECTOR_OBJS) fix_sector
