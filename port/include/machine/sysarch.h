/* WinePS4: FreeBSD's machine/sysarch.h; the functions are in port/ps4_wine_port.c. */
#ifndef PS4_MACHINE_SYSARCH_H
#define PS4_MACHINE_SYSARCH_H
#define I386_GET_LDT      0
#define I386_SET_LDT      1
#define AMD64_GET_FSBASE  128
#define AMD64_SET_FSBASE  129
#define AMD64_GET_GSBASE  130
#define AMD64_SET_GSBASE  131
int sysarch( int number, void *args );
int amd64_get_gsbase( void **base );
int amd64_set_gsbase( void *base );
int amd64_get_fsbase( void **base );
int amd64_set_fsbase( void *base );
#endif
