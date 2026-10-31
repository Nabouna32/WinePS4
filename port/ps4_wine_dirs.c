/* WinePS4: folders by descriptor (openat, fstatat, fchdir, fdopendir for ntdll and wineserver).
 *
 * The PS4 kernel only takes absolute paths, so a call relative to a folder's descriptor needs that
 * folder's path. ps4-kit knows it only for descriptors it opened with O_DIRECTORY; Wine opens its
 * folders without it (open(".", O_RDONLY)), duplicates them, and wineserver hands them to ntdll over
 * a socket, where they arrive under new numbers. So every folder Wine opens by path is remembered
 * here by its device and inode, and a descriptor is looked up by what fstat says about it: any
 * copy of it, under any number, finds the path. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SLOTS 16384  /* a power of two */

static struct { dev_t dev; ino_t ino; char *path; } dirs[SLOTS];
static pthread_mutex_t dirs_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned int slot_of( dev_t dev, ino_t ino )
{
    unsigned long long h = ((unsigned long long)dev * 0x9e3779b97f4a7c15ull) ^ (unsigned long long)ino;
    return (unsigned int)(h ^ (h >> 29)) & (SLOTS - 1);
}

static void remember( const struct stat *st, const char *path )
{
    char cwd[1024], *full;
    unsigned int i, n;

    if (path[0] == '/') full = strdup( path );
    else if (!getcwd( cwd, sizeof(cwd) ) || asprintf( &full, "%s/%s", cwd, path ) < 0) full = NULL;
    if (!full) return;

    pthread_mutex_lock( &dirs_lock );
    for (i = slot_of( st->st_dev, st->st_ino ), n = 0; n < SLOTS; i = (i + 1) & (SLOTS - 1), n++)
    {
        if (dirs[i].path && (dirs[i].dev != st->st_dev || dirs[i].ino != st->st_ino)) continue;
        free( dirs[i].path );  /* the same folder again: its latest name */
        dirs[i].dev = st->st_dev;
        dirs[i].ino = st->st_ino;
        dirs[i].path = full;
        full = NULL;
        break;
    }
    pthread_mutex_unlock( &dirs_lock );
    if (full)
    {
        fprintf( stderr, "wineps4: folder table full, %s not remembered\n", full );
        free( full );
    }
}

/* the path of the folder open as fd, in out */
static int dir_path( int fd, char *out, size_t size )
{
    struct stat st;
    unsigned int i, n;
    int found = 0;

    if (fstat( fd, &st ) == -1) return -1;
    if (!S_ISDIR( st.st_mode )) { errno = ENOTDIR; return -1; }
    pthread_mutex_lock( &dirs_lock );
    for (i = slot_of( st.st_dev, st.st_ino ), n = 0; n < SLOTS && dirs[i].path; i = (i + 1) & (SLOTS - 1), n++)
    {
        if (dirs[i].dev != st.st_dev || dirs[i].ino != st.st_ino) continue;
        snprintf( out, size, "%s", dirs[i].path );
        found = 1;
        break;
    }
    pthread_mutex_unlock( &dirs_lock );
    if (!found)
    {
        fprintf( stderr, "wineps4: folder descriptor %d (inode %llu) was not opened by path\n", fd,
                 (unsigned long long)st.st_ino );
        errno = EBADF;
        return -1;
    }
    return 0;
}

/* name relative to the folder open as dirfd, as a path the kernel takes (or relative to the
 * working directory, which ps4-kit resolves) */
static int full_name( int dirfd, const char *name, char *out, size_t size )
{
    size_t len;

    if (name[0] == '/' || dirfd == AT_FDCWD)
    {
        snprintf( out, size, "%s", name );
        return 0;
    }
    if (dir_path( dirfd, out, size ) == -1) return -1;
    len = strlen( out );
    if (name[0]) snprintf( out + len, size - len, "/%s", name );
    return 0;
}

int ps4_wine_open( const char *path, int flags, ... )
{
    struct stat st;
    int mode = 0, fd;

    if (flags & O_CREAT)
    {
        va_list args;
        va_start( args, flags );
        mode = va_arg( args, int );
        va_end( args );
    }
    if ((fd = open( path, flags, mode )) >= 0 && !fstat( fd, &st ) && S_ISDIR( st.st_mode ))
        remember( &st, path );
    return fd;
}

int ps4_wine_openat( int dirfd, const char *name, int flags, ... )
{
    char full[2048];
    int mode = 0;

    if (flags & O_CREAT)
    {
        va_list args;
        va_start( args, flags );
        mode = va_arg( args, int );
        va_end( args );
    }
    if (full_name( dirfd, name, full, sizeof(full) ) == -1) return -1;
    return ps4_wine_open( full, flags, mode );
}

int ps4_wine_fstatat( int dirfd, const char *name, struct stat *st, int flags )
{
    char full[2048];

    if (!name[0] && dirfd != AT_FDCWD) return fstat( dirfd, st );
    if (full_name( dirfd, name, full, sizeof(full) ) == -1) return -1;
    return (flags & AT_SYMLINK_NOFOLLOW) ? lstat( full, st ) : stat( full, st );
}

int ps4_wine_fchdir( int fd )
{
    char path[2048];

    if (dir_path( fd, path, sizeof(path) ) == -1) return -1;
    return chdir( path );
}

/* the folder opened again by path; fd is the DIR's from here on, so it is closed now */
DIR *ps4_wine_fdopendir( int fd )
{
    char path[2048];
    DIR *dir;

    if (dir_path( fd, path, sizeof(path) ) == -1) return NULL;
    if (!(dir = opendir( path ))) return NULL;
    close( fd );
    return dir;
}
