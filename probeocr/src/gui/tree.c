/*
 * Image folder tree: scanning and the checkable sidebar.
 *
 * Nodes are stored in display order (pre-order, sub-folders before images),
 * so every folder's descendant images form one contiguous range of A.imgs.
 */
#include "app.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 8
#define ROW_H 24
#define TREE_PANEL_W 250

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

static const char *const SKIP_DIRS[] = { "node_modules", "__pycache__", "venv", ".venv",
                                         "site-packages", "CMakeFiles", NULL };
static const char *const IMAGE_EXTS[] = { ".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff",
                                          ".gif", ".webp", NULL };
static int g_cap_nodes, g_cap_imgs;
static int g_reveal = -1;   /* image to scroll into view on the next frame */

/* "shot_2" < "shot_10", case-insensitive */
static int natural_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)((ea - a) - (eb - b));
            int c = strncmp(a, b, (size_t)(ea - a));
            if (c) return c;
            a = ea; b = eb;
        } else {
            int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
            if (c) return c;
            a++; b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

typedef struct { char *name; int is_dir; } Ent;
typedef struct { Ent *v; int n, cap; } EntList;

static void collect(const char *name, int is_dir, void *user) {
    EntList *l = user;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v); }
    l->v[l->n++] = (Ent){ strdup(name), is_dir };
}

static int ent_cmp(const void *a, const void *b) { return natural_cmp(((const Ent *)a)->name, ((const Ent *)b)->name); }

static bool is_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char ext[16];
    size_t i = 0;
    for (; dot[i] && i < sizeof ext - 1; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[i] = '\0';
    for (const char *const *e = IMAGE_EXTS; *e; e++) if (!strcmp(ext, *e)) return true;
    return false;
}

static bool skip_dir(const char *name) {
    if (name[0] == '.') return true;
    for (const char *const *s = SKIP_DIRS; *s; s++) if (!strcmp(name, *s)) return true;
    return false;
}

static int add_node(const char *name, const char *path, int depth, bool is_dir) {
    if (A.nnodes == g_cap_nodes) {
        g_cap_nodes = g_cap_nodes ? g_cap_nodes * 2 : 256;
        A.nodes = realloc(A.nodes, (size_t)g_cap_nodes * sizeof *A.nodes);
    }
    Node *n = &A.nodes[A.nnodes];
    memset(n, 0, sizeof *n);
    snprintf(n->name, sizeof n->name, "%s", name);
    n->path = strdup(path);
    n->depth = depth;
    n->is_dir = is_dir;
    n->open = true;
    n->img = -1;
    return A.nnodes++;
}

static int add_image(const char *path, const char *name) {
    if (A.nimgs == g_cap_imgs) {
        g_cap_imgs = g_cap_imgs ? g_cap_imgs * 2 : 256;
        A.imgs = realloc(A.imgs, (size_t)g_cap_imgs * sizeof *A.imgs);
    }
    A.imgs[A.nimgs] = (Img){ strdup(path), strdup(name), true, NULL };
    return A.nimgs++;
}

/* Returns the node index, or -1 if the folder holds no images (it is dropped). */
static int walk(const char *dir, const char *name, int depth) {
    int me = add_node(name, dir, depth, true);
    int first_img = A.nimgs;
    EntList l = { 0 };
    po_list_dir(dir, collect, &l);
    if (l.n) qsort(l.v, (size_t)l.n, sizeof *l.v, ent_cmp);

    char path[PO_MAX_PATH];
    for (int i = 0; i < l.n; i++) {
        if (!l.v[i].is_dir || skip_dir(l.v[i].name) || depth >= MAX_DEPTH) continue;
        snprintf(path, sizeof path, "%s" SEP "%s", dir, l.v[i].name);
        walk(path, l.v[i].name, depth + 1);
    }
    for (int i = 0; i < l.n; i++) {
        if (l.v[i].is_dir || !is_image(l.v[i].name)) continue;
        if (A.nimgs >= MAX_IMAGES) { A.truncated = true; continue; }
        snprintf(path, sizeof path, "%s" SEP "%s", dir, l.v[i].name);
        int img = add_image(path, l.v[i].name);
        int n = add_node(l.v[i].name, path, depth + 1, false);
        A.nodes[n].img = img;
    }
    for (int i = 0; i < l.n; i++) free(l.v[i].name);
    free(l.v);

    if (A.nimgs == first_img && depth > 0) {
        while (A.nnodes > me) free(A.nodes[--A.nnodes].path);
        return -1;
    }
    Node *n = &A.nodes[me];
    n->img_begin = first_img;
    n->img_end = A.nimgs;
    n->sub_end = A.nnodes;
    return me;
}

