/* A2h-arming-coverage-attribution C1/C2 fixture.
 *
 * It drives the PRODUCTION collector code and never a test-local copy of it. The link seam is the
 * gated CREATE_THREAD / EXIT_THREAD branch bodies, which collect.c exposes as
 *
 *     void jsrf_a2h_test_begin(const char *report_path);   report stream + process id
 *     void jsrf_a2h_test_geometry(DWORD64 canonical);      stand in for the mapping symbols
 *     void jsrf_a2h_test_create_thread(HANDLE, DWORD);     dr_on_create_thread, verbatim
 *     void jsrf_a2h_test_exit_thread(DWORD);               dr_on_exit_thread, verbatim
 *     int  jsrf_a2h_test_handshake(DWORD);                 dr_handshake, verbatim
 *     void jsrf_a2h_test_terminal(void);                   dr_print_terminal_summary, verbatim
 *
 * so every assertion below is an assertion about the shipped instrument, not about a reimplementation
 * of it. The arm itself is a real SetThreadContext on a real thread of this process, and the real
 * CREATE_THREAD branch is reached through a real CreateThread; only the deferred and collision states
 * are produced by calling the branch body with a chosen tid, which makes them deterministic without a
 * sleep or a race.
 *
 * Two arms are registered in ctest: the gated arm (JSRF_TRACE_A2H_DR=1) and the inertness arm (the
 * variable unset). The gate is read once and cached, so one process can only ever exercise one arm --
 * the same reason tests/apu_watch_fixture_test.c is registered twice.
 *
 * WHY EACH CASE EXISTS (packet C1/C2):
 *   deferred_birth    early birth with no mapping offset published yet  -> state=deferred, recovered
 *   recovery          the handshake sweep arms a tid that was deferred  -> recovered, not unarmed
 *   live_sweep        a pre-existing live thread is armed by the sweep  -> why=handshake
 *   silent_success    a thread born AFTER the handshake arms successfully and used to print NOTHING
 *   pre_mapping_exit  born with no mapping, exits before the handshake  -> window=PRE_MAPPING_EXIT_*
 *   collision         another debug owner holds DR state               -> refused, never clobbered
 *   readback          the DR7/DR0 readback decides the arm             -> readback= is the real value
 *   inertness         gate OFF: no line, no counter, no arming at all
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The seams collect.c provides for this fixture. Declared here rather than in a shared header so the
 * production build never sees them. */
void jsrf_a2h_test_begin(const char *report_path);
void jsrf_a2h_test_geometry(DWORD64 canonical);
void jsrf_a2h_test_create_thread(HANDLE thread, DWORD tid);
void jsrf_a2h_test_exit_thread(DWORD tid);
int jsrf_a2h_test_handshake(DWORD tid);
void jsrf_a2h_test_terminal(void);
DWORD jsrf_a2h_test_armed_count(void);
DWORD jsrf_a2h_test_failed_count(void);

/* collect.c includes src/jsrf_save_root.h, whose implementation is the toolkit's kernel_path.c /
 * kernel_io.c in the real collector. This fixture never calls into that layer, and linking it would
 * pull the whole kernel path module in for nothing, so it supplies the one symbol that layer needs
 * from the game's diagnostics. Inert by design: the fixture asserts on records, not on logs. */
void xbox_log(const char *format, ...) { (void)format; }

#define CANONICAL 0x0000000012345064ull
#define DR7_EXPECTED "00000000000D0001"
#define ARTIFACT "collect_arming_artifact.txt"
#define TID_DEFERRED 70001u
#define TID_PRE_EXIT 70002u
#define TID_COLLISION 70003u

static int failures;
/* 512 bytes per line, not 256: the terminal reconciliation lines are longer than 256 and a short
 * buffer silently TRUNCATES them -- which made the first version of this fixture fail on records that
 * were in fact present and correct. A truncated record is indistinguishable from a wrong one, which
 * is the same absent-record hazard the packet is about, so the buffer is sized with headroom. */
static char artifact[4096][512];
static unsigned artifact_lines;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* ── artifact reading ────────────────────────────────────────────────────────────────────────────
 * The assertions read the REAL artifact back, exactly as the apu_watch_fixture_test precedent does,
 * so a case cannot pass vacuously on an in-memory counter while the file says something else. */
