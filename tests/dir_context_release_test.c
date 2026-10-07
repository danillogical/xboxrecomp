/* D2: a closed directory handle must not leave a live enumeration context.
 *
 * THE BUG THIS REPRODUCES
 *   xbox_NtQueryDirectoryFile keys its per-handle enumeration state on the
 *   directory's Nt handle (s_dir_contexts, MAX_DIR_CONTEXTS = 64). Slots are
 *   released only when a query runs out of files; closing the handle does not
 *   release them. A title that opens directories, queries, and closes them --
 *   without ever draining a query to STATUS_NO_MORE_FILES -- consumes all 64
 *   slots, and a later distinct directory can no longer be enumerated
 *   (STATUS_INSUFFICIENT_RESOURCES, 0xC000009A).
 *
 * TWO MODES, RUN AS SEPARATE PROCESSES
 *   argv[1] = "direct" -> calls the real HLE entry points xbox_NtOpenFile /
 *                         xbox_NtQueryDirectoryFile / xbox_NtClose directly
 *                         (host ABI: native pointers, native handles).
 *   argv[1] = "bridge" -> calls the test-seam wrappers xbox_test_bridge_* which
 *                         drive the REAL bridge handlers through the guest
 *                         stack: guest VAs, guest tokens, results read back
 *                         from guest memory.
 *   They are separate processes so a crash or a leaked slot in one cannot
 *   influence the other.
 *
 * ASSERTIONS
 *   1. 64 directories opened and each queried successfully, all kept open;
 *      close all 64; then a 65th distinct directory must open and query.
 *   2. churn: 200 open/query/close rounds must all succeed.
 *   3. guard: closing a synthetic token is a no-op (bridge mode only).
 *
 * RED FIRST: this fixture runs before any fix, and its failure statuses are the
 * RED evidence.
 *
 * ABI NOTE (Xbox, 32-bit guest): XBOX_OBJECT_ATTRIBUTES is
 * { HANDLE RootDirectory; PXBOX_ANSI_STRING ObjectName; ULONG Attributes; } and
 * XBOX_ANSI_STRING is { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
 * The bridge reads ObjectName at obj_attrs+4 and Buffer at ansi+4, so bridge
 * mode lays those out explicitly in guest memory. Length/MaximumLength are byte
 * counts, not character counts.
 *
 * PATHS ARE GUEST PATHS. xbox_translate_path matches "D:\\" as a rule and maps
 * it onto the configured game directory, so the fixture builds D:\-rooted guest
 * paths and points the path layer at a private temp tree via xbox_path_init.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "d3d/d3d8_xbox.h"   /* IDirect3DDevice8, for the inert D3D stub below */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef void (*recomp_func_t)(void);

/* Inert stubs: the real definitions live in the game, which this standalone
 * fixture does not link. Same approach as kernel_dpc_queue_test.c. */
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }
void recomp_diag_thread_start(uint32_t start, uint32_t low, uint32_t high)
{ (void)start; (void)low; (void)high; }
void recomp_diag_thread_end(void) {}
void recomp_diag_record(uint32_t kind, uint32_t target, uint32_t site, uint32_t value)
{ (void)kind; (void)target; (void)site; (void)value; }
void jsrf_slot_latch_install(uint32_t raw_value, uint32_t installed_value)
{ (void)raw_value; (void)installed_value; }
void jsrf_slot_latch_sample(uint32_t tid, uint32_t call_index, uint32_t ordinal,
                            uint32_t before, uint32_t after)
{ (void)tid; (void)call_index; (void)ordinal; (void)before; (void)after; }
void jsrf_slot_watch_handshake(uint32_t slot_va) { (void)slot_va; }
void jsrf_slot_watch_alias_armed(uint32_t mapped_mask, uint32_t protect_mask, uint32_t alias_count)
{ (void)mapped_mask; (void)protect_mask; (void)alias_count; }
int jsrf_slot_watch_alias_touch(uint32_t alias_index, uint32_t fault_va, uint64_t rip,
                                uint32_t value, uint32_t published)
{ (void)alias_index; (void)fault_va; (void)rip; (void)value; (void)published; return 0; }
void jsrf_slot_watch_write(uint32_t provenance, uint32_t before, uint32_t after,
                           uint64_t rip, uint32_t ordinal)
{ (void)provenance; (void)before; (void)after; (void)rip; (void)ordinal; }

