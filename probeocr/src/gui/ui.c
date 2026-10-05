/*
 * Clay <-> raylib glue: window, fonts, text measurement, rendering, and the
 * small widget set the app needs (Clay only does layout).
 */
#include "app.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Font g_fonts[2];

#define ARENA_SIZE   (4u << 20)
#define SCROLL_SPEED 40.0f

static char *g_arena;
static size_t g_arena_off;
static bool g_pressed, g_consumed;
static Clay_RenderCommandArray g_cmds;

/* text field focus */
static FieldRef g_focus;
static Clay_ElementId g_focus_id;
static char g_edit[1024];
static int g_cursor;          /* byte offset into g_edit */
static float g_field_scroll;  /* horizontal scroll of the focused field */

static inline Color rl(Clay_Color c) {
    return (Color){ (unsigned char)c.r, (unsigned char)c.g, (unsigned char)c.b, (unsigned char)c.a };
}

/* ---------------- per-frame strings ---------------- */

static void *arena_alloc(size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (g_arena_off + n > ARENA_SIZE) return NULL;
    void *p = g_arena + g_arena_off;
    g_arena_off += n;
    return p;
}

Clay_String fstr(const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    char *p = arena_alloc((size_t)n + 1);
    if (!p) return (Clay_String){ .length = 0, .chars = "" };
    memcpy(p, tmp, (size_t)n + 1);
    return (Clay_String){ .length = n, .chars = p };
}

Clay_String cstr(const char *s) {
    size_t n = strlen(s);
    char *p = arena_alloc(n + 1);
    if (!p) return (Clay_String){ .length = 0, .chars = "" };
    memcpy(p, s, n + 1);
    return (Clay_String){ .length = (int32_t)n, .chars = p };
}

CustomTag *ui_tag(CustomKind kind, int state, float value) {
    CustomTag *t = arena_alloc(sizeof *t);
    if (t) { t->kind = kind; t->state = state; t->value = value; }
    return t;
}

/* ---------------- fonts & text ---------------- */

static int g_codepoints[512];
static int g_ncodepoints;

static Font load_font(const char *const *paths) {
    for (; *paths; paths++) {
        if (!FileExists(*paths)) continue;
        Font f = LoadFontEx(*paths, 40, g_codepoints, g_ncodepoints);
        if (f.glyphCount > 0 && f.texture.id) {
            GenTextureMipmaps(&f.texture);
            SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
            return f;
        }
    }
    return GetFontDefault();
}

Vector2 ui_measure(const char *s, int size, int font) {
    return MeasureTextEx(g_fonts[font], s, (float)size, 0);
}

static Clay_Dimensions measure_text(Clay_StringSlice t, Clay_TextElementConfig *cfg, void *ud) {
    (void)ud;
    char buf[2048];
    int n = t.length < (int)sizeof buf - 1 ? t.length : (int)sizeof buf - 1;
    memcpy(buf, t.chars, (size_t)n);
    buf[n] = '\0';
    Vector2 v = MeasureTextEx(g_fonts[cfg->fontId], buf, cfg->fontSize, cfg->letterSpacing);
    return (Clay_Dimensions){ v.x, (float)cfg->fontSize };
}

static int g_clay_errors;
int ui_clay_errors(void) { return g_clay_errors; }

static void clay_error(Clay_ErrorData e) {
    g_clay_errors++;
    fprintf(stderr, "clay: %.*s\n", e.errorText.length, e.errorText.chars);
}

