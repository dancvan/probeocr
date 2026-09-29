/*
 * probeocr — batch-extract probe readings from screenshots.
 *
 * Reads a layout file describing labelled regions ("probes") on a reference
 * screenshot, optionally aligns each screenshot to the reference using an
 * anchor region (to absorb small window shifts), OCRs every probe region with
 * Tesseract and emits one JSON object per image on stdout (NDJSON). A wide
 * CSV (one row per image, one column per probe label) can also be written.
 *
 * Layout file (plain text, one directive per line, '#' starts a comment):
 *
 *   ref    <path to reference image>          (required if an anchor is used)
 *   anchor <x> <y> <w> <h> <search_px>
 *   probe  <x> <y> <w> <h> <num|text> <label ... rest of line>
 *
 * Usage:
 *   probeocr [options] <layout.txt> <image>... | -
 *     -            read image paths from stdin, one per line
 *     --csv FILE   also write a wide CSV
 *     --lang L     tesseract language (default: eng)
 *     --tessdata D folder holding <lang>.traineddata (default: tesseract's
 *                  built-in path; the server passes ./tessdata when bundled)
 *     --debug DIR  write the preprocessed crop for every probe to DIR
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <allheaders.h>
#include <tesseract/capi.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#define NULL_DEVICE "NUL"
#else
#define NULL_DEVICE "/dev/null"
#endif

#define MAX_PROBES 256
#define MAX_LINE 4096

typedef enum { MODE_NUM, MODE_TEXT } ProbeMode;

typedef struct {
    int x, y, w, h;
    ProbeMode mode;
    char label[256];
} Probe;

typedef struct {
    char ref_path[MAX_LINE];
    int has_anchor;
    int ax, ay, aw, ah, search;
    int nprobes;
    Probe probes[MAX_PROBES];
} Layout;

typedef struct {
    const char *csv_path;
    const char *lang;
    const char *tessdata;
    const char *debug_dir;
} Options;

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static void die(const char *msg, const char *arg) {
    fprintf(stderr, "probeocr: %s%s%s\n", msg, arg ? ": " : "", arg ? arg : "");
    exit(1);
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
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

/*
 * All paths in this program are UTF-8. On Windows the C runtime's fopen and
 * argv use the ANSI code page instead, so go through the wide-char APIs.
 */
static FILE *fopen_utf8(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[MAX_LINE], wmode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_LINE)) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8)) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

#ifdef _WIN32
static char **utf8_argv(int *argc) {
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), argc);
    if (!w) return NULL;
    char **a = calloc((size_t)*argc + 1, sizeof *a);
    for (int i = 0; i < *argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL, NULL);
        a[i] = malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, a[i], n, NULL, NULL);
    }
    LocalFree(w);
    return a;
}
#endif

/* Read a whole file into memory (caller frees). */
static char *slurp(const char *path, long *size) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (*size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)*size);
        if (buf && fread(buf, 1, (size_t)*size, f) != (size_t)*size) { free(buf); buf = NULL; }
    }
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Layout parsing                                                      */
/* ------------------------------------------------------------------ */

static void load_layout(const char *path, Layout *L) {
    FILE *f = fopen_utf8(path, "r");
    if (!f) die("cannot open layout", path);
    memset(L, 0, sizeof *L);

    char buf[MAX_LINE];
    int lineno = 0;
    while (fgets(buf, sizeof buf, f)) {
        lineno++;
        char *line = trim(buf);
        if (*line == '\0' || *line == '#') continue;

        if (strncmp(line, "ref ", 4) == 0) {
            snprintf(L->ref_path, sizeof L->ref_path, "%s", trim(line + 4));
        } else if (strncmp(line, "anchor ", 7) == 0) {
            if (sscanf(line + 7, "%d %d %d %d %d",
                       &L->ax, &L->ay, &L->aw, &L->ah, &L->search) != 5)
                goto bad;
            L->has_anchor = 1;
        } else if (strncmp(line, "probe ", 6) == 0) {
            if (L->nprobes >= MAX_PROBES) die("too many probes in layout", path);
            Probe *p = &L->probes[L->nprobes];
            char mode[16];
            int consumed = 0;
            if (sscanf(line + 6, "%d %d %d %d %15s %n",
                       &p->x, &p->y, &p->w, &p->h, mode, &consumed) != 5)
                goto bad;
            p->mode = strcmp(mode, "text") == 0 ? MODE_TEXT : MODE_NUM;
            snprintf(p->label, sizeof p->label, "%s", trim(line + 6 + consumed));
            if (p->label[0] == '\0')
                snprintf(p->label, sizeof p->label, "probe%d", L->nprobes + 1);
            L->nprobes++;
        } else {
            goto bad;
        }
        continue;
    bad:
        fprintf(stderr, "probeocr: %s:%d: cannot parse: %s\n", path, lineno, line);
        exit(1);
    }
    fclose(f);
    if (L->has_anchor && L->ref_path[0] == '\0')
        die("layout has an anchor but no 'ref' image", path);
}

