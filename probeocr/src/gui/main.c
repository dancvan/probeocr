/*
 * probeocr-gui — native desktop frontend for probeocr.
 *
 *   probeocr-gui [--root DIR]
 *   probeocr-gui --selftest SAMPLES_DIR OUT.png   (runs the samples, checks truth.csv)
 *   probeocr-gui --version
 */
#include "app.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define PROBEOCR_VERSION "0.2.0"
#define ROW_H 28
#define SIDE_W 290
#define TREE_W 250

App A;

static char g_root_input[PO_MAX_PATH];
static bool g_open_root;
static char g_layouts[64][128];
static int g_nlayouts;

/* ---------------- small helpers ---------------- */

void app_set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.status, sizeof A.status, fmt, ap);
    va_end(ap);
}

void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof A.toast, fmt, ap);
    va_end(ap);
    A.toast_until = GetTime() + 2.5;
}

static int first_selected(void) {
    for (int i = 0; i < A.nimgs; i++) if (A.imgs[i].checked) return i;
    return -1;
}

void app_select_image(int img) {
    if (img < 0 || img >= A.nimgs) return;
    A.cur = img;
    canvas_show(img);
    tree_reveal(img);
}

void app_set_ref(int img) {
    if (img < 0 || img >= A.nimgs || img == A.ref) return;
    /* keep boxes where they appear on the new reference */
    int dx, dy;
    canvas_offset(img, &dx, &dy);
    for (int i = 0; i < A.nprobes; i++) { A.probes[i].x += dx; A.probes[i].y += dy; }
    if (A.anchor.on) { A.anchor.x += dx; A.anchor.y += dy; }
    A.ref = img;
    invalidate();
}

static void open_root(const char *root) {
    batch_cancel();
    tree_open(root);
    snprintf(g_root_input, sizeof g_root_input, "%s", A.root);
    A.cur = A.ref = -1;
    canvas_unload();
    if (!A.nimgs) { toast("No images under that folder"); return; }
    if (A.truncated) toast("Showing the first %d images only; open a narrower folder", MAX_IMAGES);
    A.ref = 0;
    app_select_image(0);
}

/* ---------------- layouts ---------------- */

static void sanitize_name(char *s) {
    char *o = s;
    for (char *p = s; *p; p++)
        if (isalnum((unsigned char)*p) || *p == ' ' || *p == '_' || *p == '-' || *p == '.') *o++ = *p;
    *o = '\0';
    while (o > s && (o[-1] == ' ' || o[-1] == '.')) *--o = '\0';
}

static void build_layout(PoLayout *L) {
    memset(L, 0, sizeof *L);
    snprintf(L->root, sizeof L->root, "%s", A.root);
    if (A.ref >= 0) snprintf(L->ref_path, sizeof L->ref_path, "%s", A.imgs[A.ref].path);
    L->has_anchor = A.anchor.on;
    L->ax = A.anchor.x; L->ay = A.anchor.y; L->aw = A.anchor.w; L->ah = A.anchor.h; L->search = A.anchor.search;
    L->nprobes = A.nprobes;
    for (int i = 0; i < A.nprobes; i++) {
        PoProbe *p = &L->probes[i];
        Box *b = &A.probes[i];
        p->x = b->x; p->y = b->y; p->w = b->w; p->h = b->h; p->mode = b->mode;
        snprintf(p->label, sizeof p->label, "%s", b->label);
    }
}

static void save_layout(void) {
    char name[128];
    snprintf(name, sizeof name, "%s", A.layout_name);
    sanitize_name(name);
    if (!name[0]) { toast("Give the layout a name first"); return; }
    po_mkdir(A.layouts_dir);
    char path[PO_MAX_PATH];
    snprintf(path, sizeof path, "%s/%s.txt", A.layouts_dir, name);
    PoLayout *L = malloc(sizeof *L);
    build_layout(L);
    int rc = po_layout_save(path, L);
    free(L);
    if (rc == 0) toast("Saved layout \"%s\"", name);
    else toast("Could not write %s", path);
}