void ui_init(int width, int height) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    /* PROBEOCR_LOG=info shows raylib's startup log (OpenGL driver etc.) */
    const char *log = getenv("PROBEOCR_LOG");
    SetTraceLogLevel(log && !strcmp(log, "info") ? LOG_INFO : LOG_WARNING);
    InitWindow(width, height, "Probe OCR");
    /* Never open taller/wider than the display: the OS would shrink the window
       behind raylib's back and the top of the layout would be cut off. */
    int mon = GetCurrentMonitor();
    int mw = GetMonitorWidth(mon), mh = GetMonitorHeight(mon);
    if (mw > 0 && mh > 0 && (width > mw * 0.92f || height > mh * 0.85f)) {
        width = (int)fminf((float)width, mw * 0.92f);
        height = (int)fminf((float)height, mh * 0.85f);
        SetWindowSize(width, height);
        SetWindowPosition((mw - width) / 2, (int)((mh - height) * 0.4f));
    }
    SetWindowMinSize(900, 600);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);

    for (int c = 32; c < 127; c++) g_codepoints[g_ncodepoints++] = c;
    for (int c = 160; c < 256; c++) g_codepoints[g_ncodepoints++] = c;
    static const int extra[] = { 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                 0x2026, 0x2212, 0x2190, 0x2191, 0x2192, 0x2193, 0x00B5 };
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) g_codepoints[g_ncodepoints++] = extra[i];

    static const char *const ui_fonts[] = {
        "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", NULL };
    static const char *const mono_fonts[] = {
        "C:/Windows/Fonts/consola.ttf", "C:/Windows/Fonts/cour.ttf",
        "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
        "/System/Library/Fonts/Supplemental/Courier New.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", NULL };
    g_fonts[FONT_UI] = load_font(ui_fonts);
    g_fonts[FONT_MONO] = load_font(mono_fonts);

    Clay_SetMaxElementCount(16384);
    uint32_t mem = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem, malloc(mem));
    Clay_Initialize(arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
                    (Clay_ErrorHandler){ clay_error, 0 });
    Clay_SetMeasureTextFunction(measure_text, NULL);
    g_arena = malloc(ARENA_SIZE);
}

void ui_shutdown(void) {
    for (int i = 0; i < 2; i++)
        if (g_fonts[i].texture.id != GetFontDefault().texture.id) UnloadFont(g_fonts[i]);
    CloseWindow();
}

/* ---------------- clicks ---------------- */

bool ui_click_pending(void) { return g_pressed && !g_consumed; }
void ui_consume_click(void) { g_consumed = true; }

bool ui_take_click(Clay_ElementId id) {
    if (!g_pressed || g_consumed || !Clay_PointerOver(id)) return false;
    g_consumed = true;
    return true;
}

/* ---------------- text fields ---------------- */

static bool same_ref(FieldRef a, FieldRef b) { return a.kind == b.kind && a.a == b.a && a.b == b.b; }
bool ui_field_focused(void) { return g_focus.kind != F_NONE; }
bool ui_field_is(FieldRef ref) { return g_focus.kind != F_NONE && same_ref(g_focus, ref); }

void ui_field_blur(void) {
    if (g_focus.kind == F_NONE) return;
    FieldRef ref = g_focus;
    g_focus.kind = F_NONE;
    app_field_commit(ref, g_edit, true);
}

void ui_focus(Clay_ElementId id, FieldRef ref, const char *value) {
    ui_field_blur();
    g_focus = ref;
    g_focus_id = id;
    snprintf(g_edit, sizeof g_edit, "%s", value);
    g_cursor = (int)strlen(g_edit);
    g_field_scroll = 0;
}

static void edit_insert(const char *s) {
    size_t len = strlen(g_edit), add = strlen(s);
    if (len + add >= sizeof g_edit) return;
    memmove(g_edit + g_cursor + add, g_edit + g_cursor, len - (size_t)g_cursor + 1);
    memcpy(g_edit + g_cursor, s, add);
    g_cursor += (int)add;
}

static int prev_cp(int i) { do i--; while (i > 0 && (g_edit[i] & 0xC0) == 0x80); return i < 0 ? 0 : i; }
static int next_cp(int i) {
    int len = (int)strlen(g_edit);
    if (i >= len) return len;
    do i++; while (i < len && (g_edit[i] & 0xC0) == 0x80);
    return i;
}

static bool pressed(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

static void field_keyboard(void) {
    if (g_focus.kind == F_NONE) return;
    bool changed = false;
    bool cmd = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) ||
               IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    int c;
    while ((c = GetCharPressed()) > 0) {
        int sz;
        const char *u = CodepointToUTF8(c, &sz);
        char tmp[8] = { 0 };
        memcpy(tmp, u, (size_t)sz);
        edit_insert(tmp);
        changed = true;
    }
    if (pressed(KEY_BACKSPACE) && g_cursor > 0) {
        int from = cmd ? 0 : prev_cp(g_cursor);
        memmove(g_edit + from, g_edit + g_cursor, strlen(g_edit) - (size_t)g_cursor + 1);
        g_cursor = from;
        changed = true;
    }
    if (pressed(KEY_DELETE) && g_edit[g_cursor]) {
        int to = next_cp(g_cursor);
        memmove(g_edit + g_cursor, g_edit + to, strlen(g_edit) - (size_t)to + 1);
        changed = true;
    }
    if (pressed(KEY_LEFT)) g_cursor = cmd ? 0 : prev_cp(g_cursor);
    if (pressed(KEY_RIGHT)) g_cursor = cmd ? (int)strlen(g_edit) : next_cp(g_cursor);
    if (IsKeyPressed(KEY_HOME)) g_cursor = 0;
    if (IsKeyPressed(KEY_END)) g_cursor = (int)strlen(g_edit);
    if (cmd && IsKeyPressed(KEY_V)) {
        const char *clip = GetClipboardText();
        if (clip) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", clip);
            for (char *p = tmp; *p; p++) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
            edit_insert(tmp);
            changed = true;
        }
    }
    if (changed) app_field_commit(g_focus, g_edit, false);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) ||
        IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_TAB))
        ui_field_blur();
}

