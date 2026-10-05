#ifndef NTFSFS_LOG_H
#define NTFSFS_LOG_H

#include <psp2kern/kernel/debug.h>

#define NTFSFS_LOG(fmt, ...) ksceKernelPrintf("[ntfsfs] " fmt, ##__VA_ARGS__)

#endif