static void collect_layout(const char *name, int is_dir, void *user) {
    (void)user;
    size_t n = strlen(name);
    if (is_dir || n < 5 || strcmp(name + n - 4, ".txt") || g_nlayouts >= 64) return;
    snprintf(g_layouts[g_nlayouts], sizeof g_layouts[0], "%.*s", (int)(n - 4), name);
    g_nlayouts++;
}

static int cmp_names(const void *a, const void *b) { return strcmp(a, b); }

static void refresh_layouts(void) {
    g_nlayouts = 0;
    po_list_dir(A.layouts_dir, collect_layout, NULL);
    qsort(g_layouts, (size_t)g_nlayouts, sizeof g_layouts[0], cmp_names);
}

static void load_layout(const char *name) {
    char path[PO_MAX_PATH], err[PO_MAX_PATH + 128];
    snprintf(path, sizeof path, "%s/%s.txt", A.layouts_dir, name);
    PoLayout *L = malloc(sizeof *L);
    if (po_layout_load(path, L, err, sizeof err) != 0) { toast("%s", err); free(L); return; }
    int ref = -1;
    for (int i = 0; i < A.nimgs; i++) if (!strcmp(A.imgs[i].path, L->ref_path)) ref = i;
    if (ref < 0 && L->root[0] && strcmp(L->root, A.root)) {
        open_root(L->root);
        for (int i = 0; i < A.nimgs; i++) if (!strcmp(A.imgs[i].path, L->ref_path)) ref = i;
    }
    invalidate();
    A.nprobes = L->nprobes;
    for (int i = 0; i < L->nprobes; i++) {
        Box *b = &A.probes[i];
        PoProbe *p = &L->probes[i];
        b->x = p->x; b->y = p->y; b->w = p->w; b->h = p->h; b->mode = p->mode;
        snprintf(b->label, sizeof b->label, "%s", p->label);
    }
    A.anchor = (Anchor){ L->has_anchor, L->ax, L->ay, L->aw, L->ah, L->search ? L->search : 30 };
    A.sel = SEL_NONE;
    snprintf(A.layout_name, sizeof A.layout_name, "%s", name);
    if (ref >= 0) { A.ref = ref; app_select_image(ref); }
    else if (A.anchor.on) toast("The layout's reference image isn't in this folder; using the current reference");
    free(L);
}

/* ---------------- text field edits ---------------- */

void app_field_commit(FieldRef ref, const char *text, bool final) {
    switch (ref.kind) {
    case F_ROOT:
        snprintf(g_root_input, sizeof g_root_input, "%s", text);
        if (final && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) g_open_root = true;
        break;
    case F_LAYOUT:
        snprintf(A.layout_name, sizeof A.layout_name, "%s", text);
        break;
    case F_LABEL:
        if (ref.a < A.nprobes) {
            snprintf(A.probes[ref.a].label, sizeof A.probes[ref.a].label, "%s", text);
            if (ref.a < A.ncols)
                snprintf(A.cols[ref.a], sizeof A.cols[ref.a], "%s", text[0] ? text : TextFormat("probe%d", ref.a + 1));
        }
        break;
    case F_SEARCH:
        if (final && A.anchor.on) {
            int v = atoi(text);
            if (v < 2) v = 2;
            if (v > 400) v = 400;
            if (v != A.anchor.search) { A.anchor.search = v; invalidate(); }
        }
        break;
    case F_CELL:
        if (final && ref.a < A.nimgs && A.imgs[ref.a].res && ref.b < A.imgs[ref.a].res->n) {
            Result *r = A.imgs[ref.a].res;
            char v[96];
            snprintf(v, sizeof v, "%s", text);
            char *s = v;
            while (isspace((unsigned char)*s)) s++;
            char *e = s + strlen(s);
            while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
            if (strcmp(s, r->p[ref.b].value)) {
                snprintf(r->p[ref.b].value, sizeof r->p[ref.b].value, "%s", s);
                r->edited[ref.b] = true;
            }
        }
        break;
    default: break;
    }
}

void app_focus_label(int i) {
    ui_focus(CLAY_IDI("ProbeLabel", i), (FieldRef){ F_LABEL, i, 0 }, A.probes[i].label);
}

