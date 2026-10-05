/*
 * probeocr core — see core.h.
 */
#include "core.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <allheaders.h>
#include <tesseract/capi.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#define NULL_DEVICE "NUL"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define NULL_DEVICE "/dev/null"
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

struct PoEngine {
    TessBaseAPI *tess;
    const PoLayout *layout;
    PIX *anchor_tmpl;
};

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

/*
 * On Windows the C runtime's fopen and argv use the ANSI code page rather
 * than UTF-8, so go through the wide-char APIs.
 */
FILE *po_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    wchar_t wpath[PO_MAX_PATH], wmode[8];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, PO_MAX_PATH)) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 8)) return NULL;
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

#ifdef _WIN32
char **po_utf8_argv(int *argc) {
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

char *po_slurp(const char *path, long *size) {
    FILE *f = po_fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (*size = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)*size);
        if (buf && fread(buf, 1, (size_t)*size, f) != (size_t)*size) { free(buf); buf = NULL; }
    }
    fclose(f);
    return buf;
}

void po_mkdir(const char *path) {
#ifdef _WIN32
    wchar_t w[PO_MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, PO_MAX_PATH)) CreateDirectoryW(w, NULL);
#else
    mkdir(path, 0755);
#endif
}

int po_exists(const char *path) {
#ifdef _WIN32
    wchar_t w[PO_MAX_PATH];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, PO_MAX_PATH)) return 0;
    return GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

void po_exe_dir(char *out, size_t outsz) {
    char path[PO_MAX_PATH] = "";
#ifdef _WIN32
    wchar_t w[PO_MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, w, PO_MAX_PATH);
    if (n > 0 && n < PO_MAX_PATH) WideCharToMultiByte(CP_UTF8, 0, w, -1, path, sizeof path, NULL, NULL);
#elif defined(__APPLE__)
    uint32_t size = sizeof path;
    if (_NSGetExecutablePath(path, &size) != 0) path[0] = '\0';
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    path[n > 0 ? n : 0] = '\0';
#endif
    char *slash = strrchr(path, '/');
#ifdef _WIN32
    char *bs = strrchr(path, '\\');
    if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (slash) slash[1] = '\0';
    else snprintf(path, sizeof path, "./");
    snprintf(out, outsz, "%s", path);
}

int po_list_dir(const char *dir, void (*fn)(const char *, int, void *), void *user) {
#ifdef _WIN32
    wchar_t pattern[PO_MAX_PATH];
    char spec[PO_MAX_PATH];
    snprintf(spec, sizeof spec, "%s\\*", dir);
    if (!MultiByteToWideChar(CP_UTF8, 0, spec, -1, pattern, PO_MAX_PATH)) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL))
            continue;
        int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                     !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (fn) fn(name, is_dir, user);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[PO_MAX_PATH];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        int is_dir = lstat(full, &st) == 0 && S_ISDIR(st.st_mode);   /* don't follow symlinks */
        if (fn) fn(e->d_name, is_dir, user);
    }
    closedir(d);
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Layout files                                                        */
/* ------------------------------------------------------------------ */

int po_layout_load(const char *path, PoLayout *L, char *err, size_t errsz) {
    FILE *f = po_fopen(path, "r");
    if (!f) { snprintf(err, errsz, "cannot open layout: %s", path); return -1; }
    memset(L, 0, sizeof *L);

    char buf[PO_MAX_PATH];
    int lineno = 0;
    while (fgets(buf, sizeof buf, f)) {
        lineno++;
        char *line = trim(buf);
        if (*line == '\0' || *line == '#') continue;

        if (strncmp(line, "root ", 5) == 0) {
            snprintf(L->root, sizeof L->root, "%s", trim(line + 5));
        } else if (strncmp(line, "ref ", 4) == 0) {
            snprintf(L->ref_path, sizeof L->ref_path, "%s", trim(line + 4));
        } else if (strncmp(line, "anchor ", 7) == 0) {
            if (sscanf(line + 7, "%d %d %d %d %d",
                       &L->ax, &L->ay, &L->aw, &L->ah, &L->search) != 5)
                goto bad;
            L->has_anchor = 1;
        } else if (strncmp(line, "probe ", 6) == 0) {
            if (L->nprobes >= PO_MAX_PROBES) {
                snprintf(err, errsz, "too many probes in layout: %s", path);
                fclose(f);
                return -1;
            }
            PoProbe *p = &L->probes[L->nprobes];
            char mode[16];
            int consumed = 0;
            if (sscanf(line + 6, "%d %d %d %d %15s %n",
                       &p->x, &p->y, &p->w, &p->h, mode, &consumed) != 5)
                goto bad;
            p->mode = strcmp(mode, "text") == 0 ? PO_MODE_TEXT : PO_MODE_NUM;
            snprintf(p->label, sizeof p->label, "%s", trim(line + 6 + consumed));
            if (p->label[0] == '\0')
                snprintf(p->label, sizeof p->label, "probe%d", L->nprobes + 1);
            L->nprobes++;
        } else {
            goto bad;
        }
        continue;
    bad:
        snprintf(err, errsz, "%s:%d: cannot parse: %s", path, lineno, line);
        fclose(f);
        return -1;
    }
    fclose(f);
    if (L->has_anchor && L->ref_path[0] == '\0') {
        snprintf(err, errsz, "layout has an anchor but no 'ref' image: %s", path);
        return -1;
    }
    return 0;
}

