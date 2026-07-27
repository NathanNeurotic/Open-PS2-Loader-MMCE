/* IOP shim for <setjmp.h>. Only a jmp_buf type is needed to satisfy declarations libsmb2 pulls in
   transitively; this module never longjmps. */
#ifndef SMB2MAN_SHIM_SETJMP_H
#define SMB2MAN_SHIM_SETJMP_H
typedef struct { unsigned int regs[32]; } jmp_buf[1];
#endif
