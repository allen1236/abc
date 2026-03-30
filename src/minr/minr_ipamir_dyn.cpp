/**CFile****************************************************************
  FileName    [minr_ipamir_dyn.cpp]
  SystemName  [ABC: Logic synthesis and verification system.]
  PackageName [Minr: MaxSAT-based partial reset minimization.]
  Synopsis    [Runtime loading of IPAMIR MaxSAT API.]
***********************************************************************/

#include "minr.h"
#include "minr_ipamir_dyn.h"

#include <string.h>

#ifndef WIN32
#include <dlfcn.h>
#endif

ABC_NAMESPACE_IMPL_START

static void * Minr_DlSym( void * h, const char * s )
{
#ifdef WIN32
    (void)h; (void)s;
    return NULL;
#else
    return dlsym( h, s );
#endif
}

int Minr_IpamirApiLoad( Minr_IpamirApi_t * pApi, const char * pSoPath )
{
    memset( pApi, 0, sizeof(*pApi) );
#ifdef WIN32
    printf("[Minr] IPAMIR dlopen not supported on WIN32 build.\n");
    (void)pSoPath;
    return 0;
#else
    void * h = dlopen( pSoPath, RTLD_NOW | RTLD_LOCAL );
    if ( !h )
    {
        printf("[Minr] dlopen failed for %s: %s\n", pSoPath, dlerror() );
        return 0;
    }

    pApi->handle = h;

    pApi->ipamir_signature     = (const char * (*)())Minr_DlSym( h, "ipamir_signature" );
    pApi->ipamir_init          = (void * (*)())Minr_DlSym( h, "ipamir_init" );
    pApi->ipamir_release       = (void (*)(void *))Minr_DlSym( h, "ipamir_release" );
    pApi->ipamir_add_hard      = (void (*)(void *, int32_t))Minr_DlSym( h, "ipamir_add_hard" );
    pApi->ipamir_add_soft_lit  = (void (*)(void *, int32_t, uint64_t))Minr_DlSym( h, "ipamir_add_soft_lit" );
    pApi->ipamir_assume        = (void (*)(void *, int32_t))Minr_DlSym( h, "ipamir_assume" );
    pApi->ipamir_solve         = (int32_t (*)(void *))Minr_DlSym( h, "ipamir_solve" );
    pApi->ipamir_val_obj       = (uint64_t (*)(void *))Minr_DlSym( h, "ipamir_val_obj" );
    pApi->ipamir_val_lit       = (int32_t (*)(void *, int32_t))Minr_DlSym( h, "ipamir_val_lit" );
    pApi->ipamir_set_terminate = (void (*)(void *, void *, int (*)(void *)))Minr_DlSym( h, "ipamir_set_terminate" );

    if ( !pApi->ipamir_signature || !pApi->ipamir_init || !pApi->ipamir_release ||
         !pApi->ipamir_add_hard || !pApi->ipamir_add_soft_lit || !pApi->ipamir_assume ||
         !pApi->ipamir_solve || !pApi->ipamir_val_obj || !pApi->ipamir_val_lit )
    {
        printf("[Minr] Missing required IPAMIR symbols in %s\n", pSoPath);
        Minr_IpamirApiUnload( pApi );
        return 0;
    }
    return 1;
#endif
}

void Minr_IpamirApiUnload( Minr_IpamirApi_t * pApi )
{
#ifdef WIN32
    (void)pApi;
#else
    if ( pApi->handle )
        dlclose( pApi->handle );
    memset( pApi, 0, sizeof(*pApi) );
#endif
}

ABC_NAMESPACE_IMPL_END

