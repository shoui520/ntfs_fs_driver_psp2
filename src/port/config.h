/* Hand-written config.h for building libntfs-3g inside a PS Vita kernel module. */
#ifndef NTFSFS_CONFIG_H
#define NTFSFS_CONFIG_H

#define PACKAGE_NAME    "ntfs-3g"
#define PACKAGE_VERSION "2022.10.3-psp2"
#define VERSION         PACKAGE_VERSION

#define HAVE_STDIO_H     1
#define HAVE_STDLIB_H    1
#define HAVE_STRING_H    1
#define HAVE_ERRNO_H     1
#define HAVE_STDARG_H    1
#define HAVE_STDINT_H    1
#define HAVE_STDDEF_H    1
#define HAVE_INTTYPES_H  1
#define HAVE_LIMITS_H    1
#define HAVE_CTYPE_H     1
#define HAVE_TIME_H      1
#define HAVE_FCNTL_H     1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H  1
#define HAVE_SYS_PARAM_H 1
#define HAVE_UNISTD_H    1
#ifdef NTFSFS_HOST
#define HAVE_ENDIAN_H 1  /* host test build (glibc) */
#else
#define HAVE_MACHINE_ENDIAN_H 1
#endif
#define HAVE_DAEMON      1 /* not built: never called */

/* Kernel build: no FUSE, no syslog, no locale-dependent conversion. */
#define NO_NTFS_DEVICE_DEFAULT_IO_OPS 1
#define WORDS_LITTLEENDIAN 1

#include "port.h"

#endif
