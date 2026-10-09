/* Second translation unit for nv2a_mono_clock_test: exposes the clock as seen from file B. */
#include <stdint.h>

#include "nv2a/nv2a_mono_clock.h"

int64_t mono_clock_test_b_now_ns(void)
{
    return nv2a_mono_now_ns();
}
