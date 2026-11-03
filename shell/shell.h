/* WinePS4's desktop (shell/): what the port (port/ps4_main.c) calls, and what the desktop calls in
 * the port. The desktop process runs the desktop and wineserver; every Windows program is a process
 * of its own (/app0/wineps4.bin), its output shown in the desktop's Terminal. */
#ifndef WINEPS4_SHELL_H
#define WINEPS4_SHELL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The desktop, until the user leaves the app */
void ps4_shell_run( const char *unused );

/* From the port: programs' output (also in wine.log), the setup's progress (NULL: done), a program
 * process's end, and a program to start (launch.txt, run.txt) */
void ps4_shell_output( const char *text, size_t len );
void ps4_shell_status( const char *text );
void ps4_shell_program_exited( int pid, int code );
void ps4_shell_launch( char **args );
/* a message for the user, shown like a program's end (the PS4 refused another process) */
void ps4_shell_notice( const char *text );
/* whether a program process's display driver has reached the desktop (explorer.exe's: the Windows
 * desktop is made) */
int ps4_shell_display_connected( int pid );

/* From the desktop, in the port: whether wineserver is up, and starting a program (its Unix path or
 * Windows command line, then its arguments, NULL-ended): its pid, or -1 (errno EAGAIN: not ready) */
int ps4_wine_ready( void );
int ps4_launch_program( char *const args[] );
/* ends every program process, before the desktop exits or restarts */
void ps4_end_programs( void );

/* A program to start at the next start of the app (written by older versions; read and removed) */
#define WINEPS4_LAUNCH_FILE "/data/WinePS4/launch.txt"

#ifdef __cplusplus
}
#endif

#endif
