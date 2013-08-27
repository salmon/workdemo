CC ?= gcc
CPP ?= g++
FETCH_BB_SOURCE := bad_blocks.c test.c
CFLAGS := -Wall -g -D_LINUX_
FETCH_BB_OBJS = $(FETCH_BB_SOURCE:.c=.o)

all: fetch_bb

fetch_bb: $(FETCH_BB_OBJS)
	$(CC) $(CFLAGS) -o $@ $(FETCH_BB_OBJS)

clean:
	-rm -f $(FETCH_BB_OBJS) fetch_bb

