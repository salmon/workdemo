#ifndef _VBFS_COMMON_H__
#define _VBFS_COMMON_H__

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned char u8;
typedef char s8;
typedef int s32;
typedef unsigned int u32;

#if __WORDSIZE == 64
typedef long s64;
typedef unsigned long u64;
#else
typedef long long s64;
typedef unsigned long long u64;
#endif

#endif