/* ------------------------------------------------------------------ */
/* Image helpers                                                       */
/* ------------------------------------------------------------------ */

/* Load any image as 8-bit grayscale, flattening alpha onto white. */
static PIX *load_gray(const char *path) {
    FILE *fp = fopen_utf8(path, "rb");
    if (!fp) return NULL;
    PIX *raw = pixReadStream(fp, 0);
    fclose(fp);
    if (!raw) return NULL;
    PIX *flat = pixGetSpp(raw) == 4 ? pixRemoveAlpha(raw) : pixClone(raw);
    PIX *gray = pixConvertTo8(flat, 0);
    pixDestroy(&flat);
    pixDestroy(&raw);
    return gray;
}

/*
 * Normalised cross-correlation of template `t` against `img` with the
 * template's top-left at every position in [x0,x1]x[y0,y1] (step `step`).
 * Returns the best score and writes the best position.
 */
static double ncc_search(PIX *img, PIX *t, int x0, int y0, int x1, int y1,
                         int step, int *bx, int *by) {
    int iw = pixGetWidth(img), ih = pixGetHeight(img);
    int tw = pixGetWidth(t), th = pixGetHeight(t);
    l_uint32 *id = pixGetData(img), *td = pixGetData(t);
    int iwpl = pixGetWpl(img), twpl = pixGetWpl(t);
    int n = tw * th;

    double tsum = 0, tsq = 0;
    for (int y = 0; y < th; y++) {
        l_uint32 *tl = td + y * twpl;
        for (int x = 0; x < tw; x++) {
            int v = GET_DATA_BYTE(tl, x);
            tsum += v; tsq += (double)v * v;
        }
    }
    double tmean = tsum / n;
    double tvar = tsq - tsum * tmean;

    double best = -2.0;
    *bx = x0; *by = y0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > iw - tw) x1 = iw - tw;
    if (y1 > ih - th) y1 = ih - th;

    for (int oy = y0; oy <= y1; oy += step) {
        for (int ox = x0; ox <= x1; ox += step) {
            double isum = 0, isq = 0, cross = 0;
            for (int y = 0; y < th; y++) {
                l_uint32 *il = id + (oy + y) * iwpl;
                l_uint32 *tl = td + y * twpl;
                for (int x = 0; x < tw; x++) {
                    int a = GET_DATA_BYTE(il, ox + x);
                    int b = GET_DATA_BYTE(tl, x);
                    isum += a; isq += (double)a * a; cross += (double)a * b;
                }
            }
            double ivar = isq - isum * isum / n;
            double denom = sqrt(ivar * tvar);
            /* Flat patches (zero variance) match anything; treat as no match. */
            double score = denom > 1e-6 ? (cross - isum * tmean) / denom : -1.0;
            if (score > best) { best = score; *bx = ox; *by = oy; }
        }
    }
    return best;
}

/*
 * Find where the anchor template sits in `img`. Coarse search on 1/4-scale
 * images when the radius is large, then a fine search at full resolution.
 */
static double locate_anchor(PIX *img, PIX *tmpl, const Layout *L, int *dx, int *dy) {
    int cx = L->ax, cy = L->ay, r = L->search;
    if (r > 12 && L->aw >= 16 && L->ah >= 16) {
        PIX *img4 = pixScaleAreaMap2(img), *t4 = pixScaleAreaMap2(tmpl);
        PIX *img4b = pixScaleAreaMap2(img4), *t4b = pixScaleAreaMap2(t4);
        int bx, by;
        ncc_search(img4b, t4b, (cx - r) / 4, (cy - r) / 4, (cx + r) / 4, (cy + r) / 4,
                   1, &bx, &by);
        cx = bx * 4; cy = by * 4; r = 6;
        pixDestroy(&img4); pixDestroy(&t4); pixDestroy(&img4b); pixDestroy(&t4b);
    }
    int bx, by;
    double score = ncc_search(img, tmpl, cx - r, cy - r, cx + r, cy + r, 1, &bx, &by);
    *dx = bx - L->ax;
    *dy = by - L->ay;
    return score;
}

