/*
 * MCPX DSP emulator - internal declarations
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef DSP_INTERNAL_H
#define DSP_INTERNAL_H

#include "dsp.h"

uint32_t read_peripheral(DSPState *dsp, uint32_t address);
void write_peripheral(DSPState *dsp, uint32_t address, uint32_t value);
void dsp_start_frame_impl(DSPState *dsp);

extern const DSPOps c_dsp_ops;
void dsp_c_init(DSPState *dsp);

/* A4b1 LOCAL MODIFICATION (dsp_internal.h:25-26 upstream).
 *
 * Upstream this header also declares the JIT backend:
 *     extern const DSPOps jit_dsp_ops;
 *     void dsp_jit_init(DSPState *dsp);
 * The JIT is an A4b1 non-goal and dsp_jit.* is deliberately absent from the
 * vendored set (A4b1 step 1), so those two declarations are removed rather than
 * satisfied by a stub. A stub backend would be a shim that invents behaviour:
 * dsp_set_engine() would then "switch" to a backend that does nothing, and the
 * GP would silently stop executing.
 *
 * The removals from dsp.c that this implies are marked there.
 */

#endif /* DSP_INTERNAL_H */
