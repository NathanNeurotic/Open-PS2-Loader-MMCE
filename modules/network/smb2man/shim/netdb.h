/* IOP shim for <netdb.h>.

   ps2ip provides getaddrinfo/freeaddrinfo (as lwip_*), but not the EAI_* error names or
   getprotobyname. Only the pieces libsmb2 references are declared. */
#ifndef SMB2MAN_SHIM_NETDB_H
#define SMB2MAN_SHIM_NETDB_H

#include <stddef.h>
#include <ps2ip.h>

#ifndef EAI_AGAIN
#define EAI_AGAIN -3
#endif
#ifndef EAI_FAIL
#define EAI_FAIL -4
#endif
#ifndef EAI_NONAME
#define EAI_NONAME -2
#endif
#ifndef EAI_MEMORY
#define EAI_MEMORY -10
#endif

struct protoent
{
    char *p_name;
    char **p_aliases;
    int p_proto;
};

struct protoent *getprotobyname(const char *name);

#endif
