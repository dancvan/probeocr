/*
 * probeocr core: layout files, screenshot alignment and probe OCR.
 * Shared by the CLI (probeocr.c) and the native GUI (gui/).
 *
 * All paths are UTF-8 on every platform.
 */
#ifndef PROBEOCR_CORE_H
#define PROBEOCR_CORE_H

#include <stddef.h>
#include <stdio.h>

#define PO_MAX_PROBES 256
#define PO_MAX_PATH   4096
#define PO_LABEL_LEN  256

typedef enum { PO_MODE_NUM, PO_MODE_TEXT } PoProbeMode;

typedef struct {
    int x, y, w, h;
    PoProbeMode mode;
    char label[PO_LABEL_LEN];
} PoProbe;

/*
 * Layout file (plain text, one directive per line, '#' starts a comment):
 *
 *   root   <folder the screenshots live in>     (optional, used by the GUI)
 *   ref    <path to reference image>            (required if an anchor is used)
 *   anchor <x> <y> <w> <h> <search_px>
 *   probe  <x> <y> <w> <h> <num|text> <label ... rest of line>
 */
typedef struct {
    char root[PO_MAX_PATH];
    char ref_path[PO_MAX_PATH];
    int has_anchor;
    int ax, ay, aw, ah, search;
    int nprobes;
    PoProbe probes[PO_MAX_PROBES];
} PoLayout;

typedef struct {
    char raw[160];     /* text exactly as Tesseract read it */
    char value[96];    /* the extracted number (num mode) or the text */
    int conf;          /* 0-100; 0 when no number could be extracted */
} PoProbeResult;

typedef struct {
    int ok;
    char error[96];
    int dx, dy;                /* shift found by the anchor search */
    double anchor_score;       /* 1.0 when there is no anchor */
    int nprobes;
    PoProbeResult *probes;     /* caller-provided array of layout->nprobes */
} PoImageResult;

/* Layouts. Return 0 on success, otherwise write a message to err. */
int po_layout_load(const char *path, PoLayout *L, char *err, size_t errsz);
int po_layout_save(const char *path, const PoLayout *L);

/*
 * The OCR engine. Each engine owns a Tesseract instance, so use one engine
 * per thread. tessdata may be NULL to use Tesseract's built-in search path.
 */
typedef struct PoEngine PoEngine;
PoEngine *po_engine_create(const char *tessdata, const char *lang, char *err, size_t errsz);
void po_engine_destroy(PoEngine *e);
/* Prepares the anchor template from the layout's reference image. */
int po_engine_set_layout(PoEngine *e, const PoLayout *L, char *err, size_t errsz);
/* out->probes must hold L->nprobes entries. debug_dir may be NULL. */
void po_engine_process(PoEngine *e, const char *path, PoImageResult *out,
                       const char *debug_dir, int image_index);

/* Decode any image the OCR can read into 8-bit RGBA (caller frees *rgba). */
int po_load_rgba(const char *path, unsigned char **rgba, int *w, int *h);

/* Platform helpers. */
FILE *po_fopen(const char *path, const char *mode);
char *po_slurp(const char *path, long *size);
#ifdef _WIN32
char **po_utf8_argv(int *argc);
#endif

/* Make a directory (ignored if it exists). */
void po_mkdir(const char *path);
/* Does path exist (file or directory)? */
int po_exists(const char *path);
/* Directory holding the running executable, with a trailing separator. */
void po_exe_dir(char *out, size_t outsz);

/*
 * List a directory: calls fn(name, is_dir, user) for each entry except
 * "." and "..". Returns 0 on success, -1 if the directory can't be opened.
 */
int po_list_dir(const char *dir, void (*fn)(const char *name, int is_dir, void *user), void *user);

#endif
