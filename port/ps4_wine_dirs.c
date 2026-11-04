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
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
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

/* --- Open files ----------------------------------------------------------------------------------------
 * The PS4 has room for 13952 open files on the whole console (12556 in one process), and wineserver
 * holds every program's files in the desktop's process (v00.48: during Steam's update the Chrome
 * installer's processes could not even open kernel32.dll). Every file opened by name is remembered
 * under its descriptor; when an open fails for lack of room, the log gets what this process holds:
 * by kind, by folder and the files open more than once (Settings > Diagnostics > Open files). */
#define FD_NAMES 16384

static struct { dev_t dev; ino_t ino; char *name; } fd_names[FD_NAMES];
static pthread_mutex_t fd_names_lock = PTHREAD_MUTEX_INITIALIZER;

int sysctlbyname( const char *name, void *old, size_t *old_size, const void *new_value, size_t new_size );

static void note_fd( int fd, const struct stat *st, const char *path )
{
    char cwd[1024], *name = NULL, *old;

    if (fd < 0 || fd >= FD_NAMES) return;
    if (path[0] == '/') name = strdup( path );
    else if (getcwd( cwd, sizeof(cwd) ) && asprintf( &name, "%s/%s", cwd, path ) < 0) name = NULL;
    pthread_mutex_lock( &fd_names_lock );
    old = fd_names[fd].name;
    fd_names[fd].dev = st->st_dev;
    fd_names[fd].ino = st->st_ino;
    fd_names[fd].name = name;
    pthread_mutex_unlock( &fd_names_lock );
    free( old );
}

static int kernel_int( const char *name )
{
    int value = -1;
    size_t size = sizeof(value);
    return sysctlbyname( name, &value, &size, NULL, 0 ) ? -1 : value;
}

struct tally { char *key; unsigned int count; };

/* one more of key (its first len characters) in a table of size entries (a power of two) */
static void tally_add( struct tally *table, unsigned int size, const char *key, size_t len )
{
    unsigned int hash = 5381, i, n;

    for (i = 0; i < len; i++) hash = hash * 33 + (unsigned char)key[i];
    for (i = hash & (size - 1), n = 0; n < size; i = (i + 1) & (size - 1), n++)
    {
        if (!table[i].key)
        {
            if ((table[i].key = strndup( key, len ))) table[i].count = 1;
            return;
        }
        if (!strncmp( table[i].key, key, len ) && !table[i].key[len])
        {
            table[i].count++;
            return;
        }
    }
}

/* the most counted (at least at_least), largest first; frees the table */
static void tally_print( struct tally *table, unsigned int size, const char *what, int lines, unsigned int at_least )
{
    unsigned int i;

    while (lines-- > 0)
    {
        unsigned int best = size;
        for (i = 0; i < size; i++)
            if (table[i].key && table[i].count >= at_least && (best == size || table[i].count > table[best].count)) best = i;
        if (best == size) break;
        fprintf( stderr, "wineps4:   %u %s %s\n", table[best].count, what, table[best].key );
        table[best].count = 0;
    }
    for (i = 0; i < size; i++) free( table[i].key );
    free( table );
}

