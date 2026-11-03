/* WinePS4: the console's name, for Windows (its computer name) and for lookups of it.
 *
 * The desktop takes the console's System Name (Settings > System > System Information) as
 * WINEPS4_HOSTNAME (port/ps4_main.c), for every process it starts. Wine's Unix side reaches these
 * through ps4_wine_compat.h: gethostname, and lookups of that name, which give the loopback address
 * without asking DNS (wineboot looks the name up at every start; ws2_32 for programs). Every other
 * name goes on to the C library, through ps4-kit's resolver. (Not built with ps4_wine_compat.h: the
 * calls here are the real ones.) */
#include <errno.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

static const char *console_name( void )
{
    const char *name = getenv( "WINEPS4_HOSTNAME" );
    return name && *name ? name : "PS4";
}

int ps4_gethostname( char *name, size_t size )
{
    const char *value = console_name();

    if (strlen( value ) >= size)
    {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy( name, value );
    return 0;
}

int ps4_getaddrinfo( const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res )
{
    static char canon[64];
    struct addrinfo local = { 0 };
    int ret;

    if (!node || strcasecmp( node, console_name() )) return getaddrinfo( node, service, hints, res );
    if (hints) local = *hints;
    if (local.ai_family != AF_UNSPEC && local.ai_family != AF_INET) return EAI_NONAME;  /* (no IPv6) */
    local.ai_family = AF_INET;
    local.ai_flags = (local.ai_flags & ~AI_CANONNAME) | AI_NUMERICHOST;
    if ((ret = getaddrinfo( "127.0.0.1", service, &local, res ))) return ret;
    if (hints && (hints->ai_flags & AI_CANONNAME))
    {
        /* (the C library frees the results as one block: the name isn't freed with them) */
        strncpy( canon, console_name(), sizeof(canon) - 1 );
        (*res)->ai_canonname = canon;
    }
    return 0;
}

struct hostent *ps4_gethostbyname( const char *name )
{
    static char canon[64];
    struct hostent *host;

    if (!name || strcasecmp( name, console_name() )) return gethostbyname( name );
    if ((host = gethostbyname( "127.0.0.1" )))  /* (ps4-kit's static result, made again each call) */
    {
        strncpy( canon, console_name(), sizeof(canon) - 1 );
        host->h_name = canon;
    }
    return host;
}