void tree_free(void) {
    for (int i = 0; i < A.nnodes; i++) free(A.nodes[i].path);
    for (int i = 0; i < A.nimgs; i++) {
        free(A.imgs[i].path);
        free(A.imgs[i].name);
    }
    A.nnodes = A.nimgs = 0;
    A.truncated = false;
}

void tree_open(const char *root) {
    results_clear();
    tree_free();
    char clean[PO_MAX_PATH];
    snprintf(clean, sizeof clean, "%s", root);
    size_t len = strlen(clean);
    while (len > 1 && (clean[len - 1] == '/' || clean[len - 1] == '\\')) clean[--len] = '\0';
    snprintf(A.root, sizeof A.root, "%s", clean);
    const char *base = strrchr(clean, '/');
    const char *bs = strrchr(clean, '\\');
    if (bs && (!base || bs > base)) base = bs;
    walk(clean, base ? base + 1 : clean, 0);
    /* Big trees start with sub-folders collapsed. */
    if (A.nimgs > 200)
        for (int i = 0; i < A.nnodes; i++) if (A.nodes[i].is_dir && A.nodes[i].depth > 0) A.nodes[i].open = false;
    tree_recount();
}

void tree_recount(void) {
    for (int i = 0; i < A.nnodes; i++) {
        Node *n = &A.nodes[i];
        if (!n->is_dir) continue;
        n->nchecked = 0;
        for (int k = n->img_begin; k < n->img_end; k++) n->nchecked += A.imgs[k].checked;
    }
}

int tree_selected_count(void) {
    int n = 0;
    for (int i = 0; i < A.nimgs; i++) n += A.imgs[i].checked;
    return n;
}

void tree_set_check_range(int begin, int end, bool on) {
    for (int k = begin; k < end; k++) A.imgs[k].checked = on;
    tree_recount();
}

void tree_set_all(bool on) { tree_set_check_range(0, A.nimgs, on); }

void tree_reveal(int img) {
    for (int i = 0; i < A.nnodes; i++) {
        Node *n = &A.nodes[i];
        if (n->is_dir && img >= n->img_begin && img < n->img_end) n->open = true;
    }
    g_reveal = img;
}

/* ---------------- sidebar ---------------- */

static int g_vis[MAX_IMAGES * 2 + 256];

static void tree_row(int ni) {
    Node *n = &A.nodes[ni];
    Clay_ElementId row = CLAY_IDI("TreeRow", ni);
    bool cur = !n->is_dir && n->img == A.cur;
    bool hov = Clay_PointerOver(row);
    bool off = !n->is_dir && !A.imgs[n->img].checked;
    CLAY({ .id = row,
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(ROW_H) },
                       .padding = { (uint16_t)(6 + n->depth * 14), 6, 0, 0 },
                       .childGap = 5,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = cur ? C_ACCENT_S : hov ? C_HOVER : C_PANEL,
           .cornerRadius = CLAY_CORNER_RADIUS(5) }) {
        if (n->is_dir) {
            Clay_ElementId caret = CLAY_IDI("TreeCaret", ni);
            CLAY({ .id = caret, .layout = { .sizing = { CLAY_SIZING_FIXED(14), CLAY_SIZING_FIXED(14) } },
                   .custom = { .customData = ui_tag(CUSTOM_CARET, n->open, 0) } }) {}
            if (ui_take_click(caret)) n->open = !n->open;
            int tot = n->img_end - n->img_begin;
            int state = n->nchecked == 0 ? 0 : n->nchecked == tot ? 1 : 2;
            if (ui_checkbox(CLAY_IDI("TreeChk", ni), state))
                tree_set_check_range(n->img_begin, n->img_end, state != 1);
            float avail = ui_width_of(CLAY_ID("Tree"), TREE_PANEL_W) - 8 - 12 - n->depth * 14 - 14 - 15 - 3 * 5 - 44;
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {
                ui_label_fit(fstr("%s/", n->name).chars, 13, C_TEXT, FONT_UI, avail);
            }
            ui_label(fstr("%d/%d", n->nchecked, tot).chars, 11, C_MUTED, FONT_UI);
            if (ui_take_click(row)) n->open = !n->open;
        } else {
            Img *im = &A.imgs[n->img];
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(14), CLAY_SIZING_FIXED(14) } } }) {}
            if (ui_checkbox(CLAY_IDI("TreeChk", ni), im->checked)) {
                im->checked = !im->checked;
                tree_recount();
            }
            Clay_Color dot = !im->res ? (Clay_Color){ 0 } : result_needs_review(im->res) ? C_WARN : C_GOOD;
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(7), CLAY_SIZING_FIXED(7) } },
                   .backgroundColor = dot, .cornerRadius = CLAY_CORNER_RADIUS(3.5f) }) {}
            float avail = ui_width_of(CLAY_ID("Tree"), TREE_PANEL_W) - 8 - 12 - n->depth * 14 - 14 - 15 - 7
                          - 4 * 5 - (n->img == A.ref ? 36 : 0);
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {
                ui_label_fit(n->name, 13, off ? C_MUTED : C_TEXT, FONT_UI, avail);
            }
            if (n->img == A.ref)
                CLAY({ .layout = { .padding = { 4, 4, 1, 1 } }, .cornerRadius = CLAY_CORNER_RADIUS(3),
                       .border = { .color = C_ANCHOR, .width = CLAY_BORDER_OUTSIDE(1) } }) {
                    ui_label("REF", 10, C_ANCHOR, FONT_UI);
                }
            if (ui_take_click(row)) app_select_image(n->img);
        }
    }
}