/* what this process has open, and the console in all, in the log */
void ps4_files_census( const char *why )
{
    static const char *const kind_names[] = { "files", "folders", "sockets", "pipes", "devices", "other" };
    unsigned int kinds[6] = { 0 }, total = 0, unnamed = 0, i;
    struct tally *folders = calloc( 1024, sizeof(*folders) ), *files = calloc( 8192, sizeof(*files) );
    char line[256];
    struct rlimit limit;
    int fd, max = FD_NAMES, saved = errno, len;

    if (!getrlimit( RLIMIT_NOFILE, &limit ) && limit.rlim_cur < (rlim_t)max) max = (int)limit.rlim_cur;
    for (fd = 0; fd < max; fd++)
    {
        struct stat st;
        char path[1024], *name = NULL;
        int kind;

        if (fstat( fd, &st ) == -1) continue;
        total++;
        switch (st.st_mode & S_IFMT)
        {
        case S_IFREG:  kind = 0; break;
        case S_IFDIR:  kind = 1; break;
        case S_IFSOCK: kind = 2; break;
        case S_IFIFO:  kind = 3; break;
        case S_IFCHR:  kind = 4; break;
        default:       kind = 5; break;
        }
        kinds[kind]++;
        if (kind > 1) continue;
        pthread_mutex_lock( &fd_names_lock );
        if (fd_names[fd].name && fd_names[fd].dev == st.st_dev && fd_names[fd].ino == st.st_ino)
            name = strdup( fd_names[fd].name );
        pthread_mutex_unlock( &fd_names_lock );
        if (!name && kind == 1 && find_path( &st, path, sizeof(path) )) name = strdup( path );
        if (!name)
        {
            unnamed++;
            continue;
        }
        if (folders)
        {
            const char *slash = strrchr( name, '/' );
            tally_add( folders, 1024, name, slash && slash > name ? (size_t)(slash - name) : strlen( name ) );
        }
        if (files) tally_add( files, 8192, name, strlen( name ) );
        free( name );
    }
    len = 0;
    for (i = 0; i < 6; i++)
        if (kinds[i]) len += snprintf( line + len, sizeof(line) - len, "%s%u %s", len ? ", " : "", kinds[i], kind_names[i] );
    fprintf( stderr, "wineps4: open files (%s): %u in this process (%s; %u files without a name), %d of %d on the "
             "console (%d for one process)\n", why, total, line, unnamed, kernel_int( "kern.openfiles" ),
             kernel_int( "kern.maxfiles" ), kernel_int( "kern.maxfilesperproc" ) );
    if (folders) tally_print( folders, 1024, "in", 8, 2 );
    if (files) tally_print( files, 8192, "times", 6, 2 );
    errno = saved;
}

/* an open just failed for lack of room: the census, at most every half a minute */
static void census_after_failure( void )
{
    static time_t last;
    const char *on = getenv( "WINEPS4_FILES_LOG" );
    int saved = errno;
    time_t now = time( NULL );

    if ((on && !strcmp( on, "0" )) || now - last < 30) return;
    last = now;
    ps4_files_census( saved == EMFILE ? "an open failed: this process is full" : "an open failed: the console is full" );
    errno = saved;
}

/* the desktop, every few seconds (Settings > Diagnostics > Open files): the console's count when it
 * has grown by a thousand since the last line, the census when it nears the end */
void ps4_files_watch( void )
{
    static int logged = -1, census_done;
    int open_now = kernel_int( "kern.openfiles" ), max = kernel_int( "kern.maxfiles" );

    if (open_now < 0) return;
    if (logged < 0 || open_now >= logged + 1000 || open_now <= logged - 1000)
    {
        fprintf( stderr, "wineps4: open files on the console: %d of %d\n", open_now, max );
        logged = open_now;
    }
    if (max > 0 && open_now > max * 85 / 100 && !census_done)
    {
        census_done = 1;
        ps4_files_census( "the console is almost full" );
    }
    else if (max > 0 && open_now < max * 70 / 100) census_done = 0;
}

/* fcntl with the kernel's struct flock (the compat header's lock types are already its numbers) */
struct ps4_kernel_flock
{
    off_t l_start;
    off_t l_len;
    pid_t l_pid;
    short l_type;
    short l_whence;
    int   l_sysid;
};

int ps4_wine_fcntl( int fd, int cmd, ... )
{
    unsigned long arg;
    va_list args;

    va_start( args, cmd );
    arg = va_arg( args, unsigned long );
    va_end( args );
    if (cmd == F_GETLK || cmd == F_SETLK || cmd == F_SETLKW)
    {
        struct flock *lock = (struct flock *)arg;
        struct ps4_kernel_flock kernel = { lock->l_start, lock->l_len, lock->l_pid, lock->l_type, lock->l_whence, 0 };
        int ret = fcntl( fd, cmd, &kernel );

        if (ret != -1 && cmd == F_GETLK)
        {
            lock->l_type = kernel.l_type;
            lock->l_whence = kernel.l_whence;
            lock->l_start = kernel.l_start;
            lock->l_len = kernel.l_len;
            lock->l_pid = kernel.l_pid;
        }
        return ret;
    }
    return fcntl( fd, cmd, arg );
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
    if ((fd = open( path, flags, mode )) >= 0 && !fstat( fd, &st ))
    {
        if (S_ISDIR( st.st_mode )) remember( &st, path );
        note_fd( fd, &st, path );
    }
    else if (fd == -1 && (errno == EMFILE || errno == ENFILE)) census_after_failure();
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
