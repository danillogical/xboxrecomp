#ifndef RECOMP_DIAGNOSTICS_H
#define RECOMP_DIAGNOSTICS_H
#include <stdint.h>
#ifdef RECOMP_DIAGNOSTICS
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high);
void recomp_diag_thread_end(void);
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value);
#else
#define recomp_diag_thread_start(start, low, high) ((void)0)
#define recomp_diag_thread_end() ((void)0)
#define recomp_diag_record(kind, target, site, value) ((void)0)
#endif
#endif
