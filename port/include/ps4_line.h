/* WinePS4: a line of text built without the C library's printf, for what must run while other threads
 * stand still holding locks (the desktop's freeze report, port/ps4_main.c and ps4_wine_port.c).
 * ps4-kit wraps vfprintf in its one stdio lock, and snprintf goes through vfprintf: v00.63's thread
 * dump stopped at a thread's line. */
#ifndef PS4_LINE_H
#define PS4_LINE_H

struct ps4_line
{
    char text[1024];
    int len;
};

static inline void ps4_line_str( struct ps4_line *l, const char *s )
{
    while (s && *s && l->len < (int)sizeof(l->text) - 1) l->text[l->len++] = *s++;
}

static inline void ps4_line_num( struct ps4_line *l, long long v )
{
    char digits[24];
    unsigned long long u = v < 0 ? 0ull - (unsigned long long)v : (unsigned long long)v;
    int n = 0;

    if (v < 0) ps4_line_str( l, "-" );
    do digits[n++] = (char)('0' + u % 10); while ((u /= 10) && n < (int)sizeof(digits));
    while (n && l->len < (int)sizeof(l->text) - 1) l->text[l->len++] = digits[--n];
}

static inline void ps4_line_hex( struct ps4_line *l, unsigned long long v )
{
    char digits[16];
    int n = 0;

    ps4_line_str( l, "0x" );
    do digits[n++] = "0123456789abcdef"[v & 15]; while ((v >>= 4) && n < (int)sizeof(digits));
    while (n && l->len < (int)sizeof(l->text) - 1) l->text[l->len++] = digits[--n];
}

/* seconds with two decimals, at least width characters before the point (the log's "[  93.08]") */
static inline void ps4_line_seconds( struct ps4_line *l, double seconds, int width )
{
    long long hundredths = (long long)(seconds * 100 + 0.5), whole = hundredths / 100;
    int n = 1;
    long long w = whole;

    while ((w /= 10)) n++;
    while (n++ < width) ps4_line_str( l, " " );
    ps4_line_num( l, whole );
    ps4_line_str( l, "." );
    ps4_line_num( l, hundredths / 10 % 10 );
    ps4_line_num( l, hundredths % 10 );
}

#endif /* PS4_LINE_H */
