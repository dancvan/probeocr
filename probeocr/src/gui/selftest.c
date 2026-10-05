/*
 * --selftest: run the samples through the real app, then drive the UI with
 * injected raylib input events (clicks, drags, keys, wheel) and check that
 * each interaction had the intended effect. Ends with a screenshot.
 */
#include "app.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* raylib's AutomationEventType values (rcore.c, raylib 6.0) */
enum { EV_KEY_UP = 1, EV_KEY_DOWN = 2, EV_MB_UP = 5, EV_MB_DOWN = 6, EV_MPOS = 7, EV_WHEEL = 8 };

typedef struct { int frame, type, a, b; } QEv;
static QEv g_q[256];
static int g_nq, g_now, g_step, g_wait, g_fail;
static const char *g_dir, *g_png;
static float g_saved_scale, g_saved_px, g_saved_py;
static Vector2 g_saved_pt, g_saved_center;
static Vector2 g_ptr = { 10, 10 };   /* the test's pointer; real mouse input is overridden */
static bool g_btn;

/* Queue an event `off` frames after the next one (this frame's events have already played). */
static void at(int off, int type, int a, int b) {
    if (g_nq < 256) g_q[g_nq++] = (QEv){ g_now + 1 + off, type, a, b };
}
/* Re-assert the pointer every frame: real mouse movement over the window
   would otherwise overwrite the injected position mid-gesture. */
static void pos(int off, Vector2 p) { at(off, EV_MPOS, (int)p.x, (int)p.y); }
static void click(int off, Vector2 p) {
    pos(off, p);
    pos(off + 1, p); at(off + 1, EV_MB_DOWN, 0, 0);
    pos(off + 2, p); at(off + 2, EV_MB_UP, 0, 0);
}
/* press at a, move through the midpoint to b, release */
static void drag(int off, Vector2 a, Vector2 b) {
    Vector2 m = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
    pos(off, a);
    pos(off + 1, a); at(off + 1, EV_MB_DOWN, 0, 0);
    pos(off + 2, m);
    pos(off + 3, b);
    pos(off + 4, b); at(off + 4, EV_MB_UP, 0, 0);
}
static void key(int off, int k) { at(off, EV_KEY_DOWN, k, 0); at(off + 1, EV_KEY_UP, k, 0); }
static Vector2 center(Clay_ElementId id) {
    Clay_ElementData d = Clay_GetElementData(id);
    return (Vector2){ d.boundingBox.x + d.boundingBox.width / 2, d.boundingBox.y + d.boundingBox.height / 2 };
}
static void check(bool ok, const char *msg) {
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", msg);
    if (!ok) g_fail++;
}
static int node_of_img(int img) {
    for (int i = 0; i < A.nnodes; i++) if (!A.nodes[i].is_dir && A.nodes[i].img == img) return i;
    return -1;
}
static char *read_file(const char *path) {
    long n = 0;
    char *s = po_slurp(path, &n);
    if (!s) return NULL;
    s = realloc(s, (size_t)n + 1);
    s[n] = '\0';
    return s;
}

void selftest_init(const char *dir, const char *png) {
    g_dir = dir;
    g_png = png;
    printf("probeocr-gui self-test\n  root: %s\n  images: %d\n", A.root, A.nimgs);
    /* the layout used by the browser version's tests */
    A.anchor = (Anchor){ true, 98, 45, 191, 30, 30 };
    const char *labels[] = { "Temperature (°C)", "Pressure, kPa", "Voltage (mV)" };
    for (int i = 0; i < 3; i++) {
        A.probes[i] = (Box){ .x = 211, .y = 80 + 90 * i, .w = 152, .h = 48, .mode = PO_MODE_NUM };
        snprintf(A.probes[i].label, sizeof A.probes[i].label, "%s", labels[i]);
    }
    A.nprobes = 3;
    snprintf(A.layout_name, sizeof A.layout_name, "selftest");
}

static int batch_check(void) {
    char path[PO_MAX_PATH];
    snprintf(path, sizeof path, "%s/truth.csv", g_dir);
    FILE *f = po_fopen(path, "r");
    if (!f) { check(false, "read truth.csv"); return 1; }
    char line[1024];
    int good = 0, total = 0, rows = 0;
    if (!fgets(line, sizeof line, f)) line[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        char *name = strtok(line, ",\r\n");
        if (!name) continue;
        int img = -1;
        for (int i = 0; i < A.nimgs; i++) if (!strcmp(A.imgs[i].name, name)) img = i;
        Result *r = img >= 0 ? A.imgs[img].res : NULL;
        rows += r != NULL;
        for (int c = 0; c < 3; c++) {
            char *exp = strtok(NULL, ",\r\n");
            if (!exp) break;
            total++;
            if (r && c < r->n && r->p[c].value[0] && fabs(atof(r->p[c].value) - atof(exp)) < 0.005) good++;
            else printf("        %s col %d: got '%s', expected %s\n", name, c, r && c < r->n ? r->p[c].value : "", exp);
        }
    }
    fclose(f);
    check(rows == A.nimgs, TextFormat("batch: %d/%d results attached", rows, A.nimgs));
    check(good == total, TextFormat("batch: %d/%d readings correct", good, total));
    return 0;
}

