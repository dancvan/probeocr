/*
 * The screenshot view: zoom/pan, drawing/moving/resizing probe boxes and the
 * anchor. Boxes are stored in reference-image pixels; on other screenshots
 * they are drawn shifted by the offset the anchor search found.
 */
#include "app.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MIN_ZOOM 0.05f
#define MAX_ZOOM 32.0f
#define MIN_BOX 6

static Texture2D g_tex;
static int g_tex_img = -1;
static int g_filter = -1;
static float g_scale = 1, g_panx, g_pany;
static bool g_fit = true, g_need_fit;
static Rectangle g_view;            /* canvas rect from the last layout */

typedef enum { D_NONE, D_NEW, D_MOVE, D_RESIZE, D_PAN } DragMode;
typedef struct {
    DragMode mode;
    int target;                     /* probe index or SEL_ANCHOR */
    Vector2 start;                  /* image coords (pan: screen coords) */
    int ox, oy, ow, oh;             /* original box */
    float px, py;                   /* original pan */
    Vector2 cur;
} Drag;
static Drag g_drag;

static const Color PALETTE[] = {
    { 37, 99, 235, 255 }, { 22, 163, 74, 255 }, { 219, 39, 119, 255 }, { 124, 58, 237, 255 },
    { 8, 145, 178, 255 }, { 101, 163, 13, 255 }, { 220, 38, 38, 255 }, { 13, 148, 136, 255 } };
static const Color ANCHOR_COL = { 217, 119, 6, 255 };

Color probe_color(int i) { return PALETTE[i % (int)(sizeof PALETTE / sizeof PALETTE[0])]; }
float canvas_scale(void) { return g_scale; }

void canvas_offset(int img, int *dx, int *dy) {
    *dx = *dy = 0;
    if (img >= 0 && img < A.nimgs && A.imgs[img].res && A.imgs[img].res->ok) {
        *dx = A.imgs[img].res->dx;
        *dy = A.imgs[img].res->dy;
    }
}

/* ---------------- view transform ---------------- */

static void clamp_pan(void) {
    if (!g_tex.id) return;
    float m = 60, w = g_tex.width * g_scale, h = g_tex.height * g_scale;
    g_panx = fminf(g_view.width - m, fmaxf(m - w, g_panx));
    g_pany = fminf(g_view.height - m, fmaxf(m - h, g_pany));
}

void canvas_fit(void) {
    if (!g_tex.id) return;
    if (g_view.width < 10) { g_need_fit = true; return; }
    g_scale = fminf(2.0f, fminf((g_view.width - 32) / g_tex.width, (g_view.height - 32) / g_tex.height));
    if (g_scale <= 0) g_scale = 0.1f;
    g_panx = (g_view.width - g_tex.width * g_scale) / 2;
    g_pany = (g_view.height - g_tex.height * g_scale) / 2;
    g_fit = true;
    g_need_fit = false;
}

static void zoom_at(float z, float cx, float cy) {
    if (!g_tex.id) return;
    z = fminf(MAX_ZOOM, fmaxf(MIN_ZOOM, z));
    float ix = (cx - g_panx) / g_scale, iy = (cy - g_pany) / g_scale;   /* keep this point fixed */
    g_scale = z;
    g_panx = cx - ix * z;
    g_pany = cy - iy * z;
    g_fit = false;
    clamp_pan();
}

void canvas_zoom_to(float z) { zoom_at(z, g_view.width / 2, g_view.height / 2); }
void canvas_zoom_by(float f) { canvas_zoom_to(g_scale * f); }

static Vector2 to_img(Vector2 screen) {
    return (Vector2){ (screen.x - g_view.x - g_panx) / g_scale, (screen.y - g_view.y - g_pany) / g_scale };
}

static Rectangle to_screen(float x, float y, float w, float h) {
    return (Rectangle){ g_view.x + g_panx + x * g_scale, g_view.y + g_pany + y * g_scale, w * g_scale, h * g_scale };
}

Vector2 canvas_img_to_screen(float x, float y) {
    Rectangle r = to_screen(x, y, 0, 0);
    return (Vector2){ r.x, r.y };
}
Vector2 canvas_screen_to_img(Vector2 p) { return to_img(p); }
void canvas_pan(float *x, float *y) { *x = g_panx; *y = g_pany; }

/* ---------------- image ---------------- */

