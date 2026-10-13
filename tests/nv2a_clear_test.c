/*
 * nv2a_clear_test.c -- nv2a_clear_to_argb, tested against the written spec in
 * src/kernel/nv2a_clear.h.
 *
 * Expected values are literals, and the round trip uses its own truncation,
 * so a wrong expansion cannot agree with itself.
 */
#include "kernel/nv2a_clear.h"

#include <stdio.h>

static int failures;

static void expect(uint32_t value, uint32_t fmt, uint32_t want)
{
    uint32_t got = nv2a_clear_to_argb(value, fmt);
    if (got != want) {
        fprintf(stderr, "FAIL: fmt 0x%X value 0x%08X: got 0x%08X, want 0x%08X\n", fmt, value, got, want);
        failures++;
    }
}

int main(void)
{
    uint32_t w, f;

    /* R5G6B5 */
    expect(0xFFFF, 3, 0xFFFFFFFFu);
    expect(0x0000, 3, 0xFF000000u);
    expect(0x20E4, 3, 0xFF211C21u);
    expect(0xF800, 3, 0xFFFF0000u);
    expect(0x07E0, 3, 0xFF00FF00u);
    expect(0x001F, 3, 0xFF0000FFu);

    /* X1R5G5B5, both format codes; bit 15 is ignored */
    for (f = 1; f <= 2; f++) {
        expect(0x7FFF, f, 0xFFFFFFFFu);
        expect(0x7C00, f, 0xFFFF0000u);
        expect(0x03E0, f, 0xFF00FF00u);
        expect(0x001F, f, 0xFF0000FFu);
        expect(0x8000, f, 0xFF000000u);
        expect(0xFFFF, f, 0xFFFFFFFFu);
    }

    /* upper 16 bits are ignored for 16-bit formats */
    expect(0xABCD20E4u, 3, 0xFF211C21u);
    expect(0xFFFF0000u, 3, 0xFF000000u);
    expect(0x12347C00u, 1, 0xFFFF0000u);

    /* 32-bit formats pass through unchanged */
    for (f = 4; f <= 8; f++) {
        expect(0xFF204060u, f, 0xFF204060u);
        expect(0x00000000u, f, 0x00000000u);
        expect(0x0000FFFFu, f, 0x0000FFFFu);
    }

    /* the low nibble selects the format; higher bits of the argument are not part of it */
    expect(0x20E4, 0x103, 0xFF211C21u);

    /* round trip: truncating the expansion back to 5:6:5 gives the word */
    for (w = 0; w <= 0xFFFF; w++) {
        uint32_t argb = nv2a_clear_to_argb(w, 3);
        uint32_t back = ((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x1F);
        if ((argb >> 24) != 0xFF || back != w) {
            if (failures < 20)
                fprintf(stderr, "FAIL: round trip 0x%04X -> 0x%08X -> 0x%04X\n", w, argb, back);
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("nv2a_clear: ok");
    return 0;
}