/* Call once per frame before input is read. Returns -1 while running, else the exit code. */
int selftest_frame(void) {
    g_now++;
    bool wheel = false;
    for (int i = 0; i < g_nq; i++) {
        if (g_q[i].frame != g_now) continue;
        switch (g_q[i].type) {
        case EV_MPOS: g_ptr = (Vector2){ (float)g_q[i].a, (float)g_q[i].b }; break;
        case EV_MB_DOWN: g_btn = true; break;
        case EV_MB_UP: g_btn = false; break;
        case EV_WHEEL: wheel = true; /* fall through */
        default: {
            AutomationEvent e = { 0 };
            e.type = (unsigned)g_q[i].type;
            e.params[0] = g_q[i].a;
            e.params[1] = g_q[i].b;
            PlayAutomationEvent(e);
        }
        }
    }
    /* Own the mouse completely while testing, so someone using the computer
       (moving, scrolling, clicking over the window) can't disturb the run. */
    PlayAutomationEvent((AutomationEvent){ .type = EV_MPOS, .params = { (int)g_ptr.x, (int)g_ptr.y } });
    PlayAutomationEvent((AutomationEvent){ .type = g_btn ? EV_MB_DOWN : EV_MB_UP, .params = { MOUSE_BUTTON_LEFT } });
    PlayAutomationEvent((AutomationEvent){ .type = EV_MB_UP, .params = { MOUSE_BUTTON_MIDDLE } });
    if (!wheel) PlayAutomationEvent((AutomationEvent){ .type = EV_WHEEL, .params = { 0, 0 } });
    if (g_wait > 0) { g_wait--; return -1; }
    g_nq = 0;

    char path[PO_MAX_PATH];
    Clay_ElementData cv = Clay_GetElementData(CLAY_ID("Canvas"));
    Vector2 cc = { cv.boundingBox.x + cv.boundingBox.width / 2, cv.boundingBox.y + cv.boundingBox.height / 2 };

    switch (g_step++) {
    case 0: {   /* run the batch */
        int *idx = malloc((size_t)A.nimgs * sizeof *idx);
        for (int i = 0; i < A.nimgs; i++) idx[i] = i;
        batch_start(idx, A.nimgs);
        free(idx);
        break;
    }
    case 1:
        if (batch_running()) { g_step--; break; }
        check(true, TextFormat("status: %s", A.status));
        batch_check();
        click(0, center(CLAY_IDI("TreeRow", node_of_img(0))));
        g_wait = 4;
        break;
    case 2:
        check(A.cur == 0, "clicking a tree row shows that screenshot");
        click(0, center(CLAY_IDI("Cell", 1 * PO_MAX_PROBES + 0)));
        g_wait = 4;
        break;
    case 3:
        check(ui_field_is((FieldRef){ F_CELL, 1, 0 }), "clicking a table cell starts editing it");
        at(0, EV_KEY_DOWN, KEY_LEFT_CONTROL, 0);
        key(1, KEY_BACKSPACE);
        at(3, EV_KEY_UP, KEY_LEFT_CONTROL, 0);
        g_wait = 5;
        break;
    case 4:
        ui_debug_type("999");
        key(0, KEY_ENTER);
        g_wait = 3;
        break;
    case 5:
        check(A.imgs[1].res && !strcmp(A.imgs[1].res->p[0].value, "999") && A.imgs[1].res->edited[0],
              "typed value + Enter is saved and marked edited");
        click(0, center(CLAY_IDI("TreeChk", node_of_img(3))));
        g_wait = 4;
        break;
    case 6:
        check(tree_selected_count() == A.nimgs - 1, "clearing one checkbox deselects one screenshot");
        click(0, center(CLAY_ID("ExportCsv")));
        g_wait = 4;
        break;
    case 7: {
        snprintf(path, sizeof path, "%s/selftest.csv", A.root);
        char *csv = read_file(path);
        int lines = 0;
        for (char *p = csv; p && *p; p++) lines += *p == '\n';
        check(csv && lines == A.nimgs, TextFormat("Export CSV writes header + %d selected rows (%d lines)", A.nimgs - 1, lines));
        check(csv && strstr(csv, "Temperature (°C)") && strstr(csv, ",999,"), "CSV keeps the Unicode header and the edit");
        free(csv);
        remove(path);
        click(0, center(CLAY_IDI("TreeChk", 0)));
        g_wait = 4;
        break;
    }
    case 8:
        check(tree_selected_count() == A.nimgs, "folder checkbox re-selects everything");
        g_saved_scale = canvas_scale();
        g_saved_center = cc;
        g_saved_pt = canvas_screen_to_img(cc);
        pos(0, cc);
        at(0, EV_KEY_DOWN, KEY_LEFT_CONTROL, 0);
        pos(1, cc); at(1, EV_WHEEL, 0, 3);
        pos(2, cc); at(2, EV_KEY_UP, KEY_LEFT_CONTROL, 0);
        g_wait = 4;
        break;
    case 9: {
        Vector2 p = canvas_screen_to_img(g_saved_center);
        check(canvas_scale() > g_saved_scale * 1.3f, TextFormat("Ctrl+scroll zooms in (%.0f%% -> %.0f%%)",
              g_saved_scale * 100, canvas_scale() * 100));
        check(fabsf(p.x - g_saved_pt.x) < 1 && fabsf(p.y - g_saved_pt.y) < 1, "zoom keeps the point under the cursor fixed");
        canvas_pan(&g_saved_px, &g_saved_py);
        at(0, EV_KEY_DOWN, KEY_SPACE, 0);
        drag(0, cc, (Vector2){ cc.x + 40, cc.y + 30 });
        at(5, EV_KEY_UP, KEY_SPACE, 0);
        g_wait = 6;
        break;
    }
    case 10: {
        float px, py;
        canvas_pan(&px, &py);
        check(fabsf(px - g_saved_px - 40) < 1.5f && fabsf(py - g_saved_py - 30) < 1.5f, "Space+drag pans by the drag distance");
        check(A.nprobes == 3, "panning does not create a box");
        key(0, KEY_ZERO);
        /* editing the table cell above switched to shot_001; go back to shot_000 */
        click(2, center(CLAY_IDI("TreeRow", node_of_img(0))));
        g_wait = 6;
        break;
    }
    case 11: {
        check(canvas_scale() < g_saved_scale * 1.01f, "0 fits the image again");
        check(A.cur == 0, "back on shot_000");
        /* draw a box around the "CH1" caption of shot_000 */
        drag(0, canvas_img_to_screen(104, 86), canvas_img_to_screen(162, 122));
        g_wait = 6;
        break;
    }
    case 12:
        check(A.nprobes == 4, "dragging on the image adds a probe box");
        check(ui_field_is((FieldRef){ F_LABEL, 3, 0 }), "the new box's header field is focused for typing");
        ui_debug_type("Channel");
        key(0, KEY_ENTER);
        g_wait = 3;
        break;
    case 13:
        check(!strcmp(A.probes[3].label, "Channel"), "typed header is applied to the box");
        click(0, center(CLAY_IDI("ModeText", 3)));
        g_wait = 4;
        break;
    case 14:
        check(A.probes[3].mode == PO_MODE_TEXT, "Text mode button switches the probe to text");
        click(0, center(CLAY_ID("TestCur")));
        g_wait = 4;
        break;
    case 15:
        if (batch_running()) { g_step--; break; }
        check(A.imgs[0].res && A.imgs[0].res->n == 4 && strstr(A.imgs[0].res->p[3].value, "CH1"),
              TextFormat("Test this image reads the new text probe ('%s')",
                         A.imgs[0].res && A.imgs[0].res->n == 4 ? A.imgs[0].res->p[3].value : ""));
        click(0, center(CLAY_ID("SaveLayout")));
        g_wait = 4;
        break;
    case 16: {
        snprintf(path, sizeof path, "%s/selftest.txt", A.layouts_dir);
        char *txt = read_file(path);
        check(txt && strstr(txt, " text Channel") && strstr(txt, "anchor 98 45 191 30 30"), "Save layout writes the layout file");
        free(txt);
        Clay_ElementData card = Clay_GetElementData(CLAY_IDI("ProbeCard", 3));
        click(0, (Vector2){ card.boundingBox.x + card.boundingBox.width - 4, card.boundingBox.y + card.boundingBox.height - 3 });
        key(4, KEY_BACKSPACE);
        g_wait = 6;
        break;
    }
    case 17:
        check(A.nprobes == 3, "selecting a box and pressing Backspace deletes it");
        click(0, center(CLAY_ID("LayoutsBtn")));
        g_wait = 4;
        break;
    case 18: {
        int i = app_layout_index("selftest");
        check(A.menu_layouts && i >= 0, "Saved layouts opens a menu listing the layout");
        if (i >= 0) click(0, center(CLAY_IDI("LayoutItem", i)));
        g_wait = 5;
        break;
    }
    case 19:
        check(A.nprobes == 4 && A.probes[3].mode == PO_MODE_TEXT && !strcmp(A.probes[3].label, "Channel"),
              "loading the saved layout restores all four boxes");
        snprintf(path, sizeof path, "%s/selftest.txt", A.layouts_dir);
        remove(path);
        g_wait = 3;
        break;
    case 20: {
        check(ui_clay_errors() == 0, TextFormat("no Clay layout errors (%d)", ui_clay_errors()));
        Image shot = LoadImageFromScreen();
        bool saved = ExportImage(shot, g_png);
        UnloadImage(shot);
        check(saved, TextFormat("screenshot %s", g_png));
        printf(g_fail ? "\nFAILED (%d)\n" : "\nPASS\n", g_fail);
        return g_fail ? 1 : 0;
    }
    }
    return -1;
}
