/* Profiler bookkeeping with fake HIP events: warm-up skipped, 1 job in NRP_EVERY timed,
 * per-kernel times aggregated by host stub, launches untouched when not sampling. */
#include "../native/hip_bridge.c"
#include <assert.h>
static int launches, records;
static float next_ms[2] = {1.5f, 0.25f};
static int fake_launch(const void *f, dim3_t g, dim3_t b, void **a, u64 sh, void *st) { (void)f; (void)g; (void)b; (void)a; (void)sh; (void)st; ++launches; return 0; }
static int fake_create(void **e, unsigned flags) { static char pool[1 << 16]; static unsigned n; (void)flags; *e = &pool[n++]; return 0; }
static int fake_record(void *e, void *st) { (void)e; (void)st; ++records; return 0; }
static int fake_sync(void *e) { (void)e; return 0; }
static int fake_elapsed(float *ms, void *a, void *b) { (void)a; (void)b; static unsigned i; *ms = next_ms[i++ % 2]; return 0; }
int main(void) {
    setenv("DLSSNR_PROFILE", "1", 1);
    real.p_hipLaunchKernel = fake_launch; real.p_hipEventCreateWithFlags = fake_create;
    real.p_hipEventRecord = fake_record; real.p_hipEventSynchronize = fake_sync; real.p_hipEventElapsedTime = fake_elapsed;
    const void *conv = (void *)0x100, *attn = (void *)0x200;
    nrp_note_name(conv, "_Z6k_convv"); nrp_note_name(attn, "_Z6k_attnv");
    unsigned jobs = NRP_WARMUP + NRP_EVERY * 3;   /* -> 3 sampled jobs */
    for (unsigned j = 0; j < jobs; ++j) {
        nrp_job_begin();
        int sampled = nrp && nrp->sampling;
        assert(nrp_launch(conv, (dim3_t){8,1,1}, (dim3_t){256,1,1}, 0, 0, (void *)1) == 0);
        assert(nrp_launch(attn, (dim3_t){4,1,1}, (dim3_t){128,1,1}, 0, 0, (void *)1) == 0);
        if (j < NRP_WARMUP) assert(!sampled);
        nrp_job_end(1);
    }
    assert(launches == (int)(2 * jobs));           /* every launch still issued exactly once */
    assert(nrp_samples == 3 && records == 3 * 2 * 2); /* only sampled jobs got event pairs */
    double conv_ms = 0, attn_ms = 0;
    for (unsigned i = 0; i < nrp_agg_n; ++i) {
        if (nrp_agg[i].f == conv) { conv_ms = nrp_agg[i].ms; assert(nrp_agg[i].launches == 3 && nrp_agg[i].grid.x == 8); }
        if (nrp_agg[i].f == attn) attn_ms = nrp_agg[i].ms;
    }
    assert(conv_ms > 4.49 && conv_ms < 4.51 && attn_ms > 0.74 && attn_ms < 0.76);
    assert(!strcmp(nrp_name(attn), "_Z6k_attnv"));
    puts("profile contract ok");
    return 0;
}
