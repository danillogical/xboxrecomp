/* NV097_SET_COLOR_CLEAR_VALUE to 0xAARRGGBB, by the surface's colour format.
 *
 * The value arrives already in the surface's own format: D3D converts the
 * D3DCOLOR a title passes to Clear before it reaches the pushbuffer. A 16-bit
 * surface's value is therefore a 5:6:5 or 1:5:5:5 word in the low half, not an
 * A8R8G8B8 colour (JSRF's are 0x0000FFFF, 0x000020E4, 0x00000000).
 * Pure, so a test can include it without the executor.
 * Cf. andeecollard/jsrf-recomp e2e739c (MIT), which found the CPU-path half. */
#ifndef NV2A_CLEAR_H
#define NV2A_CLEAR_H

#include <stdint.h>

/* surface_color_format is NV097_SET_SURFACE_FORMAT_COLOR (the low nibble of
 * SET_SURFACE_FORMAT): 0x1/0x2 X1R5G5B5, 0x3 R5G6B5. Other formats are 32-bit
 * (or not colour words) and pass through unchanged. High bits are replicated
 * into the low ones so white stays white and a 5:6:5 truncation gives the
 * same word back. Alpha of an expanded value is 0xFF. */
static inline uint32_t nv2a_clear_to_argb(uint32_t value, uint32_t surface_color_format)
{
    uint32_t r, g, b;

    switch (surface_color_format & 0xF) {
    case 0x1: case 0x2:
        r = (value >> 10) & 0x1F;
        g = (value >> 5) & 0x1F;
        b = value & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
        break;
    case 0x3:
        r = (value >> 11) & 0x1F;
        g = (value >> 5) & 0x3F;
        b = value & 0x1F;
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        break;
    default:
        return value;
    }
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

#endif