/*
 * Crop a probe region and prepare it for Tesseract: invert light-on-dark
 * text, upscale small text, and pad with a white border.
 */
static PIX *prepare_crop(PIX *gray, int x, int y, int w, int h) {
    BOX *box = boxCreate(x, y, w, h);
    PIX *crop = pixClipRectangle(gray, box, NULL);
    boxDestroy(&box);
    if (!crop) return NULL;

    l_uint32 *d = pixGetData(crop);
    int wpl = pixGetWpl(crop), cw = pixGetWidth(crop), ch = pixGetHeight(crop);
    double sum = 0;
    for (int yy = 0; yy < ch; yy++)
        for (int xx = 0; xx < cw; xx++) sum += GET_DATA_BYTE(d + yy * wpl, xx);
    if (sum / ((double)cw * ch) < 128) pixInvert(crop, crop);

    /* Tesseract is happiest with text ~30-50px tall. */
    float scale = 64.0f / (float)ch;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 5.0f) scale = 5.0f;
    PIX *scaled = scale > 1.01f ? pixScaleGrayLI(crop, scale, scale) : pixClone(crop);
    pixDestroy(&crop);

    PIX *padded = pixAddBorder(scaled, 12, 255);
    pixDestroy(&scaled);
    return padded;
}

/*
 * Pull the first number out of OCR text, e.g. "T: 37,25 C" -> "37.25".
 * Returns 1 if a number was found.
 */
static int extract_number(const char *s, char *out, size_t outsz) {
    const char *p = s;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!*p) return 0;
    const char *start = p;
    if (start > s && (start[-1] == '-' || start[-1] == '+')) start--;

    size_t n = 0;
    int seen_dot = 0;
    for (const char *q = start; *q && n + 1 < outsz; q++) {
        char c = *q;
        if (c == ',') c = '.';
        if (isdigit((unsigned char)c) || ((c == '-' || c == '+') && q == start)) {
            out[n++] = c;
        } else if (c == '.' && !seen_dot && isdigit((unsigned char)q[1])) {
            out[n++] = c; seen_dot = 1;
        } else if (c == ' ' && isdigit((unsigned char)q[1]) && n > 0 && out[n-1] == '.') {
            continue; /* "37. 25" -> "37.25" */
        } else {
            break;
        }
    }
    out[n] = '\0';
    if (out[0] == '+') memmove(out, out + 1, n);
    return n > 0;
}

