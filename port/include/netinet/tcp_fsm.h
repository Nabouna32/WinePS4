/* WinePS4: FreeBSD's TCP states, as the PS4's kernel gives them in struct tcp_info's tcpi_state
 * (OpenOrbis has no netinet/tcp_fsm.h; without one Wine's server numbers them as Linux does, and
 * the TCP table steam.exe checks its web helper's connection in would say FIN_WAIT_1 for an
 * established one). */
#ifndef PS4_NETINET_TCP_FSM_H
#define PS4_NETINET_TCP_FSM_H

#define TCP_NSTATES       11
#define TCPS_CLOSED       0
#define TCPS_LISTEN       1
#define TCPS_SYN_SENT     2
#define TCPS_SYN_RECEIVED 3
#define TCPS_ESTABLISHED  4
#define TCPS_CLOSE_WAIT   5
#define TCPS_FIN_WAIT_1   6
#define TCPS_CLOSING      7
#define TCPS_LAST_ACK     8
#define TCPS_FIN_WAIT_2   9
#define TCPS_TIME_WAIT    10

#endif
