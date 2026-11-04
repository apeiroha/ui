#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* Tests for goroutine-local storage (ui_GoroLocalHooks / ui_GoroLocalSlot).
 *
 * The invariant that matters for embedders (e.g. a language runtime that
 * binds scratch memory / a default allocator to the goroutine rather than
 * the OS thread): the resume hook must fire with the right goro's local
 * BEFORE that goro's code runs after any park — including after a
 * work-stealing migration to another vCPU thread.  We simulate the
 * embedder cache with a per-vCPU-thread TLS pointer updated by the hooks
 * and verify it matches the goro's own slot value inside the goro. */

static atomic_long n_resume;
static atomic_long n_park;
static atomic_long n_release;

/* Simulates the embedder's thread-local cache (e.g. iroha_tls_state). */
static __thread void *tls_cache;

static void *
hook_create(void)
{
    return NULL; /* lazy creation goes through the slot path */
}

static void
hook_release(void *local)
{
    if (local)
        atomic_fetch_add(&n_release, 1);
}

static void
hook_park(void *local)
{
    (void)local; /* a real embedder saves its TLS cache into `local` here */
    if (getenv("GORO_LOCAL_DBG"))
        fprintf(stderr, "[P] local=%p\n", local);
    atomic_fetch_add(&n_park, 1);
}

static void
hook_resume(void *local)
{
    tls_cache = local;
    if (getenv("GORO_LOCAL_DBG"))
        fprintf(stderr, "[R] local=%p\n", local);
    atomic_fetch_add(&n_resume, 1);
}

static ui_GoroLocalHooks hooks = {
    .create = hook_create,
    .release = hook_release,
    .park = hook_park,
    .resume = hook_resume,
};

static int failures;

#define CHECK(cond, msg)                                                   \
    do                                                                     \
    {                                                                      \
        if (!(cond))                                                       \
        {                                                                  \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);        \
            failures++;                                                    \
        }                                                                  \
    } while (0)

/* ── 1. No local use: hooks still fire, local stays NULL ── */

static void
plain_worker(void)
{
    void **slot = ui_GoroLocalSlot();
    CHECK(slot != NULL, "slot visible inside goro");
    CHECK(*slot == NULL, "fresh goro local is NULL");
    CHECK(tls_cache == NULL, "resume hook fired with NULL local");
}

static void
test_plain_lifecycle(void)
{
    long r0 = atomic_load(&n_resume), p0 = atomic_load(&n_park);
    long rel0 = atomic_load(&n_release);

    ui_Init();
    ui_Go(plain_worker);
    ui_Run();
    ui_Fini();

    CHECK(atomic_load(&n_resume) == r0 + 1, "resume fired once");
    CHECK(atomic_load(&n_park) == p0 + 1, "park fired once (exit parks)");
    CHECK(atomic_load(&n_release) == rel0, "no release without local");
}

/* ── 2. Local survives yield; cache follows the goro ── */

static void
yield_worker(void)
{
    void **slot = ui_GoroLocalSlot();
    /* Per-goro value on the GORO STACK (survives yield) — NOT __thread:
     * goros sharing a vCPU thread must not share this variable. */
    long self_magic =
        atomic_fetch_add(&n_resume, 0) * 1000003L + (long)(intptr_t)slot;

    *slot = (void *)(intptr_t)self_magic;
    tls_cache = (void *)(intptr_t)self_magic;

    for (int i = 0; i < 5; i++)
    {
        ui_Yield();
        /* After every park/resume pair the hook must have re-bound the
         * cache to THIS goro before its code ran again. */
        if (tls_cache != (void *)(intptr_t)self_magic ||
            *ui_GoroLocalSlot() != (void *)(intptr_t)self_magic)
            fprintf(stderr,
                    "MISMATCH iter=%d magic=%lx tls=%lx slot*=%lx "
                    "slot@entry=%lx slot@check=%lx\n",
                    i, self_magic,
                    (uintptr_t)tls_cache, (uintptr_t)*ui_GoroLocalSlot(),
                    (uintptr_t)slot, (uintptr_t)ui_GoroLocalSlot());
        CHECK(tls_cache == (void *)(intptr_t)self_magic,
              "cache rebound to this goro after resume");
        CHECK(*ui_GoroLocalSlot() == (void *)(intptr_t)self_magic,
              "slot value survives yield");
    }
}

static void
test_local_survives_yield(void)
{
    long r0 = atomic_load(&n_resume), p0 = atomic_load(&n_park);
    long rel0 = atomic_load(&n_release);

    ui_Init();
    ui_Go(yield_worker);
    ui_Run();
    ui_Fini();

    CHECK(atomic_load(&n_resume) == r0 + 6, "1 spawn + 5 yields resume");
    CHECK(atomic_load(&n_park) == p0 + 6, "1 exit + 5 yields park");
    CHECK(atomic_load(&n_release) == rel0 + 1, "release fired with local");
}

/* ── 3. Churn across vCPUs: pairing + cache correctness under migration ──
 *
 * NVCPUS=4 with more runnable goros than 2×vCPUs forces work stealing;
 * many goros will resume on a different thread than they parked on. */

#define CHURN_N 300
#define CHURN_YIELDS 7

static atomic_long churn_ids;
static atomic_long churn_done;

static void
churn_worker(void)
{
    long magic = atomic_fetch_add(&churn_ids, 1) + 1;
    void **slot = ui_GoroLocalSlot();
    *slot = (void *)magic;
    tls_cache = (void *)magic;

    for (int i = 0; i < CHURN_YIELDS; i++)
    {
        ui_Yield();
        CHECK(tls_cache == (void *)magic, "cache follows goro under churn");
        CHECK(*ui_GoroLocalSlot() == (void *)magic, "slot follows goro");
    }
    atomic_fetch_add(&churn_done, 1);
}

static void
test_churn_migration(void)
{
    long r0 = atomic_load(&n_resume), p0 = atomic_load(&n_park);
    long rel0 = atomic_load(&n_release);
    atomic_store(&churn_ids, 0);
    atomic_store(&churn_done, 0);

    ui_Init();
    for (int i = 0; i < CHURN_N; i++)
        ui_Go(churn_worker);
    ui_Run();
    ui_Fini();

    long expect_sw = (long)CHURN_N * (CHURN_YIELDS + 1);
    CHECK(atomic_load(&n_resume) == r0 + expect_sw, "resume count == spawns*(yields+1)");
    CHECK(atomic_load(&n_park) == p0 + expect_sw, "park count == resume count");
    CHECK(atomic_load(&n_release) == rel0 + CHURN_N, "one release per goro");
}

/* ── 4. Recycled goro starts with a fresh (NULL) local ── */

static void
recycled_check_worker(void)
{
    void **slot = ui_GoroLocalSlot();
    CHECK(*slot == NULL, "recycled goro local is NULL");
}

static void
test_recycle_clears_local(void)
{
    ui_Init();
    /* Wave 1 sets local; wave 2 should hit the pool with local cleared. */
    for (int i = 0; i < 64; i++)
        ui_Go(yield_worker);
    ui_Run();
    for (int i = 0; i < 64; i++)
        ui_Go(recycled_check_worker);
    ui_Run();
    ui_Fini();
}

int
main(void)
{
    ui_SetGoroLocalHooks(&hooks);

    test_plain_lifecycle();
    test_local_survives_yield();
    test_recycle_clears_local();
    test_churn_migration();

    if (failures)
    {
        fprintf(stderr, "=== %d check(s) failed ===\n", failures);
        return 1;
    }
    printf("goro-local hooks: all checks passed\n");
    return 0;
}
