#ifndef RECOMP_DIAGNOSTICS_H
#define RECOMP_DIAGNOSTICS_H
#include <stdint.h>
#ifdef RECOMP_DIAGNOSTICS
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high);
void recomp_diag_thread_end(void);
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value);

/* A2h NULL-slot latch, implemented in the GAME (src/diagnostics.c).
 *
 * The toolkit observes; the game stores. This is the same toolkit-to-game callback pattern the
 * bridge already uses for recomp_diag_record above -- not a new mechanism.
 *
 * WHY THE GAME OWNS THE STORAGE: the latch must survive to the debugger's frozen capture, and
 * tools/harness/collect.c already resolves the game's `g_jsrf_debug` by symbol and archives
 * sizeof(JsrfRegistry). Putting the latch inside that struct means it is archived losslessly by
 * existing machinery, with no new extraction path. That is why JSRF_REGISTRY_VERSION must be
 * bumped in the game header whenever the latch layout changes.
 *
 * OBSERVATION ONLY: neither function reads or writes guest memory, registers, the allocation
 * result, stack cleanup or device state. */
void jsrf_slot_latch_install(uint32_t raw_value, uint32_t installed_value);
void jsrf_slot_latch_sample(uint32_t tid, uint32_t call_index, uint32_t ordinal,
                            uint32_t before, uint32_t after);
#else
#define recomp_diag_thread_start(start, low, high) ((void)0)
#define recomp_diag_thread_end() ((void)0)
#define recomp_diag_record(kind, target, site, value) ((void)0)
#define jsrf_slot_latch_install(raw_value, installed_value) ((void)0)
#define jsrf_slot_latch_sample(tid, call_index, ordinal, before, after) ((void)0)
#endif
#endif
