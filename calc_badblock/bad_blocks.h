#ifndef __BAD_BLOCKS_H__
#define __BAD_BLOCKS_H__

#include "vbfscommon.h"

s32 is_badblock(s32 fd, s64 offset, s32 len, s32 rw);

#endif