void canvas_unload(void) {
    if (g_tex.id) UnloadTexture(g_tex);
    g_tex = (Texture2D){ 0 };
    g_tex_img = -1;
}

void canvas_show(int img) {
    if (img < 0 || img >= A.nimgs) { canvas_unload(); return; }
    if (img == g_tex_img && g_tex.id) return;
    int ow = g_tex.width, oh = g_tex.height;
    bool had = g_tex.id != 0;
    unsigned char *rgba = NULL;
    int w, h;
    canvas_unload();
    if (po_load_rgba(A.imgs[img].path, &rgba, &w, &h) != 0) {
        toast("Could not read %s", A.imgs[img].name);
        return;
    }
    Image im = { .data = rgba, .width = w, .height = h, .mipmaps = 1,
                 .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    g_tex = LoadTextureFromImage(im);
    free(rgba);
    if (!g_tex.id) { toast("Image too large to display: %dx%d", w, h); return; }
    GenTextureMipmaps(&g_tex);
    g_filter = -1;
    g_tex_img = img;
    /* Keep zoom/pan across same-sized screenshots so the same probe stays in view. */
    if (g_fit || !had || ow != w || oh != h) canvas_fit();
}

/* ---------------- interaction ---------------- */

static bool box_of(int target, int *x, int *y, int *w, int *h) {
    if (target == SEL_ANCHOR) {
        if (!A.anchor.on) return false;
        *x = A.anchor.x; *y = A.anchor.y; *w = A.anchor.w; *h = A.anchor.h;
    } else {
        if (target < 0 || target >= A.nprobes) return false;
        Box *b = &A.probes[target];
        *x = b->x; *y = b->y; *w = b->w; *h = b->h;
    }
    return true;
}

static void set_box(int target, int x, int y, int w, int h) {
    if (target == SEL_ANCHOR) { A.anchor.x = x; A.anchor.y = y; A.anchor.w = w; A.anchor.h = h; }
    else { Box *b = &A.probes[target]; b->x = x; b->y = y; b->w = w; b->h = h; }
}

/* Topmost box under the mouse; *resize set when on the bottom-right handle. */
static int hit(Vector2 m, bool *resize) {
    int dx, dy;
    canvas_offset(A.cur, &dx, &dy);
    float tol = 7;
    for (int t = A.nprobes - 1; t >= -2; t--) {
        if (t == -1) continue;
        int x, y, w, h;
        if (!box_of(t, &x, &y, &w, &h)) continue;
        Rectangle r = to_screen((float)(x + dx), (float)(y + dy), (float)w, (float)h);
        if (fabsf(m.x - (r.x + r.width)) < tol && fabsf(m.y - (r.y + r.height)) < tol) { *resize = true; return t; }
        if (CheckCollisionPointRec(m, r)) { *resize = false; return t; }
    }
    return SEL_NONE;
}

static int g_cursor = -1;
static void set_cursor(int c) { if (c != g_cursor) { SetMouseCursor(c); g_cursor = c; } }

void canvas_input(void) {
    Clay_ElementData d = Clay_GetElementData(CLAY_ID("Canvas"));
    if (!d.found) return;
    Rectangle v = { d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height };
    bool resized = v.width != g_view.width || v.height != g_view.height;
    g_view = v;
    if (g_tex.id && (g_need_fit || (resized && g_fit))) canvas_fit();
    else if (resized) clamp_pan();

    Vector2 m = GetMousePosition();
    bool over = Clay_PointerOver(CLAY_ID("Canvas"));
    bool space = IsKeyDown(KEY_SPACE) && !ui_field_focused();
    bool cmd = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
               IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (!g_tex.id) { if (over) set_cursor(MOUSE_CURSOR_DEFAULT); return; }

    /* wheel: Ctrl/Cmd zooms at the cursor, otherwise pans */
    Vector2 wheel = GetMouseWheelMoveV();
    if (over && (wheel.x != 0 || wheel.y != 0)) {
        if (cmd) zoom_at(g_scale * expf(wheel.y * 0.12f), m.x - v.x, m.y - v.y);
        else { g_panx += wheel.x * 30; g_pany += wheel.y * 30; g_fit = false; clamp_pan(); }
    }

    /* start a drag */
    bool left = over && ui_click_pending();
    bool middle = over && IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE);
    if (g_drag.mode == D_NONE && (left || middle)) {
        if (left) ui_consume_click();
        if (middle || space) {
            g_drag = (Drag){ .mode = D_PAN, .start = m, .px = g_panx, .py = g_pany };
        } else {
            bool resize;
            int t = hit(m, &resize);
            Vector2 p = to_img(m);
            if (t != SEL_NONE) {
                A.sel = t;
                int x, y, w, h;
                box_of(t, &x, &y, &w, &h);
                g_drag = (Drag){ .mode = resize ? D_RESIZE : D_MOVE, .target = t, .start = p,
                                           .ox = x, .oy = y, .ow = w, .oh = h };
            } else {
                g_drag = (Drag){ .mode = D_NEW, .start = p, .cur = p };
            }
        }
    }

    /* continue / finish */
    bool down = IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE);
    if (g_drag.mode == D_PAN) {
        g_panx = g_drag.px + m.x - g_drag.start.x;
        g_pany = g_drag.py + m.y - g_drag.start.y;
        g_fit = false;
        clamp_pan();
    } else if (g_drag.mode != D_NONE) {
        Vector2 p = to_img(m);
        float ddx = p.x - g_drag.start.x, ddy = p.y - g_drag.start.y;
        g_drag.cur = p;
        if (g_drag.mode == D_MOVE)
            set_box(g_drag.target, (int)roundf(g_drag.ox + ddx), (int)roundf(g_drag.oy + ddy), g_drag.ow, g_drag.oh);
        else if (g_drag.mode == D_RESIZE)
            set_box(g_drag.target, g_drag.ox, g_drag.oy,
                    (int)fmaxf(MIN_BOX, roundf(g_drag.ow + ddx)), (int)fmaxf(MIN_BOX, roundf(g_drag.oh + ddy)));
    }
    if (!down && g_drag.mode != D_NONE) {
        DragMode mode = g_drag.mode;
        g_drag.mode = D_NONE;
        if (mode == D_MOVE || mode == D_RESIZE) {
            invalidate();
        } else if (mode == D_NEW) {
            Vector2 a = g_drag.start, b = g_drag.cur;
            int x = (int)roundf(fminf(a.x, b.x)), y = (int)roundf(fminf(a.y, b.y));
            int w = (int)roundf(fabsf(b.x - a.x)), h = (int)roundf(fabsf(b.y - a.y));
            if (w < MIN_BOX || h < MIN_BOX) {
                A.sel = SEL_NONE;
            } else {
                int dx, dy;
                canvas_offset(A.cur, &dx, &dy);   /* store in reference coordinates */
                x -= dx; y -= dy;
                if (A.tool_anchor) {
                    int search = A.anchor.on ? A.anchor.search : 30;
                    A.anchor = (Anchor){ true, x, y, w, h, search };
                    A.sel = SEL_ANCHOR;
                    A.tool_anchor = false;
                } else if (A.nprobes < PO_MAX_PROBES) {
                    Box *bx = &A.probes[A.nprobes];
                    memset(bx, 0, sizeof *bx);
                    bx->x = x; bx->y = y; bx->w = w; bx->h = h; bx->mode = PO_MODE_NUM;
                    A.sel = A.nprobes++;
                    app_focus_label(A.sel);   /* jump straight to typing the column header */
                }
                invalidate();
            }
        }
    }

    /* cursor */
    if (g_drag.mode == D_PAN) set_cursor(MOUSE_CURSOR_RESIZE_ALL);
    else if (over) {
        bool resize = false;
        int t = space ? SEL_NONE : hit(m, &resize);
        set_cursor(space ? MOUSE_CURSOR_RESIZE_ALL : t == SEL_NONE ? MOUSE_CURSOR_CROSSHAIR
                   : resize ? MOUSE_CURSOR_RESIZE_NWSE : MOUSE_CURSOR_POINTING_HAND);
    } else if (g_drag.mode == D_NONE) set_cursor(MOUSE_CURSOR_DEFAULT);
}

