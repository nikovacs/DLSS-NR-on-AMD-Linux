/* Included by hip_bridge.c (after nr_ordered.c). Optional per-kernel GPU timing of the
 * upstream network, enabled with DLSSNR_PROFILE=1 (environment or bridge.conf).
 *
 * After a warm-up, one ordered job in NRP_EVERY is sampled: every kernel launched inside
 * it gets a HIP event pair on its own stream. The timings are read when the job completes,
 * after the bridge's own stream synchronisation, so unsampled jobs are untouched and
 * sampled ones gain only the event records. Every NRP_REPORT samples a table of the
 * kernels by GPU time per job is written to the bridge log. Diagnostic only: launches,
 * streams and ordering are unchanged. */
#define NRP_NAMES 4096
#define NRP_LAUNCHES 4096
#define NRP_WARMUP 240u
#define NRP_EVERY 30u
#define NRP_REPORT 20u

struct nrp_launch { const void *f; void *start, *stop; dim3_t grid, block; };
struct nrp_tls { int sampling; unsigned n; void *events[2 * NRP_LAUNCHES]; unsigned events_made;
                 struct nrp_launch l[NRP_LAUNCHES]; };
struct nrp_kernel { const void *f; double ms; unsigned long long launches; dim3_t grid, block; };

static struct { const void *f; char name[112]; } nrp_names[NRP_NAMES];
static unsigned nrp_name_n;
static struct nrp_kernel nrp_agg[NRP_NAMES];
static unsigned nrp_agg_n, nrp_samples, nrp_reports;
static double nrp_job_ms_sum;
static unsigned long long nrp_jobs_seen;
static pthread_mutex_t nrp_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local struct nrp_tls *nrp;

static int nrp_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("DLSSNR_PROFILE");
        enabled = v && !strcmp(v, "1");
        if (enabled) logmsg("profile: on (warm-up %u jobs, then 1 job in %u, table every %u samples)",
                            NRP_WARMUP, NRP_EVERY, NRP_REPORT);
    }
    return enabled;
}

static void nrp_note_name(const void *f, const char *name) {
    pthread_mutex_lock(&nrp_mutex);
    if (nrp_name_n < NRP_NAMES) {
        nrp_names[nrp_name_n].f = f;
        snprintf(nrp_names[nrp_name_n].name, sizeof(nrp_names[0].name), "%s", name);
        ++nrp_name_n;
    }
    pthread_mutex_unlock(&nrp_mutex);
}
static const char *nrp_name(const void *f) {
    for (unsigned i = 0; i < nrp_name_n; ++i)
        if (nrp_names[i].f == f) return nrp_names[i].name;
    return "?";
}

/* At a successful nr_begin_job: decide whether this job is sampled. */
static void nrp_job_begin(void) {
    if (!nrp_enabled()) return;
    unsigned long long n = __atomic_fetch_add(&nrp_jobs_seen, 1, __ATOMIC_RELAXED);
    if (!nrp) nrp = calloc(1, sizeof(*nrp));
    if (!nrp) return;
    nrp->n = 0;
    nrp->sampling = n >= NRP_WARMUP && (n - NRP_WARMUP) % NRP_EVERY == 0;
}

/* A kernel launch inside an ordered job. Returns the launch result. */
static int nrp_launch(const void *f, dim3_t nb, dim3_t db, void **a, u64 sh, void *st) {
    struct nrp_tls *t = nrp;
    if (!t || !t->sampling || t->n >= NRP_LAUNCHES || !real.p_hipEventCreateWithFlags || !real.p_hipEventRecord)
        return real.p_hipLaunchKernel(f, nb, db, a, sh, st);
    while (t->events_made < 2 * (t->n + 1)) {
        if (real.p_hipEventCreateWithFlags(&t->events[t->events_made], 0)) {
            t->sampling = 0;   /* cannot time this job; never disturb the launch */
            return real.p_hipLaunchKernel(f, nb, db, a, sh, st);
        }
        ++t->events_made;
    }
    struct nrp_launch *l = &t->l[t->n];
    l->f = f; l->grid = nb; l->block = db;
    l->start = t->events[2 * t->n]; l->stop = t->events[2 * t->n + 1];
    real.p_hipEventRecord(l->start, st);
    int e = real.p_hipLaunchKernel(f, nb, db, a, sh, st);
    real.p_hipEventRecord(l->stop, st);
    if (!e) ++t->n;
    return e;
}

static int nrp_cmp(const void *a, const void *b) {
    double x = ((const struct nrp_kernel *)a)->ms, y = ((const struct nrp_kernel *)b)->ms;
    return x < y ? 1 : x > y ? -1 : 0;
}
static void nrp_report_locked(void) {
    struct nrp_kernel sorted[NRP_NAMES];
    double total = 0;
    memcpy(sorted, nrp_agg, nrp_agg_n * sizeof(sorted[0]));
    qsort(sorted, nrp_agg_n, sizeof(sorted[0]), nrp_cmp);
    for (unsigned i = 0; i < nrp_agg_n; ++i) total += sorted[i].ms;
    logmsg("profile report %u: %u sampled jobs, %.2f ms GPU per job across %u kernels (sum of kernel times; "
           "the network-time figure upstream logs also includes gaps)", ++nrp_reports, nrp_samples,
           total / nrp_samples, nrp_agg_n);
    double cumulative = 0;
    for (unsigned i = 0; i < nrp_agg_n && i < 30; ++i) {
        cumulative += sorted[i].ms;
        logmsg("profile  %5.2f ms/job %5.1f%% (cum %5.1f%%) x%-5.1f grid %u,%u,%u block %u,%u,%u  %s",
               sorted[i].ms / nrp_samples, 100.0 * sorted[i].ms / total, 100.0 * cumulative / total,
               (double)sorted[i].launches / nrp_samples, sorted[i].grid.x, sorted[i].grid.y, sorted[i].grid.z,
               sorted[i].block.x, sorted[i].block.y, sorted[i].block.z, nrp_name(sorted[i].f));
    }
}

/* After the job's completion (its streams are synchronised). */
static void nrp_job_end(int ok) {
    struct nrp_tls *t = nrp;
    if (!t || !t->sampling) return;
    t->sampling = 0;
    if (!ok || !t->n || !real.p_hipEventElapsedTime) return;
    if (real.p_hipEventSynchronize) real.p_hipEventSynchronize(t->l[t->n - 1].stop);
    pthread_mutex_lock(&nrp_mutex);
    double job = 0;
    for (unsigned i = 0; i < t->n; ++i) {
        float ms = 0;
        if (real.p_hipEventElapsedTime(&ms, t->l[i].start, t->l[i].stop) || ms < 0) continue;
        unsigned k = 0;
        while (k < nrp_agg_n && nrp_agg[k].f != t->l[i].f) ++k;
        if (k == nrp_agg_n) {
            if (nrp_agg_n == NRP_NAMES) continue;
            nrp_agg[nrp_agg_n++] = (struct nrp_kernel){t->l[i].f, 0, 0, t->l[i].grid, t->l[i].block};
        }
        nrp_agg[k].ms += ms;
        nrp_agg[k].launches++;
        job += ms;
    }
    nrp_job_ms_sum += job;
    if (++nrp_samples % NRP_REPORT == 0) nrp_report_locked();
    pthread_mutex_unlock(&nrp_mutex);
}
