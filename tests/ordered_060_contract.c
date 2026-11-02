/* Upstream 0.6.0 ordered-handoff shapes, exercised against the production bridge.
 * Scenario 0: k_expo_resolve (FreshExposure) opens the work stream before k_import.
 * Scenario 1: an unregistered kernel still cannot open the work stream.
 * Scenario 2: after a fully consumed frame, the producer restarts at the same frame
 *             number on the same token (0.6.0 pooled flag buffer): accepted.
 * Scenario 3: the same frame number while that frame is still in flight: rejected.
 * Scenario 4: a second runtime copy registers its own kernels (AMDNR Neural passes 2):
 *             the first copy's kernels stay recognised and both copies' frames complete.
 * Scenario 5: another queue claims idle resources (AMDNR pass 2 / NR resize): handed over.
 * Scenario 6: another queue claims while a job is in flight: it waits, then takes over
 *             after the job completes.
 * Scenario 7: the in-flight job never completes: the claim is refused after the bound.
 */
#include "../native/hip_bridge.c"
#include <pthread.h>
#include <time.h>
#include <assert.h>

static int refs, launches;
static void *marker_stream, *work_stream = (void *)0x2000;
static uint64_t token;
static char trace[64];
static unsigned trace_n;
static void record(char ch) { assert(trace_n + 1 < sizeof(trace)); trace[trace_n++] = ch; }
static int retain(uint64_t owner) { assert(owner == 77); ++refs; return 0; }
static void release(uint64_t owner) { assert(owner == 77); --refs; }
static int native_sync(void *stream) { record(stream == work_stream ? 'w' : 'm'); return 0; }
static int device_sync(void) { record('d'); return 0; }
static int native_launch(const void *f, dim3_t g, dim3_t b, void **a, u64 sh, void *stream) {
    (void)g; (void)b; (void)a; (void)sh;
    ++launches;
    if (nr_is_role(NR_ROLE_FLAG_SET, f)) { assert(stream == marker_stream); record('F'); }
    else { assert(stream == work_stream);
           record(nr_is_role(NR_ROLE_EXPO, f) ? 'E' : nr_is_role(NR_ROLE_IMPORT, f) ? 'I'
                  : nr_is_role(NR_ROLE_EXPORT, f) ? 'X' : 'K'); }
    return 0;
}
static int send(const void *fn, void **args, void *stream) {
    return stub_launch(fn, (dim3_t){1, 1, 1}, (dim3_t){32, 1, 1}, args, 0, stream);
}

static uint32_t flags[16];
static void *flag_ptr = flags, *abort_ptr = (void *)0x3000;
static uint32_t limit = 20000000;

struct kernels { void *wait, *set, *imp, *exp, *expo; };
static struct kernels K;

/* One complete 0.6.0-shaped job for `frame`: wait, expo, import, network, export, set. */
static void run_frame(struct nr_buffer *snapshot, uint32_t count, uint32_t frame) {
    void *wait_args[] = {&flag_ptr, &frame, &limit};
    void *set_args[] = {&flag_ptr, &frame, &abort_ptr};
    void *ordinary[] = {&flag_ptr};
    assert(!nr_publish_input(token, frame, snapshot, count));
    assert(!send(K.wait, wait_args, marker_stream) && nr_job.active);
    assert(!send(K.expo, ordinary, work_stream));
    assert(!send(K.imp, ordinary, work_stream));
    assert(!send((void *)0x9999, ordinary, work_stream));
    assert(!send(K.exp, ordinary, work_stream));
    assert(!send(K.set, set_args, marker_stream) && !nr_job.active);
    assert(!nr_wait_output(token, frame, 0));
    assert(!nr_consumer_submitted(token, frame));
}

static struct nr_buffer claim_snapshot[2];
static uint32_t claim_count;
static volatile int claim_done = -1;
static void *claim_thread(void *arg) {
    claim_done = nr_claim_queue(claim_snapshot, claim_count, (uint64_t)(uintptr_t)arg);
    return NULL;
}
static void msleep(long ms) { struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL); }

