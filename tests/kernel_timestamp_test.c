#include <windows.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_THREADS 16
#define TEST_ROUNDS 64
#define XBOX_TSC_HZ 733333333ll

uint64_t xbox_ReadTimeStampCounter(void);
void xbox_timestamp_test_reset(LONGLONG frequency, LONGLONG origin,
                               HANDLE frequency_ready, HANDLE allow_origin);
void xbox_timestamp_test_use_host_counter(void);

typedef struct TEST_WORKER {
    HANDLE start;
    volatile LONG *completed;
    uint64_t first;
    uint64_t second;
} TEST_WORKER;

static DWORD WINAPI timestamp_worker(LPVOID parameter)
{
    TEST_WORKER *worker = (TEST_WORKER *)parameter;
    WaitForSingleObject(worker->start, INFINITE);
    worker->first = xbox_ReadTimeStampCounter();
    worker->second = xbox_ReadTimeStampCounter();
    InterlockedIncrement(worker->completed);
    return 0;
}

static int run_forced_round(unsigned round)
{
    HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE frequency_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE allow_origin = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE threads[TEST_THREADS];
    TEST_WORKER workers[TEST_THREADS];
    volatile LONG completed = 0;
    unsigned seen[TEST_THREADS * 2 + 1] = {0};
    unsigned i;
    int ok = start && frequency_ready && allow_origin;

    if (!ok)
        goto cleanup;
    xbox_timestamp_test_reset(XBOX_TSC_HZ, 0x1234567800000000ll,
                              frequency_ready, allow_origin);
    for (i = 0; i < TEST_THREADS; ++i) {
        workers[i].start = start;
        workers[i].completed = &completed;
        workers[i].first = workers[i].second = 0;
        threads[i] = CreateThread(NULL, 0, timestamp_worker, &workers[i], 0, NULL);
        if (!threads[i]) {
            ok = 0;
            break;
        }
    }
    if (!ok)
        goto release_and_join;

    SetEvent(start);
    if (WaitForSingleObject(frequency_ready, 5000) != WAIT_OBJECT_0) {
        fprintf(stderr, "round %u: initializer did not reach forced pause\n", round);
        ok = 0;
        goto release_and_join;
    }
    Sleep(20);
    if (InterlockedCompareExchange(&completed, 0, 0) != 0) {
        fprintf(stderr, "round %u: caller escaped before origin publication\n", round);
        ok = 0;
    }

release_and_join:
    if (allow_origin)
        SetEvent(allow_origin);
    for (unsigned j = 0; j < i; ++j) {
        if (WaitForSingleObject(threads[j], 5000) != WAIT_OBJECT_0) {
            fprintf(stderr, "round %u: worker %u did not finish\n", round, j);
            ok = 0;
        }
        CloseHandle(threads[j]);
    }
    if (i != TEST_THREADS)
        ok = 0;

    if (ok) {
        for (i = 0; i < TEST_THREADS; ++i) {
            uint64_t values[2] = {workers[i].first, workers[i].second};
            if (workers[i].second <= workers[i].first) {
                fprintf(stderr, "round %u: worker %u went backward (%llu -> %llu)\n",
                        round, i, (unsigned long long)workers[i].first,
                        (unsigned long long)workers[i].second);
                ok = 0;
            }
            for (unsigned sample = 0; sample < 2; ++sample) {
                if (values[sample] < 1 || values[sample] > TEST_THREADS * 2) {
                    fprintf(stderr, "round %u: sample escaped shared origin: %llu\n",
                            round, (unsigned long long)values[sample]);
                    ok = 0;
                } else {
                    ++seen[values[sample]];
                }
            }
        }
        for (i = 1; i <= TEST_THREADS * 2; ++i) {
            if (seen[i] != 1) {
                fprintf(stderr, "round %u: scaled sample %u seen %u times\n",
                        round, i, seen[i]);
                ok = 0;
            }
        }
    }

cleanup:
    if (start) CloseHandle(start);
    if (frequency_ready) CloseHandle(frequency_ready);
    if (allow_origin) CloseHandle(allow_origin);
    return ok;
}

int main(void)
{
    uint64_t previous;
    unsigned i;

    for (i = 0; i < TEST_ROUNDS; ++i) {
        if (!run_forced_round(i))
            return 1;
    }

    xbox_timestamp_test_use_host_counter();
    previous = xbox_ReadTimeStampCounter();
    for (i = 0; i < 10000; ++i) {
        uint64_t current = xbox_ReadTimeStampCounter();
        if (current < previous) {
            fprintf(stderr, "host adapter went backward: %llu -> %llu\n",
                    (unsigned long long)previous,
                    (unsigned long long)current);
            return 1;
        }
        previous = current;
    }

    printf("timestamp publication: %u forced concurrent rounds and host adapter pass\n",
           TEST_ROUNDS);
    return 0;
}
