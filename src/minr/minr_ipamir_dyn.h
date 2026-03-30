#ifndef ABC__src__minr__minr_ipamir_dyn_h
#define ABC__src__minr__minr_ipamir_dyn_h

#include <stdint.h>

ABC_NAMESPACE_HEADER_START

typedef struct Minr_IpamirApi_t_ Minr_IpamirApi_t;

struct Minr_IpamirApi_t_
{
    void *  handle; // dlopen handle

    const char * (*ipamir_signature)();
    void *  (*ipamir_init)();
    void    (*ipamir_release)(void *);
    void    (*ipamir_add_hard)(void *, int32_t);
    void    (*ipamir_add_soft_lit)(void *, int32_t, uint64_t);
    void    (*ipamir_assume)(void *, int32_t);
    int32_t (*ipamir_solve)(void *);
    uint64_t(*ipamir_val_obj)(void *);
    int32_t (*ipamir_val_lit)(void *, int32_t);
    void    (*ipamir_set_terminate)(void *, void *, int (*)(void *));
};

// Loads ipamir API from a shared library path (dlopen).
// Returns 1 on success, 0 on failure (prints reason to stdout/stderr).
int Minr_IpamirApiLoad( Minr_IpamirApi_t * pApi, const char * pSoPath );
void Minr_IpamirApiUnload( Minr_IpamirApi_t * pApi );

ABC_NAMESPACE_HEADER_END

#endif