int po_layout_save(const char *path, const PoLayout *L) {
    FILE *f = po_fopen(path, "w");
    if (!f) return -1;
    fputs("# probeocr layout\n", f);
    if (L->root[0]) fprintf(f, "root %s\n", L->root);
    if (L->ref_path[0]) fprintf(f, "ref %s\n", L->ref_path);
    if (L->has_anchor)
        fprintf(f, "anchor %d %d %d %d %d\n", L->ax, L->ay, L->aw, L->ah, L->search);
    for (int i = 0; i < L->nprobes; i++) {
        const PoProbe *p = &L->probes[i];
        char label[PO_LABEL_LEN];
        /* Labels are the rest of the line, so they can't hold newlines. */
        snprintf(label, sizeof label, "%s", p->label);
        for (char *c = label; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        fprintf(f, "probe %d %d %d %d %s %s\n", p->x, p->y, p->w, p->h,
                p->mode == PO_MODE_TEXT ? "text" : "num", label);
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Image helpers                                                       */
/* ------------------------------------------------------------------ */

static PIX *read_pix(const char *path) {
    FILE *fp = po_fopen(path, "rb");
    if (!fp) return NULL;
    PIX *pix = pixReadStream(fp, 0);
    fclose(fp);
    return pix;
}

/* Load any image as 8-bit grayscale, flattening alpha onto white. */
static PIX *load_gray(const char *path) {
    PIX *raw = read_pix(path);
    if (!raw) return NULL;
    PIX *flat = pixGetSpp(raw) == 4 ? pixRemoveAlpha(raw) : pixClone(raw);
    PIX *gray = pixConvertTo8(flat, 0);
    pixDestroy(&flat);
    pixDestroy(&raw);
    return gray;
}

int po_load_rgba(const char *path, unsigned char **rgba, int *w, int *h) {
    PIX *raw = read_pix(path);
    if (!raw) return -1;
    int has_alpha = pixGetSpp(raw) == 4;
    PIX *pix = pixConvertTo32(raw);
    pixDestroy(&raw);
    if (!pix) return -1;
    *w = pixGetWidth(pix); *h = pixGetHeight(pix);
    unsigned char *out = malloc((size_t)*w * *h * 4);
    if (!out) { pixDestroy(&pix); return -1; }
    l_uint32 *data = pixGetData(pix);
    int wpl = pixGetWpl(pix);
    for (int y = 0; y < *h; y++) {
        l_uint32 *line = data + y * wpl;
        unsigned char *o = out + (size_t)y * *w * 4;
        for (int x = 0; x < *w; x++, o += 4) {
            l_int32 r, g, b, a;
            extractRGBAValues(line[x], &r, &g, &b, &a);
            o[0] = (unsigned char)r; o[1] = (unsigned char)g; o[2] = (unsigned char)b;
            o[3] = has_alpha ? (unsigned char)a : 255;
        }
    }
    pixDestroy(&pix);
    *rgba = out;
    return 0;
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
static double locate_anchor(PIX *img, PIX *tmpl, const PoLayout *L, int *dx, int *dy) {
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
/* Engine                                                              */
/* ------------------------------------------------------------------ */

PoEngine *po_engine_create(const char *tessdata, const char *lang, char *err, size_t errsz) {
    setMsgSeverity(L_SEVERITY_NONE); /* keep leptonica quiet on stderr */
    PoEngine *e = calloc(1, sizeof *e);
    e->tess = TessBaseAPICreate();
    int rc;
    if (tessdata) {
        /* Load the model ourselves so non-ASCII install paths work everywhere. */
        char model[PO_MAX_PATH];
        long size = 0;
        snprintf(model, sizeof model, "%s/%s.traineddata", tessdata, lang);
        char *data = po_slurp(model, &size);
        if (!data) {
            snprintf(err, errsz, "cannot read tesseract model: %s", model);
            po_engine_destroy(e);
            return NULL;
        }
        rc = TessBaseAPIInit5(e->tess, data, (int)size, lang, OEM_DEFAULT,
                              NULL, 0, NULL, NULL, 0, 0);
        free(data);
        if (rc != 0) snprintf(err, errsz, "could not initialise tesseract from: %s", model);
    } else {
        rc = TessBaseAPIInit3(e->tess, NULL, lang);
        if (rc != 0) snprintf(err, errsz, "could not initialise tesseract for language: %s", lang);
    }
    if (rc != 0) { po_engine_destroy(e); return NULL; }
    TessBaseAPISetPageSegMode(e->tess, PSM_SINGLE_LINE);
    TessBaseAPISetVariable(e->tess, "debug_file", NULL_DEVICE);
    return e;
}

void po_engine_destroy(PoEngine *e) {
    if (!e) return;
    pixDestroy(&e->anchor_tmpl);
    if (e->tess) { TessBaseAPIEnd(e->tess); TessBaseAPIDelete(e->tess); }
    free(e);
}

int po_engine_set_layout(PoEngine *e, const PoLayout *L, char *err, size_t errsz) {
    pixDestroy(&e->anchor_tmpl);
    e->layout = L;
    if (!L->has_anchor) return 0;
    PIX *ref = load_gray(L->ref_path);
    if (!ref) { snprintf(err, errsz, "cannot read reference image: %s", L->ref_path); return -1; }
    BOX *b = boxCreate(L->ax, L->ay, L->aw, L->ah);
    e->anchor_tmpl = pixClipRectangle(ref, b, NULL);
    boxDestroy(&b);
    pixDestroy(&ref);
    if (!e->anchor_tmpl) { snprintf(err, errsz, "anchor lies outside the reference image"); return -1; }
    return 0;
}

void po_engine_process(PoEngine *e, const char *path, PoImageResult *out,
                       const char *debug_dir, int image_index) {
    const PoLayout *L = e->layout;
    out->nprobes = L->nprobes;
    out->dx = out->dy = 0;
    out->anchor_score = 1.0;
    out->error[0] = '\0';
    for (int i = 0; i < L->nprobes; i++) {
        out->probes[i].raw[0] = out->probes[i].value[0] = '\0';
        out->probes[i].conf = 0;
    }

    PIX *gray = load_gray(path);
    if (!gray) {
        out->ok = 0;
        snprintf(out->error, sizeof out->error, "could not read image");
        return;
    }
    out->ok = 1;
    if (e->anchor_tmpl)
        out->anchor_score = locate_anchor(gray, e->anchor_tmpl, L, &out->dx, &out->dy);

    for (int i = 0; i < L->nprobes; i++) {
        const PoProbe *p = &L->probes[i];
        PoProbeResult *r = &out->probes[i];

        PIX *prep = prepare_crop(gray, p->x + out->dx, p->y + out->dy, p->w, p->h);
        if (!prep) continue;

        if (debug_dir) {
            char dbg[PO_MAX_PATH];
            snprintf(dbg, sizeof dbg, "%s/img%04d_probe%02d.png", debug_dir, image_index, i + 1);
            FILE *df = po_fopen(dbg, "wb");
            if (df) { pixWriteStream(df, prep, IFF_PNG); fclose(df); }
        }

        TessBaseAPISetVariable(e->tess, "tessedit_char_whitelist",
                               p->mode == PO_MODE_NUM ? "0123456789.,-+" : "");
        TessBaseAPISetImage2(e->tess, prep);
        TessBaseAPISetSourceResolution(e->tess, 300);
        char *text = TessBaseAPIGetUTF8Text(e->tess);
        r->conf = TessBaseAPIMeanTextConf(e->tess);
        if (text) {
            snprintf(r->raw, sizeof r->raw, "%s", trim(text));
            TessDeleteText(text);
        }
        if (p->mode == PO_MODE_NUM) {
            if (!extract_number(r->raw, r->value, sizeof r->value)) r->conf = 0;
        } else {
            snprintf(r->value, sizeof r->value, "%s", r->raw);
        }
        pixDestroy(&prep);
    }
    pixDestroy(&gray);
}