static void remove_selected(void) {
    if (A.sel == SEL_ANCHOR) A.anchor.on = false;
    else if (A.sel >= 0 && A.sel < A.nprobes) {
        ui_field_blur();
        memmove(&A.probes[A.sel], &A.probes[A.sel + 1], (size_t)(A.nprobes - A.sel - 1) * sizeof A.probes[0]);
        A.nprobes--;
    }
    A.sel = SEL_NONE;
    invalidate();
}

/* ---------------- panels ---------------- */

static void spacer(void) { CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {} }

static void section_title(const char *s) {
    CLAY({ .layout = { .padding = { 0, 0, 4, 2 } } }) { ui_label(s, 11, C_MUTED, FONT_UI); }
}

static void wrapped(const char *s, Clay_Color color) {
    CLAY_TEXT(cstr(s), CLAY_TEXT_CONFIG({ .fontId = FONT_UI, .fontSize = 13, .textColor = color,
                                          .lineHeight = 18 }));
}

static void header_bar(void) {
    CLAY({ .id = CLAY_ID("Header"),
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(48) },
                       .padding = { 14, 14, 0, 0 }, .childGap = 8,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = C_PANEL,
           .border = { .color = C_LINE, .width = { .bottom = 1 } } }) {
        CLAY({ .layout = { .padding = { 0, 8, 0, 0 } } }) { ui_label("Probe OCR", FS_TITLE, C_TEXT, FONT_UI); }
        ui_text_field(CLAY_ID("RootField"), (FieldRef){ F_ROOT, 0, 0 }, g_root_input,
                      "Folder of screenshots (or drop one on the window)", 0, false);
        if (ui_button(CLAY_ID("OpenBtn"), "Open", false, false, true)) g_open_root = true;
        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(24) } }, .backgroundColor = C_LINE }) {}
        if (ui_button(CLAY_ID("LayoutsBtn"), "Saved layouts", A.menu_layouts, false, true)) {
            A.menu_layouts = !A.menu_layouts;
            if (A.menu_layouts) refresh_layouts();
        }
        ui_text_field(CLAY_ID("LayoutName"), (FieldRef){ F_LAYOUT, 0, 0 }, A.layout_name, "Layout name", 150, false);
        if (ui_button(CLAY_ID("SaveLayout"), "Save layout", false, false, true)) save_layout();
    }
}

static void layouts_menu(void) {
    if (!A.menu_layouts) return;
    Clay_ElementId menu = CLAY_ID("LayoutsMenu");
    CLAY({ .id = menu,
           .layout = { .sizing = { CLAY_SIZING_FIT(220) }, .padding = CLAY_PADDING_ALL(6),
                       .childGap = 2, .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = C_PANEL,
           .cornerRadius = CLAY_CORNER_RADIUS(8),
           .border = { .color = C_LINE, .width = CLAY_BORDER_OUTSIDE(1) },
           .floating = { .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID,
                         .parentId = CLAY_ID("LayoutsBtn").id, .zIndex = 10,
                         .offset = { 0, 4 },
                         .attachPoints = { .element = CLAY_ATTACH_POINT_LEFT_TOP,
                                           .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM } } }) {
        if (!g_nlayouts)
            CLAY({ .layout = { .padding = CLAY_PADDING_ALL(8) } }) {
                ui_label("No saved layouts yet", 13, C_MUTED, FONT_UI);
            }
        for (int i = 0; i < g_nlayouts; i++) {
            Clay_ElementId id = CLAY_IDI("LayoutItem", i);
            CLAY({ .id = id,
                   .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28) }, .padding = { 10, 10, 0, 0 },
                               .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                   .backgroundColor = Clay_PointerOver(id) ? C_HOVER : C_PANEL,
                   .cornerRadius = CLAY_CORNER_RADIUS(5) }) {
                ui_label(g_layouts[i], FS_BODY, C_TEXT, FONT_UI);
            }
            if (ui_take_click(id)) { A.menu_layouts = false; load_layout(g_layouts[i]); }
        }
    }
    /* click anywhere else closes it */
    if (ui_click_pending() && !Clay_PointerOver(menu) && !Clay_PointerOver(CLAY_ID("LayoutsBtn")))
        A.menu_layouts = false;
}