/* ---------------- drawing ---------------- */

static void dashed_rect(Rectangle r, float thick, Color c) {
    const float dash = 6, gap = 4;
    for (float x = r.x; x < r.x + r.width; x += dash + gap) {
        float len = fminf(dash, r.x + r.width - x);
        DrawRectangleRec((Rectangle){ x, r.y, len, thick }, c);
        DrawRectangleRec((Rectangle){ x, r.y + r.height - thick, len, thick }, c);
    }
    for (float y = r.y; y < r.y + r.height; y += dash + gap) {
        float len = fminf(dash, r.y + r.height - y);
        DrawRectangleRec((Rectangle){ r.x, y, thick, len }, c);
        DrawRectangleRec((Rectangle){ r.x + r.width - thick, y, thick, len }, c);
    }
}

static void draw_box(Rectangle r, Color c, bool dashed, bool selected, const char *tag) {
    DrawRectangleRec(r, Fade(c, 0.14f));
    float t = selected ? 2.5f : 1.5f;
    if (dashed) dashed_rect(r, t, c);
    else DrawRectangleLinesEx(r, t, c);
    if (selected) DrawRectangleRec((Rectangle){ r.x + r.width - 4, r.y + r.height - 4, 8, 8 }, c);
    if (tag && tag[0]) {
        Vector2 ts = MeasureTextEx(g_fonts[FONT_UI], tag, 12, 0);
        float th = 17, ty = r.y - th >= g_view.y ? r.y - th : r.y + r.height;
        DrawRectangleRec((Rectangle){ r.x, ty, ts.x + 8, th }, c);
        DrawTextEx(g_fonts[FONT_UI], tag, (Vector2){ roundf(r.x + 4), roundf(ty + 2.5f) }, 12, 0, WHITE);
    }
}

