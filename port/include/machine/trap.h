/* WinePS4: FreeBSD amd64 trap numbers (mcontext mc_trapno). The probe saw ud2 = 1, int3 = 3,
 * a page fault = 12 and a divide by zero = 18 on the console. */
#ifndef PS4_MACHINE_TRAP_H
#define PS4_MACHINE_TRAP_H
#define T_PRIVINFLT   1
#define T_BPTFLT      3
#define T_ARITHTRAP   6
#define T_PROTFLT     9
#define T_TRCTRAP     10
#define T_PAGEFLT     12
#define T_ALIGNFLT    14
#define T_DIVIDE      18
#define T_NMI         19
#define T_OFLOW       20
#define T_BOUND       21
#define T_DNA         22
#define T_DOUBLEFLT   23
#define T_FPOPFLT     24
#define T_TSSFLT      25
#define T_SEGNPFLT    26
#define T_STKFLT      27
#define T_MCHK        28
#define T_XMMFLT      29
#define T_RESERVED    30
#endif