static void toolbar(void) {
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .padding = { 12, 12, 8, 8 }, .childGap = 6,
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .border = { .color = C_LINE, .width = { .bottom = 1 } } }) {
        CLAY({ .layout = { .childGap = 6, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (ui_button(CLAY_ID("ToolProbe"), "+ Probe box", !A.tool_anchor, false, true)) A.tool_anchor = false;
            if (ui_button(CLAY_ID("ToolAnchor"), "+ Anchor", A.tool_anchor, false, true)) A.tool_anchor = !A.tool_anchor;
            if (ui_button(CLAY_ID("SetRef"), "Make this the reference", false, false, A.cur >= 0 && A.cur != A.ref))
                { app_set_ref(A.cur); toast("Reference changed; boxes kept where they appear on this image"); }
            if (ui_button(CLAY_ID("TestCur"), "Test this image", false, false, A.cur >= 0 && !batch_running()))
                batch_start(&A.cur, 1);
        }
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 4,
                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (ui_button(CLAY_ID("ZoomOut"), "-", false, false, A.cur >= 0)) canvas_zoom_by(1 / 1.25f);
            if (ui_button(CLAY_ID("ZoomPct"), TextFormat("%d%%", (int)roundf(canvas_scale() * 100)), false, false, A.cur >= 0))
                canvas_zoom_to(1);
            if (ui_button(CLAY_ID("ZoomIn"), "+", false, false, A.cur >= 0)) canvas_zoom_by(1.25f);
            if (ui_button(CLAY_ID("ZoomFit"), "Fit", false, false, A.cur >= 0)) canvas_fit();
            Clay_ElementId hint_id = CLAY_ID("Hint");
            CLAY({ .id = hint_id, .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .padding = { 8, 0, 0, 0 } } }) {
                const char *hint;
                if (A.cur < 0) hint = "Open a folder to begin";
                else if (A.tool_anchor) hint = TextFormat("%d / %d · drag around something every screenshot shares", A.cur + 1, A.nimgs);
                else hint = TextFormat("%d / %d%s · drag = new box · Ctrl/Cmd+scroll = zoom · scroll or Space+drag = pan",
                                       A.cur + 1, A.nimgs, A.cur != A.ref ? " · not the reference (boxes at detected shift)" : "");
                ui_label_fit(hint, FS_SMALL, C_MUTED, FONT_UI, ui_width_of(hint_id, 300) - 8);
            }
        }
    }
}

static void probe_card(int i) {
    Box *b = &A.probes[i];
    Clay_ElementId card = CLAY_IDI("ProbeCard", i);
    const Result *res = A.cur >= 0 ? A.imgs[A.cur].res : NULL;
    Color pc = probe_color(i);
    CLAY({ .id = card,
           .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(8), .childGap = 6,
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .cornerRadius = CLAY_CORNER_RADIUS(8),
           .border = { .color = A.sel == i ? C_ACCENT : C_LINE, .width = CLAY_BORDER_OUTSIDE(A.sel == i ? 2 : 1) } }) {
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 6, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } },
                   .backgroundColor = { pc.r, pc.g, pc.b, 255 }, .cornerRadius = CLAY_CORNER_RADIUS(2) }) {}
            ui_text_field(CLAY_IDI("ProbeLabel", i), (FieldRef){ F_LABEL, i, 0 }, b->label,
                          "Column header (e.g. Temp °C)", 0, false);
            if (ui_button(CLAY_IDI("ProbeDel", i), "×", false, false, true)) { A.sel = i; remove_selected(); return; }
        }
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 4, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            if (ui_button(CLAY_IDI("ModeNum", i), "Number", b->mode == PO_MODE_NUM, false, true) && b->mode != PO_MODE_NUM)
                { b->mode = PO_MODE_NUM; invalidate(); }
            if (ui_button(CLAY_IDI("ModeText", i), "Text", b->mode == PO_MODE_TEXT, false, true) && b->mode != PO_MODE_TEXT)
                { b->mode = PO_MODE_TEXT; invalidate(); }
            CLAY({ .layout = { .padding = { 4, 0, 0, 0 } } }) {
                ui_label(TextFormat("%d,%d %dx%d", b->x, b->y, b->w, b->h), 11, C_MUTED, FONT_MONO);
            }
            spacer();
            if (res && res->ok && i < res->n)
                ui_label(res->p[i].value[0] ? res->p[i].value : "-", 13,
                         res->p[i].conf < LOW_CONF ? C_WARN : C_TEXT, FONT_MONO);
        }
    }
    if (ui_take_click(card)) A.sel = i;
}