/* Video/framebuffer and D3D symbols referenced by kernel_bridge.c and
 * nv2a_pb_exec.c but defined by the GAME. They are only reached on paths this
 * fixture never takes, so inert stubs are correct here. */
int  xbox_VideoIsPlaying(void) { return 0; }
int  xbox_VideoPlayFile(const char *host_path) { (void)host_path; return 0; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }
void xbox_FramebufferWindowPresent(void) {}
uint32_t xbox_FramebufferPresentSerial(void) { return 0; }
unsigned long long xbox_FramebufferPresentHash(void) { return 0; }
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }

extern ptrdiff_t g_xbox_mem_offset;

#define CANONICAL_LO 0x00010000u
#define RAM_SIZE     (64u * 1024u * 1024u)

/* Guest scratch, clear of the bridge's own test stack at 0x0003F000. */
#define G_HANDLE_VA    0x00100000u
#define G_IOS_VA       0x00100100u
#define G_ANSI_VA      0x00100200u
#define G_PATH_VA      0x00100300u
#define G_INFO_VA      0x00101000u
#define G_PATTERN_VA   0x00102000u
#define G_PATTERN_BUF  0x00102100u
#define OA_VA          0x00103000u

#define DIRS_OPENED    64u
#define CHURN_ROUNDS   200u

/* Guest argument values, per the approved RED design. */
#define ACCESS_VAL     0x00100001u   /* FILE_READ_DATA | SYNCHRONIZE */
#define SHARE_VAL      3u            /* FILE_SHARE_READ | FILE_SHARE_WRITE */
#define OPTIONS_VAL    0x00004021u   /* DIRECTORY_FILE|SYNCHRONIZE_IO_NONALERT|... */
#define INFO_CLASS_VAL 1u            /* XboxFileDirectoryInformation */
#define INFO_LEN       0x148u
#define RESTART_VAL    0u            /* mandatory: the original guest parameter */
#define PATTERN        "M.CMP"

static unsigned checks;
static uint8_t *ram;
static int use_bridge;               /* 0 = direct HLE, 1 = bridge seam */