static void artifact_load(void)
{
    FILE *file = fopen(ARTIFACT, "r");
    char line[512];
    artifact_lines = 0;
    if (!file) return;
    while (fgets(line, sizeof(line), file) && artifact_lines < 4096) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
        snprintf(artifact[artifact_lines], sizeof(artifact[0]), "%s", line);
        artifact_lines++;
    }
    fclose(file);
}

/* A whole-line-prefix match, which is how the greppable records are defined. */
static int artifact_has(const char *prefix)
{
    size_t length = strlen(prefix);
    for (unsigned i = 0; i < artifact_lines; i++)
        if (!strncmp(artifact[i], prefix, length)) return 1;
    return 0;
}

static int artifact_count(const char *prefix)
{
    size_t length = strlen(prefix);
    int count = 0;
    for (unsigned i = 0; i < artifact_lines; i++)
        if (!strncmp(artifact[i], prefix, length)) count++;
    return count;
}

/* The last line starting with `prefix`, or "" when absent: assertions about terminal reconciliation
 * must read the terminal record, not an intermediate summary. */
static const char *artifact_last(const char *prefix)
{
    size_t length = strlen(prefix);
    const char *found = "";
    for (unsigned i = 0; i < artifact_lines; i++)
        if (!strncmp(artifact[i], prefix, length)) found = artifact[i];
    return found;
}

static int text_has(const char *text, const char *needle)
{
    return strstr(text, needle) != NULL;
}

static DWORD WINAPI idle_thread(LPVOID parameter)
{
    (void)parameter;
    Sleep(400);   /* outlives the handshake and the sweep; the fixture never waits on its work */
    return 0;
}

/* The pre-existing live thread. It must still be ALIVE at the handshake for the sweep to see it, so
 * it outlives the whole fixture; the first version shared idle_thread's 400 ms and had already exited
 * by the time the sweep ran, which is why the sweep did not arm it. The fixture's own failure, not
 * the instrument's -- but exactly the kind of "no record therefore no thread" mistake the packet is
 * about, so the wait is explicit here. */
static DWORD WINAPI long_lived_thread(LPVOID parameter)
{
    (void)parameter;
    Sleep(20000);
    return 0;
}

/* The gate-OFF arm. Every seam is exercised and NOTHING may happen: no arming, no counter, no line.
 * This is the inertness the packet demands, and it is checked against the artifact rather than
 * against the absence of a log line, because an absent line is exactly the inference this packet
 * exists to stop making. */