static void anchor_card(void) {
    const Result *res = A.cur >= 0 ? A.imgs[A.cur].res : NULL;
    Clay_ElementId card = CLAY_ID("AnchorCard");
    CLAY({ .id = card,
           .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .padding = CLAY_PADDING_ALL(8), .childGap = 6,
                       .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .cornerRadius = CLAY_CORNER_RADIUS(8),
           .border = { .color = C_ANCHOR, .width = CLAY_BORDER_OUTSIDE(A.sel == SEL_ANCHOR ? 2 : 1) } }) {
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 6, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } },
                   .backgroundColor = C_ANCHOR, .cornerRadius = CLAY_CORNER_RADIUS(2) }) {}
            ui_label("Search ±", FS_BODY, C_TEXT, FONT_UI);
            ui_text_field(CLAY_ID("AnchorSearch"), (FieldRef){ F_SEARCH, 0, 0 },
                          TextFormat("%d", A.anchor.search), NULL, 60, true);
            ui_label("px", FS_BODY, C_TEXT, FONT_UI);
            spacer();
            if (ui_button(CLAY_ID("AnchorDel"), "×", false, false, true)) { A.sel = SEL_ANCHOR; remove_selected(); return; }
        }
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .childGap = 6 } }) {
            ui_label(TextFormat("%d,%d %dx%d", A.anchor.x, A.anchor.y, A.anchor.w, A.anchor.h), 11, C_MUTED, FONT_MONO);
            spacer();
            if (res && res->ok)
                ui_label(TextFormat("shift %d,%d · match %d%%", res->dx, res->dy, (int)roundf(res->score * 100)),
                         12, res->score < 0.7 ? C_WARN : C_TEXT, FONT_UI);
        }
    }
    if (ui_take_click(card)) A.sel = SEL_ANCHOR;
}

static void side_panel(void) {
    CLAY({ .id = CLAY_ID("Side"),
           .layout = { .sizing = { CLAY_SIZING_FIXED(SIDE_W), CLAY_SIZING_GROW(0) },
                       .padding = CLAY_PADDING_ALL(12), .childGap = 8, .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = C_PANEL,
           .border = { .color = C_LINE, .width = { .left = 1 } },
           .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
        section_title("PROBES / COLUMN HEADERS");
        if (!A.nprobes) wrapped("No probes yet. Drag a box over a reading on the image.", C_MUTED);
        for (int i = 0; i < A.nprobes; i++) probe_card(i);
        section_title("ALIGNMENT ANCHOR");
        if (A.anchor.on) anchor_card();
        else wrapped("None. If screenshots don't all line up exactly, click \"+ Anchor\" and drag around "
                     "something every screenshot shares, like a window title bar or a fixed label.", C_MUTED);
    }
}

static int g_rows[MAX_IMAGES];

static float col_width(int c) {
    float w = ui_measure(A.cols[c], 13, FONT_UI).x + 24;
    return w < 140 ? 140 : w > 320 ? 320 : w;
}

static void table_cell_text(const char *s, float w, Clay_Color fg, int font, Clay_Color bg) {
    CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(ROW_H) }, .padding = { 8, 8, 0, 0 },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = bg }) {
        ui_label_fit(s, 13, fg, font, w - 16);
    }
}

