/*
 * probeocr-gui: native desktop frontend (Clay layout + raylib rendering).
 *
 *   ui.c      Clay setup, renderer, fonts, widgets (buttons, checkboxes, text fields)
 *   tree.c    scanning the image folder tree and the sidebar
 *   canvas.c  screenshot view: zoom/pan, drawing and editing boxes
 *   batch.c   background OCR workers, results, CSV export
 *   main.c    window, panels, layouts, keyboard shortcuts, self-test
 */
#ifndef PROBEOCR_GUI_APP_H
#define PROBEOCR_GUI_APP_H

#include <stdbool.h>
#include "vendor/clay.h"
#include "raylib.h"
#include "../core.h"

/* ---------------- look ---------------- */
#define COL(r, g, b) ((Clay_Color){ r, g, b, 255 })
#define C_BG       COL(20, 23, 28)
#define C_PANEL    COL(28, 32, 39)
#define C_LINE     COL(45, 51, 61)
#define C_TEXT     COL(228, 231, 236)
#define C_MUTED    COL(139, 147, 163)
#define C_ACCENT   COL(107, 155, 255)
#define C_ACCENT_S COL(35, 48, 74)
#define C_ANCHOR   COL(240, 165, 58)
#define C_WARN_BG  COL(58, 47, 20)
#define C_WARN     COL(243, 201, 105)
#define C_EDITED   COL(24, 50, 37)
#define C_STAGE    COL(15, 18, 22)
#define C_HOVER    COL(36, 41, 50)
#define C_GOOD     COL(34, 197, 94)

enum { FONT_UI = 0, FONT_MONO = 1 };
#define FS_SMALL 12
#define FS_BODY  14
#define FS_TITLE 15

#define LOW_CONF 60
#define SEL_NONE   (-1)
#define SEL_ANCHOR (-2)
#define MAX_IMAGES 5000

/* ---------------- model ---------------- */
typedef struct Result {
    int ok;
    char error[96];
    int dx, dy;
    double score;
    int n;
    PoProbeResult *p;
    bool *edited;
} Result;

typedef struct {
    char *path;
    char *name;
    bool checked;
    Result *res;
} Img;

typedef struct {
    char name[256];
    char *path;
    int depth;
    bool is_dir, open;
    int img;                  /* files: index into A.imgs */
    int img_begin, img_end;   /* dirs: descendant images are contiguous */
    int sub_end;              /* index of the first node after this subtree */
    int nchecked;             /* dirs: cached count of checked descendants */
} Node;

typedef struct {
    char label[PO_LABEL_LEN];
    int x, y, w, h;
    PoProbeMode mode;
} Box;

typedef struct {
    bool on;
    int x, y, w, h, search;
} Anchor;

typedef struct {
    /* folder tree */
    char root[PO_MAX_PATH];
    Img *imgs; int nimgs;
    Node *nodes; int nnodes;
    bool truncated;
    int cur, ref;

    /* layout being edited */
    Box probes[PO_MAX_PROBES]; int nprobes;
    Anchor anchor;
    int sel;                  /* probe index, SEL_NONE or SEL_ANCHOR */
    bool tool_anchor;         /* next drag places the anchor */
    char layout_name[128];

    /* results table */
    char cols[PO_MAX_PROBES][PO_LABEL_LEN]; int ncols;
    bool with_conf;

    /* feedback */
    char status[512];
    char toast[512]; double toast_until;
    bool menu_layouts;        /* saved-layouts popup open */

    /* paths */
    char app_dir[PO_MAX_PATH];
    char layouts_dir[PO_MAX_PATH];
    char tessdata[PO_MAX_PATH]; bool has_tessdata;
} App;

extern App A;

/* ---------------- ui.c ---------------- */
typedef enum { F_NONE, F_ROOT, F_LAYOUT, F_LABEL, F_SEARCH, F_CELL } FieldKind;
typedef struct { FieldKind kind; int a, b; } FieldRef;