void canvas_draw(Rectangle r) {
    g_view = r;
    BeginScissorMode((int)r.x, (int)r.y, (int)r.width, (int)r.height);
    if (!g_tex.id) {
        const char *msg = A.nimgs ? "Loading..." : "Open a folder of screenshots, or drop one onto this window.";
        Vector2 s = MeasureTextEx(g_fonts[FONT_UI], msg, FS_BODY, 0);
        DrawTextEx(g_fonts[FONT_UI], msg, (Vector2){ roundf(r.x + (r.width - s.x) / 2), roundf(r.y + r.height / 2 - 8) },
                   FS_BODY, 0, (Color){ 139, 147, 163, 255 });
        EndScissorMode();
        return;
    }
    int filter = g_scale >= 2 ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_TRILINEAR;
    if (filter != g_filter) { SetTextureFilter(g_tex, filter); g_filter = filter; }
    Rectangle dst = to_screen(0, 0, (float)g_tex.width, (float)g_tex.height);
    DrawTexturePro(g_tex, (Rectangle){ 0, 0, (float)g_tex.width, (float)g_tex.height }, dst, (Vector2){ 0 }, 0, WHITE);
    DrawRectangleLinesEx((Rectangle){ dst.x - 1, dst.y - 1, dst.width + 2, dst.height + 2 }, 1, (Color){ 0, 0, 0, 120 });

    int dx, dy;
    canvas_offset(A.cur, &dx, &dy);
    const Result *res = A.cur >= 0 ? A.imgs[A.cur].res : NULL;
    if (A.anchor.on) {
        Anchor *a = &A.anchor;
        Rectangle sr = to_screen((float)(a->x - a->search + dx), (float)(a->y - a->search + dy),
                                 (float)(a->w + 2 * a->search), (float)(a->h + 2 * a->search));
        DrawRectangleLinesEx(sr, 1, Fade(ANCHOR_COL, 0.4f));
        draw_box(to_screen((float)(a->x + dx), (float)(a->y + dy), (float)a->w, (float)a->h),
                 ANCHOR_COL, true, A.sel == SEL_ANCHOR, "anchor");
    }
    for (int i = 0; i < A.nprobes; i++) {
        Box *b = &A.probes[i];
        char tag[PO_LABEL_LEN + 128];
        const char *name = b->label[0] ? b->label : TextFormat("probe %d", i + 1);
        if (res && res->ok && i < res->n)
            snprintf(tag, sizeof tag, "%s: %s", name, res->p[i].value[0] ? res->p[i].value : "-");
        else
            snprintf(tag, sizeof tag, "%s", name);
        draw_box(to_screen((float)(b->x + dx), (float)(b->y + dy), (float)b->w, (float)b->h),
                 probe_color(i), false, A.sel == i, tag);
    }
    if (g_drag.mode == D_NEW) {
        Vector2 a = g_drag.start, b = g_drag.cur;
        Rectangle pr = to_screen(fminf(a.x, b.x), fminf(a.y, b.y), fabsf(b.x - a.x), fabsf(b.y - a.y));
        draw_box(pr, A.tool_anchor ? ANCHOR_COL : PALETTE[0], true, false, NULL);
    }
    EndScissorMode();
}