static void results_table(int nrows) {
    float img_w = 200, shift_w = 80;
    Clay_ElementId body = CLAY_ID("TableBody");
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(body);
    float sx = sc.found ? sc.scrollPosition->x : 0, sy = sc.found ? -sc.scrollPosition->y : 0;
    float view_h = sc.found ? sc.scrollContainerDimensions.height : 300;
    float max_h = GetScreenHeight() * 0.32f;
    float body_h = fminf(nrows * ROW_H, max_h);

    /* header row follows the body's horizontal scroll */
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30) } },
           .border = { .color = C_LINE, .width = { .bottom = 1, .top = 1 } },
           .clip = { .horizontal = true, .childOffset = { sx, 0 } } }) {
        CLAY({ .layout = { .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            table_cell_text("Image", img_w, C_TEXT, FONT_UI, C_PANEL);
            table_cell_text("Shift", shift_w, C_TEXT, FONT_UI, C_PANEL);
            for (int c = 0; c < A.ncols; c++) table_cell_text(A.cols[c], col_width(c), C_TEXT, FONT_UI, C_PANEL);
        }
    }

    int first = (int)floorf(sy / ROW_H) - 2, last = (int)ceilf((sy + view_h) / ROW_H) + 2;
    if (first < 0) first = 0;
    if (last > nrows) last = nrows;
    CLAY({ .id = body,
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(body_h) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .clip = { .horizontal = true, .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
        if (first > 0) CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED((float)first * ROW_H) } } }) {}
        for (int rix = first; rix < last; rix++) {
            int img = g_rows[rix];
            Img *im = &A.imgs[img];
            Result *r = im->res;
            bool cur = img == A.cur;
            Clay_Color rowbg = cur ? C_ACCENT_S : C_PANEL;
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(ROW_H) } },
                   .border = { .color = C_LINE, .width = { .bottom = 1 } } }) {
                Clay_ElementId name = CLAY_IDI("RowImg", img);
                CLAY({ .id = name, .layout = { .sizing = { CLAY_SIZING_FIXED(img_w), CLAY_SIZING_FIXED(ROW_H) },
                                               .padding = { 8, 8, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                       .backgroundColor = rowbg }) {
                    ui_label_fit(im->name, 13, C_ACCENT, FONT_UI, img_w - 16);
                }
                if (ui_take_click(name)) app_select_image(img);
                bool weak = r->ok && A.anchor.on && r->score < 0.7;
                table_cell_text(r->ok ? TextFormat("%d,%d", r->dx, r->dy) : r->error, shift_w,
                                weak ? C_WARN : C_MUTED, FONT_MONO, weak ? C_WARN_BG : rowbg);
                for (int c = 0; c < A.ncols; c++) {
                    float w = col_width(c);
                    FieldRef ref = { F_CELL, img, c };
                    Clay_ElementId cell = CLAY_IDI("Cell", img * PO_MAX_PROBES + c);
                    if (c >= r->n) { table_cell_text("", w, C_TEXT, FONT_MONO, rowbg); continue; }
                    if (ui_field_is(ref)) {
                        CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(ROW_H) }, .padding = { 2, 2, 0, 0 },
                                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                            ui_text_field(cell, ref, r->p[c].value, NULL, w - 4, true);
                        }
                        continue;
                    }
                    bool low = !r->edited[c] && (r->p[c].conf < LOW_CONF || !r->p[c].value[0]);
                    Clay_Color bg = r->edited[c] ? C_EDITED : low ? C_WARN_BG : rowbg;
                    CLAY({ .id = cell, .layout = { .sizing = { CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(ROW_H) },
                                                   .padding = { 8, 8, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                           .backgroundColor = Clay_PointerOver(cell) ? C_HOVER : bg }) {
                        ui_label_fit(r->p[c].value, 13, low ? C_WARN : C_TEXT, FONT_MONO, w - 16);
                    }
                    if (ui_take_click(cell)) {
                        ui_focus(cell, ref, r->p[c].value);
                        if (A.cur != img) app_select_image(img);
                    }
                }
            }
        }
        if (last < nrows) CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED((float)(nrows - last) * ROW_H) } } }) {}
    }
}