typedef enum { CUSTOM_CANVAS = 1, CUSTOM_CHECK, CUSTOM_CARET, CUSTOM_PROGRESS } CustomKind;
typedef struct { CustomKind kind; int state; float value; } CustomTag;

extern Font g_fonts[2];
void ui_init(int width, int height);
void ui_shutdown(void);
void ui_begin_frame(void);              /* input + Clay_BeginLayout */
void ui_end_layout(void);               /* Clay_EndLayout (then handle canvas input) */
void ui_render(void);                   /* draw the frame */
Clay_String fstr(const char *fmt, ...); /* per-frame formatted string */
Clay_String cstr(const char *s);        /* per-frame copy of s */
bool ui_take_click(Clay_ElementId id);  /* left click on id, not yet consumed */
bool ui_click_pending(void);            /* an unconsumed left click this frame */
void ui_consume_click(void);
bool ui_button(Clay_ElementId id, const char *label, bool on, bool primary, bool enabled);
bool ui_checkbox(Clay_ElementId id, int state);  /* 0 off, 1 on, 2 partial */
void ui_text_field(Clay_ElementId id, FieldRef ref, const char *value,
                   const char *placeholder, float width, bool mono);
bool ui_field_focused(void);
bool ui_field_is(FieldRef ref);
void ui_field_blur(void);
void ui_focus(Clay_ElementId id, FieldRef ref, const char *value);
void ui_label(const char *s, int size, Clay_Color color, int font);
void ui_label_fit(const char *s, int size, Clay_Color color, int font, float maxw);
const char *ui_fit(const char *s, int size, int font, float maxw);
float ui_width_of(Clay_ElementId id, float fallback);   /* last frame's width */
Vector2 ui_measure(const char *s, int size, int font);
CustomTag *ui_tag(CustomKind kind, int state, float value);
/* implemented by main.c: apply a text field edit (final = Enter/blur) */
void app_field_commit(FieldRef ref, const char *text, bool final);

/* ---------------- tree.c ---------------- */
void tree_open(const char *root);
void tree_free(void);
void tree_recount(void);
int tree_selected_count(void);
void tree_set_all(bool on);
void tree_panel(void);
void tree_reveal(int img);
void tree_set_check_range(int begin, int end, bool on);

/* ---------------- canvas.c ---------------- */
void canvas_show(int img);              /* load texture, keep zoom if same size */
void canvas_unload(void);
void canvas_input(void);                /* call after layout, with canvas rect known */
void canvas_draw(Rectangle r);          /* called by the renderer */
void canvas_fit(void);
void canvas_zoom_by(float f);
void canvas_zoom_to(float z);
float canvas_scale(void);
void canvas_offset(int img, int *dx, int *dy);
Color probe_color(int i);

/* ---------------- batch.c ---------------- */
void batch_start(const int *img_idx, int n);
void batch_poll(void);                  /* attach finished results, once per frame */
void batch_cancel(void);
bool batch_running(void);
float batch_progress(int *done, int *total);
void results_clear(void);
bool result_needs_review(const Result *r);
int export_csv(char *out_path, size_t outsz);
void invalidate(void);                  /* geometry changed: results are stale */

/* ---------------- canvas helpers used by the self-test ---------------- */
Vector2 canvas_img_to_screen(float x, float y);
Vector2 canvas_screen_to_img(Vector2 p);
void canvas_pan(float *x, float *y);

/* ---------------- selftest.c ---------------- */
void selftest_init(const char *dir, const char *png);
int selftest_frame(void);               /* -1 while running, else exit code */
void ui_debug_type(const char *s);      /* insert text into the focused field */
int ui_clay_errors(void);

/* ---------------- main.c ---------------- */
int app_layout_index(const char *name);
void app_select_image(int img);
void app_set_ref(int img);
void app_focus_label(int i);
void toast(const char *fmt, ...);
void app_set_status(const char *fmt, ...);

#endif
