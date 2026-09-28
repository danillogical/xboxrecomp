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

/* A2h slot-WRITE watch (registry version 3), implemented in the GAME (src/diagnostics.c).
 *
 * The version-2 latch samples the slot across BRIDGE BOUNDARIES. It read the installed value at
 * every one of 15498 sampled boundaries and then read 0 at the terminal read, so the change
 * happened in a gap with no bracketing sample. These records are the writes themselves rather than
 * the samples: fixed, class-keyed, write-once witnesses plus a fixed alias census.
 *
 * OBSERVATION ONLY, exactly like the latch above. The handshake raises a DISTINCT first-chance
 * exception that the collector recognizes and acknowledges; it changes no guest register, no
 * memory, no allocation result, no stack cleanup and no device state, and it is raised only when
 * the diagnostic gate is on. */
/* Provenance of an observed write. These values are PART OF THE ARCHIVED FORMAT and must match
 * the game's src/diagnostics.h exactly: UNKNOWN is a first-class answer, because a native RIP
 * alone does not identify a guest instruction and the packet forbids blaming an unknown class on
 * either side. */
enum { JSRF_PROV_GUEST = 0, JSRF_PROV_HOST = 1, JSRF_PROV_UNKNOWN = 2 };

void jsrf_slot_watch_handshake(uint32_t slot_va);
void jsrf_slot_watch_alias_armed(uint32_t mapped_mask, uint32_t protect_mask, uint32_t alias_count);
/* 1 = the touch is recorded; the caller must NOT open the page on a 0 (record-before-open). */
int jsrf_slot_watch_alias_touch(uint32_t alias_index, uint32_t fault_va, uint64_t rip,
                                uint32_t value, uint32_t published);
void jsrf_slot_watch_write(uint32_t provenance, uint32_t before, uint32_t after,
                           uint64_t rip, uint32_t ordinal);
#else
#define recomp_diag_thread_start(start, low, high) ((void)0)
#define recomp_diag_thread_end() ((void)0)
#define recomp_diag_record(kind, target, site, value) ((void)0)
#define jsrf_slot_latch_install(raw_value, installed_value) ((void)0)
#define jsrf_slot_latch_sample(tid, call_index, ordinal, before, after) ((void)0)
#define jsrf_slot_watch_handshake(slot_va) ((void)0)
#define jsrf_slot_watch_alias_armed(mapped_mask, protect_mask, alias_count) ((void)0)
#define jsrf_slot_watch_alias_touch(alias_index, fault_va, rip, value, published) (0)
#define jsrf_slot_watch_write(provenance, before, after, rip, ordinal) ((void)0)
#endif
#endif