static void footer(void) {
    int nrows = 0;
    for (int i = 0; i < A.nimgs; i++) if (A.imgs[i].checked && A.imgs[i].res) g_rows[nrows++] = i;
    int nsel = tree_selected_count();
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = C_PANEL,
           .border = { .color = C_LINE, .width = { .top = 1 } } }) {
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(46) }, .padding = { 14, 14, 0, 0 },
                           .childGap = 10, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
            bool running = batch_running();
            if (ui_button(CLAY_ID("RunAll"), nsel ? TextFormat("Run batch (%d)", nsel) : "Run batch", false, true, !running)) {
                if (!nsel) toast("Tick at least one image in the tree");
                else {
                    int *idx = malloc((size_t)nsel * sizeof *idx), k = 0;
                    for (int i = 0; i < A.nimgs; i++) if (A.imgs[i].checked) idx[k++] = i;
                    batch_start(idx, k);
                    free(idx);
                }
            }
            if (running) {
                int d, t;
                float p = batch_progress(&d, &t);
                CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(160), CLAY_SIZING_FIXED(8) } },
                       .custom = { .customData = ui_tag(CUSTOM_PROGRESS, 0, p) } }) {}
                if (ui_button(CLAY_ID("StopBtn"), "Stop", false, false, true)) batch_cancel();
            }
            Clay_ElementId status_id = CLAY_ID("Status");
            CLAY({ .id = status_id, .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {
                ui_label_fit(A.status, 13, C_MUTED, FONT_UI, ui_width_of(status_id, 300));
            }
            char path[PO_MAX_PATH];
            if (ui_button(CLAY_ID("ExportCsv"), "Export CSV", false, false, nrows > 0 && !running)) {
                int n = export_csv(path, sizeof path);
                if (n < 0) toast("Could not write %s", path);
                else { app_set_status("Wrote %d rows to %s", n, path); toast("Saved %s", path); }
            }
            Clay_ElementId cc = CLAY_ID("ConfToggle");
            CLAY({ .id = cc, .layout = { .childGap = 6, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                if (ui_checkbox(CLAY_ID("ConfChk"), A.with_conf)) A.with_conf = !A.with_conf;
                ui_label("include confidence", 13, C_MUTED, FONT_UI);
            }
            if (ui_take_click(cc)) A.with_conf = !A.with_conf;
            CLAY({ .layout = { .childGap = 5, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } }, .backgroundColor = C_WARN_BG,
                       .border = { .color = C_WARN, .width = CLAY_BORDER_OUTSIDE(1) } }) {}
                ui_label("needs review", 12, C_MUTED, FONT_UI);
                CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10) } }, .backgroundColor = C_EDITED }) {}
                ui_label("edited", 12, C_MUTED, FONT_UI);
            }
        }
        if (nrows) results_table(nrows);
    }
}

static void toast_view(void) {
    if (GetTime() > A.toast_until) return;
    CLAY({ .layout = { .padding = { 14, 14, 9, 9 } },
           .backgroundColor = C_TEXT, .cornerRadius = CLAY_CORNER_RADIUS(6),
           .floating = { .attachTo = CLAY_ATTACH_TO_ROOT, .zIndex = 20,
                         .offset = { 0, -70 },
                         .attachPoints = { .element = CLAY_ATTACH_POINT_CENTER_BOTTOM,
                                           .parent = CLAY_ATTACH_POINT_CENTER_BOTTOM },
                         .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH } }) {
        ui_label(A.toast, FS_BODY, C_BG, FONT_UI);
    }
}

static void build_ui(void) {
    CLAY({ .id = CLAY_ID("Root"),
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .backgroundColor = C_BG }) {
        header_bar();
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(TREE_W), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
                   .backgroundColor = C_PANEL, .border = { .color = C_LINE, .width = { .right = 1 } } }) {
                tree_panel();
            }
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .layoutDirection = CLAY_TOP_TO_BOTTOM } }) {
                toolbar();
                CLAY({ .id = CLAY_ID("Canvas"), .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } },
                       .backgroundColor = C_STAGE, .custom = { .customData = ui_tag(CUSTOM_CANVAS, 0, 0) } }) {}
            }
            side_panel();
        }
        footer();
        layouts_menu();
        toast_view();
    }
}

/* ---------------- keyboard & files ---------------- */

