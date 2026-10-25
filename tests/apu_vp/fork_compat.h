/* Forced into test_main.c (CMakeLists.txt): upstream's test defines an
 * `ApuIrqStats apu_irq_stats` for upstream's APU core, which this fork's APU
 * does not have. The type is declared here so the line compiles; nothing
 * reads it. */
#ifndef APU_VP_FORK_COMPAT_H
#define APU_VP_FORK_COMPAT_H
typedef struct ApuIrqStats { int unused; } ApuIrqStats;
#endif