void tree_panel(void) {
    int nsel = tree_selected_count();
    CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(38) },
                       .padding = { 10, 8, 0, 0 }, .childGap = 4,
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .border = { .color = C_LINE, .width = { .bottom = 1 } } }) {
        ui_label(A.nimgs ? fstr("%d of %d selected", nsel, A.nimgs).chars : "No images", FS_SMALL, C_MUTED, FONT_UI);
        CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}
        if (ui_button(CLAY_ID("SelAll"), "All", false, false, A.nimgs > 0)) tree_set_all(true);
        if (ui_button(CLAY_ID("SelNone"), "None", false, false, A.nimgs > 0)) tree_set_all(false);
    }

    /* visible rows: skip the contents of collapsed folders */
    int nvis = 0;
    for (int i = 0; i < A.nnodes;) {
        g_vis[nvis++] = i;
        i = (A.nodes[i].is_dir && !A.nodes[i].open) ? A.nodes[i].sub_end : i + 1;
    }

    Clay_ElementId tid = CLAY_ID("Tree");
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(tid);
    if (g_reveal >= 0 && sc.found) {
        for (int v = 0; v < nvis; v++) {
            Node *n = &A.nodes[g_vis[v]];
            if (n->is_dir || n->img != g_reveal) continue;
            float top = v * ROW_H + 6, h = sc.scrollContainerDimensions.height;
            float y = -sc.scrollPosition->y;
            if (top < y) sc.scrollPosition->y = -top;
            else if (top + ROW_H > y + h) sc.scrollPosition->y = -(top + ROW_H - h);
            break;
        }
        g_reveal = -1;
    }
    float scroll = sc.found ? -sc.scrollPosition->y : 0;
    float view_h = sc.found ? sc.scrollContainerDimensions.height : 2000;
    int first = (int)floorf(scroll / ROW_H) - 2, last = (int)ceilf((scroll + view_h) / ROW_H) + 2;
    if (first < 0) first = 0;
    if (last > nvis) last = nvis;

    CLAY({ .id = tid,
           .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                       .padding = { 4, 4, 6, 10 }, .layoutDirection = CLAY_TOP_TO_BOTTOM },
           .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
        if (!A.nimgs) {
            CLAY({ .layout = { .padding = CLAY_PADDING_ALL(8) } }) {
                ui_label("No images found under this folder.", FS_SMALL, C_MUTED, FONT_UI);
            }
        }
        if (first > 0)
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)first * ROW_H) } } }) {}
        for (int v = first; v < last; v++) tree_row(g_vis[v]);
        if (last < nvis)
            CLAY({ .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)(nvis - last) * ROW_H) } } }) {}
        if (A.truncated)
            CLAY({ .layout = { .padding = CLAY_PADDING_ALL(8) } }) {
                ui_label("Showing the first 5000 images only.", FS_SMALL, C_WARN, FONT_UI);
            }
    }
}
