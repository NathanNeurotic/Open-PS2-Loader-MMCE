#ifndef STUB_PS2IP_H
#define STUB_PS2IP_H
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
typedef signed char s8;
typedef unsigned char u8;
typedef unsigned short u16;
typedef short s16;
typedef signed int s32;
typedef unsigned int u32;
typedef unsigned long long u64;
#define SHUT_RDWR_STUB 2
/* the scripted peer, driven by the test */
extern const unsigned char *tstData;
extern int tstLen, tstPos, tstChunk, tstSelectFail;
int stub_select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *t);
int stub_recv(int s, void *b, int l, int f);
int stub_send(int s, const void *b, int l, int f);
int stub_closesocket(int s);
int stub_shutdown(int s, int how);
#define select      stub_select
#define recv        stub_recv
#define send        stub_send
#define closesocket stub_closesocket
#define shutdown    stub_shutdown
#define SHUT_RDWR   2
#endif
