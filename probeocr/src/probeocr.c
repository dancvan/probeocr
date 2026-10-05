/*
 * probeocr — batch-extract probe readings from screenshots (command line).
 *
 * Reads a layout file describing labelled regions ("probes") on a reference
 * screenshot, optionally aligns each screenshot to the reference using an
 * anchor region (to absorb small window shifts), OCRs every probe region with
 * Tesseract and emits one JSON object per image on stdout (NDJSON). A wide
 * CSV (one row per image, one column per probe label) can also be written.
 * The layout format is described in core.h.
 *
 * Usage:
 *   probeocr [options] <layout.txt> <image>... | -
 *     -            read image paths from stdin, one per line
 *     --csv FILE   also write a wide CSV
 *     --lang L     tesseract language (default: eng)
 *     --tessdata D folder holding <lang>.traineddata (default: tesseract's
 *                  built-in path)
 *     --debug DIR  write the preprocessed crop for every probe to DIR
 */
#include "core.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *csv_path;
    const char *lang;
    const char *tessdata;
    const char *debug_dir;
} Options;

static void die(const char *msg) {
    fprintf(stderr, "probeocr: %s\n", msg);
    exit(1);
}

static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else fputc(c, f);
        }
    }
    fputc('"', f);
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

static void emit(const char *path, const PoLayout *L, const PoImageResult *r, FILE *csv) {
    printf("{\"image\":"); json_str(stdout, path);
    if (!r->ok) {
        printf(",\"ok\":false,\"error\":"); json_str(stdout, r->error); printf("}\n");
        fflush(stdout);
        if (csv) { csv_field(csv, path); fputs(",error\n", csv); }
        return;
    }
    printf(",\"ok\":true,\"dx\":%d,\"dy\":%d,\"anchor_score\":%.3f,\"probes\":[",
           r->dx, r->dy, r->anchor_score);
    for (int i = 0; i < L->nprobes; i++) {
        if (i) putchar(',');
        printf("{\"label\":"); json_str(stdout, L->probes[i].label);
        printf(",\"value\":"); json_str(stdout, r->probes[i].value);
        printf(",\"raw\":"); json_str(stdout, r->probes[i].raw);
        printf(",\"conf\":%d}", r->probes[i].conf);
    }
    printf("]}\n");
    fflush(stdout);

    if (csv) {
        csv_field(csv, path);
        fprintf(csv, ",%d,%d", r->dx, r->dy);
        for (int i = 0; i < L->nprobes; i++) {
            fputc(',', csv);
            csv_field(csv, r->probes[i].value);
        }
        fputc('\n', csv);
    }
}

static void usage(void) {
    fputs("usage: probeocr [--csv FILE] [--lang L] [--tessdata DIR] [--debug DIR]\n"
          "                <layout.txt> <image>... | -\n", stderr);
    exit(2);
}

int main(int argc, char **argv) {
#ifdef _WIN32
    char **wargv = po_utf8_argv(&argc);
    if (wargv) argv = wargv;
#endif
    Options opt = { .lang = "eng" };
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--csv") == 0 && i + 1 < argc) opt.csv_path = argv[++i];
        else if (strcmp(a, "--lang") == 0 && i + 1 < argc) opt.lang = argv[++i];
        else if (strcmp(a, "--tessdata") == 0 && i + 1 < argc) opt.tessdata = argv[++i];
        else if (strcmp(a, "--debug") == 0 && i + 1 < argc) opt.debug_dir = argv[++i];
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) usage();
        else if (a[0] == '-' && a[1] == '-') usage();
        else break;
    }
    if (argc - i < 2) usage();

    char err[PO_MAX_PATH + 128];
    PoLayout *L = calloc(1, sizeof *L);
    if (po_layout_load(argv[i++], L, err, sizeof err) != 0) die(err);
    if (L->nprobes == 0) die("layout defines no probes");

    PoEngine *eng = po_engine_create(opt.tessdata, opt.lang, err, sizeof err);
    if (!eng) die(err);
    if (po_engine_set_layout(eng, L, err, sizeof err) != 0) die(err);

    FILE *csv = NULL;
    if (opt.csv_path) {
        csv = po_fopen(opt.csv_path, "w");
        if (!csv) { snprintf(err, sizeof err, "cannot write csv: %s", opt.csv_path); die(err); }
        fputs("image,dx,dy", csv);
        for (int k = 0; k < L->nprobes; k++) {
            fputc(',', csv);
            csv_field(csv, L->probes[k].label);
        }
        fputc('\n', csv);
    }

    PoProbeResult *probes = calloc((size_t)L->nprobes, sizeof *probes);
    PoImageResult res = { .probes = probes };
    int n = 0;
    if (strcmp(argv[i], "-") == 0) {
        char buf[PO_MAX_PATH];
        while (fgets(buf, sizeof buf, stdin)) {
            char *p = buf;
            while (isspace((unsigned char)*p)) p++;
            char *e = p + strlen(p);
            while (e > p && isspace((unsigned char)e[-1])) *--e = '\0';
            if (!*p) continue;
            po_engine_process(eng, p, &res, opt.debug_dir, n++);
            emit(p, L, &res, csv);
        }
    } else {
        for (; i < argc; i++) {
            po_engine_process(eng, argv[i], &res, opt.debug_dir, n++);
            emit(argv[i], L, &res, csv);
        }
    }

    if (csv) fclose(csv);
    free(probes);
    po_engine_destroy(eng);
    free(L);
    return 0;
}
