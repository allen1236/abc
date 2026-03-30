#ifndef IPAMIR_H
#define IPAMIR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char * ipamir_signature ();
void * ipamir_init ();
void ipamir_release (void * s);
void ipamir_add_hard (void * s, int32_t lit_or_zero);
void ipamir_add_soft_lit (void * s, int32_t lit, uint64_t weight);
void ipamir_assume (void * s, int32_t lit);
int32_t ipamir_solve (void * s);
uint64_t ipamir_val_obj (void * s);
int32_t ipamir_val_lit (void * s, int32_t lit);
void ipamir_set_terminate (void * s, void * state, int (*terminate)(void * state));

#ifdef __cplusplus
}
#endif

#endif