static void shortcuts(void) {
    if (ui_field_focused()) return;
    bool cmd = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
               IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (cmd) {
        if (IsKeyPressed(KEY_S)) save_layout();
        return;
    }
    if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_PAGE_DOWN)) app_select_image(A.cur + 1);
    if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_PAGE_UP)) app_select_image(A.cur - 1);
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) canvas_zoom_by(1.25f);
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) canvas_zoom_by(1 / 1.25f);
    if (IsKeyPressed(KEY_ZERO)) canvas_fit();
    if (IsKeyPressed(KEY_ONE)) canvas_zoom_to(1);
    if (A.sel == SEL_NONE || (A.sel == SEL_ANCHOR && !A.anchor.on)) return;
    int step = shift ? 10 : 1, dx = 0, dy = 0;
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) dx = -step;
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) dx = step;
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) dy = -step;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) dy = step;
    if (dx || dy) {
        if (A.sel == SEL_ANCHOR) { A.anchor.x += dx; A.anchor.y += dy; }
        else { A.probes[A.sel].x += dx; A.probes[A.sel].y += dy; }
        invalidate();
    }
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) remove_selected();
}

static void dropped_files(void) {
    if (!IsFileDropped()) return;
    FilePathList files = LoadDroppedFiles();
    if (files.count > 0) {
        const char *p = files.paths[0];
        int is_dir = po_list_dir(p, NULL, NULL) == 0;
        if (is_dir) open_root(p);
        else {
            char file[PO_MAX_PATH];
            snprintf(file, sizeof file, "%s", p);
            open_root(GetDirectoryPath(file));
            for (int i = 0; i < A.nimgs; i++) if (!strcmp(A.imgs[i].path, file)) app_select_image(i);
        }
    }
    UnloadDroppedFiles(files);
}

/* Before any boxes exist, keep the reference on a selected image. */
static void keep_ref_selected(void) {
    if (A.nprobes || A.anchor.on || A.ref < 0 || A.imgs[A.ref].checked) return;
    int f = first_selected();
    if (f >= 0) { A.ref = f; app_select_image(f); }
}

int app_layout_index(const char *name) {
    for (int i = 0; i < g_nlayouts; i++) if (!strcmp(g_layouts[i], name)) return i;
    return -1;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
#ifdef _WIN32
    char **wargv = po_utf8_argv(&argc);
    if (wargv) argv = wargv;
#endif
    const char *root = NULL, *selftest_dir = NULL, *selftest_png = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) { printf("probeocr-gui %s (Clay + raylib %s)\n", PROBEOCR_VERSION, RAYLIB_VERSION); return 0; }
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--selftest") && i + 2 < argc) { selftest_dir = argv[++i]; selftest_png = argv[++i]; }
        else { fprintf(stderr, "usage: probeocr-gui [--root DIR] | --selftest SAMPLES_DIR OUT.png | --version\n"); return 2; }
    }

    ui_init(1400, 880);
    SetWindowMinSize(1100, 640);

    /* bundle layout: <app>/bin/probeocr-gui, <app>/tessdata, <app>/layouts */
    po_exe_dir(A.app_dir, sizeof A.app_dir);   /* UTF-8 safe, unlike GetApplicationDirectory on Windows */
    snprintf(A.layouts_dir, sizeof A.layouts_dir, "%s../layouts", A.app_dir);
    snprintf(A.tessdata, sizeof A.tessdata, "%s../tessdata", A.app_dir);
    char probe[PO_MAX_PATH];
    snprintf(probe, sizeof probe, "%s/eng.traineddata", A.tessdata);
    A.has_tessdata = po_exists(probe);
    A.sel = SEL_NONE;
    A.cur = A.ref = -1;
    snprintf(A.status, sizeof A.status, "Ready");

    if (selftest_dir) root = selftest_dir;
    open_root(root ? root : GetWorkingDirectory());

    int selftest_rc = -1;
    if (selftest_dir) selftest_init(selftest_dir, selftest_png);

    while (!WindowShouldClose()) {
        if (selftest_dir && (selftest_rc = selftest_frame()) >= 0) break;
        dropped_files();
        batch_poll();
        if (g_open_root) { g_open_root = false; open_root(g_root_input); }

        ui_begin_frame();
        build_ui();
        ui_end_layout();
        canvas_input();
        shortcuts();
        keep_ref_selected();
        ui_render();

        /* idle without spinning the CPU unless something is animating */
        if (batch_running() || GetTime() < A.toast_until || selftest_dir) DisableEventWaiting();
        else EnableEventWaiting();

    }
    batch_cancel();
    canvas_unload();
    ui_shutdown();
    return selftest_dir ? (selftest_rc < 0 ? 1 : selftest_rc) : 0;
}
