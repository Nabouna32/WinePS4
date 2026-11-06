/* WinePS4: the priorities of the app's threads (256 highest .. 767 lowest, 700 the system's default).
 * The PS4 runs an app's threads by priority alone, with no turns among threads of the same one: a
 * thread that never waits keeps its core, and threads of its priority that want one wait behind it for
 * as long as it runs (ps4-kit's notes). v00.70's freeze: Steam's web helper, which draws Chromium's
 * pages on the CPU (--disable-gpu), had six threads busy on six cores and Iron Snout's loading had the
 * seventh; the desktop's loop, wineserver and the audio mixer were ready to run and ran for 0 s in 44,
 * and every program's next request to wineserver waited with them (v00.63 to v00.69's freezes looked
 * the same: the threads that "never answered" a signal were threads that never got a core). So the
 * desktop's threads run above every program's, wineserver and the mixer above the rest of the
 * desktop, the freeze report above all of it, and the launchers' web helpers below the games. */
#ifndef PS4_PRIO_H
#define PS4_PRIO_H

#define PS4_PRIO_REPORT   300  /* the freeze watch, its kernel report and memory probe */
#define PS4_PRIO_AUDIO    450  /* the desktop's mixer: the audio hardware never runs dry */
#define PS4_PRIO_SERVER   600  /* wineserver: every program waits on it */
#define PS4_PRIO_DESKTOP  640  /* the desktop's loop and the desktop's other threads */
#define PS4_PRIO_PROGRAM  700  /* a program's threads (the system's default) */
#define PS4_PRIO_WEB      760  /* a launcher's web helper (Steam's): below the games */

/* (ps4-kit) */
void ps4_kit_set_new_thread_priority( int priority );
void ps4_kit_set_thread_priority( int priority );
int ps4_kit_thread_priority( void );

#endif /* PS4_PRIO_H */