/* ------------------------------------------------------------------ */
/* Main processing                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char raw[512];
    char value[128];
    int conf;
} ProbeResult;

static void process_image(const char *path, const Layout *L, PIX *anchor_tmpl,
                          TessBaseAPI *tess, const Options *opt, FILE *csv,
                          int image_index) {
    PIX *gray = load_gray(path);
    if (!gray) {
        printf("{\"image\":"); json_str(stdout, path);
        printf(",\"ok\":false,\"error\":\"could not read image\"}\n");
        fflush(stdout);
        if (csv) { csv_field(csv, path); fputs(",error\n", csv); }
        return;
    }

    int dx = 0, dy = 0;
    double score = 1.0;
    if (anchor_tmpl) score = locate_anchor(gray, anchor_tmpl, L, &dx, &dy);

    ProbeResult res[MAX_PROBES];
    for (int i = 0; i < L->nprobes; i++) {
        const Probe *p = &L->probes[i];
        ProbeResult *r = &res[i];
        r->raw[0] = r->value[0] = '\0';
        r->conf = 0;

        PIX *prep = prepare_crop(gray, p->x + dx, p->y + dy, p->w, p->h);
        if (!prep) continue;

        if (opt->debug_dir) {
            char dbg[MAX_LINE];
            snprintf(dbg, sizeof dbg, "%s/img%04d_probe%02d.png", opt->debug_dir,
                     image_index, i + 1);
            FILE *df = fopen_utf8(dbg, "wb");
            if (df) { pixWriteStream(df, prep, IFF_PNG); fclose(df); }
        }

        TessBaseAPISetVariable(tess, "tessedit_char_whitelist",
                               p->mode == MODE_NUM ? "0123456789.,-+" : "");
        TessBaseAPISetImage2(tess, prep);
        TessBaseAPISetSourceResolution(tess, 300);
        char *text = TessBaseAPIGetUTF8Text(tess);
        r->conf = TessBaseAPIMeanTextConf(tess);
        if (text) {
            snprintf(r->raw, sizeof r->raw, "%s", trim(text));
            TessDeleteText(text);
        }
        if (p->mode == MODE_NUM) {
            if (!extract_number(r->raw, r->value, sizeof r->value)) r->conf = 0;
        } else {
            snprintf(r->value, sizeof r->value, "%s", r->raw);
        }
        pixDestroy(&prep);
    }
    pixDestroy(&gray);

    printf("{\"image\":"); json_str(stdout, path);
    printf(",\"ok\":true,\"dx\":%d,\"dy\":%d,\"anchor_score\":%.3f,\"probes\":[",
           dx, dy, score);
    for (int i = 0; i < L->nprobes; i++) {
        if (i) putchar(',');
        printf("{\"label\":"); json_str(stdout, L->probes[i].label);
        printf(",\"value\":"); json_str(stdout, res[i].value);
        printf(",\"raw\":"); json_str(stdout, res[i].raw);
        printf(",\"conf\":%d}", res[i].conf);
    }
    printf("]}\n");
    fflush(stdout);

    if (csv) {
        csv_field(csv, path);
        fprintf(csv, ",%d,%d", dx, dy);
        for (int i = 0; i < L->nprobes; i++) {
            fputc(',', csv);
            csv_field(csv, res[i].value);
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
    char **wargv = utf8_argv(&argc);
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

    Layout *L = calloc(1, sizeof *L);
    load_layout(argv[i++], L);
    if (L->nprobes == 0) die("layout defines no probes", NULL);

    PIX *anchor_tmpl = NULL;
    if (L->has_anchor) {
        PIX *ref = load_gray(L->ref_path);
        if (!ref) die("cannot read reference image", L->ref_path);
        BOX *b = boxCreate(L->ax, L->ay, L->aw, L->ah);
        anchor_tmpl = pixClipRectangle(ref, b, NULL);
        boxDestroy(&b);
        pixDestroy(&ref);
        if (!anchor_tmpl) die("anchor lies outside the reference image", NULL);
    }

    setMsgSeverity(L_SEVERITY_NONE); /* keep leptonica quiet on stderr */
    TessBaseAPI *tess = TessBaseAPICreate();
    if (opt.tessdata) {
        /* Load the model ourselves so non-ASCII install paths work everywhere. */
        char model[MAX_LINE];
        long size = 0;
        snprintf(model, sizeof model, "%s/%s.traineddata", opt.tessdata, opt.lang);
        char *data = slurp(model, &size);
        if (!data) die("cannot read tesseract model", model);
        int rc = TessBaseAPIInit5(tess, data, (int)size, opt.lang, OEM_DEFAULT,
                                  NULL, 0, NULL, NULL, 0, 0);
        free(data);
        if (rc != 0) die("could not initialise tesseract from", model);
    } else if (TessBaseAPIInit3(tess, NULL, opt.lang) != 0) {
        die("could not initialise tesseract for language", opt.lang);
    }
    TessBaseAPISetPageSegMode(tess, PSM_SINGLE_LINE);
    TessBaseAPISetVariable(tess, "debug_file", NULL_DEVICE);

    FILE *csv = NULL;
    if (opt.csv_path) {
        csv = fopen_utf8(opt.csv_path, "w");
        if (!csv) die("cannot write csv", opt.csv_path);
        fputs("image,dx,dy", csv);
        for (int k = 0; k < L->nprobes; k++) {
            fputc(',', csv);
            csv_field(csv, L->probes[k].label);
        }
        fputc('\n', csv);
    }

    int n = 0;
    if (strcmp(argv[i], "-") == 0) {
        char buf[MAX_LINE];
        while (fgets(buf, sizeof buf, stdin)) {
            char *p = trim(buf);
            if (*p) process_image(p, L, anchor_tmpl, tess, &opt, csv, n++);
        }
    } else {
        for (; i < argc; i++) process_image(argv[i], L, anchor_tmpl, tess, &opt, csv, n++);
    }

    if (csv) fclose(csv);
    pixDestroy(&anchor_tmpl);
    TessBaseAPIEnd(tess);
    TessBaseAPIDelete(tess);
    free(L);
    return 0;
}
