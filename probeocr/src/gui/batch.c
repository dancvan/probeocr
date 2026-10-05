/*
 * Background OCR: a small pool of worker threads, each with its own Tesseract
 * engine, pulls images off a shared counter. The UI thread polls once per
 * frame and attaches finished results.
 */
#include "app.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define MAX_WORKERS 4

typedef struct {
    PoLayout layout;
    int n;
    int *img;
    char **paths;
    PoImageResult *res;
    PoProbeResult *store;
    unsigned char *ready, *taken;
    pthread_mutex_t mu;
    int next, done, finished, nthreads;
    volatile int cancel;
    pthread_t th[MAX_WORKERS];
    char tessdata[PO_MAX_PATH];
    bool has_tessdata;
    char err[512];
    double t0;
} Job;

static Job *J;

static int cpu_count(void) {
#ifdef _WIN32
    /* <windows.h> clashes with raylib.h, so ask the environment instead */
    const char *n = getenv("NUMBER_OF_PROCESSORS");
    return n && atoi(n) > 0 ? atoi(n) : 2;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

static void *worker(void *arg) {
    Job *j = arg;
    char err[512] = "";
    PoEngine *e = po_engine_create(j->has_tessdata ? j->tessdata : NULL, "eng", err, sizeof err);
    if (e && po_engine_set_layout(e, &j->layout, err, sizeof err) != 0) { po_engine_destroy(e); e = NULL; }
    if (!e) {
        pthread_mutex_lock(&j->mu);
        if (!j->err[0]) snprintf(j->err, sizeof j->err, "%s", err);
        j->finished++;
        pthread_mutex_unlock(&j->mu);
        return NULL;
    }
    for (;;) {
        pthread_mutex_lock(&j->mu);
        int i = j->cancel || j->next >= j->n ? -1 : j->next++;
        pthread_mutex_unlock(&j->mu);
        if (i < 0) break;
        po_engine_process(e, j->paths[i], &j->res[i], NULL, i);
        pthread_mutex_lock(&j->mu);
        j->ready[i] = 1;
        j->done++;
        pthread_mutex_unlock(&j->mu);
    }
    po_engine_destroy(e);
    pthread_mutex_lock(&j->mu);
    j->finished++;
    pthread_mutex_unlock(&j->mu);
    return NULL;
}

static void job_free(Job *j) {
    for (int i = 0; i < j->n; i++) free(j->paths[i]);
    free(j->paths); free(j->img); free(j->res); free(j->store); free(j->ready); free(j->taken);
    pthread_mutex_destroy(&j->mu);
    free(j);
}

bool batch_running(void) { return J != NULL; }

float batch_progress(int *done, int *total) {
    if (!J) { *done = *total = 0; return 0; }
    pthread_mutex_lock(&J->mu);
    *done = J->done;
    pthread_mutex_unlock(&J->mu);
    *total = J->n;
    return J->n ? (float)*done / J->n : 0;
}

void batch_start(const int *img_idx, int n) {
    if (J || n <= 0) return;
    if (!A.nprobes) { toast("Draw at least one probe box first"); return; }

    Job *j = calloc(1, sizeof *j);
    PoLayout *L = &j->layout;
    snprintf(L->root, sizeof L->root, "%s", A.root);
    if (A.ref >= 0) snprintf(L->ref_path, sizeof L->ref_path, "%s", A.imgs[A.ref].path);
    L->has_anchor = A.anchor.on;
    L->ax = A.anchor.x; L->ay = A.anchor.y; L->aw = A.anchor.w; L->ah = A.anchor.h; L->search = A.anchor.search;
    L->nprobes = A.nprobes;
    A.ncols = A.nprobes;
    for (int i = 0; i < A.nprobes; i++) {
        Box *b = &A.probes[i];
        PoProbe *p = &L->probes[i];
        p->x = b->x; p->y = b->y; p->w = b->w; p->h = b->h; p->mode = b->mode;
        snprintf(p->label, sizeof p->label, "%s", b->label[0] ? b->label : TextFormat("probe%d", i + 1));
        snprintf(A.cols[i], sizeof A.cols[i], "%s", p->label);
    }

    j->n = n;
    j->img = malloc((size_t)n * sizeof *j->img);
    j->paths = malloc((size_t)n * sizeof *j->paths);
    j->res = calloc((size_t)n, sizeof *j->res);
    j->store = calloc((size_t)n * (size_t)L->nprobes, sizeof *j->store);
    j->ready = calloc((size_t)n, 1);
    j->taken = calloc((size_t)n, 1);
    for (int i = 0; i < n; i++) {
        j->img[i] = img_idx[i];
        j->paths[i] = strdup(A.imgs[img_idx[i]].path);
        j->res[i].probes = j->store + (size_t)i * L->nprobes;
    }
    snprintf(j->tessdata, sizeof j->tessdata, "%s", A.tessdata);
    j->has_tessdata = A.has_tessdata;
    pthread_mutex_init(&j->mu, NULL);
    j->t0 = GetTime();

    int workers = cpu_count();
    if (workers > MAX_WORKERS) workers = MAX_WORKERS;
    if (workers > n) workers = n;
    j->nthreads = 0;
    for (int i = 0; i < workers; i++)
        if (pthread_create(&j->th[j->nthreads], NULL, worker, j) == 0) j->nthreads++;
    if (!j->nthreads) { job_free(j); toast("Could not start OCR threads"); return; }
    J = j;
    app_set_status("Processing 0 / %d...", n);
}

static void attach(Job *j, int i) {
    Img *im = &A.imgs[j->img[i]];
    PoImageResult *src = &j->res[i];
    Result *r = calloc(1, sizeof *r);
    r->ok = src->ok;
    snprintf(r->error, sizeof r->error, "%s", src->error);
    r->dx = src->dx; r->dy = src->dy; r->score = src->anchor_score;
    r->n = j->layout.nprobes;
    r->p = malloc((size_t)r->n * sizeof *r->p);
    memcpy(r->p, src->probes, (size_t)r->n * sizeof *r->p);
    r->edited = calloc((size_t)r->n, sizeof *r->edited);
    if (im->res) { free(im->res->p); free(im->res->edited); free(im->res); }
    im->res = r;
}

static void finish(Job *j) {
    for (int i = 0; i < j->nthreads; i++) pthread_join(j->th[i], NULL);
    J = NULL;
    int review = 0;
    for (int i = 0; i < j->n; i++) {
        if (j->ready[i] && !j->taken[i]) attach(j, i);
        if (j->ready[i] && result_needs_review(A.imgs[j->img[i]].res)) review++;
    }
    if (j->err[0] && !j->done) {
        app_set_status("Error: %s", j->err);
        toast("%s", j->err);
    } else if (j->cancel) {
        app_set_status("Stopped after %d of %d images", j->done, j->n);
    } else {
        app_set_status("%d image%s in %.1fs%s", j->n, j->n == 1 ? "" : "s", GetTime() - j->t0,
                       review ? TextFormat(" · %d need review", review) : " · all confident");
    }
    job_free(j);
}

void batch_poll(void) {
    Job *j = J;
    if (!j) return;
    pthread_mutex_lock(&j->mu);
    for (int i = 0; i < j->n; i++)
        if (j->ready[i] && !j->taken[i]) { attach(j, i); j->taken[i] = 1; }
    bool done = j->finished == j->nthreads;
    int d = j->done;
    pthread_mutex_unlock(&j->mu);
    if (done) finish(j);
    else app_set_status("Processing %d / %d...", d, j->n);
}

void batch_cancel(void) {
    if (!J) return;
    J->cancel = 1;
    finish(J);
}

/* ---------------- results ---------------- */

void results_clear(void) {
    for (int i = 0; i < A.nimgs; i++) {
        Result *r = A.imgs[i].res;
        if (!r) continue;
        free(r->p); free(r->edited); free(r);
        A.imgs[i].res = NULL;
    }
    A.ncols = 0;
}

void invalidate(void) {
    batch_cancel();
    bool any = false;
    for (int i = 0; i < A.nimgs && !any; i++) any = A.imgs[i].res != NULL;
    if (any) results_clear();
}

bool result_needs_review(const Result *r) {
    if (!r) return true;
    if (!r->ok) return true;
    for (int i = 0; i < r->n; i++)
        if (!r->edited[i] && (r->p[i].conf < LOW_CONF || !r->p[i].value[0])) return true;
    return false;
}

static void csv_field(FILE *f, const char *s) {
    if (strpbrk(s, ",\"\n\r") == NULL) { fputs(s, f); return; }
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"') fputc('"', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

int export_csv(char *out_path, size_t outsz) {
    const char *name = A.layout_name[0] ? A.layout_name : "probe_readings";
#ifdef _WIN32
    snprintf(out_path, outsz, "%s\\%s.csv", A.root, name);
#else
    snprintf(out_path, outsz, "%s/%s.csv", A.root, name);
#endif
    FILE *f = po_fopen(out_path, "w");
    if (!f) return -1;
    fputs("image", f);
    for (int c = 0; c < A.ncols; c++) {
        fputc(',', f); csv_field(f, A.cols[c]);
        if (A.with_conf) { fputc(',', f); csv_field(f, TextFormat("%s (conf)", A.cols[c])); }
    }
    fputc('\n', f);
    int rows = 0;
    for (int i = 0; i < A.nimgs; i++) {
        Img *im = &A.imgs[i];
        if (!im->checked || !im->res) continue;
        csv_field(f, im->name);
        for (int c = 0; c < A.ncols; c++) {
            fputc(',', f);
            if (c < im->res->n) csv_field(f, im->res->p[c].value);
            if (A.with_conf) {
                fputc(',', f);
                if (c < im->res->n)
                    fputs(im->res->edited[c] ? "edited" : TextFormat("%d", im->res->p[c].conf), f);
            }
        }
        fputc('\n', f);
        rows++;
    }
    return fclose(f) == 0 ? rows : -1;
}