static int inertness_arm(void)
{
    DWORD before_armed, before_failed;
    jsrf_a2h_test_geometry(CANONICAL);
    before_armed = jsrf_a2h_test_armed_count();
    before_failed = jsrf_a2h_test_failed_count();
    jsrf_a2h_test_create_thread(NULL, TID_DEFERRED);
    jsrf_a2h_test_exit_thread(TID_PRE_EXIT);
    jsrf_a2h_test_handshake(GetCurrentThreadId());
    jsrf_a2h_test_terminal();
    check(jsrf_a2h_test_armed_count() == before_armed &&
          jsrf_a2h_test_failed_count() == before_failed,
          "gate OFF: no arming state changed at all");
    artifact_load();
    check(artifact_lines == 0, "gate OFF: the artifact is empty -- no record, no counter, no line");
    if (failures) {
        printf("collector arming fixture (gate OFF): %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("collector arming fixture (gate OFF): inert, artifact empty\n");
    return 0;
}

int main(void)
{
    HANDLE live = NULL, collision = NULL;
    DWORD live_tid = 0, collision_tid = 0, real_tid = 0;

    remove(ARTIFACT);
    jsrf_a2h_test_begin(ARTIFACT);

    if (getenv("JSRF_TRACE_A2H_DR") == NULL) return inertness_arm();

    /* ── 1. the pre-existing thread, created while no mapping offset is published ────────────────
     * It is a REAL thread of this process, so the handshake sweep really sees it. That is what makes
     * the recovery case a real recovery rather than an assertion about a fabricated tid the sweep
     * could never have armed. */
    live = CreateThread(NULL, 0, long_lived_thread, NULL, 0, &live_tid);
    if (!live) { check(0, "CreateThread for the pre-existing live thread"); return 1; }

    /* ── 2. deferred birth: its real CREATE_THREAD event arrives with no mapping available ─────── */
    jsrf_a2h_test_create_thread(live, live_tid);
    artifact_load();
    check(artifact_has("GUEST_DR_BIRTH ") &&
          text_has(artifact_last("GUEST_DR_BIRTH "), "mapping_available=0"),
          "deferred: a birth with no mapping available is recorded even though no attempt was possible");

    /* ── 3. pre-mapping EXIT: gone before the handshake, so the sweep can never see it ─────────── */
    jsrf_a2h_test_create_thread(NULL, TID_PRE_EXIT);
    jsrf_a2h_test_exit_thread(TID_PRE_EXIT);
    artifact_load();
    check(artifact_count("GUEST_DR_BIRTH ") == 2,
          "deferred: both pre-mapping births are recorded, attempt or not");

    /* ── 4. collision: another debug owner already holds DR state on the thread ────────────────── */
    collision = CreateThread(NULL, 0, idle_thread, NULL, CREATE_SUSPENDED, &collision_tid);
    if (collision) {
        CONTEXT planted = {0};
        planted.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(collision, &planted);
        planted.Dr1 = 0xDEADBEEF;      /* somebody else's watch */
        planted.Dr7 = 0x00000004;      /* L1, inside A2H_DR7_OWNED */
        SetThreadContext(collision, &planted);
    }
    /* Mapping is published only NOW, so the collision is decided by DR ownership and not by the
     * deferred path. */
    jsrf_a2h_test_geometry(CANONICAL);
    jsrf_a2h_test_create_thread(collision, TID_COLLISION);
    if (collision) {
        CONTEXT after = {0};
        after.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(collision, &after);
        check(after.Dr1 == 0xDEADBEEF && after.Dr0 == 0,
              "collision: the other owner's DR1 is intact and DR0 was never taken");
        /* Joined before the handshake, so the sweep does not re-attempt it: the fixture asserts on a
         * deterministic population, and a tid live at the handshake would legitimately be attempted
         * twice. */
        ResumeThread(collision);
        WaitForSingleObject(collision, 5000);
        CloseHandle(collision);
    }
    artifact_load();
    check(text_has(artifact_last("GUEST_DR_ARM_FAIL "), "stage=collision"),
          "collision: the refusal is recorded with its stage");

    /* ── 5. live sweep: the pre-existing thread and the handshake's own thread ─────────────────── */
    check(jsrf_a2h_test_handshake(GetCurrentThreadId()) != 0,
          "handshake: the sweep ran against a published mapping");
    {
        CONTEXT live_context = {0};
        live_context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        check(GetThreadContext(live, &live_context) && live_context.Dr0 == CANONICAL &&
              (live_context.Dr7 & 0x1),
              "live sweep: the pre-existing live thread carries DR0 and L0 after the handshake");
    }
    artifact_load();
    check(artifact_count("GUEST_DR_ARM_OK seq=") >= 1,
          "live sweep: each successful sweep arm is a positive per-arm record");
    check(text_has(artifact_last("GUEST_DR_ARM_OK seq="), "why=handshake"),
          "live sweep: the record names handshake as the arm source");
    check(text_has(artifact_last("GUEST_DR_ARM_OK seq="), "dr7_readback=" DR7_EXPECTED),
          "readback: the record carries the DR7 actually read back, not the value written");
    check(text_has(artifact_last("GUEST_DR_ARM_OK seq="), "phase=at_handshake"),
          "live sweep: the sweep's arms are named at_handshake, distinct from post_handshake");
    {
        /* RECOVERY, stated as the packet states it: the tid whose birth attempt was deferred is
         * armed by the sweep, so it is RECOVERED and not a terminal unarmed tid. Proven at event time
         * by two records for the SAME tid -- a birth with mapping_available=0 and a handshake arm --
         * because that pair is what makes the recovery a fact rather than an inference from counts.
         * The BIRTH_ROW dump that repeats them is terminal-only and is asserted in section 8. */
        char needle_birth[96], needle_arm[96];
        int saw_birth = 0, saw_arm = 0;
        snprintf(needle_birth, sizeof(needle_birth), " tid=%lu event=create_thread mapping_available=0",
                 live_tid);
        snprintf(needle_arm, sizeof(needle_arm), " tid=%lu why=handshake", live_tid);
        for (unsigned i = 0; i < artifact_lines; i++) {
            if (!strncmp(artifact[i], "GUEST_DR_BIRTH ", 15) && text_has(artifact[i], needle_birth))
                saw_birth = 1;
            if (!strncmp(artifact[i], "GUEST_DR_ARM_OK ", 16) && text_has(artifact[i], needle_arm))
                saw_arm = 1;
        }
        check(saw_birth && saw_arm,
              "recovery: the deferred tid has BOTH a deferred birth record and a handshake arm record");
    }

    /* ── 6. C1(2) THE CASE THAT WAS INVISIBLE: a successful arm AFTER the handshake ────────────── */
    {
        HANDLE thread = CreateThread(NULL, 0, idle_thread, NULL, CREATE_SUSPENDED, &real_tid);
        if (!thread) {
            check(0, "post-handshake success: CreateThread");
        } else {
            jsrf_a2h_test_create_thread(thread, real_tid);
            ResumeThread(thread);
            CloseHandle(thread);
        }
    }
    artifact_load();
    check(artifact_count("GUEST_DR_ARM_OK seq=") >= 2,
          "post-handshake success: a second per-arm record exists for the post-handshake arm");
    check(text_has(artifact_last("GUEST_DR_ARM_OK seq="), "why=create_thread"),
          "post-handshake success: the record names create_thread as the arm source");
    check(text_has(artifact_last("GUEST_DR_ARM_OK seq="), "phase=post_handshake"),
          "post-handshake success: the record is named post_handshake, the population that was silent");
    check(text_has(artifact_last("GUEST_DR_ARM_OK seq="), "dr7_readback=" DR7_EXPECTED),
          "post-handshake success: the post-handshake arm carries its own DR7 readback");
    {
        HANDLE probe = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, real_tid);
        if (probe) {
            CONTEXT after = {0};
            after.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            check(GetThreadContext(probe, &after) && after.Dr0 == CANONICAL,
                  "post-handshake success: the real thread really is armed");
            CloseHandle(probe);
        }
    }

    /* ── 7. terminal reconciliation ────────────────────────────────────────────────────────────── */
    jsrf_a2h_test_terminal();
    artifact_load();

    check(jsrf_a2h_test_armed_count() >= 3, "terminal: the run performed the expected arms");
    {
        const char *terminal = artifact_last("GUEST_DR_ARM_TERMINAL ");
        const char *reconcile = artifact_last("GUEST_DR_ARM_RECONCILE ");
        const char *bound = artifact_last("GUEST_DR_ARM_PRE_MAPPING_BOUND ");
        check(terminal[0] != 0, "terminal: a TERMINAL line was published");
        check(text_has(terminal, "arms_recorded=") && text_has(terminal, "recovered_by_sweep=") &&
              text_has(terminal, "failed_never_recovered=") && text_has(terminal, "birth_rows=") &&
              text_has(terminal, "armed_before_handshake=") &&
              text_has(terminal, "armed_at_or_after_handshake="),
              "terminal: arm, recovery and unarmed populations are reported separately");
        check(text_has(terminal, "create_thread_events=4"),
              "terminal: all four create_thread events are counted, whether or not an arm followed");
        check(text_has(terminal, "deferred_births=2"),
              "deferred: both pre-mapping births are counted as deferred, not as failed");
        check(text_has(terminal, "failed_births=1"),
              "collision: the refused attempt is counted as a failed attempt");
        check(text_has(terminal, "recovered_by_sweep=1"),
              "recovery: the deferred tid armed by the sweep is reported as recovered");
        check(text_has(terminal, "failed_never_recovered=1"),
              "terminal: the tid that was REFUSED because another owner holds its DRs is the one "
              "terminal unarmed tid, and the recovered tid is not counted with it");
        check(text_has(terminal, "exited_unarmed=1"),
              "terminal: the tid that exited before it could be armed is reported in the EXIT "
              "population, never as a terminal unarmed tid");
        check(!text_has(terminal, "birth_overflow=1"),
              "terminal: the ledger did not overflow, so it is complete for this fixture");
        check(text_has(terminal, "handshake_seen=1"),
              "terminal: the handshake was seen, so the before/after split is meaningful");
        check(text_has(terminal, "armed_at_or_after_handshake=") &&
              !text_has(terminal, "armed_at_or_after_handshake=0"),
              "terminal: the at/after-handshake arm population is non-zero and reported");
        check(reconcile[0] != 0 && text_has(reconcile, "disarm_cleared_is_not_an_arm_count=1"),
              "terminal: the reconciliation refuses to read cleared= as an arm count");
        check(text_has(reconcile, "incomplete=0"),
              "terminal: no record family overflowed, so the reconciliation is complete");
        check(text_has(reconcile, "distinct_armed_tids="),
              "terminal: a distinct armed-tid count is published alongside the arm-record count");
        check(bound[0] != 0 && text_has(bound, "window=PRE_MAPPING_EXIT_UNCOVERED"),
              "pre-mapping exit: a pre-mapping birth that exited before the handshake yields "
              "PRE_MAPPING_EXIT_UNCOVERED, never absence");
        check(text_has(bound, "decision=decidable_from_records"),
              "pre-mapping exit: the bound is a derived decision, not two counts to combine");
    }

    /* ── 8. lossless per-birth ledger ──────────────────────────────────────────────────────────── */
    check(artifact_has("GUEST_DR_BIRTH ") && artifact_count("GUEST_DR_BIRTH ") == 4,
          "births: every create_thread event has its own GUEST_DR_BIRTH record");
    {
        /* One row per lifecycle event, and the COMPOSITION is asserted rather than a bare total: a
         * fixed expected count would be a fixture that has to be edited every time the sweep's live
         * set changes, and an edited constant proves nothing. */
        int births = 0, sweeps = 0, exits = 0;
        for (unsigned i = 0; i < artifact_lines; i++) {
            if (strncmp(artifact[i], "GUEST_DR_BIRTH_ROW ", 19)) continue;
            if (text_has(artifact[i], "why=exit")) exits++;
            else if (text_has(artifact[i], "why=handshake")) sweeps++;
            else births++;
        }
        check(births == 4 && exits == 1 && sweeps >= 2,
              "births: the terminal row dump reconciles (4 create_thread births + one row per live "
              "swept thread + 1 exit)");
    }
    check(artifact_has("GUEST_DR_BIRTH_ROW index=2 seq=5 tid=70002 why=exit state=exited reason=none "
                       "mapping_available=0"),
          "births: the pre-handshake exit is recorded as an exited row with tid, seq and mapping state");
    {
        /* The deferred row is asserted by CONTENT, not by index: the index of a given tid's row
         * depends on how many threads the sweep happened to arm, and a fixture that hard-codes that
         * index would break for a reason that has nothing to do with the record it is checking. */
        int found = 0;
        char needle[128];
        snprintf(needle, sizeof(needle),
                 " tid=%lu why=create_thread state=deferred reason=no_mapping_offset "
                 "mapping_available=0", live_tid);
        for (unsigned i = 0; i < artifact_lines; i++)
            if (!strncmp(artifact[i], "GUEST_DR_BIRTH_ROW ", 19) && text_has(artifact[i], needle))
                found = 1;
        check(found,
              "births: a deferred row carries tid, seq, state, reason and mapping availability");
    }
    {
        /* The same recovery, now visible in the TERMINAL dump: one tid, a deferred birth row and a
         * handshake armed row. This is the reconciliation a reader performs offline. */
        char needle_deferred[96], needle_armed[96];
        int saw_deferred = 0, saw_armed = 0;
        snprintf(needle_deferred, sizeof(needle_deferred),
                 " tid=%lu why=create_thread state=deferred", live_tid);
        snprintf(needle_armed, sizeof(needle_armed), " tid=%lu why=handshake state=armed", live_tid);
        for (unsigned i = 0; i < artifact_lines; i++) {
            if (strncmp(artifact[i], "GUEST_DR_BIRTH_ROW ", 19)) continue;
            if (text_has(artifact[i], needle_deferred)) saw_deferred = 1;
            if (text_has(artifact[i], needle_armed)) saw_armed = 1;
        }
        check(saw_deferred && saw_armed,
              "births: the terminal dump reconciles the recovered tid (deferred birth + handshake arm)");
    }
    check(artifact_has("GUEST_DR_ARM_TID_TERMINAL ") &&
          text_has(artifact_last("GUEST_DR_ARM_TID_TERMINAL "), "source="),
          "terminal: every armed tid is listed with its arm source");
    check(artifact_has("GUEST_DR_ARM_TID index=0 tid="),
          "compatibility: the pre-existing GUEST_DR_ARM_TID line shape is unchanged");

    if (failures) {
        printf("collector arming fixture (gate ON): %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("collector arming fixture (gate ON): every C1/C2 record present and reconciled\n");
    return 0;
}
