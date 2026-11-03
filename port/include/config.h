/* WinePS4: Wine's config.h for the PS4 (OpenOrbis C library on the PS4's FreeBSD 9 kernel).
 * Written by hand from the Linux build's: only what the console has. Kept small on purpose; each
 * HAVE_ added here must be something the PS4 really provides (a header can exist in OpenOrbis
 * without the kernel behind it, e.g. sys/epoll.h). */
#ifndef __WINE_CONFIG_H
#define __WINE_CONFIG_H

#include "ps4_wine_compat.h"

#define EXEEXT ""

#define HAVE_ARPA_INET_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_ELF_H 1
#define HAVE_FUTIMES 1
#define HAVE_GETADDRINFO 1
/* ps4-kit's: the kernel's kern.arandom (the C library's is a stub; without it ntdll only read
 * /dev/urandom, which leaves the buffer as it was when it can't be opened: BCryptGenRandom, TLS keys) */
#define HAVE_GETRANDOM 1
#define HAVE_IFADDRS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_PTHREAD_GETTHREADID_NP 1
#define HAVE_PWD_H 1
#define HAVE_SCHED_H 1
#define HAVE_SCHED_YIELD 1
#define HAVE_SIGPROCMASK 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_STRUCT_STAT_ST_ATIM 1
#define HAVE_STRUCT_STAT_ST_CTIM 1
#define HAVE_STRUCT_STAT_ST_MTIM 1
#define HAVE_STRUCT_STAT_ST_BIRTHTIM 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_STATFS_H 1
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIMES_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UCONTEXT_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_UTSNAME_H 1
#define HAVE_UNISTD_H 1
#define HAVE___BUILTIN_POPCOUNT 1

/* FreeType is linked into the app; win32u "dlopens" it from the built-in table (ps4_unixlibs.c) */
#define HAVE_FT2BUILD_H 1
#define SONAME_LIBFREETYPE "libfreetype.so.6"

#define PACKAGE_BUGREPORT "wine-devel@winehq.org"
#define PACKAGE_NAME "Wine"
#define PACKAGE_STRING "Wine 11.0"
#define PACKAGE_TARNAME "wine"
#define PACKAGE_URL "https://www.winehq.org"
#define PACKAGE_VERSION "11.0"
#define STDC_HEADERS 1

#endif