void ui_debug_type(const char *s) {
    if (g_focus.kind == F_NONE) return;
    edit_insert(s);
    app_field_commit(g_focus, g_edit, false);
}

void ui_text_field(Clay_ElementId id, FieldRef ref, const char *value,
                   const char *placeholder, float width, bool mono) {
    bool foc = ui_field_is(ref);
    if (ui_take_click(id) && !foc) {
        ui_field_blur();
        g_focus = ref;
        g_focus_id = id;
        snprintf(g_edit, sizeof g_edit, "%s", value);
        g_cursor = (int)strlen(g_edit);
        g_field_scroll = 0;
        foc = true;
    }
    const char *shown = foc ? g_edit : value;
    int font = mono ? FONT_MONO : FONT_UI;
    if (!foc) shown = ui_fit(shown, FS_BODY, font, ui_width_of(id, width > 0 ? width : 200) - 16);

    /* keep the caret inside the visible part of the field */
    float caret_x = 0;
    if (foc) {
        char pre[1024];
        memcpy(pre, g_edit, (size_t)g_cursor);
        pre[g_cursor] = '\0';
        caret_x = ui_measure(pre, FS_BODY, font).x;
        Clay_ElementData d = Clay_GetElementData(id);
        float avail = d.found ? d.boundingBox.width - 18 : 100;
        if (caret_x - g_field_scroll > avail) g_field_scroll = caret_x - avail;
        if (caret_x - g_field_scroll < 0) g_field_scroll = caret_x;
    }

    CLAY({ .id = id,
           .layout = { .sizing = { width > 0 ? CLAY_SIZING_FIXED(width) : CLAY_SIZING_GROW(0),
                                   CLAY_SIZING_FIXED(28) },
                       .padding = { 8, 8, 0, 0 },
                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = C_BG,
           .cornerRadius = CLAY_CORNER_RADIUS(5),
           .border = { .color = foc ? C_ACCENT : C_LINE, .width = CLAY_BORDER_OUTSIDE(1) },
           /* only the field being edited scrolls (and so needs clipping) */
           .clip = { .horizontal = foc, .childOffset = { foc ? -g_field_scroll : 0, 0 } } }) {
        if (shown[0])
            CLAY_TEXT(cstr(shown), CLAY_TEXT_CONFIG({ .fontId = font, .fontSize = FS_BODY,
                      .textColor = C_TEXT, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        else if (!foc && placeholder)
            CLAY_TEXT(cstr(placeholder), CLAY_TEXT_CONFIG({ .fontId = FONT_UI, .fontSize = FS_BODY,
                      .textColor = C_MUTED, .wrapMode = CLAY_TEXT_WRAP_NONE }));
        if (foc)
            CLAY({ .layout = { .sizing = { CLAY_SIZING_FIXED(1.5f), CLAY_SIZING_FIXED(17) } },
                   .backgroundColor = C_TEXT,
                   .floating = { .attachTo = CLAY_ATTACH_TO_PARENT,
                                 .clipTo = CLAY_CLIP_TO_ATTACHED_PARENT,
                                 .offset = { 8 + caret_x - g_field_scroll, 5.5f } } }) {}
    }
}

/* ---------------- widgets ---------------- */

/*
 * Clay only has room for a handful of clipping (scroll) containers, so text
 * that might overflow is shortened to fit, with an ellipsis, instead.
 */
const char *ui_fit(const char *s, int size, int font, float maxw) {
    if (maxw <= 0 || ui_measure(s, size, font).x <= maxw) return s;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    int len = (int)strlen(buf);
    while (len > 0) {
        do len--; while (len > 0 && (buf[len] & 0xC0) == 0x80);
        char tmp[1040];
        snprintf(tmp, sizeof tmp, "%.*s\u2026", len, buf);
        if (ui_measure(tmp, size, font).x <= maxw) return cstr(tmp).chars;
    }
    return "\u2026";
}

void ui_label_fit(const char *s, int size, Clay_Color color, int font, float maxw) {
    ui_label(ui_fit(s, size, font, maxw), size, color, font);
}

float ui_width_of(Clay_ElementId id, float fallback) {
    Clay_ElementData d = Clay_GetElementData(id);
    return d.found ? d.boundingBox.width : fallback;
}

void ui_label(const char *s, int size, Clay_Color color, int font) {
    CLAY_TEXT(cstr(s), CLAY_TEXT_CONFIG({ .fontId = font, .fontSize = size, .textColor = color,
                                          .wrapMode = CLAY_TEXT_WRAP_NONE }));
}

bool ui_button(Clay_ElementId id, const char *label, bool on, bool primary, bool enabled) {
    bool hov = enabled && Clay_PointerOver(id);
    Clay_Color bg = primary ? (hov ? COL(130, 172, 255) : C_ACCENT)
                  : on ? C_ACCENT_S : hov ? C_HOVER : C_PANEL;
    Clay_Color border = primary || on || hov ? C_ACCENT : C_LINE;
    Clay_Color fg = primary ? COL(255, 255, 255) : enabled ? C_TEXT : C_MUTED;
    if (!enabled) { bg = C_PANEL; border = C_LINE; }
    CLAY({ .id = id,
           .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(28) },
                       .padding = { 11, 11, 0, 0 },
                       .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } },
           .backgroundColor = bg,
           .cornerRadius = CLAY_CORNER_RADIUS(6),
           .border = { .color = border, .width = CLAY_BORDER_OUTSIDE(1) } }) {
        CLAY_TEXT(cstr(label), CLAY_TEXT_CONFIG({ .fontId = FONT_UI, .fontSize = FS_BODY,
                  .textColor = fg, .wrapMode = CLAY_TEXT_WRAP_NONE }));
    }
    return enabled && ui_take_click(id);
}

bool ui_checkbox(Clay_ElementId id, int state) {
    bool hov = Clay_PointerOver(id);
    CLAY({ .id = id,
           .layout = { .sizing = { CLAY_SIZING_FIXED(15), CLAY_SIZING_FIXED(15) } },
           .backgroundColor = state ? C_ACCENT : C_BG,
           .cornerRadius = CLAY_CORNER_RADIUS(3),
           .border = { .color = state || hov ? C_ACCENT : C_MUTED, .width = CLAY_BORDER_OUTSIDE(1) },
           .custom = { .customData = state ? ui_tag(CUSTOM_CHECK, state, 0) : NULL } }) {}
    return ui_take_click(id);
}

/* ---------------- frame & rendering ---------------- */

void ui_begin_frame(void) {
    g_arena_off = 0;
    g_pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    g_consumed = false;
    Vector2 m = GetMousePosition();
    Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });
    Clay_SetPointerState((Clay_Vector2){ m.x, m.y }, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
    /* Clicking anywhere but the focused field commits it first, so a button
       pressed in the same frame (e.g. "Open") sees the new value. */
    if (g_pressed && g_focus.kind != F_NONE && !Clay_PointerOver(g_focus_id)) ui_field_blur();
    Vector2 wheel = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(false, (Clay_Vector2){ wheel.x * SCROLL_SPEED, wheel.y * SCROLL_SPEED },
                                GetFrameTime());
    field_keyboard();
    Clay_BeginLayout();
}

