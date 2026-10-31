/* WinePS4's desktop (shell/): what the port (port/ps4_main.c) calls. The desktop runs on the main
 * thread; Wine runs one Windows program on a thread of its own, its output shown in the desktop's
 * Terminal. Picking another program restarts the app into it (one program per run of the app). */
#ifndef WINEPS4_SHELL_H
#define WINEPS4_SHELL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The desktop, until the user leaves the app. program: what Wine runs now (NULL: nothing). */
void ps4_shell_run( const char *program );

/* From the program's thread: the program's output (also in wine.log), its first-run progress,
 * and its end (Wine's exit, which then waits forever: the desktop stays). */
void ps4_shell_output( const char *text, size_t len );
void ps4_shell_status( const char *text );
void ps4_shell_program_ended( int status );

/* The program to run in the next run of the app: written by the desktop before it restarts the
 * app, read (and removed) by ps4_main at the start */
#define WINEPS4_LAUNCH_FILE "/data/WinePS4/launch.txt"

#ifdef __cplusplus
}
#endif

#endif
