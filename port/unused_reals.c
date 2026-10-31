// The kit wraps these file functions (every call goes to its ps4_fileat.cpp versions), and with no
// other library calling the real ones lld leaves them as imports that no PS4 module provides:
// create-fself refuses the eboot ("missing module for symbol (openat)"); lld does not fetch libc.a's
// definitions for a wrap. Nothing calls these.
#include <errno.h>

#define UNUSED_REAL(name) \
    int name() {          \
        errno = ENOSYS;   \
        return -1;        \
    }

UNUSED_REAL(openat)
UNUSED_REAL(fchdir)
UNUSED_REAL(fchmodat)
UNUSED_REAL(fstatat)
UNUSED_REAL(fstatvfs)
UNUSED_REAL(readlink)
UNUSED_REAL(statvfs)
UNUSED_REAL(unlinkat)
UNUSED_REAL(utimensat)

char* getcwd() {
    errno = ENOSYS;
    return 0;
}
char* realpath() {
    errno = ENOSYS;
    return 0;
}