int main(int argc, char **argv) {
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    g_hip_ok = 1;
    real.p_hipLaunchKernel = native_launch;
    real.p_hipStreamSynchronize = native_sync;
    real.p_hipDeviceSynchronize = device_sync;
    g_flag_wait = (void *)1; g_flag_set = (void *)2; g_k_import = (void *)3;
    g_k_export = (void *)4; g_k_expo = (void *)5;
    K = (struct kernels){g_flag_wait, g_flag_set, g_k_import, g_k_export, g_k_expo};
    struct nr_buffer snapshot[2];
    uint32_t count, frame = 1;
    assert(!nr_register_resource(1, 2, 3, 4, sizeof(flags), 77, retain, release, &token));
    nr_associate_import(2, (void *)6);
    nr_associate_mapping((void *)6, flags, 0, sizeof(flags));
    assert(!nr_snapshot(1, snapshot, 2, &count) && count == 1);
    assert(!nr_claim_queue(snapshot, 1, 9));

    if (scenario == 0) {
        run_frame(snapshot, count, 1);
        assert(!strcmp(trace, "mEIKXwFm") || !strcmp(trace, "EIKXwFm"));
    } else if (scenario == 1) {
        void *wait_args[] = {&flag_ptr, &frame, &limit};
        void *ordinary[] = {&flag_ptr};
        assert(!nr_publish_input(token, frame, snapshot, count));
        assert(!send(g_flag_wait, wait_args, marker_stream));
        assert(send((void *)0x9999, ordinary, work_stream) == 1 && !launches);
        assert(stub_sync() != 0 && !nr_job.active);
    } else if (scenario == 2) {
        run_frame(snapshot, count, 1);
        run_frame(snapshot, count, 2);
        trace_n = 0; memset(trace, 0, sizeof trace);
        run_frame(snapshot, count, 1);   /* counter restart on the same token */
        run_frame(snapshot, count, 2);
    } else if (scenario == 4) {
        struct kernels a = K, b = {(void *)11, (void *)12, (void *)13, (void *)14, (void *)15};
        assert(nr_role_add(NR_ROLE_FLAG_WAIT, b.wait) == 1 && nr_role_add(NR_ROLE_FLAG_SET, b.set) == 1);
        assert(nr_role_add(NR_ROLE_IMPORT, b.imp) == 1 && nr_role_add(NR_ROLE_EXPORT, b.exp) == 1);
        assert(nr_role_add(NR_ROLE_EXPO, b.expo) == 1);
        assert(nr_role_add(NR_ROLE_FLAG_WAIT, b.wait) == 1);   /* re-registration is idempotent */
        assert(g_flag_wait == a.wait);                           /* first copy keeps its slot */
        K = a; run_frame(snapshot, count, 1);                    /* pass 1 after pass 2 loaded */
        K = b; run_frame(snapshot, count, 2);                    /* pass 2 */
        K = a; run_frame(snapshot, count, 3);
        unsigned sets = 0, imports = 0;
        for (unsigned i = 0; i < trace_n; ++i) { sets += trace[i] == 'F'; imports += trace[i] == 'I'; }
        assert(sets == 3 && imports == 3);                       /* every frame went through the handoff */
    } else if (scenario == 5) {
        run_frame(snapshot, count, 1);                           /* queue 9 owns, idle again */
        assert(!nr_claim_queue(snapshot, 1, 10));                /* handover to queue 10 */
        run_frame(snapshot, count, 2);
        assert(!nr_claim_queue(snapshot, 1, 9));                 /* and back */
        run_frame(snapshot, count, 3);
    } else if (scenario == 6 || scenario == 7) {
        uint32_t f1 = 1;
        void *wait_args[] = {&flag_ptr, &f1, &limit};
        void *set_args[] = {&flag_ptr, &f1, &abort_ptr};
        void *ordinary[] = {&flag_ptr};
        pthread_t t;
        assert(!nr_publish_input(token, 1, snapshot, count));    /* job in flight on queue 9 */
        memcpy(claim_snapshot, snapshot, sizeof(snapshot)); claim_count = count;
        assert(!pthread_create(&t, NULL, claim_thread, (void *)(uintptr_t)10));
        msleep(150);
        assert(claim_done == -1);                                /* queue 10 waits */
        if (scenario == 6) {
            assert(!send(K.wait, wait_args, marker_stream));
            assert(!send(K.imp, ordinary, work_stream));
            assert(!send(K.set, set_args, marker_stream));       /* completes, releases pins */
            pthread_join(t, NULL);
            assert(claim_done == 0);
            assert(!nr_wait_output(token, 1, 0) && !nr_consumer_submitted(token, 1));
        } else {
            pthread_join(t, NULL);                               /* ~2 s bound */
            assert(claim_done == 1);
        }
    } else if (scenario == 3) {
        assert(!nr_publish_input(token, 1, snapshot, count));
        assert(nr_publish_input(token, 1, snapshot, count) != 0);   /* in flight: replay rejected */
    }
    snapshot[0].release(snapshot[0].owner);
    printf("scenario %d passed: %s refs=%d\n", scenario, trace, refs);
    return 0;
}
