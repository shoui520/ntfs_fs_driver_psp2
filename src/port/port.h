/* Freestanding environment glue for libntfs-3g in a PS Vita kernel module. */
#ifndef NTFSFS_PORT_H
#define NTFSFS_PORT_H

#define __timespec_defined 1
#define HAVE_LOCALE_H 1

#include <sys/types.h>

#ifndef makedev
#define major(dev)        ((int)(((unsigned int)(dev) >> 8) & 0xfff))
#define minor(dev)        ((int)((unsigned int)(dev) & 0xff))
#define makedev(maj, min) ((dev_t)(((maj) << 8) | (min)))
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#endif
