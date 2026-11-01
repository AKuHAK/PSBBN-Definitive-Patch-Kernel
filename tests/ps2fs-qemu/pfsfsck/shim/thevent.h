/* Minimal thevent.h for the host build of fsck */
#ifndef PFSFSCK_THEVENT_H
#define PFSFSCK_THEVENT_H
typedef struct { unsigned int attr, option, bits; } iop_event_t;
#define EA_MULTI	0x02
#define WEF_OR		0x01
#define WEF_CLEAR	0x10
int CreateEventFlag(iop_event_t *event);
int SetEventFlag(int ef, unsigned int bits);
int WaitEventFlag(int ef, unsigned int bits, int mode, unsigned int *resbits);
int PollEventFlag(int ef, unsigned int bits, int mode, unsigned int *resbits);
#endif