void ui_end_layout(void) { g_cmds = Clay_EndLayout(); }

/* raylib's scissor isn't nestable, so keep our own stack of clip rects */
static Rectangle g_clip[32];
static int g_nclip;

static void apply_clip(void) {
    if (!g_nclip) { EndScissorMode(); return; }
    Rectangle r = g_clip[g_nclip - 1];
    BeginScissorMode((int)r.x, (int)r.y, (int)fmaxf(0, r.width), (int)fmaxf(0, r.height));
}

static void push_clip(Rectangle r) {
    if (g_nclip) {
        Rectangle t = g_clip[g_nclip - 1];
        float x0 = fmaxf(r.x, t.x), y0 = fmaxf(r.y, t.y);
        float x1 = fminf(r.x + r.width, t.x + t.width), y1 = fminf(r.y + r.height, t.y + t.height);
        r = (Rectangle){ x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0) };
    }
    if (g_nclip < 32) g_clip[g_nclip++] = r;
    apply_clip();
}

static void pop_clip(void) {
    if (g_nclip) g_nclip--;
    apply_clip();
}

static float roundness(float radius, Rectangle r) {
    float m = fminf(r.width, r.height);
    return m > 0 ? fminf(1.0f, radius * 2 / m) : 0;
}

static void draw_custom(CustomTag *t, Rectangle r) {
    switch (t->kind) {
    case CUSTOM_CANVAS:
        canvas_draw(r);
        apply_clip();   /* canvas sets its own scissor */
        break;
    case CUSTOM_CHECK:
        if (t->state == 2) {
            DrawRectangleRec((Rectangle){ r.x + 3.5f, r.y + r.height / 2 - 1, r.width - 7, 2 }, WHITE);
        } else {
            Vector2 a = { r.x + 3.5f, r.y + r.height * 0.52f }, b = { r.x + r.width * 0.42f, r.y + r.height - 4 },
                    c = { r.x + r.width - 3.5f, r.y + 4 };
            DrawLineEx(a, b, 2, WHITE);
            DrawLineEx(b, c, 2, WHITE);
        }
        break;
    case CUSTOM_CARET: {
        float cx = r.x + r.width / 2, cy = r.y + r.height / 2;
        Color col = rl(C_MUTED);
        if (t->state) DrawTriangle((Vector2){ cx - 4, cy - 2 }, (Vector2){ cx, cy + 3 }, (Vector2){ cx + 4, cy - 2 }, col);
        else DrawTriangle((Vector2){ cx - 2, cy - 4 }, (Vector2){ cx - 2, cy + 4 }, (Vector2){ cx + 3, cy }, col);
        break;
    }
    case CUSTOM_PROGRESS:
        DrawRectangleRounded(r, 1, 6, rl(C_LINE));
        DrawRectangleRounded((Rectangle){ r.x, r.y, r.width * fminf(1, fmaxf(0, t->value)), r.height }, 1, 6, rl(C_ACCENT));
        break;
    }
}

