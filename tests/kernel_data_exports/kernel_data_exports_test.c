/* Every kernel DATA export resolves to backed data, not a function thunk.
 *
 * A title reads a data export through its thunk slot ("mov eax,[slot];
 * mov eax,[eax]"), so after xbox_kernel_bridge_init() the slot must hold an
 * address inside the kernel data page. A slot left with a synthetic function
 * VA (0xFE000000 + 4*slot) is dereferenced as data and faults. The 34 ordinals
 * are the ones nxdk's xboxkrnl.exe.def marks DATA; the sizes are the kernel's
 * layouts, or the storage this runtime reserves where it models less. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static const struct { uint32_t ordinal, size; const char *name; } k_data[] = {
    {  16,   4, "ExEventObjectType" },
    {  22,   4, "ExMutantObjectType" },
    {  30,   4, "ExSemaphoreObjectType" },
    {  31,   4, "ExTimerObjectType" },
    {  40,   4, "HalDiskCachePartitionCount" },
    {  41,   8, "HalDiskModelNumber" },
    {  42,   8, "HalDiskSerialNumber" },
    {  64,   4, "IoCompletionObjectType" },
    {  70,   4, "IoDeviceObjectType" },
    {  71,   4, "IoFileObjectType" },
    {  88,   1, "KdDebuggerEnabled" },
    {  89,   1, "KdDebuggerNotPresent" },
    { 102,  32, "MmGlobalData" },
    { 120,  12, "KeInterruptTime" },
    { 154,  12, "KeSystemTime" },
    { 156,   4, "KeTickCount" },
    { 157,   4, "KeTimeIncrement" },
    { 162,  20, "KiBugCheckData" },
    { 164,   4, "LaunchDataPage" },
    { 240,  28, "ObDirectoryObjectType" },
    { 245,  48, "ObpObjectHandleTable" },
    { 249,  28, "ObSymbolicLinkObjectType" },
    { 259,   4, "PsThreadObjectType" },
    { 321,  16, "XboxEEPROMKey" },
    { 322,   8, "XboxHardwareInfo" },
    { 323,  16, "XboxHDKey" },
    { 324,   8, "XboxKrnlVersion" },
    { 325,  16, "XboxSignatureKey" },
    { 326,   8, "XeImageFileName" },
    { 353,  16, "XboxLANKey" },
    { 354, 256, "XboxAlternateSignatureKeys" },
    { 355, 284, "XePublicKeyData" },
    { 356,   4, "HalBootSMCVideoMode" },
    { 357, 512, "IdexChannelObject" },
};
#define N_DATA ((int)(sizeof k_data / sizeof k_data[0]))
#define CONTROL_ORDINAL 173u   /* MmGetPhysicalAddress, a function */

static uint8_t *g_mem;
static uint32_t g_va[N_DATA + 1];
static int g_failures;

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); \
                       g_failures++; } while (0)

static uint32_t rd32(uint32_t va) { uint32_t v; memcpy(&v, g_mem + va, 4); return v; }

/* The thunk value installed for a data ordinal. */
static uint32_t at(uint32_t ordinal)
{
    int i;
    for (i = 0; i < N_DATA; i++)
        if (k_data[i].ordinal == ordinal) return g_va[i];
    abort();
}

static int all_zero(uint32_t va, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (g_mem[va + i]) return 0;
    return 1;
}

int main(void)
{
    const uint32_t THUNK_VA = 0x10000;
    const uint32_t lo = XBOX_KERNEL_DATA_BASE, hi = lo + XBOX_KERNEL_DATA_SIZE;
    int i, j;

    g_mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!g_mem) return 10;
    g_xbox_mem_offset = (ptrdiff_t)g_mem;

    for (i = 0; i < N_DATA; i++)
        ((uint32_t *)(g_mem + THUNK_VA))[i] = 0x80000000u | k_data[i].ordinal;
    ((uint32_t *)(g_mem + THUNK_VA))[N_DATA] = 0x80000000u | CONTROL_ORDINAL;
    xbox_kernel_set_thunk_address(THUNK_VA, N_DATA + 1);
    xbox_kernel_bridge_init();
    for (i = 0; i <= N_DATA; i++)
        g_va[i] = rd32(THUNK_VA + 4u * (uint32_t)i);

    /* The control proves the table was rewritten and functions still get a
     * synthetic dispatch VA, so the data checks cannot pass vacuously. */
    if (g_va[N_DATA] < 0xFE000000u)
        FAIL("control ordinal %u got 0x%08X, not a synthetic function VA",
             CONTROL_ORDINAL, g_va[N_DATA]);

    for (i = 0; i < N_DATA; i++)
        if (g_va[i] < lo || g_va[i] + k_data[i].size > hi)
            FAIL("ordinal %u (%s) -> 0x%08X is not backed by the kernel data page",
                 k_data[i].ordinal, k_data[i].name, g_va[i]);
    for (i = 0; i < N_DATA; i++)
        for (j = i + 1; j < N_DATA; j++)
            if (g_va[i] < g_va[j] + k_data[j].size && g_va[j] < g_va[i] + k_data[i].size)
                FAIL("ordinal %u (%s) overlaps ordinal %u (%s)", k_data[i].ordinal,
                     k_data[i].name, k_data[j].ordinal, k_data[j].name);
    if (g_failures) goto done;

    /* Contents of the exports that used to be function-routed. */
    if (g_mem[at(88)] != 0) FAIL("KdDebuggerEnabled = %u", g_mem[at(88)]);
    if (g_mem[at(89)] != 1) FAIL("KdDebuggerNotPresent = %u", g_mem[at(89)]);
    if (!all_zero(at(102), 32)) FAIL("MmGlobalData is not zero");
    if (rd32(at(120) + 4) != rd32(at(120) + 8)) FAIL("KeInterruptTime high parts disagree");
    if (rd32(at(154) + 4) != rd32(at(154) + 8)) FAIL("KeSystemTime high parts disagree");
    if (rd32(at(154) + 4) == 0) FAIL("KeSystemTime is not set");
    if (!all_zero(at(162), 20)) FAIL("KiBugCheckData is not zero");
    if (rd32(at(240)) != at(240) || rd32(at(249)) != at(249))
        FAIL("object types are not self-addressed: %08X %08X", rd32(at(240)), rd32(at(249)));
    if (!all_zero(at(245), 48)) FAIL("ObpObjectHandleTable is not zero");
    if (!all_zero(at(321), 16)) FAIL("XboxEEPROMKey is not zero");
    /* The IDE channel's empty queue survives the new initialisation. */
    if (rd32(at(357) + 0x28) != at(357) + 0x28 || rd32(at(357) + 0x2C) != at(357) + 0x28)
        FAIL("IDE channel queue links are %08X/%08X",
             rd32(at(357) + 0x28), rd32(at(357) + 0x2C));

done:
    VirtualFree(g_mem, 0, MEM_RELEASE);
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("PASS: all %d kernel DATA exports resolve to distinct backed data\n", N_DATA);
    return 0;
}