static int check(int condition, const char *name)
{
    ++checks;
    if (!condition) fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

static uint8_t *native(uint32_t va) { return ram + (va - CANONICAL_LO); }
static void put32(uint32_t va, uint32_t v) { *(uint32_t *)native(va) = v; }
static uint32_t get32(uint32_t va) { return *(uint32_t *)native(va); }
static void put16(uint32_t va, uint16_t v) { *(uint16_t *)native(va) = v; }

/* 65 distinct directories, each holding M.CMP, plus their guest paths. */
static char base_dir[MAX_PATH];
static char host_paths[DIRS_OPENED + 1][MAX_PATH];
static char guest_paths[DIRS_OPENED + 1][MAX_PATH];

static int make_dirs(void)
{
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return 0;
    if (!GetTempFileNameA(tmp, "d2c", 0, base_dir)) return 0;
    DeleteFileA(base_dir);
    if (!CreateDirectoryA(base_dir, NULL)) return 0;
    for (unsigned i = 0; i <= DIRS_OPENED; ++i) {
        char file[MAX_PATH];
        snprintf(host_paths[i], MAX_PATH, "%s\\d%03u", base_dir, i);
        if (!CreateDirectoryA(host_paths[i], NULL)) return 0;
        snprintf(file, MAX_PATH, "%s\\M.CMP", host_paths[i]);
        FILE *fh = fopen(file, "wb");
        if (!fh) return 0;
        fputs("x", fh);
        fclose(fh);
        snprintf(guest_paths[i], MAX_PATH, "D:\\d%03u", i);
    }
    xbox_path_init(base_dir, base_dir);
    return 1;
}

static void remove_dirs(void)
{
    for (unsigned i = 0; i <= DIRS_OPENED; ++i) {
        char file[MAX_PATH];
        snprintf(file, MAX_PATH, "%s\\M.CMP", host_paths[i]);
        DeleteFileA(file);
        RemoveDirectoryA(host_paths[i]);
    }
    RemoveDirectoryA(base_dir);
}

/* ---------------------------------------------------------------- guest setup */

static void build_guest_oa(uint32_t oa_va, const char *path)
{
    size_t len = strlen(path);
    memcpy(native(G_PATH_VA), path, len);
    native(G_PATH_VA)[len] = 0;
    put16(G_ANSI_VA, (uint16_t)len);
    put16(G_ANSI_VA + 2, (uint16_t)(len + 1));
    put32(G_ANSI_VA + 4, G_PATH_VA);
    put32(oa_va, 0);
    put32(oa_va + 4, G_ANSI_VA);
    put32(oa_va + 8, 0);
}

static void build_guest_pattern(void)
{
    size_t len = strlen(PATTERN);
    memcpy(native(G_PATTERN_BUF), PATTERN, len);
    native(G_PATTERN_BUF)[len] = 0;
    put16(G_PATTERN_VA, (uint16_t)len);
    put16(G_PATTERN_VA + 2, (uint16_t)(len + 1));
    put32(G_PATTERN_VA + 4, G_PATTERN_BUF);
}

/* --------------------------------------------------------------- direct setup */

static XBOX_ANSI_STRING        d_name;
static XBOX_OBJECT_ATTRIBUTES  d_oa;
static XBOX_IO_STATUS_BLOCK    d_ios;
static uint8_t                 d_info[INFO_LEN];
static XBOX_ANSI_STRING        d_pattern;
static char                    d_path_buf[MAX_PATH];
static char                    d_pattern_buf[64];

static void build_direct_oa(const char *path)
{
    size_t len = strlen(path);
    memcpy(d_path_buf, path, len);
    d_path_buf[len] = 0;
    d_name.Length = (USHORT)len;
    d_name.MaximumLength = (USHORT)(len + 1);
    d_name.Buffer = d_path_buf;
    d_oa.RootDirectory = NULL;
    d_oa.ObjectName = &d_name;
    d_oa.Attributes = 0;
}

static void build_direct_pattern(void)
{
    size_t len = strlen(PATTERN);
    memcpy(d_pattern_buf, PATTERN, len);
    d_pattern_buf[len] = 0;
    d_pattern.Length = (USHORT)len;
    d_pattern.MaximumLength = (USHORT)(len + 1);
    d_pattern.Buffer = d_pattern_buf;
}

/* ------------------------------------------------------------ mode-agnostic API
 * `token` is an opaque handle: a native HANDLE in direct mode, a 32-bit guest
 * token in bridge mode. It is carried as uintptr_t so the direct path cannot
 * truncate a host pointer.
 *
 * ASSUMPTION (Windows, documented not hidden): on Win32 a HANDLE value fits in
 * 32 bits in practice, which is why it can also be passed to the bridge as a
 * guest token. That is a property of this host, not a portable guarantee; the
 * fixture is Windows-only (the CMake registration is inside `if(WIN32)`).
 */

static uintptr_t dir_open(const char *guest_path)
{
    if (use_bridge) {
        NTSTATUS st;
        put32(G_HANDLE_VA, 0);
        put32(G_IOS_VA, 0);
        put32(G_IOS_VA + 4, 0);
        build_guest_oa(OA_VA, guest_path);
        st = xbox_test_bridge_NtOpenFile(G_HANDLE_VA, ACCESS_VAL, OA_VA, G_IOS_VA,
                                         SHARE_VAL, OPTIONS_VAL);
        return (st == 0) ? (uintptr_t)get32(G_HANDLE_VA) : 0;
    } else {
        HANDLE h = NULL;
        NTSTATUS st;
        build_direct_oa(guest_path);
        memset(&d_ios, 0, sizeof(d_ios));
        st = xbox_NtOpenFile(&h, ACCESS_VAL, &d_oa, &d_ios, SHARE_VAL, OPTIONS_VAL);
        return (st == 0) ? (uintptr_t)h : 0;
    }
}

static NTSTATUS dir_query(uintptr_t token, uint32_t restart)
{
    if (use_bridge) {
        memset(native(G_INFO_VA), 0, INFO_LEN);
        put32(G_IOS_VA, 0);
        put32(G_IOS_VA + 4, 0);
        return xbox_test_bridge_NtQueryDirectoryFile(
            (uint32_t)token, 0, 0, 0, G_IOS_VA, G_INFO_VA, INFO_LEN, INFO_CLASS_VAL,
            G_PATTERN_VA, restart);
    } else {
        memset(d_info, 0, INFO_LEN);
        memset(&d_ios, 0, sizeof(d_ios));
        return xbox_NtQueryDirectoryFile(
            (HANDLE)token, NULL, NULL, NULL, &d_ios, d_info, INFO_LEN,
            (XBOX_FILE_INFORMATION_CLASS)INFO_CLASS_VAL, &d_pattern, (BOOLEAN)restart);
    }
}

static void dir_close(uintptr_t token)
{
    if (use_bridge)
        xbox_test_bridge_NtClose((uint32_t)token);
    else
        xbox_NtClose((HANDLE)token);
}

/* ------------------------------------------------------------------- the tests */

/* (1) 64 distinct directories opened and each queried successfully, all kept
 *     open; close all 64; then a 65th distinct directory must open AND query. */
static int run_open64_then_65th(void)
{
    uintptr_t tokens[DIRS_OPENED];
    int ok = 1;
    unsigned opened = 0, queried = 0;
    NTSTATUS q65 = (NTSTATUS)0x80000000;
    uintptr_t t65 = 0;

    memset(tokens, 0, sizeof(tokens));
    for (unsigned i = 0; i < DIRS_OPENED; ++i) {
        uintptr_t t = dir_open(guest_paths[i]);
        if (!t) { fprintf(stderr, "  open64: open #%u returned no handle\n", i); break; }
        tokens[i] = t;          /* recorded before the query so it is closed below */
        ++opened;
        if (dir_query(t, RESTART_VAL) != 0) {
            fprintf(stderr, "  open64: query #%u failed\n", i);
            break;
        }
        ++queried;
    }
    ok &= check(opened == DIRS_OPENED,
                "all 64 distinct directories open");
    /* The load-bearing assertion is the QUERY count, not the open count: a
     * handle that opens but cannot be enumerated is the failure under test. */
    ok &= check(queried == DIRS_OPENED,
                "all 64 distinct directories complete a successful query");
    fprintf(stderr, "  open64: opened=%u queried=%u\n", opened, queried);

    for (unsigned i = 0; i < DIRS_OPENED; ++i)
        if (tokens[i]) dir_close(tokens[i]);

    t65 = dir_open(guest_paths[DIRS_OPENED]);
    ok &= check(t65 != 0, "65th distinct directory opens after closing the first 64");
    if (t65) {
        q65 = dir_query(t65, RESTART_VAL);
        ok &= check(q65 == 0, "65th distinct directory queries successfully");
        if (q65 != 0)
            fprintf(stderr, "  65th query status = 0x%08X\n", (unsigned)q65);
        dir_close(t65);
    } else {
        ok &= check(0, "65th distinct directory queries successfully");
    }
    /* Printed on PASS as well as failure, so the counts are evidence rather
     * than something a reader has to infer from an exit code. */
    fprintf(stderr, "  open64: RESULT opened=%u queried=%u 65th_open=%s "
                    "65th_query_status=0x%08X\n",
            opened, queried, t65 ? "ok" : "FAILED", (unsigned)q65);
    return ok;
}

/* (2) 200 open/query/close rounds: additional lifetime coverage.
 *
 *     The 64-then-65 test above is the discriminator and fails deterministically
 *     under RestartScan = 0. This test adds churn -- repeated open/query/close
 *     within one process, so the handle table carries over between rounds --
 *     to show the leak also accumulates across many short-lived handles rather
 *     than only at the 64-boundary. */
static int run_churn(void)
{
    int ok = 1;
    int failed = 0;                 /* explicit flag: round 0 is a valid index,
                                     * so failed_at==0 must not read as success */
    unsigned failed_at = 0, queried = 0;
    NTSTATUS failed_status = 0;
    for (unsigned i = 0; i < CHURN_ROUNDS; ++i) {
        uintptr_t t = dir_open(guest_paths[i % (DIRS_OPENED + 1)]);
        NTSTATUS q;
        if (!t) {
            failed = 1; failed_at = i;
            failed_status = (NTSTATUS)0x80000000;
            break;
        }
        q = dir_query(t, RESTART_VAL);
        if (q != 0) {
            failed = 1; failed_at = i; failed_status = q;
            dir_close(t);           /* do not leak the handle we just opened */
            break;
        }
        ++queried;
        dir_close(t);
    }
    if (failed)
        fprintf(stderr, "  churn failed at round %u of %u after %u successful "
                        "queries; status = 0x%08X\n",
                failed_at, CHURN_ROUNDS, queried, (unsigned)failed_status);
    ok &= check(!failed, "200 open/query/close rounds all succeed");
    ok &= check(queried == CHURN_ROUNDS, "all 200 rounds completed a successful query");
    /* Printed on PASS too: the count is the evidence. */
    fprintf(stderr, "  churn: RESULT rounds=%u successful_queries=%u failed=%s\n",
            CHURN_ROUNDS, queried, failed ? "yes" : "no");
    return ok;
}

/* (3) the guard: a synthetic token must be a no-op. Bridge mode only --
 *     synthetic tokens are a bridge concept and the direct HLE never sees one. */
static int run_synthetic_guard(void)
{
    NTSTATUS st;
    if (!use_bridge) {
        fprintf(stderr, "  guard: skipped (direct mode has no synthetic tokens)\n");
        return 1;
    }
    st = xbox_test_bridge_NtClose(0xDEAD0001u);
    /* Printed on PASS too: the returned status is the evidence. */
    fprintf(stderr, "  guard: synthetic token close returned 0x%08X (expected 0)\n",
            (unsigned)st);
    return check(st == 0, "closing a synthetic token still returns success");
}

int main(int argc, char **argv)
{
    int ok = 1;
    const char *mode = (argc > 1) ? argv[1] : "direct";

    if (strcmp(mode, "bridge") == 0)      use_bridge = 1;
    else if (strcmp(mode, "direct") == 0) use_bridge = 0;
    else { fprintf(stderr, "usage: %s [direct|bridge]\n", argv[0]); return 2; }

    ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!ram) return 2;
    g_xbox_mem_offset = (ptrdiff_t)ram - (ptrdiff_t)CANONICAL_LO;

    if (!make_dirs()) { free(ram); return 2; }
    if (use_bridge) build_guest_pattern();
    else            build_direct_pattern();

    printf("mode=%s\n", mode);
    ok &= run_open64_then_65th();
    ok &= run_churn();
    ok &= run_synthetic_guard();

    remove_dirs();
    free(ram);
    /* The summary prints on both outcomes so a log always carries the counts. */
    printf("%s: %u D2 directory-context checks passed (mode=%s)\n",
           ok ? "PASS" : "FAIL", checks, mode);
    return ok ? 0 : 1;
}