void ui_render(void) {
    BeginDrawing();
    ClearBackground(rl(C_BG));
    g_nclip = 0;
    char buf[2048];
    for (int i = 0; i < g_cmds.length; i++) {
        Clay_RenderCommand *c = Clay_RenderCommandArray_Get(&g_cmds, i);
        Clay_BoundingBox b = c->boundingBox;
        Rectangle r = { roundf(b.x), roundf(b.y), roundf(b.width), roundf(b.height) };
        switch (c->commandType) {
        case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
            Clay_RectangleRenderData *d = &c->renderData.rectangle;
            if (d->cornerRadius.topLeft > 0)
                DrawRectangleRounded(r, roundness(d->cornerRadius.topLeft, r), 6, rl(d->backgroundColor));
            else
                DrawRectangleRec(r, rl(d->backgroundColor));
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_BORDER: {
            Clay_BorderRenderData *d = &c->renderData.border;
            Clay_BorderWidth w = d->width;
            Color col = rl(d->color);
            if (d->cornerRadius.topLeft > 0 && w.left == w.right && w.left == w.top && w.left == w.bottom) {
                DrawRectangleRoundedLinesEx((Rectangle){ r.x + 0.5f, r.y + 0.5f, r.width - 1, r.height - 1 },
                                            roundness(d->cornerRadius.topLeft, r), 6, w.left, col);
            } else {
                if (w.left)   DrawRectangleRec((Rectangle){ r.x, r.y, w.left, r.height }, col);
                if (w.right)  DrawRectangleRec((Rectangle){ r.x + r.width - w.right, r.y, w.right, r.height }, col);
                if (w.top)    DrawRectangleRec((Rectangle){ r.x, r.y, r.width, w.top }, col);
                if (w.bottom) DrawRectangleRec((Rectangle){ r.x, r.y + r.height - w.bottom, r.width, w.bottom }, col);
            }
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_TEXT: {
            Clay_TextRenderData *d = &c->renderData.text;
            int n = d->stringContents.length < (int)sizeof buf - 1 ? d->stringContents.length : (int)sizeof buf - 1;
            memcpy(buf, d->stringContents.chars, (size_t)n);
            buf[n] = '\0';
            DrawTextEx(g_fonts[d->fontId], buf, (Vector2){ r.x, r.y }, d->fontSize, d->letterSpacing, rl(d->textColor));
            break;
        }
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START: push_clip(r); break;
        case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END: pop_clip(); break;
        case CLAY_RENDER_COMMAND_TYPE_CUSTOM:
            if (c->renderData.custom.customData) draw_custom(c->renderData.custom.customData, r);
            break;
        default: break;
        }
    }
    if (g_nclip) { g_nclip = 0; EndScissorMode(); }
    EndDrawing();
}
