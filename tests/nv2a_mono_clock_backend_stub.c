/* Link stub for nv2a_mono_clock_test: nv2a_core.c needs a PGRAPH backend symbol, and this test never submits a method. */
#include <stdint.h>

#include "nv2a/nv2a_pgraph_d3d11.h"

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    (void)subchannel; (void)method; (void)param;
    return 1;
}
