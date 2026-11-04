/* WinePS4: folders by descriptor (openat, fstatat, fchdir, fdopendir for ntdll and wineserver).
 *
 * The PS4 kernel only takes absolute paths, so a call relative to a folder's descriptor needs that
 * folder's path. ps4-kit knows it only for descriptors it opened with O_DIRECTORY; Wine opens its
 * folders without it (open(".", O_RDONLY)), duplicates them, and wineserver hands them to ntdll over
 * a socket, where they arrive under new numbers. So every folder Wine opens by path is remembered
 * here by its device and inode, and a descriptor is looked up by what fstat says about it: any
 * copy of it, under any number, finds the path, in any process (the log below). */
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

/* Every process's folders, for the others: wineserver (in the desktop's process) opens the folders
 * programs ask for and hands them the descriptors, whose paths only the desktop's table had (v00.46:
 * 100 failures a session, among them every program's look-up of comctl32 6 in winsxs, and Steam's
 * clean-up after an update). Each folder remembered is also appended to a log as a "dev ino path"
 * line; a process that doesn't know a descriptor reads what the others added since it last looked.
 * The desktop empties it at every start (ps4_dirs_reset). */
#define FOLDER_LOG "/data/WinePS4/.folders"

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_write_fd = -1, log_read_fd = -1;
static off_t log_read_pos;  /* how far this process has read the log */

static unsigned int slot_of( dev_t dev, ino_t ino )
{
    unsigned long long h = ((unsigned long long)dev * 0x9e3779b97f4a7c15ull) ^ (unsigned long long)ino;
    return (unsigned int)(h ^ (h >> 29)) & (SLOTS - 1);
}

/* one line, written whole (O_APPEND: lines from several processes don't mix) */
static void share( dev_t dev, ino_t ino, const char *path )
{
    char line[1100];
    int len;

    if (strchr( path, '\n' )) return;
    len = snprintf( line, sizeof(line), "%llx %llx %s\n", (unsigned long long)dev, (unsigned long long)ino, path );
    if (len <= 0 || len >= (int)sizeof(line)) return;
    pthread_mutex_lock( &log_lock );
    if (log_write_fd == -1 && (log_write_fd = open( FOLDER_LOG, O_WRONLY | O_APPEND | O_CREAT, 0666 )) != -1)
        fcntl( log_write_fd, F_SETFD, FD_CLOEXEC );
    if (log_write_fd != -1 && write( log_write_fd, line, len ) != len)
        fprintf( stderr, "wineps4: %s: %s\n", FOLDER_LOG, strerror( errno ) );
    pthread_mutex_unlock( &log_lock );
}

/* full (allocated) as the path of the folder dev/ino; shared with the other processes when it is new
 * to this one or has a new name */
static void remember_path( dev_t dev, ino_t ino, char *full, int shared )
{
    char copy[1024] = "";
    unsigned int i, n;

    pthread_mutex_lock( &dirs_lock );
    for (i = slot_of( dev, ino ), n = 0; n < SLOTS; i = (i + 1) & (SLOTS - 1), n++)
    {
        if (dirs[i].path && (dirs[i].dev != dev || dirs[i].ino != ino)) continue;
        if (shared && (!dirs[i].path || strcmp( dirs[i].path, full ))) snprintf( copy, sizeof(copy), "%s", full );
        free( dirs[i].path );  /* the same folder again: its latest name */
        dirs[i].dev = dev;
        dirs[i].ino = ino;
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
    else if (copy[0]) share( dev, ino, copy );
}

static void remember( const struct stat *st, const char *path )
{
    char cwd[1024], *full;

    if (path[0] == '/') full = strdup( path );
    else if (!getcwd( cwd, sizeof(cwd) ) || asprintf( &full, "%s/%s", cwd, path ) < 0) full = NULL;
    if (full) remember_path( st->st_dev, st->st_ino, full, 1 );
}

/* what the other processes added to the log since this one last read it (whole lines) */
static void read_shared( void )
{
    char buffer[16384];
    ssize_t got;

    pthread_mutex_lock( &log_lock );
    if (log_read_fd == -1 && (log_read_fd = open( FOLDER_LOG, O_RDONLY )) != -1)
        fcntl( log_read_fd, F_SETFD, FD_CLOEXEC );
    while (log_read_fd != -1 && (got = pread( log_read_fd, buffer, sizeof(buffer) - 1, log_read_pos )) > 0)
    {
        char *line = buffer, *end;

        buffer[got] = 0;
        while ((end = strchr( line, '\n' )))
        {
            unsigned long long dev, ino;
            int pos = 0;

            *end = 0;
            if (sscanf( line, "%llx %llx %n", &dev, &ino, &pos ) == 2 && pos && line[pos] == '/')
            {
                char *full = strdup( line + pos );
                if (full) remember_path( (dev_t)dev, (ino_t)ino, full, 0 );
            }
            line = end + 1;
        }
        if (line == buffer) break;  /* (a line longer than the buffer, or one still being written) */
        log_read_pos += line - buffer;
    }
    pthread_mutex_unlock( &log_lock );
}

/* the desktop, before any program runs: a new log for this session */
void ps4_dirs_reset( void )
{
    int fd = open( FOLDER_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0666 );
    if (fd != -1) close( fd );
}

static int find_path( const struct stat *st, char *out, size_t size )
{
    unsigned int i, n;
    int found = 0;

    pthread_mutex_lock( &dirs_lock );
    for (i = slot_of( st->st_dev, st->st_ino ), n = 0; n < SLOTS && dirs[i].path; i = (i + 1) & (SLOTS - 1), n++)
    {
        if (dirs[i].dev != st->st_dev || dirs[i].ino != st->st_ino) continue;
        snprintf( out, size, "%s", dirs[i].path );
        found = 1;
        break;
    }
    pthread_mutex_unlock( &dirs_lock );
    return found;
}

/* the path of the folder open as fd, in out */
static int dir_path( int fd, char *out, size_t size )
{
    struct stat st;
    int found;

    if (fstat( fd, &st ) == -1) return -1;
    if (!S_ISDIR( st.st_mode )) { errno = ENOTDIR; return -1; }
    if (!(found = find_path( &st, out, size )))
    {
        read_shared();
        found = find_path( &st, out, size );
    }
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
