/* Upstream 0.6.0 ordered-handoff shapes, exercised against the production bridge.
 * Scenario 0: k_expo_resolve (FreshExposure) opens the work stream before k_import.
 * Scenario 1: an unregistered kernel still cannot open the work stream.
 * Scenario 2: after a fully consumed frame, the producer restarts at the same frame
 *             number on the same token (0.6.0 pooled flag buffer): accepted.
 * Scenario 3: the same frame number while that frame is still in flight: rejected.
 */
#include "../native/hip_bridge.c"
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
    if (f == g_flag_set) { assert(stream == marker_stream); record('F'); }
    else { assert(stream == work_stream);
           record(f == g_k_expo ? 'E' : f == g_k_import ? 'I' : f == g_k_export ? 'X' : 'K'); }
    return 0;
}
static int send(const void *fn, void **args, void *stream) {
    return stub_launch(fn, (dim3_t){1, 1, 1}, (dim3_t){32, 1, 1}, args, 0, stream);
}

static uint32_t flags[16];
static void *flag_ptr = flags, *abort_ptr = (void *)0x3000;
static uint32_t limit = 20000000;

/* One complete 0.6.0-shaped job for `frame`: wait, expo, import, network, export, set. */
static void run_frame(struct nr_buffer *snapshot, uint32_t count, uint32_t frame) {
    void *wait_args[] = {&flag_ptr, &frame, &limit};
    void *set_args[] = {&flag_ptr, &frame, &abort_ptr};
    void *ordinary[] = {&flag_ptr};
    assert(!nr_publish_input(token, frame, snapshot, count));
    assert(!send(g_flag_wait, wait_args, marker_stream) && nr_job.active);
    assert(!send(g_k_expo, ordinary, work_stream));
    assert(!send(g_k_import, ordinary, work_stream));
    assert(!send((void *)0x9999, ordinary, work_stream));
    assert(!send(g_k_export, ordinary, work_stream));
    assert(!send(g_flag_set, set_args, marker_stream) && !nr_job.active);
    assert(!nr_wait_output(token, frame, 0));
    assert(!nr_consumer_submitted(token, frame));
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int scenario = atoi(argv[1]);
    g_hip_ok = 1;
    real.p_hipLaunchKernel = native_launch;
    real.p_hipStreamSynchronize = native_sync;
    real.p_hipDeviceSynchronize = device_sync;
    g_flag_wait = (void *)1; g_flag_set = (void *)2; g_k_import = (void *)3;
    g_k_export = (void *)4; g_k_expo = (void *)5;
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
    } else if (scenario == 3) {
        assert(!nr_publish_input(token, 1, snapshot, count));
        assert(nr_publish_input(token, 1, snapshot, count) != 0);   /* in flight: replay rejected */
    }
    snapshot[0].release(snapshot[0].owner);
    printf("scenario %d passed: %s refs=%d\n", scenario, trace, refs);
    return 0;
}
