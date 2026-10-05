/*
 * sesar-shell  —  SESAR DE en C puro (Xlib + FreeType). Sin Python, sin GTK.
 *
 * Compilar (Termux o Linux):
 *   cc -O2 -o sesar-shell sesar-shell.c $(pkg-config --cflags --libs x11 xext freetype2) -lm
 *
 * Uso:
 *   sesar-shell setup       genera ~/.jwmrc (barra abajo, tema neon, atajos)
 *   sesar-shell session     aplica recursos de xterm y ejecuta jwm
 *   sesar-shell menu        menu de inicio (se abre desde la barra o Super+Espacio)
 *   sesar-shell power       dialogo de energia / sesion      (Super+Esc)
 *   sesar-shell hud         HUD de sistema (CPU / RAM / bateria)
 *   sesar-shell wallpaper   pinta el fondo synthwave en la raiz de X
 *   sesar-shell xres        aplica el tema neon a xterm (RESOURCE_MANAGER)
 *   sesar-shell appmenu     imprime el menu de apps en XML para JWM
 *   sesar-shell files       abre el gestor de archivos disponible
 *   sesar-shell uninstall   restaura la configuracion anterior
 *
 * Variables: SESAR_SCALE (escala de UI), SESAR_FONT / SESAR_FONT_BOLD (rutas TTF),
 *            SESAR_TERM (terminal, por defecto xterm), SESAR_TRAY (alto de la barra).
 */
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/shape.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ paleta */
#define C_BG      0x04060D
#define C_FIELD   0x070C1A
#define C_PANEL1  0x0E1730
#define C_PANEL2  0x090F22
#define C_CYAN    0x00E5FF
#define C_MAGENTA 0xFF2BD6
#define C_GREEN   0x39FF88
#define C_AMBER   0xFFB020
#define C_RED     0xFF3B5C
#define C_VIOLET  0x7C4DFF
#define C_TEXT    0xE8F4FF
#define C_DIM     0xB4C4E0
#define C_MUTED   0x6F82A8
#define C_LINE    0x24365E

#define MARK "sesar-de"

static Display *dpy;
static int scr;
static float S = 1.0f;
static char self_path[1024] = "sesar-shell";
static char prefix_dir[1024] = "";
static FT_Library ftlib;

static int sc(float v) { return (int)lrintf(v * S); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clamp01(float v) { return clampf(v, 0.f, 1.f); }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static void die(const char *msg) { fprintf(stderr, "sesar-shell: %s\n", msg); exit(1); }

static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) die("sin memoria");
    return p;
}

static int file_exists(const char *p) { return access(p, F_OK) == 0; }

static int which(const char *cmd, char *out, size_t n) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char *copy = xstrdup(path), *save = NULL;
    for (char *d = strtok_r(copy, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        char full[1200];
        snprintf(full, sizeof full, "%s/%s", d, cmd);
        if (access(full, X_OK) == 0) {
            if (out) snprintf(out, n, "%s", full);
            free(copy);
            return 1;
        }
    }
    free(copy);
    return 0;
}

static void spawn_cmd(const char *cmd) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        if (dpy) close(ConnectionNumber(dpy));
        execlp("sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static int single_instance(const char *name) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char path[1100];
    snprintf(path, sizeof path, "%s/sesar-%s-%d.lock", dir, name, (int)getuid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return 1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) return 0;
    return 1; /* el fd queda abierto mientras viva el proceso */
}

static const char *terminal_cmd(void) {
    const char *t = getenv("SESAR_TERM");
    if (t && *t) return t;
    if (!which("xterm", NULL, 0) && which("aterm", NULL, 0)) return "aterm";
    return "xterm";
}

/* ------------------------------------------------------------------ canvas */
typedef struct { int w, h; uint32_t *px; int cx0, cy0, cx1, cy1; } Canvas;

static Canvas canvas_new(int w, int h) {
    Canvas c;
    c.w = w; c.h = h;
    c.px = calloc((size_t)w * (size_t)h, 4);
    if (!c.px) die("sin memoria para el canvas");
    c.cx0 = 0; c.cy0 = 0; c.cx1 = w; c.cy1 = h;
    return c;
}

static void clip_reset(Canvas *c) { c->cx0 = 0; c->cy0 = 0; c->cx1 = c->w; c->cy1 = c->h; }
static void clip_set(Canvas *c, int x, int y, int w, int h) {
    c->cx0 = imax(0, x); c->cy0 = imax(0, y);
    c->cx1 = imin(c->w, x + w); c->cy1 = imin(c->h, y + h);
}

static uint32_t mix(uint32_t d, uint32_t s, float a) {
    if (a <= 0.f) return d;
    if (a >= 1.f) return s;
    int dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
    int sr = (s >> 16) & 255, sg = (s >> 8) & 255, sb = s & 255;
    int r = dr + (int)lrintf((sr - dr) * a);
    int g = dg + (int)lrintf((sg - dg) * a);
    int b = db + (int)lrintf((sb - db) * a);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t lerp_col(uint32_t a, uint32_t b, float t) { return mix(a, b, clamp01(t)); }

static void put(Canvas *c, int x, int y, uint32_t rgb, float a) {
    if (x < c->cx0 || y < c->cy0 || x >= c->cx1 || y >= c->cy1) return;
    uint32_t *p = &c->px[(size_t)y * c->w + x];
    *p = mix(*p, rgb, a);
}

static void fill_rect(Canvas *c, int x, int y, int w, int h, uint32_t rgb, float a) {
    int x0 = imax(x, c->cx0), y0 = imax(y, c->cy0);
    int x1 = imin(x + w, c->cx1), y1 = imin(y + h, c->cy1);
    for (int iy = y0; iy < y1; iy++)
        for (int ix = x0; ix < x1; ix++) {
            uint32_t *p = &c->px[(size_t)iy * c->w + ix];
            *p = mix(*p, rgb, a);
        }
}

/* ---- formas con distancia con signo (bordes suaves, esquinas biseladas) ---- */
typedef struct {
    float cut;
    uint32_t top, bot; float fa;       /* relleno vertical */
    uint32_t stroke; float sa, sw;     /* borde interior */
    uint32_t glow; float ga, gw;       /* brillo interior */
    uint32_t accent; float aw;         /* diagonales de acento */
} Shape;

static float sdf_chamfer(float px, float py, float x, float y, float w, float h, float cut) {
    float dl = x - px, dr = px - (x + w), dt = y - py, db = py - (y + h);
    float d = fmaxf(fmaxf(dl, dr), fmaxf(dt, db));
    float tl = (cut - ((px - x) + (py - y))) * 0.70710678f;
    float br = (cut - (((x + w) - px) + ((y + h) - py))) * 0.70710678f;
    return fmaxf(d, fmaxf(tl, br));
}

static void draw_chamfer(Canvas *c, float x, float y, float w, float h, const Shape *st) {
    int x0 = imax((int)floorf(x) - 1, c->cx0), y0 = imax((int)floorf(y) - 1, c->cy0);
    int x1 = imin((int)ceilf(x + w) + 1, c->cx1), y1 = imin((int)ceilf(y + h) + 1, c->cy1);
    float cut = fminf(st->cut, fminf(w, h) * 0.4f);
    for (int iy = y0; iy < y1; iy++) {
        for (int ix = x0; ix < x1; ix++) {
            float px = ix + 0.5f, py = iy + 0.5f;
            float d = sdf_chamfer(px, py, x, y, w, h, cut);
            if (d > 1.0f) continue;
            float t = clamp01((py - y) / h);
            if (st->fa > 0.f) {
                float cov = clamp01(0.5f - d);
                if (cov > 0.f) put(c, ix, iy, lerp_col(st->top, st->bot, t), cov * st->fa);
            }
            if (st->glow && st->ga > 0.f && d < 0.f) {
                float g = 1.0f + d / st->gw;
                if (g > 0.f) put(c, ix, iy, st->glow, g * g * st->ga);
            }
            if (st->sw > 0.f && st->sa > 0.f) {
                float cov = clamp01(0.5f - (fabsf(d + st->sw * 0.5f) - st->sw * 0.5f));
                if (cov > 0.f) put(c, ix, iy, st->stroke, cov * st->sa);
            }
            if (st->accent && st->aw > 0.f) {
                if (px <= x + cut + 1 && py <= y + cut + 1) {
                    float tl = (cut - ((px - x) + (py - y))) * 0.70710678f;
                    float cov = clamp01(0.5f - (fabsf(tl) - st->aw * 0.5f));
                    if (cov > 0.f) put(c, ix, iy, st->accent, cov);
                }
                if (px >= x + w - cut - 1 && py >= y + h - cut - 1) {
                    float br = (cut - (((x + w) - px) + ((y + h) - py))) * 0.70710678f;
                    float cov = clamp01(0.5f - (fabsf(br) - st->aw * 0.5f));
                    if (cov > 0.f) put(c, ix, iy, st->accent, cov);
                }
            }
        }
    }
}

static float sdf_hex(float dx, float dy, float a) {
    dx = fabsf(dx); dy = fabsf(dy);
    return fmaxf(dx, dy * 0.8660254f + dx * 0.5f) - a;
}

/* hexagono (vertice arriba) de radio R */
static void draw_hex(Canvas *c, float cx, float cy, float R, uint32_t col,
                     float fill_a, float sw, float stroke_a, float glow_a, float gw) {
    float a = R * 0.8660254f;
    int x0 = imax((int)floorf(cx - R - gw) - 1, c->cx0), x1 = imin((int)ceilf(cx + R + gw) + 1, c->cx1);
    int y0 = imax((int)floorf(cy - R - gw) - 1, c->cy0), y1 = imin((int)ceilf(cy + R + gw) + 1, c->cy1);
    for (int iy = y0; iy < y1; iy++)
        for (int ix = x0; ix < x1; ix++) {
            float d = sdf_hex(ix + 0.5f - cx, iy + 0.5f - cy, a);
            if (d > gw) continue;
            if (d > 0.f) {
                if (glow_a > 0.f) { float g = 1.f - d / gw; put(c, ix, iy, col, g * g * glow_a); }
                float cov = clamp01(0.5f - d);
                if (cov > 0.f) put(c, ix, iy, col, cov * stroke_a);
                continue;
            }
            float cov = clamp01(0.5f - d);
            if (fill_a > 0.f) put(c, ix, iy, col, cov * fill_a);
            if (glow_a > 0.f) { float g = clamp01(1.f + d / gw); put(c, ix, iy, col, g * g * glow_a * 0.6f); }
            float sc2 = clamp01(0.5f - (fabsf(d + sw * 0.5f) - sw * 0.5f));
            if (sc2 > 0.f) put(c, ix, iy, col, sc2 * stroke_a);
        }
}

static void draw_line_a(Canvas *c, float x0, float y0, float x1, float y1, float w,
                        uint32_t rgb, float a0, float a1) {
    float dx = x1 - x0, dy = y1 - y0;
    if (fabsf(dx) >= fabsf(dy)) {
        if (x1 < x0) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; t = a0; a0 = a1; a1 = t; }
        dx = x1 - x0; dy = y1 - y0;
        float slope = dx != 0.f ? dy / dx : 0.f;
        float we = w * sqrtf(1.f + slope * slope);
        int xs = imax((int)floorf(x0), c->cx0), xe = imin((int)ceilf(x1), c->cx1 - 1);
        for (int x = xs; x <= xe; x++) {
            float cxp = x + 0.5f;
            float t = dx != 0.f ? clamp01((cxp - x0) / dx) : 0.f;
            float cy = y0 + slope * (cxp - x0);
            float a = a0 + (a1 - a0) * t;
            float top = cy - we * 0.5f, bot = cy + we * 0.5f;
            for (int y = (int)floorf(top); y <= (int)floorf(bot); y++) {
                float cov = fminf(bot, y + 1.f) - fmaxf(top, (float)y);
                if (cov > 0.f) put(c, x, y, rgb, clamp01(cov) * a);
            }
        }
    } else {
        if (y1 < y0) { float t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; t = a0; a0 = a1; a1 = t; }
        dx = x1 - x0; dy = y1 - y0;
        float slope = dy != 0.f ? dx / dy : 0.f;
        float we = w * sqrtf(1.f + slope * slope);
        int ys = imax((int)floorf(y0), c->cy0), ye = imin((int)ceilf(y1), c->cy1 - 1);
        for (int y = ys; y <= ye; y++) {
            float cyp = y + 0.5f;
            float t = dy != 0.f ? clamp01((cyp - y0) / dy) : 0.f;
            float cx = x0 + slope * (cyp - y0);
            float a = a0 + (a1 - a0) * t;
            float left = cx - we * 0.5f, right = cx + we * 0.5f;
            for (int x = (int)floorf(left); x <= (int)floorf(right); x++) {
                float cov = fminf(right, x + 1.f) - fmaxf(left, (float)x);
                if (cov > 0.f) put(c, x, y, rgb, clamp01(cov) * a);
            }
        }
    }
}

/* ------------------------------------------------------------------ fuentes */
typedef struct {
    uint32_t cp; int used;
    int w, h, left, top, adv;
    unsigned char *bmp;
} GlyphC;

typedef struct {
    FT_Face face;
    int px, ascent, height, faux;
    GlyphC cache[1024];
} SFont;

static int utf8_next(const char **sp) {
    const unsigned char *s = (const unsigned char *)*sp;
    int cp, n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); n = 2; }
    else if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); n = 3;
    } else if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); n = 4;
    } else { cp = s[0]; n = 1; }
    *sp += n;
    return cp;
}

static int utf8_put(char *out, int cp) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static SFont *font_open(const char *path, int px, int faux) {
    SFont *f = calloc(1, sizeof *f);
    if (!f) die("sin memoria");
    if (FT_New_Face(ftlib, path, 0, &f->face)) { free(f); return NULL; }
    if (px < 6) px = 6;
    FT_Set_Pixel_Sizes(f->face, 0, (FT_UInt)px);
    f->px = px;
    f->ascent = (int)((f->face->size->metrics.ascender + 63) >> 6);
    int desc = (int)((-f->face->size->metrics.descender + 63) >> 6);
    f->height = f->ascent + desc;
    f->faux = faux;
    return f;
}

static GlyphC *glyph_get(SFont *f, uint32_t cp) {
    unsigned i = cp & 1023;
    for (int n = 0; n < 1024; n++, i = (i + 1) & 1023) {
        GlyphC *g = &f->cache[i];
        if (g->used && g->cp == cp) return g;
        if (!g->used) {
            g->used = 1; g->cp = cp;
            if (FT_Load_Char(f->face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
                FT_GlyphSlot s = f->face->glyph;
                g->w = (int)s->bitmap.width; g->h = (int)s->bitmap.rows;
                g->left = s->bitmap_left; g->top = s->bitmap_top;
                g->adv = (int)((s->advance.x + 32) >> 6);
                if (g->w && g->h) {
                    g->bmp = malloc((size_t)g->w * (size_t)g->h);
                    if (g->bmp)
                        for (int r = 0; r < g->h; r++)
                            memcpy(g->bmp + (size_t)r * g->w, s->bitmap.buffer + (size_t)r * s->bitmap.pitch, (size_t)g->w);
                }
            }
            return g;
        }
    }
    return NULL;
}

static int text_width(SFont *f, const char *s, float sp) {
    int w = 0, n = 0;
    while (*s) {
        GlyphC *g = glyph_get(f, (uint32_t)utf8_next(&s));
        if (g) w += g->adv;
        n++;
    }
    return w + (int)lrintf(sp * (n > 0 ? n - 1 : 0));
}

static void draw_text(Canvas *c, SFont *f, int x, int y, const char *s, uint32_t rgb,
                      float a, float sp, int maxw) {
    char buf[512];
    if (maxw > 0 && text_width(f, s, sp) > maxw) {
        int dots = text_width(f, "...", sp), acc = 0, len = 0;
        const char *p = s;
        while (*p) {
            const char *q = p;
            int cp = utf8_next(&q);
            GlyphC *g = glyph_get(f, (uint32_t)cp);
            int adv = (g ? g->adv : 0) + (int)lrintf(sp);
            if (acc + adv + dots > maxw || len + 8 >= (int)sizeof buf) break;
            memcpy(buf + len, p, (size_t)(q - p)); len += (int)(q - p);
            acc += adv; p = q;
        }
        memcpy(buf + len, "...", 4);
        s = buf;
    }
    int pen = x, base = y + f->ascent;
    while (*s) {
        GlyphC *g = glyph_get(f, (uint32_t)utf8_next(&s));
        if (!g) continue;
        if (g->bmp) {
            for (int pass = 0; pass <= (f->faux ? 1 : 0); pass++)
                for (int gy = 0; gy < g->h; gy++)
                    for (int gx = 0; gx < g->w; gx++) {
                        unsigned char v = g->bmp[(size_t)gy * g->w + gx];
                        if (v) put(c, pen + g->left + gx + pass, base - g->top + gy, rgb, (v / 255.f) * a);
                    }
        }
        pen += g->adv + (int)lrintf(sp);
    }
}

static void draw_text_c(Canvas *c, SFont *f, int cx, int y, const char *s, uint32_t rgb,
                        float a, float sp) {
    draw_text(c, f, cx - text_width(f, s, sp) / 2, y, s, rgb, a, sp, 0);
}

static int scan_font(const char *dir, const char *const *names, char *out, size_t n, int depth) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0 && scan_font(full, names, out, n, depth - 1)) found = 1;
        } else {
            for (int i = 0; names[i]; i++)
                if (strcmp(e->d_name, names[i]) == 0) { snprintf(out, n, "%s", full); found = 1; break; }
        }
    }
    closedir(d);
    return found;
}

static int find_font(int bold, char *out, size_t n) {
    const char *env = getenv(bold ? "SESAR_FONT_BOLD" : "SESAR_FONT");
    if (env && file_exists(env)) { snprintf(out, n, "%s", env); return 1; }
    static const char *reg[] = {"sesar.ttf", "DejaVuSansMono.ttf", "JetBrainsMono-Regular.ttf",
                                "RobotoMono-Regular.ttf", "DroidSansMono.ttf", "CutiveMono.ttf", NULL};
    static const char *bld[] = {"sesar-bold.ttf", "DejaVuSansMono-Bold.ttf", "JetBrainsMono-Bold.ttf",
                                "RobotoMono-Bold.ttf", NULL};
    const char *const *names = bold ? bld : reg;
    char dirs[4][1100];
    snprintf(dirs[0], sizeof dirs[0], "%s/share/sesar", prefix_dir);
    snprintf(dirs[1], sizeof dirs[1], "%s/share/fonts", prefix_dir);
    snprintf(dirs[2], sizeof dirs[2], "/usr/share/fonts");
    snprintf(dirs[3], sizeof dirs[3], "/system/fonts");
    for (int i = 0; i < 4; i++)
        if (scan_font(dirs[i], names, out, n, 4)) return 1;
    return 0;
}

typedef struct { SFont *ui, *uib, *sm, *smb, *title; } Fonts;

static Fonts *fonts_open(void) {
    char rp[1400], bp[1400];
    int hr = find_font(0, rp, sizeof rp), hb = find_font(1, bp, sizeof bp);
    if (!hr && !hb) die("no encuentro ninguna fuente TTF (definí SESAR_FONT=/ruta/fuente.ttf)");
    if (!hr) snprintf(rp, sizeof rp, "%s", bp);
    int faux = !hb;
    if (!hb) snprintf(bp, sizeof bp, "%s", rp);
    Fonts *F = calloc(1, sizeof *F);
    F->ui = font_open(rp, sc(13), 0);
    F->uib = font_open(bp, sc(13), faux);
    F->sm = font_open(rp, sc(11), 0);
    F->smb = font_open(bp, sc(11), faux);
    F->title = font_open(bp, sc(19), faux);
    if (!F->ui || !F->uib || !F->sm || !F->smb || !F->title) die("no se pudo abrir la fuente");
    return F;
}

/* ------------------------------------------------------------------ ventana X */
typedef struct {
    Window win;
    int w, h;
    Canvas cv;
    XImage *img;
    GC gc;
    uint32_t *conv;
} Win;

static Visual *vis;
static int depth, std_masks = 1;

static void x_init(void) {
    vis = DefaultVisual(dpy, scr);
    depth = DefaultDepth(dpy, scr);
    std_masks = (vis->red_mask == 0xFF0000 && vis->green_mask == 0x00FF00 && vis->blue_mask == 0x0000FF);
}

static Win *win_create(int x, int y, int w, int h, int override, const char *name) {
    Win *W = calloc(1, sizeof *W);
    XSetWindowAttributes a;
    memset(&a, 0, sizeof a);
    a.override_redirect = override ? True : False;
    a.background_pixel = C_BG;
    a.event_mask = ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask |
                   PointerMotionMask | StructureNotifyMask;
    W->win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y, (unsigned)w, (unsigned)h, 0, depth,
                           InputOutput, vis, CWOverrideRedirect | CWBackPixel | CWEventMask, &a);
    if (name) {
        XStoreName(dpy, W->win, name);
        XClassHint ch = {(char *)name, (char *)"Sesar-shell"};
        XSetClassHint(dpy, W->win, &ch);
    }
    W->w = w; W->h = h;
    W->cv = canvas_new(w, h);
    if (!std_masks) W->conv = calloc((size_t)w * (size_t)h, 4);
    W->img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0,
                          (char *)(std_masks ? W->cv.px : W->conv), (unsigned)w, (unsigned)h, 32, 0);
    W->gc = XCreateGC(dpy, W->win, 0, NULL);
    return W;
}

static unsigned mask_shift(unsigned long m) { unsigned s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; }
static unsigned mask_bits(unsigned long m) { unsigned n = 0; while (m & 1) { m >>= 1; n++; } return n; }

static void win_present(Win *W) {
    if (!std_masks) {
        unsigned rs = mask_shift(vis->red_mask), gs = mask_shift(vis->green_mask), bs = mask_shift(vis->blue_mask);
        unsigned rb = mask_bits(vis->red_mask >> rs), gb = mask_bits(vis->green_mask >> gs), bb = mask_bits(vis->blue_mask >> bs);
        for (size_t i = 0; i < (size_t)W->w * (size_t)W->h; i++) {
            uint32_t p = W->cv.px[i];
            uint32_t r = ((p >> 16) & 255) >> (8 - rb), g = ((p >> 8) & 255) >> (8 - gb), b = (p & 255) >> (8 - bb);
            W->conv[i] = (r << rs) | (g << gs) | (b << bs);
        }
    }
    XPutImage(dpy, W->win, W->gc, W->img, 0, 0, 0, 0, (unsigned)W->w, (unsigned)W->h);
    XFlush(dpy);
}

static void win_shape_chamfer(Win *W, int cut) {
    int ev, er;
    if (!XShapeQueryExtension(dpy, &ev, &er) || cut < 2) return;
    int w = W->w, h = W->h;
    XRectangle *r = malloc(sizeof(XRectangle) * (size_t)(2 * cut + 2));
    int n = 0;
    for (int y = 0; y < cut; y++) {
        r[n].x = (short)(cut - y); r[n].y = (short)y; r[n].width = (unsigned short)(w - (cut - y)); r[n].height = 1; n++;
    }
    r[n].x = 0; r[n].y = (short)cut; r[n].width = (unsigned short)w; r[n].height = (unsigned short)(h - 2 * cut); n++;
    for (int k = 0; k < cut; k++) {
        r[n].x = 0; r[n].y = (short)(h - cut + k); r[n].width = (unsigned short)(w - k); r[n].height = 1; n++;
    }
    XShapeCombineRectangles(dpy, W->win, ShapeBounding, 0, 0, r, n, ShapeSet, YXBanded);
    free(r);
}

static void grab_input(Win *W) {
    for (int i = 0; i < 200; i++) {
        if (XGrabKeyboard(dpy, W->win, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess) break;
        usleep(10000);
    }
    XGrabPointer(dpy, W->win, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
}

/* ------------------------------------------------------------------ bucle de eventos */
typedef struct App App;
struct App {
    Win *w;
    void *u;
    int tick_ms, dirty, quit;
    void (*draw)(App *);
    void (*key)(App *, KeySym ks, int cp, unsigned state);
    void (*button)(App *, int x, int y, int button, int press);
    void (*motion)(App *, int x, int y);
    void (*tick)(App *);
};

static int keysym_cp(KeySym ks) {
    if (ks >= 0x20 && ks < 0x7F) return (int)ks;
    if (ks >= 0xA0 && ks <= 0xFF) return (int)ks;
    if ((ks & 0xFF000000UL) == 0x01000000UL) return (int)(ks & 0x00FFFFFFUL);
    return 0;
}

static void run_app(App *a) {
    int fd = ConnectionNumber(dpy);
    a->dirty = 1;
    while (!a->quit) {
        while (XPending(dpy) && !a->quit) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case Expose: if (ev.xexpose.count == 0) a->dirty = 1; break;
            case KeyPress: {
                char buf[32];
                KeySym ks = NoSymbol;
                XLookupString(&ev.xkey, buf, sizeof buf, &ks, NULL);
                if (a->key) a->key(a, ks, (ev.xkey.state & (ControlMask | Mod1Mask)) ? 0 : keysym_cp(ks), ev.xkey.state);
                break;
            }
            case ButtonPress:
                if (a->button) a->button(a, ev.xbutton.x, ev.xbutton.y, (int)ev.xbutton.button, 1);
                break;
            case ButtonRelease:
                if (a->button) a->button(a, ev.xbutton.x, ev.xbutton.y, (int)ev.xbutton.button, 0);
                break;
            case MotionNotify:
                if (a->motion) a->motion(a, ev.xmotion.x, ev.xmotion.y);
                break;
            default: break;
            }
        }
        if (a->quit) break;
        if (a->dirty) { a->draw(a); win_present(a->w); a->dirty = 0; }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { a->tick_ms / 1000, (a->tick_ms % 1000) * 1000 };
        int r = select(fd + 1, &fds, NULL, NULL, a->tick_ms > 0 ? &tv : NULL);
        if (r == 0 && a->tick) a->tick(a);
    }
}

/* ------------------------------------------------------------------ widgets comunes */
typedef struct { int x, y, w, h; } Rect;
static int in_rect(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static Shape panel_shape(void) {
    Shape s;
    memset(&s, 0, sizeof s);
    s.cut = (float)sc(18); s.top = C_PANEL1; s.bot = C_PANEL2; s.fa = 0.98f;
    s.stroke = C_CYAN; s.sa = 0.60f; s.sw = fmaxf(1.f, S * 1.2f);
    s.glow = C_CYAN; s.ga = 0.30f; s.gw = (float)sc(12);
    s.accent = C_CYAN; s.aw = fmaxf(2.f, S * 2.5f);
    return s;
}

enum { BTN_NORMAL, BTN_DANGER, BTN_PRIMARY };

static void draw_button(Canvas *c, Fonts *F, Rect r, const char *label, int style, int hover) {
    Shape s;
    memset(&s, 0, sizeof s);
    uint32_t col = style == BTN_DANGER ? C_RED : C_CYAN, txt = col;
    s.cut = (float)sc(9); s.sw = fmaxf(1.f, S * 1.4f);
    s.stroke = col; s.sa = 0.85f;
    if (style == BTN_PRIMARY) {
        s.top = hover ? 0x8AF4FF : 0x00E5FF; s.bot = hover ? 0x35C8E8 : 0x009BC7; s.fa = 1.f; txt = C_BG;
    } else {
        s.top = hover ? col : 0x0E1A33; s.bot = hover ? col : 0x0A1226; s.fa = hover ? 0.28f : 1.f;
    }
    draw_chamfer(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, &s);
    draw_text_c(c, F->uib, r.x + r.w / 2, r.y + (r.h - F->uib->height) / 2, label, txt, 1.f, S);
}

/* ------------------------------------------------------------------ aplicaciones (.desktop) */
static const char *CAT_NAMES[] = {"Internet", "Oficina", "Multimedia", "Gráficos", "Desarrollo",
                                  "Juegos", "Sistema", "Accesorios", "Otros"};
#define NCATS 9

typedef struct {
    char *name, *lname, *exec, *comment;
    unsigned cats;
    int term;
    uint32_t color;
} AppEntry;

static AppEntry *apps;
static int napps, capps;
static char **seen_ids;
static int nseen, cseen;

static unsigned cat_bits(const char *cats) {
    unsigned b = 0;
    char *copy = xstrdup(cats), *save = NULL;
    for (char *t = strtok_r(copy, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        if (!strcmp(t, "Network") || !strcmp(t, "WebBrowser") || !strcmp(t, "Email")) b |= 1u << 0;
        else if (!strcmp(t, "Office")) b |= 1u << 1;
        else if (!strcmp(t, "AudioVideo") || !strcmp(t, "Audio") || !strcmp(t, "Video")) b |= 1u << 2;
        else if (!strcmp(t, "Graphics")) b |= 1u << 3;
        else if (!strcmp(t, "Development")) b |= 1u << 4;
        else if (!strcmp(t, "Game")) b |= 1u << 5;
        else if (!strcmp(t, "System") || !strcmp(t, "Settings") || !strcmp(t, "PackageManager")) b |= 1u << 6;
        else if (!strcmp(t, "Utility")) b |= 1u << 7;
    }
    free(copy);
    return b ? b : (1u << 8);
}

static void clean_exec(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < n; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%') { out[o++] = '%'; }
            p++;
            continue;
        }
        out[o++] = *p;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

static int id_seen(const char *id) {
    for (int i = 0; i < nseen; i++) if (!strcmp(seen_ids[i], id)) return 1;
    if (nseen == cseen) { cseen = cseen ? cseen * 2 : 64; seen_ids = realloc(seen_ids, (size_t)cseen * sizeof *seen_ids); }
    seen_ids[nseen++] = xstrdup(id);
    return 0;
}

static void lang_code(char *out, size_t n) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    out[0] = 0;
    if (l && strlen(l) >= 2 && isalpha((unsigned char)l[0]) && isalpha((unsigned char)l[1])) {
        snprintf(out, n, "Name[%c%c]", l[0], l[1]);
    }
}

static void load_desktop_file(const char *path, const char *id) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048], name[256] = "", namel[256] = "", exec[1024] = "", comment[256] = "", cats[512] = "";
    char lkey[16];
    lang_code(lkey, sizeof lkey);
    int in = 0, term = 0, nodisp = 0, hidden = 0, isapp = 1;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (line[0] == '[') { in = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "Name")) snprintf(name, sizeof name, "%s", v);
        else if (lkey[0] && !strcmp(k, lkey)) snprintf(namel, sizeof namel, "%s", v);
        else if (!strcmp(k, "Exec")) snprintf(exec, sizeof exec, "%s", v);
        else if (!strcmp(k, "Comment")) snprintf(comment, sizeof comment, "%s", v);
        else if (!strcmp(k, "Categories")) snprintf(cats, sizeof cats, "%s", v);
        else if (!strcmp(k, "Terminal")) term = !strcmp(v, "true");
        else if (!strcmp(k, "NoDisplay")) nodisp = !strcmp(v, "true");
        else if (!strcmp(k, "Hidden")) hidden = !strcmp(v, "true");
        else if (!strcmp(k, "Type")) isapp = !strcmp(v, "Application");
    }
    fclose(f);
    if (!isapp || nodisp || hidden || !exec[0] || !(namel[0] || name[0])) return;
    if (id_seen(id)) return;
    if (napps == capps) { capps = capps ? capps * 2 : 64; apps = realloc(apps, (size_t)capps * sizeof *apps); }
    AppEntry *a = &apps[napps++];
    char cl[1024];
    clean_exec(exec, cl, sizeof cl);
    a->name = xstrdup(namel[0] ? namel : name);
    a->lname = xstrdup(a->name);
    for (char *p = a->lname; *p; p++) *p = (char)tolower((unsigned char)*p);
    a->exec = xstrdup(cl);
    a->comment = xstrdup(comment);
    a->cats = cat_bits(cats);
    a->term = term;
    uint32_t pal[] = {C_CYAN, C_MAGENTA, C_GREEN, C_AMBER, C_VIOLET, 0x4D7CFF};
    unsigned h = 5381;
    for (const char *p = a->name; *p; p++) h = h * 33 + (unsigned char)*p;
    a->color = pal[h % 6];
}

static void scan_apps_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 9 || strcmp(e->d_name + L - 8, ".desktop") != 0) continue;
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        load_desktop_file(full, e->d_name);
    }
    closedir(d);
}

static int app_cmp(const void *a, const void *b) {
    return strcmp(((const AppEntry *)a)->lname, ((const AppEntry *)b)->lname);
}

static void load_apps(void) {
    char dir[1400];
    const char *home = getenv("HOME"), *xh = getenv("XDG_DATA_HOME"), *xd = getenv("XDG_DATA_DIRS");
    if (xh && *xh) snprintf(dir, sizeof dir, "%s/applications", xh);
    else snprintf(dir, sizeof dir, "%s/.local/share/applications", home ? home : "");
    scan_apps_dir(dir);
    if (xd && *xd) {
        char *copy = xstrdup(xd), *save = NULL;
        for (char *t = strtok_r(copy, ":", &save); t; t = strtok_r(NULL, ":", &save)) {
            snprintf(dir, sizeof dir, "%s/applications", t);
            scan_apps_dir(dir);
        }
        free(copy);
    }
    snprintf(dir, sizeof dir, "%s/share/applications", prefix_dir);
    scan_apps_dir(dir);
    scan_apps_dir("/usr/share/applications");
    qsort(apps, (size_t)napps, sizeof *apps, app_cmp);
}

static void launch_app_ex(const AppEntry *a);
static void launch_app(const AppEntry *a) { launch_app_ex(a); }

/* ------------------------------------------------------------------ acciones */
enum { ACT_TERM, ACT_FILES, ACT_RESTART, ACT_LOGOUT };

static int files_cmd(char *out, size_t n) {
    static const char *c[] = {"sesar-files", "pcmanfm", "thunar", "xfe", "rox", "nemo", "caja", NULL};
    for (int i = 0; c[i]; i++)
        if (which(c[i], NULL, 0)) { snprintf(out, n, "%s", c[i]); return 1; }
    return 0;
}

static void run_action(int act) {
    char buf[300];
    switch (act) {
    case ACT_TERM: spawn_cmd(terminal_cmd()); break;
    case ACT_FILES: if (files_cmd(buf, sizeof buf)) spawn_cmd(buf); break;
    case ACT_RESTART: spawn_cmd("jwm -restart"); break;
    case ACT_LOGOUT: spawn_cmd("jwm -exit"); break;
    }
}

/* ------------------------------------------------------------------ MENU DE INICIO */
typedef struct {
    Fonts *F;
    int W, H, pad, rowH, listX, listY, listW, listH, footY, rightX, rightW, searchY, searchH;
    char query[128];
    int qlen, cat, sel, scroll, hover, blink;
    int *flt, nflt;
    const char *tabLabel[NCATS + 1];
    int tabCat[NCATS + 1], ntabs;
    Rect tabR[NCATS + 1], searchR, infoR;
    Rect btnR[4];
    const char *btnLabel[4];
    int btnAct[4], btnStyle[4], nbtn;
    int press, px, py, scroll0, moved;
} Menu;

static int menu_matches(Menu *m, const AppEntry *a) {
    if (m->cat >= 0 && !((a->cats >> m->cat) & 1u)) return 0;
    if (m->qlen == 0) return 1;
    return strcasestr(a->lname, m->query) != NULL || strcasestr(a->comment, m->query) != NULL;
}

static int menu_maxscroll(Menu *m) { return imax(0, m->nflt * m->rowH - m->listH); }

static void menu_refilter(Menu *m) {
    m->nflt = 0;
    for (int i = 0; i < napps; i++)
        if (menu_matches(m, &apps[i])) m->flt[m->nflt++] = i;
    m->sel = 0;
    m->scroll = 0;
}

static void menu_ensure_visible(Menu *m) {
    int top = m->sel * m->rowH;
    if (top < m->scroll) m->scroll = top;
    if (top + m->rowH > m->scroll + m->listH) m->scroll = top + m->rowH - m->listH;
    m->scroll = imax(0, imin(m->scroll, menu_maxscroll(m)));
}

static void menu_layout(Menu *m) {
    m->pad = sc(16);
    m->rowH = sc(48);
    {
        int Rw = sc(15);
        int titleX = m->pad + Rw * 2 + sc(12);
        int titleW = text_width(m->F->title, "SESAR // INICIO", S * 2.f);
        m->infoR = (Rect){titleX + titleW + sc(14), m->pad + sc(2), sc(26), sc(26)};
    }
    m->rightW = m->W >= sc(560) ? sc(210) : 0;
    int leftW = m->W - m->pad * 2 - (m->rightW ? m->rightW + m->pad : 0);
    m->listX = m->pad;
    m->listW = leftW;
    m->rightX = m->W - m->pad - m->rightW;
    int y = m->pad + sc(34) + sc(8);
    m->searchY = y;
    m->searchH = sc(38);
    m->searchR = (Rect){m->pad, y, leftW, m->searchH};
    y += m->searchH + sc(10);

    unsigned present = 0;
    for (int i = 0; i < napps; i++) present |= apps[i].cats;
    m->ntabs = 0;
    m->tabLabel[m->ntabs] = "TODO"; m->tabCat[m->ntabs++] = -1;
    for (int c = 0; c < NCATS; c++)
        if ((present >> c) & 1u) { m->tabLabel[m->ntabs] = CAT_NAMES[c]; m->tabCat[m->ntabs++] = c; }
    int x = m->pad, rowh = sc(26), gap = sc(6), rows = 1;
    for (int i = 0; i < m->ntabs; i++) {
        int tw = text_width(m->F->smb, m->tabLabel[i], S * 0.5f) + sc(20);
        if (x + tw > m->pad + leftW && x > m->pad) { x = m->pad; y += rowh + gap; rows++; }
        m->tabR[i] = (Rect){x, y, tw, rowh};
        x += tw + gap;
    }
    y += rowh + sc(10);
    m->footY = m->H - m->pad - sc(16);
    m->listY = y;
    m->listH = m->footY - y - sc(6);

    m->nbtn = 0;
    int bx = m->rightX, bw = m->rightW, bh = sc(40), by = m->pad + sc(34) + sc(8) + sc(22);
    if (m->rightW) {
        char tmp[300];
        by = m->searchY + sc(24);
        m->btnLabel[m->nbtn] = "TERMINAL"; m->btnAct[m->nbtn] = ACT_TERM; m->btnStyle[m->nbtn] = BTN_NORMAL;
        m->btnR[m->nbtn++] = (Rect){bx, by, bw, bh}; by += bh + sc(8);
        if (files_cmd(tmp, sizeof tmp)) {
            m->btnLabel[m->nbtn] = "ARCHIVOS"; m->btnAct[m->nbtn] = ACT_FILES; m->btnStyle[m->nbtn] = BTN_NORMAL;
            m->btnR[m->nbtn++] = (Rect){bx, by, bw, bh}; by += bh + sc(8);
        }
        int sy = m->footY - sc(8) - bh * 2 - sc(8);
        m->btnLabel[m->nbtn] = "REINICIAR ESCRITORIO"; m->btnAct[m->nbtn] = ACT_RESTART; m->btnStyle[m->nbtn] = BTN_NORMAL;
        m->btnR[m->nbtn++] = (Rect){bx, sy, bw, bh};
        m->btnLabel[m->nbtn] = "CERRAR SESIÓN"; m->btnAct[m->nbtn] = ACT_LOGOUT; m->btnStyle[m->nbtn] = BTN_DANGER;
        m->btnR[m->nbtn++] = (Rect){bx, sy + bh + sc(8), bw, bh};
    }
}

/* resultado de hit test: 0 nada, 1000+i fila, 2000+i tab, 3000+i boton */
static int menu_hit(Menu *m, int x, int y) {
    if (in_rect(m->infoR, x, y)) return 4000;
    for (int i = 0; i < m->nbtn; i++) if (in_rect(m->btnR[i], x, y)) return 3000 + i;
    for (int i = 0; i < m->ntabs; i++) if (in_rect(m->tabR[i], x, y)) return 2000 + i;
    if (x >= m->listX && x < m->listX + m->listW && y >= m->listY && y < m->listY + m->listH) {
        int row = (y - m->listY + m->scroll) / m->rowH;
        if (row >= 0 && row < m->nflt) return 1000 + row;
    }
    return 0;
}

static void menu_draw(App *a) {
    Menu *m = a->u;
    Canvas *c = &a->w->cv;
    Fonts *F = m->F;
    clip_reset(c);
    fill_rect(c, 0, 0, m->W, m->H, C_BG, 1.f);
    Shape ps = panel_shape();
    draw_chamfer(c, 0, 0, (float)m->W, (float)m->H, &ps);

    /* cabecera */
    int R = sc(15);
    draw_hex(c, (float)(m->pad + R), (float)(m->pad + R + sc(2)), (float)R, C_CYAN, 0.12f, fmaxf(1.f, S * 1.4f), 1.f, 0.9f, (float)sc(6));
    draw_text_c(c, F->uib, m->pad + R, m->pad + R + sc(2) - F->uib->height / 2, "S", C_CYAN, 1.f, 0);
    draw_text(c, F->title, m->pad + R * 2 + sc(12), m->pad, "SESAR // INICIO", C_TEXT, 1.f, S * 2.f, 0);
    {
        Shape is; memset(&is, 0, sizeof is);
        int hov = (m->hover == 4000);
        is.cut = (float)sc(6);
        is.top = hov ? C_MAGENTA : 0x0E1830;
        is.bot = hov ? C_MAGENTA : 0x0A1226;
        is.fa = hov ? 0.35f : 1.f;
        is.stroke = C_MAGENTA; is.sa = hov ? 1.f : 0.8f; is.sw = fmaxf(1.f, S);
        draw_chamfer(c, (float)m->infoR.x, (float)m->infoR.y,
                     (float)m->infoR.w, (float)m->infoR.h, &is);
        draw_text_c(c, F->uib, m->infoR.x + m->infoR.w / 2,
                    m->infoR.y + (m->infoR.h - F->uib->height) / 2,
                    "i", C_MAGENTA, 1.f, 0);
    }
    char cnt[64];
    snprintf(cnt, sizeof cnt, "%d APPS", m->nflt);
    int cw = text_width(F->smb, cnt, S);
    draw_text(c, F->smb, m->listX + m->listW - cw, m->pad + sc(6), cnt, C_CYAN, 1.f, S, 0);

    /* búsqueda */
    Shape ss;
    memset(&ss, 0, sizeof ss);
    ss.cut = (float)sc(8); ss.top = C_FIELD; ss.bot = C_FIELD; ss.fa = 1.f;
    ss.stroke = C_CYAN; ss.sa = m->qlen ? 0.9f : 0.5f; ss.sw = fmaxf(1.f, S * 1.2f);
    draw_chamfer(c, (float)m->searchR.x, (float)m->searchR.y, (float)m->searchR.w, (float)m->searchR.h, &ss);
    int ty = m->searchR.y + (m->searchR.h - F->uib->height) / 2, tx = m->searchR.x + sc(12);
    if (m->qlen == 0) {
        draw_text(c, F->ui, tx + sc(10), ty, "Buscar aplicación...", C_MUTED, 1.f, 0, m->searchR.w - sc(30));
        if (m->blink) fill_rect(c, tx, ty, imax(2, sc(2)), F->uib->height, C_CYAN, 1.f);
    } else {
        draw_text(c, F->uib, tx, ty, m->query, C_CYAN, 1.f, 0, m->searchR.w - sc(30));
        int w = imin(text_width(F->uib, m->query, 0), m->searchR.w - sc(30));
        if (m->blink) fill_rect(c, tx + w + sc(2), ty, imax(2, sc(2)), F->uib->height, C_CYAN, 1.f);
    }

    /* pestañas */
    for (int i = 0; i < m->ntabs; i++) {
        Shape ts;
        memset(&ts, 0, sizeof ts);
        int active = m->tabCat[i] == m->cat, hov = m->hover == 2000 + i;
        ts.cut = (float)sc(6); ts.sw = fmaxf(1.f, S);
        ts.top = active ? C_CYAN : 0x0E1830; ts.bot = active ? C_CYAN : 0x0A1226;
        ts.fa = active ? 0.24f : (hov ? 0.5f : 1.f);
        ts.stroke = active ? C_CYAN : (hov ? C_MUTED : C_LINE); ts.sa = 1.f;
        Rect r = m->tabR[i];
        draw_chamfer(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, &ts);
        draw_text_c(c, F->smb, r.x + r.w / 2, r.y + (r.h - F->smb->height) / 2, m->tabLabel[i],
                    active ? C_CYAN : C_MUTED, 1.f, S * 0.5f);
    }

    /* lista */
    clip_set(c, m->listX, m->listY, m->listW, m->listH);
    int first = m->scroll / m->rowH, last = imin(m->nflt, (m->scroll + m->listH) / m->rowH + 2);
    for (int i = first; i < last; i++) {
        AppEntry *e = &apps[m->flt[i]];
        int ry = m->listY + i * m->rowH - m->scroll;
        int selected = (i == m->sel), hov = (m->hover == 1000 + i);
        if (selected || hov) {
            Shape rs;
            memset(&rs, 0, sizeof rs);
            rs.cut = (float)sc(8); rs.top = C_CYAN; rs.bot = C_CYAN; rs.fa = selected ? 0.16f : 0.08f;
            rs.stroke = C_CYAN; rs.sa = selected ? 0.9f : 0.35f; rs.sw = fmaxf(1.f, S);
            draw_chamfer(c, (float)m->listX, (float)(ry + 2), (float)(m->listW - sc(8)), (float)(m->rowH - 4), &rs);
        }
        int hr = sc(15);
        draw_hex(c, (float)(m->listX + sc(10) + hr), (float)(ry + m->rowH / 2), (float)hr, e->color, 0.12f,
                 fmaxf(1.f, S * 1.3f), 1.f, 0.f, 1.f);
        char ini[8] = {0};
        { const char *p = e->name; int cp = utf8_next(&p); if (cp >= 'a' && cp <= 'z') cp -= 32; utf8_put(ini, cp); }
        draw_text_c(c, F->smb, m->listX + sc(10) + hr, ry + m->rowH / 2 - F->smb->height / 2, ini, e->color, 1.f, 0);
        int nx = m->listX + sc(10) + hr * 2 + sc(12), maxw = m->listW - (nx - m->listX) - sc(16);
        if (e->comment[0]) {
            draw_text(c, F->uib, nx, ry + sc(7), e->name, selected ? C_CYAN : C_TEXT, 1.f, 0, maxw);
            draw_text(c, F->sm, nx, ry + sc(7) + F->uib->height + sc(1), e->comment, C_MUTED, 1.f, 0, maxw);
        } else {
            draw_text(c, F->uib, nx, ry + (m->rowH - F->uib->height) / 2, e->name, selected ? C_CYAN : C_TEXT, 1.f, 0, maxw);
        }
    }
    if (m->nflt == 0)
        draw_text_c(c, F->ui, m->listX + m->listW / 2, m->listY + sc(30), "Sin resultados", C_MUTED, 1.f, S);
    if (menu_maxscroll(m) > 0) {
        int tw = imax(3, sc(4)), tx2 = m->listX + m->listW - tw;
        fill_rect(c, tx2, m->listY, tw, m->listH, C_LINE, 0.5f);
        int th = imax(sc(24), m->listH * m->listH / (m->nflt * m->rowH));
        int ty2 = m->listY + (m->listH - th) * m->scroll / menu_maxscroll(m);
        fill_rect(c, tx2, ty2, tw, th, C_CYAN, 0.85f);
    }
    clip_reset(c);

    /* columna derecha */
    if (m->rightW) {
        draw_text(c, F->smb, m->rightX, m->pad + sc(6), "ACCESOS", C_MUTED, 1.f, S * 1.5f, 0);
        draw_text(c, F->smb, m->rightX, m->btnR[m->nbtn - 2].y - sc(20), "SESIÓN", C_MUTED, 1.f, S * 1.5f, 0);
        for (int i = 0; i < m->nbtn; i++)
            draw_button(c, F, m->btnR[i], m->btnLabel[i], m->btnStyle[i], m->hover == 3000 + i);
        fill_rect(c, m->rightX - m->pad / 2, m->pad, imax(1, sc(1)), m->footY - m->pad - sc(4), C_LINE, 0.7f);
    }

    /* pie */
    draw_text(c, F->sm, m->pad, m->footY, "ENTER lanzar  ·  TAB categoría  ·  ESC cerrar", C_MUTED, 1.f, S * 0.5f, m->W - m->pad * 2);
}

static void menu_key(App *a, KeySym ks, int cp, unsigned state) {
    Menu *m = a->u;
    int page = imax(1, m->listH / m->rowH - 1);
    a->dirty = 1;
    switch (ks) {
    case XK_Escape: a->quit = 1; return;
    case XK_Return: case XK_KP_Enter:
        if (m->nflt > 0) { launch_app(&apps[m->flt[m->sel]]); a->quit = 1; }
        return;
    case XK_Up: m->sel = imax(0, m->sel - 1); menu_ensure_visible(m); return;
    case XK_Down: m->sel = imin(m->nflt - 1, m->sel + 1); if (m->sel < 0) m->sel = 0; menu_ensure_visible(m); return;
    case XK_Page_Up: m->sel = imax(0, m->sel - page); menu_ensure_visible(m); return;
    case XK_Page_Down: m->sel = imax(0, imin(m->nflt - 1, m->sel + page)); menu_ensure_visible(m); return;
    case XK_Home: m->sel = 0; menu_ensure_visible(m); return;
    case XK_End: m->sel = imax(0, m->nflt - 1); menu_ensure_visible(m); return;
    case XK_Tab: case XK_ISO_Left_Tab: {
        int idx = 0;
        for (int i = 0; i < m->ntabs; i++) if (m->tabCat[i] == m->cat) idx = i;
        idx = (state & ShiftMask) || ks == XK_ISO_Left_Tab ? (idx + m->ntabs - 1) % m->ntabs : (idx + 1) % m->ntabs;
        m->cat = m->tabCat[idx];
        menu_refilter(m);
        return;
    }
    case XK_BackSpace:
        if (m->qlen > 0) {
            do { m->qlen--; } while (m->qlen > 0 && (m->query[m->qlen] & 0xC0) == 0x80);
            m->query[m->qlen] = 0;
            menu_refilter(m);
        }
        return;
    default: break;
    }
    if ((state & ControlMask) && (ks == XK_u || ks == XK_U)) { m->qlen = 0; m->query[0] = 0; menu_refilter(m); return; }
    if (cp >= 32 && m->qlen < (int)sizeof m->query - 6) {
        m->qlen += utf8_put(m->query + m->qlen, cp);
        m->query[m->qlen] = 0;
        menu_refilter(m);
    }
}

static void menu_button(App *a, int x, int y, int button, int press) {
    Menu *m = a->u;
    int inside = x >= 0 && y >= 0 && x < m->W && y < m->H;
    if (press && button == 1 && !inside) { a->quit = 1; return; }
    if (press && (button == 4 || button == 5)) {
        if (x >= m->listX && x < m->listX + m->listW && y >= m->listY && y < m->listY + m->listH) {
            m->scroll = imax(0, imin(menu_maxscroll(m), m->scroll + (button == 5 ? 1 : -1) * m->rowH * 2));
            a->dirty = 1;
        }
        return;
    }
    if (button != 1) return;
    if (press) { m->press = 1; m->px = x; m->py = y; m->scroll0 = m->scroll; m->moved = 0; return; }
    if (!m->press) return;
    m->press = 0;
    if (m->moved) return;
    int h = menu_hit(m, x, y);
    if (h == 4000) { char cmd[1400]; snprintf(cmd, sizeof cmd, "%s games", self_path); spawn_cmd(cmd); a->quit = 1; }
    else if (h >= 3000) { run_action(m->btnAct[h - 3000]); a->quit = 1; }
    else if (h >= 2000) { m->cat = m->tabCat[h - 2000]; menu_refilter(m); a->dirty = 1; }
    else if (h >= 1000) { launch_app(&apps[m->flt[h - 1000]]); a->quit = 1; }
}

static void menu_motion(App *a, int x, int y) {
    Menu *m = a->u;
    if (m->press && m->py >= m->listY && m->py < m->listY + m->listH && m->px >= m->listX && m->px < m->listX + m->listW) {
        if (m->moved || abs(y - m->py) > sc(8)) {
            m->moved = 1;
            m->scroll = imax(0, imin(menu_maxscroll(m), m->scroll0 - (y - m->py)));
            a->dirty = 1;
        }
        return;
    }
    int h = menu_hit(m, x, y);
    if (h != m->hover) { m->hover = h; a->dirty = 1; }
}

static void menu_tick(App *a) {
    Menu *m = a->u;
    m->blink = !m->blink;
    a->dirty = 1;
}

static int run_menu(int argc, char **argv) {
    if (!single_instance("menu")) return 0;
    int tray = getenv("SESAR_TRAY") ? atoi(getenv("SESAR_TRAY")) : sc(40);
    int top = 0;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--top")) top = 1;
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    load_apps();
    Menu *m = calloc(1, sizeof *m);
    m->F = fonts_open();
    m->W = imin(sc(700), sw - sc(8));
    m->H = imin(sc(580), sh - tray - sc(10));
    m->cat = -1; m->hover = 0; m->blink = 1;
    m->flt = malloc(sizeof(int) * (size_t)(napps + 1));
    menu_layout(m);
    menu_refilter(m);
    int x = 0, y = top ? tray + sc(2) : sh - tray - m->H - sc(2);
    Win *w = win_create(x, y, m->W, m->H, 1, "sesar-menu");
    win_shape_chamfer(w, sc(18));
    XMapRaised(dpy, w->win);
    XSync(dpy, False);
    grab_input(w);
    App a = {0};
    a.w = w; a.u = m; a.tick_ms = 530;
    a.draw = menu_draw; a.key = menu_key; a.button = menu_button; a.motion = menu_motion; a.tick = menu_tick;
    run_app(&a);
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XSync(dpy, False);
    return 0;
}

/* ------------------------------------------------------------------ ENERGIA */
typedef struct { Fonts *F; int W, H, sel, hover, n; Rect r[3]; const char *label[3]; int act[3], style[3]; } Power;

static void power_activate(App *a, int i) {
    Power *p = a->u;
    if (p->act[i] >= 0) run_action(p->act[i]);
    a->quit = 1;
}

static void power_draw(App *a) {
    Power *p = a->u;
    Canvas *c = &a->w->cv;
    fill_rect(c, 0, 0, p->W, p->H, C_BG, 1.f);
    Shape ps = panel_shape();
    ps.accent = C_MAGENTA; ps.stroke = C_MAGENTA; ps.glow = C_MAGENTA;
    draw_chamfer(c, 0, 0, (float)p->W, (float)p->H, &ps);
    int pad = sc(22);
    draw_text(c, p->F->title, pad, pad, "ENERGÍA // SESIÓN", C_TEXT, 1.f, S * 2.f, 0);
    draw_text(c, p->F->smb, pad, pad + sc(30), "ELEGÍ UNA ACCIÓN", C_MUTED, 1.f, S * 1.5f, 0);
    for (int i = 0; i < p->n; i++)
        draw_button(c, p->F, p->r[i], p->label[i], p->style[i], p->hover == i || p->sel == i);
}

static void power_key(App *a, KeySym ks, int cp, unsigned st) {
    Power *p = a->u;
    (void)cp; (void)st;
    a->dirty = 1;
    if (ks == XK_Escape) a->quit = 1;
    else if (ks == XK_Up) p->sel = (p->sel + p->n - 1) % p->n;
    else if (ks == XK_Down || ks == XK_Tab) p->sel = (p->sel + 1) % p->n;
    else if (ks == XK_Return || ks == XK_KP_Enter) power_activate(a, p->sel);
}

static void power_button(App *a, int x, int y, int button, int press) {
    Power *p = a->u;
    if (button != 1 || !press) return;
    if (x < 0 || y < 0 || x >= p->W || y >= p->H) { a->quit = 1; return; }
    for (int i = 0; i < p->n; i++) if (in_rect(p->r[i], x, y)) power_activate(a, i);
}

static void power_motion(App *a, int x, int y) {
    Power *p = a->u;
    int h = -1;
    for (int i = 0; i < p->n; i++) if (in_rect(p->r[i], x, y)) h = i;
    if (h != p->hover) { p->hover = h; a->dirty = 1; }
}

static int run_power(void) {
    if (!single_instance("power")) return 0;
    Power *p = calloc(1, sizeof *p);
    p->F = fonts_open();
    int pad = sc(22), bh = sc(44), gap = sc(10);
    p->W = imin(sc(380), DisplayWidth(dpy, scr) - sc(16));
    p->label[0] = "REINICIAR ESCRITORIO"; p->act[0] = ACT_RESTART; p->style[0] = BTN_NORMAL;
    p->label[1] = "CERRAR SESIÓN"; p->act[1] = ACT_LOGOUT; p->style[1] = BTN_DANGER;
    p->label[2] = "CANCELAR"; p->act[2] = -1; p->style[2] = BTN_NORMAL;
    p->n = 3;
    int y = pad + sc(58);
    for (int i = 0; i < p->n; i++) { p->r[i] = (Rect){pad, y, p->W - pad * 2, bh}; y += bh + gap; }
    p->H = y - gap + pad;
    p->hover = -1;
    int x = (DisplayWidth(dpy, scr) - p->W) / 2, yy = (DisplayHeight(dpy, scr) - p->H) / 2;
    Win *w = win_create(x, yy, p->W, p->H, 1, "sesar-power");
    win_shape_chamfer(w, sc(18));
    XMapRaised(dpy, w->win);
    XSync(dpy, False);
    grab_input(w);
    App a = {0};
    a.w = w; a.u = p;
    a.draw = power_draw; a.key = power_key; a.button = power_button; a.motion = power_motion;
    run_app(&a);
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XSync(dpy, False);
    return 0;
}

/* ------------------------------------------------------------------ HUD */
typedef struct {
    Fonts *F;
    int W, H;
    float cpu, ram, bat;
    int has_cpu, has_bat;
    char ramtxt[32];
    unsigned long long last_idle, last_total;
    int have_last, tick_n;
} Hud;

static int read_cpu(unsigned long long *idle, unsigned long long *total) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4) return 0;
    unsigned long long t = 0;
    for (int i = 0; i < 8; i++) t += v[i];
    *idle = v[3] + v[4];
    *total = t;
    return 1;
}

static void hud_update(Hud *h) {
    unsigned long long idle, total;
    if (read_cpu(&idle, &total)) {
        if (h->have_last && total > h->last_total)
            h->cpu = clamp01(1.f - (float)(idle - h->last_idle) / (float)(total - h->last_total));
        h->last_idle = idle; h->last_total = total;
        h->have_last = 1; h->has_cpu = 1;
    } else h->has_cpu = 0;

    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        long tot = 0, av = 0;
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %ld", &tot);
            sscanf(line, "MemAvailable: %ld", &av);
        }
        fclose(f);
        if (tot > 0) {
            h->ram = clamp01((float)(tot - av) / (float)tot);
            snprintf(h->ramtxt, sizeof h->ramtxt, "%.1f/%.1fG", (tot - av) / 1048576.0, tot / 1048576.0);
        }
    }
    h->has_bat = 0;
    DIR *d = opendir("/sys/class/power_supply");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[600], buf[64] = "";
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
            FILE *t = fopen(p, "r");
            if (!t) continue;
            if (fgets(buf, sizeof buf, t)) {}
            fclose(t);
            if (strncmp(buf, "Battery", 7) != 0) continue;
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
            FILE *cf = fopen(p, "r");
            int cap = -1;
            if (cf) { if (fscanf(cf, "%d", &cap) != 1) cap = -1; fclose(cf); }
            if (cap >= 0) { h->bat = clamp01(cap / 100.f); h->has_bat = 1; }
            break;
        }
        closedir(d);
    }
}

static void hud_bar(Canvas *c, int x, int y, int w, int hgt, float frac) {
    Shape s;
    memset(&s, 0, sizeof s);
    s.cut = (float)(hgt / 2); s.top = 0x0B1224; s.bot = 0x0B1224; s.fa = 1.f;
    s.stroke = C_LINE; s.sa = 1.f; s.sw = fmaxf(1.f, S);
    draw_chamfer(c, (float)x, (float)y, (float)w, (float)hgt, &s);
    int fw = (int)((w - 2) * clamp01(frac));
    for (int ix = 0; ix < fw; ix++) {
        uint32_t col = lerp_col(C_CYAN, C_MAGENTA, (float)ix / (float)imax(1, w - 2));
        fill_rect(c, x + 1 + ix, y + 1, 1, hgt - 2, col, 1.f);
    }
    int seg = sc(8);
    for (int ix = seg; ix < w; ix += seg) fill_rect(c, x + ix, y + 1, imax(1, sc(1)), hgt - 2, 0x070C1A, 0.9f);
}

static void hud_draw(App *a) {
    Hud *h = a->u;
    Canvas *c = &a->w->cv;
    Fonts *F = h->F;
    fill_rect(c, 0, 0, h->W, h->H, C_BG, 1.f);
    Shape ps = panel_shape();
    ps.cut = (float)sc(12); ps.accent = C_MAGENTA; ps.stroke = C_MAGENTA; ps.glow = C_MAGENTA; ps.ga = 0.2f; ps.gw = (float)sc(8);
    draw_chamfer(c, 0, 0, (float)h->W, (float)h->H, &ps);
    int pad = sc(14), y = pad;
    draw_text(c, F->uib, pad, y, "SYS // HUD", C_TEXT, 1.f, S * 2.f, 0);
    y += F->uib->height + sc(12);
    struct { const char *l; float v; int ok; char t[32]; } rows[3];
    rows[0].l = "CPU"; rows[0].v = h->cpu; rows[0].ok = h->has_cpu; snprintf(rows[0].t, 32, "%.0f%%", h->cpu * 100.f);
    rows[1].l = "RAM"; rows[1].v = h->ram; rows[1].ok = 1; snprintf(rows[1].t, 32, "%s", h->ramtxt);
    rows[2].l = "BAT"; rows[2].v = h->bat; rows[2].ok = h->has_bat; snprintf(rows[2].t, 32, "%.0f%%", h->bat * 100.f);
    for (int i = 0; i < 3; i++) {
        if (!rows[i].ok && i == 2) continue;
        draw_text(c, F->sm, pad, y, rows[i].l, C_MUTED, 1.f, S, 0);
        const char *t = rows[i].ok ? rows[i].t : "--";
        draw_text(c, F->smb, h->W - pad - text_width(F->smb, t, 0), y, t, C_CYAN, 1.f, 0, 0);
        y += F->sm->height + sc(4);
        hud_bar(c, pad, y, h->W - pad * 2, sc(9), rows[i].ok ? rows[i].v : 0.f);
        y += sc(9) + sc(10);
    }
}

static void hud_tick(App *a) {
    Hud *h = a->u;
    hud_update(h);
    a->dirty = 1;
    /* XRaiseWindow eliminado: forzaba restackeo cada 2s, causaba expone del
     * root y con el pixmap del wallpaper liberado -> pantalla negra ciclica. */
}

static int run_hud(void) {
    if (!single_instance("hud")) return 0;
    Hud *h = calloc(1, sizeof *h);
    h->F = fonts_open();
    snprintf(h->ramtxt, sizeof h->ramtxt, "--");
    hud_update(h);
    h->W = sc(240);
    h->H = sc(14) * 2 + h->F->uib->height + sc(12) + (h->F->sm->height + sc(4) + sc(9) + sc(10)) * (h->has_bat ? 3 : 2) - sc(6);
    int x = DisplayWidth(dpy, scr) - h->W - sc(12), y = sc(12);
    Win *w = win_create(x, y, h->W, h->H, 1, "sesar-hud");
    win_shape_chamfer(w, sc(12));
    XMapRaised(dpy, w->win);
    App a = {0};
    a.w = w; a.u = h; a.tick_ms = 1000;
    a.draw = hud_draw; a.tick = hud_tick;
    run_app(&a);
    return 0;
}

/* ------------------------------------------------------------------ FONDO */
static void radial_glow(Canvas *c, float cx, float cy, float r, uint32_t col, float amt) {
    int x0 = imax(0, (int)(cx - r)), x1 = imin(c->w, (int)(cx + r) + 1);
    int y0 = imax(0, (int)(cy - r)), y1 = imin(c->h, (int)(cy + r) + 1);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x - cx, dy = y - cy, d = sqrtf(dx * dx + dy * dy) / r;
            if (d >= 1.f) continue;
            float g = 1.f - d;
            put(c, x, y, col, g * g * amt);
        }
}

static void render_wallpaper(Canvas *c) {
    int w = c->w, h = c->h;
    float hy = h * 0.66f;
    for (int y = 0; y < h; y++) {
        float t = (float)y / (float)h;
        uint32_t col = t < 0.62f ? lerp_col(0x03050B, 0x0A1030, t / 0.62f) : lerp_col(0x0A1030, 0x170A36, (t - 0.62f) / 0.38f);
        for (int x = 0; x < w; x++) c->px[(size_t)y * w + x] = col;
    }
    float big = (float)imax(w, h);
    radial_glow(c, w * 0.12f, h * 0.06f, big * 0.60f, C_MAGENTA, 0.26f);
    radial_glow(c, w * 0.95f, h * 0.22f, big * 0.55f, C_CYAN, 0.20f);

    unsigned seed = 7;
    int stars = w * h / 9000;
    for (int i = 0; i < stars; i++) {
        seed = seed * 1664525u + 1013904223u; int sx = (int)(seed % (unsigned)w);
        seed = seed * 1664525u + 1013904223u; int sy = (int)(seed % (unsigned)(hy * 0.95f));
        seed = seed * 1664525u + 1013904223u; float a = 0.15f + (seed % 1000) / 1800.f;
        put(c, sx, sy, 0xE6F2FF, a);
        if (a > 0.5f) { put(c, sx + 1, sy, 0xE6F2FF, a * 0.5f); put(c, sx, sy + 1, 0xE6F2FF, a * 0.5f); }
    }
    for (int y = (int)(hy - h * 0.14f); y < (int)hy; y++) {
        float t = (y - (hy - h * 0.14f)) / (h * 0.14f);
        fill_rect(c, 0, y, w, 1, C_MAGENTA, t * t * 0.28f);
    }

    /* hexágono de marca */
    float cx = w / 2.f, cy = hy * 0.46f, R = h * 0.17f;
    draw_hex(c, cx, cy, R, C_CYAN, 0.05f, fmaxf(2.f, h / 360.f), 0.30f, 0.15f, R * 0.25f);

    /* horizonte y piso */
    draw_line_a(c, 0, hy, (float)w, hy, fmaxf(1.5f, h / 540.f), C_MAGENTA, 0.55f, 0.55f);
    float lw = fmaxf(1.f, h / 720.f);
    float step = w / 7.f;
    for (int i = -14; i <= 14; i++)
        draw_line_a(c, w / 2.f, hy, w / 2.f + i * step, (float)h, lw, C_CYAN, 0.f, 0.55f);
    int rows = 13;
    for (int k = 1; k <= rows; k++) {
        float z = (float)k / rows, y = hy + (h - hy) * z * z;
        draw_line_a(c, 0, y, (float)w, y, lw, C_CYAN, 0.06f + 0.5f * z, 0.06f + 0.5f * z);
    }

    /* viñeta */
    float rin = fminf((float)w, (float)h) * 0.35f, rout = big * 0.75f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = x - w / 2.f, dy = y - h / 2.f, d = sqrtf(dx * dx + dy * dy);
            if (d <= rin) continue;
            float v = clamp01((d - rin) / (rout - rin)) * 0.55f;
            uint32_t *p = &c->px[(size_t)y * w + x];
            *p = mix(*p, 0x000000, v);
        }
}

static Pixmap g_wallpaper_pm = None;

static int run_wallpaper(void) {
    int w = DisplayWidth(dpy, scr), h = DisplayHeight(dpy, scr);
    Canvas c = canvas_new(w, h);
    render_wallpaper(&c);
    /* texto de marca con una fuente grande */
    char bp[1400];
    if (find_font(1, bp, sizeof bp) || find_font(0, bp, sizeof bp)) {
        SFont *f = font_open(bp, imax(12, (int)(h * 0.045f)), 0);
        if (f) {
            float hy = h * 0.66f;
            draw_text_c(&c, f, w / 2, (int)(hy * 0.46f + h * 0.17f + h * 0.04f), "S E S A R   D E", C_CYAN, 0.5f, 0);
        }
    }
    Win *tmp = calloc(1, sizeof *tmp);
    tmp->w = w; tmp->h = h; tmp->cv = c;
    /* Reusar el pixmap si ya existe (evita leak acumulado si JWM hace restart). */
    if (g_wallpaper_pm != None) {
        XFreePixmap(dpy, g_wallpaper_pm);
        g_wallpaper_pm = None;
    }
    Pixmap pm = g_wallpaper_pm = XCreatePixmap(dpy, RootWindow(dpy, scr), (unsigned)w, (unsigned)h, (unsigned)depth);
    if (!std_masks) tmp->conv = calloc((size_t)w * (size_t)h, 4);
    tmp->img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, (char *)(std_masks ? c.px : tmp->conv), (unsigned)w, (unsigned)h, 32, 0);
    tmp->gc = XCreateGC(dpy, pm, 0, NULL);
    if (!std_masks) {
        unsigned rs = mask_shift(vis->red_mask), gs = mask_shift(vis->green_mask), bs = mask_shift(vis->blue_mask);
        unsigned rb = mask_bits(vis->red_mask >> rs), gb = mask_bits(vis->green_mask >> gs), bb = mask_bits(vis->blue_mask >> bs);
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
            uint32_t p = c.px[i];
            tmp->conv[i] = (((p >> 16) & 255) >> (8 - rb)) << rs | (((p >> 8) & 255) >> (8 - gb)) << gs | ((p & 255) >> (8 - bb)) << bs;
        }
    }
    XPutImage(dpy, pm, tmp->gc, tmp->img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    XSetWindowBackgroundPixmap(dpy, RootWindow(dpy, scr), pm);
    XClearWindow(dpy, RootWindow(dpy, scr));
    /* NO liberar pm: el root window lo referencia como fondo. */
    XSync(dpy, False);

    /* El proceso NO debe terminar: si sale, X11 libera TODOS sus recursos
     * (incluido el pixmap del fondo) y el root queda negro en el proximo
     * expose. Quedamos en un loop de eventos hasta que muera la sesion. */
    single_instance("wallpaper");
    XSelectInput(dpy, RootWindow(dpy, scr), StructureNotifyMask | ExposureMask);
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == Expose || ev.type == ConfigureNotify)
            XClearWindow(dpy, RootWindow(dpy, scr));
    }
    return 0;
}

/* ------------------------------------------------------------------ recursos de xterm */
static const char XRES_BLOCK[] =
    "! BEGIN " MARK "\n"
    "XTerm*background: #05070F\n"
    "XTerm*foreground: #E8F4FF\n"
    "XTerm*cursorColor: #00E5FF\n"
    "XTerm*pointerColor: #00E5FF\n"
    "XTerm*pointerColorBackground: #05070F\n"
    "XTerm*color0: #0B1224\nXTerm*color1: #FF3B5C\nXTerm*color2: #39FF88\nXTerm*color3: #FFB020\n"
    "XTerm*color4: #4D7CFF\nXTerm*color5: #FF2BD6\nXTerm*color6: #00E5FF\nXTerm*color7: #B4C4E0\n"
    "XTerm*color8: #24365E\nXTerm*color9: #FF6B85\nXTerm*color10: #7DFFB0\nXTerm*color11: #FFD166\n"
    "XTerm*color12: #7FA2FF\nXTerm*color13: #FF7BE8\nXTerm*color14: #7DF3FF\nXTerm*color15: #E8F4FF\n"
    "XTerm*faceName: Monospace\n"
    "XTerm*faceSize: 11\n"
    "XTerm*scrollBar: false\n"
    "XTerm*internalBorder: 10\n"
    "XTerm*borderWidth: 0\n"
    "XTerm*saveLines: 10000\n"
    "XTerm*cursorBlink: true\n"
    "XTerm*selectToClipboard: true\n"
    "XTerm*metaSendsEscape: true\n"
    "XTerm*termName: xterm-256color\n"
    "XTerm*vt100.translations: #override \\\n"
    "    Ctrl Shift <Key>C: copy-selection(CLIPBOARD) \\n\\\n"
    "    Ctrl Shift <Key>V: insert-selection(CLIPBOARD)\n"
    "! END " MARK "\n";

static int run_xres(void) {
    Window root = RootWindow(dpy, scr);
    Atom type; int fmt; unsigned long nitems, after;
    unsigned char *data = NULL;
    char *old = xstrdup("");
    if (XGetWindowProperty(dpy, root, XA_RESOURCE_MANAGER, 0, 1 << 20, False, XA_STRING,
                           &type, &fmt, &nitems, &after, &data) == Success && data) {
        free(old);
        old = xstrdup((char *)data);
        XFree(data);
    }
    char *b = strstr(old, "! BEGIN " MARK), *e = b ? strstr(b, "! END " MARK) : NULL;
    size_t cap = strlen(old) + sizeof XRES_BLOCK + 8;
    char *out = calloc(1, cap);
    if (b && e) {
        e = strchr(e, '\n');
        size_t head = (size_t)(b - old);
        memcpy(out, old, head);
        strcat(out, e ? e + 1 : "");
    } else strcat(out, old);
    size_t L = strlen(out);
    if (L && out[L - 1] != '\n') strcat(out, "\n");
    strcat(out, XRES_BLOCK);
    XChangeProperty(dpy, root, XA_RESOURCE_MANAGER, XA_STRING, 8, PropModeReplace,
                    (unsigned char *)out, (int)strlen(out));
    XSync(dpy, False);
    free(old); free(out);
    return 0;
}

/* ------------------------------------------------------------------ appmenu (XML para JWM) */
static void xml_esc(const char *s, FILE *f) {
    for (; *s; s++) {
        switch (*s) {
        case '&': fputs("&amp;", f); break;
        case '<': fputs("&lt;", f); break;
        case '>': fputs("&gt;", f); break;
        case '"': fputs("&quot;", f); break;
        default: fputc(*s, f);
        }
    }
}

static int run_appmenu(void) {
    load_apps();
    printf("<?xml version=\"1.0\"?>\n<JWM>\n<Menu label=\"Aplicaciones\">\n");
    for (int c = 0; c < NCATS; c++) {
        int any = 0;
        for (int i = 0; i < napps; i++) {
            AppEntry *a = &apps[i];
            int first = 0;
            while (first < NCATS && !((a->cats >> first) & 1u)) first++;
            if (first != c) continue;
            if (!any) { printf("<Menu label=\""); xml_esc(CAT_NAMES[c], stdout); printf("\">\n"); any = 1; }
            char cmd[2200];
            if (a->term) snprintf(cmd, sizeof cmd, "%s -e gl-run %s", terminal_cmd(), a->exec);
            else snprintf(cmd, sizeof cmd, "gl-run %s", a->exec);
            printf("<Program label=\""); xml_esc(a->name, stdout); printf("\">"); xml_esc(cmd, stdout); printf("</Program>\n");
        }
        if (any) printf("</Menu>\n");
    }
    printf("</Menu>\n</JWM>\n");
    return 0;
}

/* ------------------------------------------------------------------ configuración de JWM */
#define L(x) x "\n"
static const char JWMRC_TEMPLATE[] =
L("<?xml version=`1.0`?>")
L("<!-- " MARK ": generado por sesar-shell setup; los cambios manuales se pierden al reinstalar -->")
L("<JWM>")
L("  <StartupCommand>@SELF@ xres</StartupCommand>")
L("  <StartupCommand>@SELF@ wallpaper</StartupCommand>")
L("  <StartupCommand>@SELF@ hud</StartupCommand>")
L("")
L("  <RootMenu onroot=`3`>")
L("    <Program label=`Inicio`>@SELF@ menu</Program>")
L("    <Program label=`Juegos soportados`>@SELF@ games</Program>")
L("    <Program label=`Terminal`>@TERM@</Program>")
L("    <Program label=`Archivos`>@SELF@ files</Program>")
L("    <Separator/>")
L("    <Include>exec:@SELF@ appmenu</Include>")
L("    <Separator/>")
L("    <Program label=`Energía`>@SELF@ power</Program>")
L("    <Restart label=`Reiniciar JWM`/>")
L("    <Exit label=`Salir` confirm=`false`/>")
L("  </RootMenu>")
L("")
L("  <Tray x=`0` y=`-1` height=`@TRAYH@` layout=`horizontal` autohide=`off` layer=`above`>")
L("    <TrayButton label=`SESAR` popup=`Inicio`>exec:@SELF@ menu</TrayButton>")
L("    <Pager labeled=`true`/>")
L("    <TaskList maxwidth=`240`/>")
L("    <Dock/>")
L("    <TrayButton label=`PWR` popup=`Energía`>exec:@SELF@ power</TrayButton>")
L("    <Clock format=`%a %d %b  %H:%M`/>")
L("  </Tray>")
L("")
L("  <Desktops width=`3` height=`1`>")
L("    <Desktop name=`01`/>")
L("    <Desktop name=`02`/>")
L("    <Desktop name=`03`/>")
L("  </Desktops>")
L("")
L("  <WindowStyle decorations=`flat`>")
L("    <Font>DejaVu Sans Mono-9:bold</Font>")
L("    <Width>2</Width>")
L("    <Height>28</Height>")
L("    <Corner>0</Corner>")
L("    <Foreground>#6F82A8</Foreground>")
L("    <Background>#090F22</Background>")
L("    <Outline>#24365E</Outline>")
L("    <Opacity>1.0</Opacity>")
L("    <Active>")
L("      <Foreground>#04060D</Foreground>")
L("      <Background>#00E5FF:#009BC7</Background>")
L("      <Outline>#00E5FF</Outline>")
L("      <Opacity>1.0</Opacity>")
L("    </Active>")
L("  </WindowStyle>")
L("")
L("  <TrayStyle>")
L("    <Font>DejaVu Sans Mono-9:bold</Font>")
L("    <Background>#0E1730:#070C1A</Background>")
L("    <Foreground>#E8F4FF</Foreground>")
L("    <Outline>#00E5FF</Outline>")
L("    <Opacity>0.96</Opacity>")
L("  </TrayStyle>")
L("")
L("  <TrayButtonStyle>")
L("    <Font>DejaVu Sans Mono-9:bold</Font>")
L("    <Foreground>#00E5FF</Foreground>")
L("    <Background>#070C1A</Background>")
L("    <Active>")
L("      <Foreground>#04060D</Foreground>")
L("      <Background>#00E5FF</Background>")
L("    </Active>")
L("  </TrayButtonStyle>")
L("")
L("  <TaskListStyle>")
L("    <Font>DejaVu Sans Mono-8</Font>")
L("    <Foreground>#6F82A8</Foreground>")
L("    <Background>#090F22</Background>")
L("    <Active>")
L("      <Foreground>#04060D</Foreground>")
L("      <Background>#00E5FF:#009BC7</Background>")
L("    </Active>")
L("  </TaskListStyle>")
L("")
L("  <PagerStyle>")
L("    <Outline>#24365E</Outline>")
L("    <Foreground>#070C1A</Foreground>")
L("    <Background>#090F22</Background>")
L("    <Text>#6F82A8</Text>")
L("    <Active>")
L("      <Foreground>#FF2BD6</Foreground>")
L("      <Background>#00E5FF</Background>")
L("    </Active>")
L("  </PagerStyle>")
L("")
L("  <ClockStyle>")
L("    <Font>DejaVu Sans Mono-9:bold</Font>")
L("    <Foreground>#00E5FF</Foreground>")
L("    <Background>#070C1A</Background>")
L("  </ClockStyle>")
L("")
L("  <MenuStyle>")
L("    <Font>DejaVu Sans Mono-9:bold</Font>")
L("    <Foreground>#E8F4FF</Foreground>")
L("    <Background>#090F22</Background>")
L("    <Outline>#00E5FF</Outline>")
L("    <Opacity>0.98</Opacity>")
L("    <Active>")
L("      <Foreground>#04060D</Foreground>")
L("      <Background>#00E5FF:#009BC7</Background>")
L("    </Active>")
L("  </MenuStyle>")
L("")
L("  <PopupStyle enabled=`true` delay=`500`>")
L("    <Font>DejaVu Sans Mono-8</Font>")
L("    <Outline>#00E5FF</Outline>")
L("    <Foreground>#E8F4FF</Foreground>")
L("    <Background>#070C1A</Background>")
L("  </PopupStyle>")
L("")
L("  <FocusModel>click</FocusModel>")
L("  <SnapMode distance=`10`>border</SnapMode>")
L("  <MoveMode>opaque</MoveMode>")
L("  <ResizeMode>opaque</ResizeMode>")
L("  <DoubleClickSpeed>400</DoubleClickSpeed>")
L("  <DoubleClickDelta>4</DoubleClickDelta>")
L("")
L("  <Key key=`Up`>up</Key>")
L("  <Key key=`Down`>down</Key>")
L("  <Key key=`Right`>right</Key>")
L("  <Key key=`Left`>left</Key>")
L("  <Key key=`Return`>select</Key>")
L("  <Key key=`Escape`>escape</Key>")
L("  <Key mask=`A` key=`Tab`>next</Key>")
L("  <Key mask=`A` key=`F4`>close</Key>")
L("  <Key mask=`A` key=`F1`>exec:@SELF@ menu</Key>")
L("  <Key mask=`4` key=`space`>exec:@SELF@ menu</Key>")
L("  <Key mask=`4` key=`Return`>exec:@TERM@</Key>")
L("  <Key mask=`4` key=`e`>exec:@SELF@ files</Key>")
L("  <Key mask=`4` key=`Escape`>exec:@SELF@ power</Key>")
L("  <Key mask=`4` key=`q`>close</Key>")
L("  <Key mask=`4` key=`d`>showdesktop</Key>")
L("  <Key mask=`4` key=`m`>maximize</Key>")
L("  <Key mask=`4` key=`f`>fullscreen</Key>")
L("  <Key mask=`4` key=`Left`>ldesktop</Key>")
L("  <Key mask=`4` key=`Right`>rdesktop</Key>")
L("  <Key mask=`CA` key=`Left`>ldesktop</Key>")
L("  <Key mask=`CA` key=`Right`>rdesktop</Key>")
L("")
L("  <Mouse context=`title` button=`1`>move</Mouse>")
L("  <Mouse context=`title` button=`2`>move</Mouse>")
L("  <Mouse context=`title` button=`3`>window</Mouse>")
L("  <Mouse context=`title` button=`4`>shade</Mouse>")
L("  <Mouse context=`title` button=`5`>shade</Mouse>")
L("  <Mouse context=`title` button=`11`>maximize</Mouse>")
L("  <Mouse context=`icon` button=`1`>window</Mouse>")
L("  <Mouse context=`icon` button=`3`>window</Mouse>")
L("  <Mouse context=`border` button=`1`>resize</Mouse>")
L("  <Mouse context=`border` button=`2`>move</Mouse>")
L("  <Mouse context=`border` button=`3`>window</Mouse>")
L("  <Mouse context=`close` button=`-1`>close</Mouse>")
L("  <Mouse context=`close` button=`2`>kill</Mouse>")
L("  <Mouse context=`maximize` button=`-1`>maximize</Mouse>")
L("  <Mouse context=`minimize` button=`-1`>minimize</Mouse>")
L("  <Mouse context=`root` button=`4`>ldesktop</Mouse>")
L("  <Mouse context=`root` button=`5`>rdesktop</Mouse>")
L("</JWM>");

static char *replace_all(const char *src, const char *key, const char *val) {
    size_t kl = strlen(key), vl = strlen(val), n = 0;
    for (const char *p = src; (p = strstr(p, key)); p += kl) n++;
    char *out = malloc(strlen(src) + n * (vl > kl ? vl - kl : 0) + 1), *o = out;
    if (!out) die("sin memoria");
    const char *p = src, *q;
    while ((q = strstr(p, key))) {
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        memcpy(o, val, vl); o += vl;
        p = q + kl;
    }
    strcpy(o, p);
    return out;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static int copy_file(const char *from, const char *to) {
    char *d = read_file(from);
    if (!d) return 0;
    FILE *f = fopen(to, "wb");
    if (!f) { free(d); return 0; }
    fwrite(d, 1, strlen(d), f);
    fclose(f);
    free(d);
    return 1;
}

static int run_setup(void) {
    const char *home = getenv("HOME");
    if (!home) die("HOME no está definido");
    char path[1200], bak[1300];
    snprintf(path, sizeof path, "%s/.jwmrc", home);
    snprintf(bak, sizeof bak, "%s.pre-sesar", path);
    char *old = read_file(path);
    if (old && !strstr(old, MARK) && !strstr(old, "gladiator-desktop") && !file_exists(bak)) {
        if (copy_file(path, bak)) printf("[ok] copia de tu configuración anterior -> %s\n", bak);
    }
    free(old);

    char *t = xstrdup(JWMRC_TEMPLATE);
    for (char *p = t; *p; p++) if (*p == '`') *p = '"';
    char trayh[16];
    snprintf(trayh, sizeof trayh, "%d", getenv("SESAR_TRAY") ? atoi(getenv("SESAR_TRAY")) : 40);
    char *a = replace_all(t, "@SELF@", self_path); free(t);
    char *b = replace_all(a, "@TERM@", terminal_cmd()); free(a);
    char *cfg = replace_all(b, "@TRAYH@", trayh); free(b);
    FILE *f = fopen(path, "wb");
    if (!f) die("no puedo escribir ~/.jwmrc");
    fputs(cfg, f);
    fclose(f);
    free(cfg);
    printf("[ok] configuración JWM -> %s\n", path);
    if (which("jwm", NULL, 0)) printf("     validala con: jwm -p\n");
    else printf("[!!] jwm no está instalado (pkg install jwm)\n");
    if (!which(terminal_cmd(), NULL, 0)) printf("[!!] no encuentro la terminal '%s'\n", terminal_cmd());
    printf("Listo. Iniciá el escritorio con:  %s session\n", self_path);
    return 0;
}

static int run_uninstall(void) {
    const char *home = getenv("HOME");
    if (!home) return 1;
    char path[1200], b1[1300], b2[1300];
    snprintf(path, sizeof path, "%s/.jwmrc", home);
    snprintf(b1, sizeof b1, "%s.pre-sesar", path);
    snprintf(b2, sizeof b2, "%s.pre-gladiator", path);
    const char *bak = file_exists(b1) ? b1 : (file_exists(b2) ? b2 : NULL);
    if (bak && copy_file(bak, path)) { unlink(bak); printf("[ok] restaurado %s\n", path); }
    else {
        char *cur = read_file(path);
        if (cur && strstr(cur, MARK)) { unlink(path); printf("[ok] eliminado %s\n", path); }
        free(cur);
    }
    return 0;
}

static int run_session(void) {
    const char *home = getenv("HOME");
    if (!getenv("DISPLAY")) die("no hay DISPLAY (abrí el servidor X y exportá DISPLAY=:0)");
    if (!which("jwm", NULL, 0)) die("jwm no está instalado (pkg install jwm)");
    char path[1200];
    snprintf(path, sizeof path, "%s/.jwmrc", home ? home : "");
    char *cur = read_file(path);
    int ok = cur && strstr(cur, MARK);
    free(cur);
    if (!ok) run_setup();
    run_xres();
    XCloseDisplay(dpy);
    setenv("XDG_SESSION_TYPE", "x11", 1);
    execlp("jwm", "jwm", (char *)NULL);
    die("no pude ejecutar jwm");
    return 1;
}

static int run_files(void) {
    char buf[300];
    if (!files_cmd(buf, sizeof buf)) {
        fprintf(stderr, "sesar-shell: no hay gestor de archivos instalado (pcmanfm, thunar, xfe...)\n");
        return 1;
    }
    execlp(buf, buf, (char *)NULL);
    return 1;
}


/* ---------------------------------------------------------------- prefixes */

typedef struct {
    char binname[128];
    char name[160];
    char description[640];
    char state[32];
    char env[24][512];
    int  nenv;
    char args[512];
    char graphics[32];
    char audio[32];
} PrefixEntry;

static PrefixEntry *g_prefixes;
static int g_nprefixes;

static const char *PREFIXES_URL =
    "https://raw.githubusercontent.com/LexusYTG/gladiator-init-setup/main/prefixes.json";

static char *fetch_prefixes_json(void) {
    /* 1) La app Android inyecta prefixes.json vía bind host-tmp.
     *    Cero dependencias de curl/wget dentro del container. */
    char *p = read_file("/host-tmp/prefixes.json");
    if (p && *p) return p;
    free(p);
    p = read_file("/host-tmp/prefixes/prefixes.json");
    if (p && *p) return p;
    free(p);
    /* 2) Fallback opcional: curl/wget dentro del container. */
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char fpath[1200];
    snprintf(fpath, sizeof fpath, "%s/prefixes.json", tmp);
    char cmd[1500];
    snprintf(cmd, sizeof cmd,
             "curl -sfL --max-time 8 '%s' -o '%s' 2>/dev/null || "
             "wget -q -T 8 -O '%s' '%s' 2>/dev/null",
             PREFIXES_URL, fpath, fpath, PREFIXES_URL);
    if (system(cmd) != 0) return NULL;
    return read_file(fpath);
}

static char *jget_str(const char *p, const char *end, const char *key,
                      char *out, size_t n) {
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *k = p;
    while ((k = strstr(k, pat)) && k < end) {
        const char *q = k + strlen(pat);
        while (q < end && (*q == ' ' || *q == '\t' || *q == ':' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != '"') { k = q; continue; }
        q++;
        const char *s = q;
        while (q < end && *q != '"') q++;
        size_t L = (size_t)(q - s);
        if (L >= n) L = n - 1;
        memcpy(out, s, L); out[L] = 0;
        return out;
    }
    out[0] = 0; return out;
}

static void parse_env_object(const char *p, const char *end, PrefixEntry *e) {
    while (p < end) {
        const char *k = memchr(p, '"', (size_t)(end - p));
        if (!k) break;
        k++;
        const char *kend = memchr(k, '"', (size_t)(end - k));
        if (!kend) break;
        char key[128];
        size_t kL = (size_t)(kend - k);
        if (kL >= sizeof key) kL = sizeof key - 1;
        memcpy(key, k, kL); key[kL] = 0;
        const char *q = kend + 1;
        while (q < end && (*q == ' ' || *q == ':')) q++;
        if (q >= end || *q != '"') { p = kend + 1; continue; }
        q++;
        const char *vend = memchr(q, '"', (size_t)(end - q));
        if (!vend) break;
        char val[512];
        size_t vL = (size_t)(vend - q);
        if (vL >= sizeof val) vL = sizeof val - 1;
        memcpy(val, q, vL); val[vL] = 0;
        if (e->nenv < 24) {
            snprintf(e->env[e->nenv], 512, "%s=%s", key, val);
            e->nenv++;
        }
        p = vend + 1;
    }
}

static int parse_prefixes(const char *json) {
    if (!json) return 0;
    const char *p = json;
    while ((p = strstr(p, "\"binname\""))) {
        const char *start = p;
        while (start > json && *start != '{') start--;
        int depth = 0;
        const char *end = start;
        while (*end) {
            if (*end == '{') depth++;
            else if (*end == '}') { depth--; if (depth == 0) { end++; break; } }
            end++;
        }
        if (end <= start) break;
        PrefixEntry e;
        memset(&e, 0, sizeof e);
        jget_str(start, end, "binname",     e.binname,     sizeof e.binname);
        jget_str(start, end, "name",        e.name,        sizeof e.name);
        jget_str(start, end, "description", e.description, sizeof e.description);
        jget_str(start, end, "state",       e.state,       sizeof e.state);
        jget_str(start, end, "args",        e.args,        sizeof e.args);
        jget_str(start, end, "graphics",    e.graphics,    sizeof e.graphics);
        jget_str(start, end, "audio",       e.audio,       sizeof e.audio);
        const char *envk = strstr(start, "\"env\"");
        if (envk && envk < end) {
            const char *eb = strchr(envk, '{');
            if (eb && eb < end) {
                int ed = 1;
                const char *ee = eb + 1;
                while (*ee && ed) {
                    if (*ee == '{') ed++;
                    else if (*ee == '}') ed--;
                    ee++;
                }
                parse_env_object(eb + 1, ee - 1, &e);
            }
        }
        if (e.binname[0]) {
            g_prefixes = realloc(g_prefixes, sizeof(PrefixEntry) * (size_t)(g_nprefixes + 1));
            g_prefixes[g_nprefixes++] = e;
        }
        p = end;
    }
    return g_nprefixes;
}

static uint32_t state_color(const char *s) {
    if (!strcasecmp(s, "very good")) return C_GREEN;
    if (!strcasecmp(s, "good"))      return C_CYAN;
    if (!strcasecmp(s, "limited"))   return C_AMBER;
    if (!strcasecmp(s, "inwork"))    return C_VIOLET;
    if (!strcasecmp(s, "fail"))      return C_RED;
    return C_MUTED;
}

static void launch_prefix(const PrefixEntry *e) {
    char envbuf[4096]; envbuf[0] = 0;
    for (int i = 0; i < e->nenv; i++) {
        size_t rem = sizeof envbuf - strlen(envbuf) - 1;
        strncat(envbuf, "'", rem); rem = sizeof envbuf - strlen(envbuf) - 1;
        strncat(envbuf, e->env[i], rem); rem = sizeof envbuf - strlen(envbuf) - 1;
        strncat(envbuf, "' ", rem);
    }
    char cmd[4600];
    snprintf(cmd, sizeof cmd, "env %s%s %s", envbuf, e->binname, e->args);
    spawn_cmd(cmd);
}

/* ------------------------------------------------------------- app "games" */

typedef struct {
    Fonts *F;
    int W, H, pad, cardH, listY, listH;
    int sel, hover, scroll, press, px, py, scroll0, moved;
} Games;

static int games_maxscroll(Games *g) {
    return imax(0, g_nprefixes * g->cardH - g->listH);
}

static void games_ensure_visible(Games *g) {
    int top = g->sel * g->cardH;
    if (top < g->scroll) g->scroll = top;
    if (top + g->cardH > g->scroll + g->listH) g->scroll = top + g->cardH - g->listH;
    g->scroll = imax(0, imin(g->scroll, games_maxscroll(g)));
}

static void games_layout(Games *g) {
    g->pad = sc(16);
    g->cardH = sc(84);
    g->listY = g->pad + sc(34) + sc(8) + sc(10);
    g->listH = g->H - g->listY - g->pad - sc(20);
}

static int games_hit(Games *g, int x, int y) {
    if (x < g->pad || x >= g->W - g->pad) return -1;
    if (y < g->listY || y >= g->listY + g->listH) return -1;
    int row = (y - g->listY + g->scroll) / g->cardH;
    if (row < 0 || row >= g_nprefixes) return -1;
    return row;
}

static void games_draw(App *a) {
    Games *g = a->u;
    Canvas *c = &a->w->cv;
    Fonts *F = g->F;
    clip_reset(c);
    fill_rect(c, 0, 0, g->W, g->H, C_BG, 1.f);
    Shape ps = panel_shape();
    draw_chamfer(c, 0, 0, (float)g->W, (float)g->H, &ps);

    int R = sc(15);
    draw_hex(c, (float)(g->pad + R), (float)(g->pad + R + sc(2)), (float)R, C_MAGENTA, 0.12f,
             fmaxf(1.f, S * 1.4f), 1.f, 0.9f, (float)sc(6));
    draw_text_c(c, F->uib, g->pad + R, g->pad + R + sc(2) - F->uib->height / 2, "G", C_MAGENTA, 1.f, 0);
    draw_text(c, F->title, g->pad + R * 2 + sc(12), g->pad, "JUEGOS SOPORTADOS", C_TEXT, 1.f, S * 2.f, 0);
    char cnt[64];
    snprintf(cnt, sizeof cnt, "%d PREFIX", g_nprefixes);
    int cw = text_width(F->smb, cnt, S);
    draw_text(c, F->smb, g->W - g->pad - cw, g->pad + sc(6), cnt, C_MAGENTA, 1.f, S, 0);

    clip_set(c, g->pad, g->listY, g->W - g->pad * 2, g->listH);
    int first = g->scroll / g->cardH;
    int last  = imin(g_nprefixes, (g->scroll + g->listH) / g->cardH + 2);
    for (int i = first; i < last; i++) {
        PrefixEntry *e = &g_prefixes[i];
        int ry = g->listY + i * g->cardH - g->scroll;
        int selected = (i == g->sel), hov = (g->hover == i);
        uint32_t scol = state_color(e->state);
        Shape cs;
        memset(&cs, 0, sizeof cs);
        cs.cut = (float)sc(10);
        cs.top = 0x0E1830; cs.bot = 0x0A1226; cs.fa = 1.f;
        cs.stroke = selected ? C_CYAN : (hov ? C_MUTED : C_LINE);
        cs.sa = selected ? 1.f : (hov ? 0.7f : 0.5f);
        cs.sw = fmaxf(1.f, S * 1.2f);
        if (selected) { cs.glow = C_CYAN; cs.ga = 0.2f; cs.gw = (float)sc(10); }
        draw_chamfer(c, (float)g->pad, (float)(ry + 3), (float)(g->W - g->pad * 2),
                     (float)(g->cardH - 6), &cs);

        int bw = text_width(F->smb, e->state, S * 0.5f) + sc(18);
        int bh = sc(20);
        int bx = g->W - g->pad - sc(10) - bw;
        int by = ry + 3 + sc(10);
        Shape bs;
        memset(&bs, 0, sizeof bs);
        bs.cut = (float)sc(5);
        bs.top = scol; bs.bot = scol; bs.fa = 0.22f;
        bs.stroke = scol; bs.sa = 1.f; bs.sw = fmaxf(1.f, S);
        draw_chamfer(c, (float)bx, (float)by, (float)bw, (float)bh, &bs);
        draw_text_c(c, F->smb, bx + bw / 2, by + (bh - F->smb->height) / 2,
                    e->state, scol, 1.f, S * 0.5f);

        int tx = g->pad + sc(14);
        int ty = ry + 3 + sc(10);
        int maxw = g->W - g->pad * 2 - sc(28) - bw - sc(16);
        draw_text(c, F->uib, tx, ty, e->name, selected ? C_CYAN : C_TEXT, 1.f, 0, maxw);
        ty += F->uib->height + sc(2);
        char sub[200];
        snprintf(sub, sizeof sub, "%s  ·  %s/%s", e->binname,
                 e->graphics[0] ? e->graphics : "gl",
                 e->audio[0] ? e->audio : "pulse");
        draw_text(c, F->sm, tx, ty, sub, C_MUTED, 1.f, 0, maxw);
        ty += F->sm->height + sc(4);
        draw_text(c, F->sm, tx, ty, e->description, C_DIM, 0.9f, 0, maxw);
    }
    if (g_nprefixes == 0)
        draw_text_c(c, F->ui, g->W / 2, g->listY + sc(40),
                    "Sin datos. Abrí Gladiator con internet una vez para descargar la lista.", C_MUTED, 1.f, S);
    if (games_maxscroll(g) > 0) {
        int tw = imax(3, sc(4)), tx2 = g->W - g->pad - tw;
        fill_rect(c, tx2, g->listY, tw, g->listH, C_LINE, 0.5f);
        int th = imax(sc(24), g->listH * g->listH / (g_nprefixes * g->cardH));
        int ty2 = g->listY + (g->listH - th) * g->scroll / games_maxscroll(g);
        fill_rect(c, tx2, ty2, tw, th, C_MAGENTA, 0.85f);
    }
    clip_reset(c);

    draw_text(c, F->sm, g->pad, g->H - g->pad - F->sm->height,
              "ENTER lanzar  ·  ↑↓ navegar  ·  ESC cerrar",
              C_MUTED, 1.f, S * 0.5f, g->W - g->pad * 2);
}

static void games_key(App *a, KeySym ks, int cp, unsigned st) {
    Games *g = a->u;
    (void)cp; (void)st;
    a->dirty = 1;
    if (ks == XK_Escape) { a->quit = 1; return; }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (g->sel >= 0 && g->sel < g_nprefixes) { launch_prefix(&g_prefixes[g->sel]); a->quit = 1; }
        return;
    }
    int page = imax(1, g->listH / g->cardH - 1);
    if (ks == XK_Up)   { g->sel = imax(0, g->sel - 1); games_ensure_visible(g); return; }
    if (ks == XK_Down) { g->sel = imin(imax(0, g_nprefixes - 1), g->sel + 1); games_ensure_visible(g); return; }
    if (ks == XK_Home) { g->sel = 0; games_ensure_visible(g); return; }
    if (ks == XK_End)  { g->sel = imax(0, g_nprefixes - 1); games_ensure_visible(g); return; }
    if (ks == XK_Page_Down) { g->sel = imin(imax(0, g_nprefixes - 1), g->sel + page); games_ensure_visible(g); return; }
    if (ks == XK_Page_Up)   { g->sel = imax(0, g->sel - page); games_ensure_visible(g); return; }
}

static void games_button(App *a, int x, int y, int button, int press) {
    Games *g = a->u;
    if (press && (button == 4 || button == 5)) {
        g->scroll = imax(0, imin(games_maxscroll(g), g->scroll + (button == 5 ? 1 : -1) * g->cardH * 2));
        a->dirty = 1;
        return;
    }
    if (button != 1) return;
    if (press) { g->press = 1; g->px = x; g->py = y; g->scroll0 = g->scroll; g->moved = 0; return; }
    if (!g->press) return;
    g->press = 0;
    if (g->moved) return;
    int h = games_hit(g, x, y);
    if (h >= 0 && h < g_nprefixes) { launch_prefix(&g_prefixes[h]); a->quit = 1; }
}

static void games_motion(App *a, int x, int y) {
    Games *g = a->u;
    if (g->press) {
        if (g->moved || abs(y - g->py) > sc(8)) {
            g->moved = 1;
            g->scroll = imax(0, imin(games_maxscroll(g), g->scroll0 - (y - g->py)));
            a->dirty = 1;
        }
        return;
    }
    int h = games_hit(g, x, y);
    if (h != g->hover) { g->hover = h; a->dirty = 1; }
}

static int run_games(void) {
    if (!single_instance("games")) return 0;
    char *json = fetch_prefixes_json();
    parse_prefixes(json);
    free(json);

    Games *g = calloc(1, sizeof *g);
    g->F = fonts_open();
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    g->W = imin(sc(620), sw - sc(8));
    g->H = imin(sc(560), sh - sc(60));
    g->hover = -1;
    games_layout(g);

    int x = (sw - g->W) / 2, y = (sh - g->H) / 2;
    Win *w = win_create(x, y, g->W, g->H, 1, "sesar-games");
    win_shape_chamfer(w, sc(18));
    XMapRaised(dpy, w->win);
    XSync(dpy, False);
    grab_input(w);

    App a = {0};
    a.w = w; a.u = g;
    a.draw = games_draw; a.key = games_key; a.button = games_button; a.motion = games_motion;
    run_app(&a);

    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XSync(dpy, False);
    return 0;
}


static void prefixes_load_once(void) {
    static int loaded = 0;
    if (loaded) return;
    loaded = 1;
    char *json = fetch_prefixes_json();
    if (json) { parse_prefixes(json); free(json); }
}

static PrefixEntry *prefix_for_exec(const char *exec) {
    prefixes_load_once();
    if (g_nprefixes == 0 || !exec || !*exec) return NULL;
    char first[512]; size_t i = 0;
    while (exec[i] && exec[i] != ' ' && i < sizeof first - 1) { first[i] = exec[i]; i++; }
    first[i] = 0;
    const char *base = strrchr(first, '/');
    base = base ? base + 1 : first;
    for (int k = 0; k < g_nprefixes; k++) {
        const char *bb = strrchr(g_prefixes[k].binname, '/');
        bb = bb ? bb + 1 : g_prefixes[k].binname;
        if (bb[0] && !strcmp(bb, base)) return &g_prefixes[k];
    }
    return NULL;
}

static void launch_app_ex(const AppEntry *a) {
    char cmd[4600];
    PrefixEntry *pe = prefix_for_exec(a->exec);
    if (pe) {
        char envbuf[4096]; envbuf[0] = 0;
        for (int i = 0; i < pe->nenv; i++) {
            size_t rem = sizeof envbuf - strlen(envbuf) - 1;
            strncat(envbuf, "'", rem); rem = sizeof envbuf - strlen(envbuf) - 1;
            strncat(envbuf, pe->env[i], rem); rem = sizeof envbuf - strlen(envbuf) - 1;
            strncat(envbuf, "' ", rem);
        }
        if (a->term)
            snprintf(cmd, sizeof cmd, "%s -e env %s%s %s", terminal_cmd(), envbuf, a->exec, pe->args);
        else
            snprintf(cmd, sizeof cmd, "env %s%s %s", envbuf, a->exec, pe->args);
        spawn_cmd(cmd);
        return;
    }
    if (a->term) snprintf(cmd, sizeof cmd, "%s -e %s", terminal_cmd(), a->exec);
    else snprintf(cmd, sizeof cmd, "%s", a->exec);
    spawn_cmd(cmd);
}

/* ------------------------------------------------------------------ main */
static void usage(void) {
    puts("sesar-shell: setup | session | menu | games | power | hud | wallpaper | xres | appmenu | files | uninstall");
}

static int x_error(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

int main(int argc, char **argv) {
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof self_path - 1);
    if (n > 0) self_path[n] = 0;
    else if (argc > 0) snprintf(self_path, sizeof self_path, "%s", argv[0]);
    {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", self_path);
        char *s = strrchr(tmp, '/');
        if (s) {
            *s = 0;
            s = strrchr(tmp, '/');
            if (s && !strcmp(s, "/bin")) { *s = 0; snprintf(prefix_dir, sizeof prefix_dir, "%s", tmp); }
        } else if (getenv("PREFIX")) snprintf(prefix_dir, sizeof prefix_dir, "%s", getenv("PREFIX"));
    }
    if (argc < 2) { usage(); return 1; }
    const char *mode = argv[1];

    if (!strcmp(mode, "setup")) return run_setup();
    if (!strcmp(mode, "uninstall")) return run_uninstall();
    if (!strcmp(mode, "appmenu")) return run_appmenu();
    if (!strcmp(mode, "files")) return run_files();
    if (!strcmp(mode, "help") || !strcmp(mode, "--help") || !strcmp(mode, "-h")) { usage(); return 0; }

    dpy = XOpenDisplay(NULL);
    if (!dpy) die("no puedo abrir el display (¿DISPLAY?)");
    XSetErrorHandler(x_error);
    scr = DefaultScreen(dpy);
    x_init();
    int sw = DisplayWidth(dpy, scr), sh = DisplayHeight(dpy, scr);
    S = clampf((float)imin(sw, sh) / 720.f, 1.f, 3.f);
    if (getenv("SESAR_SCALE")) S = clampf((float)atof(getenv("SESAR_SCALE")), 0.5f, 4.f);
    if (FT_Init_FreeType(&ftlib)) die("no se pudo iniciar FreeType");

    int rc = 0;
    if (!strcmp(mode, "menu")) rc = run_menu(argc, argv);
    else if (!strcmp(mode, "games")) rc = run_games();
    else if (!strcmp(mode, "power")) rc = run_power();
    else if (!strcmp(mode, "hud")) rc = run_hud();
    else if (!strcmp(mode, "wallpaper")) rc = run_wallpaper();
    else if (!strcmp(mode, "xres")) rc = run_xres();
    else if (!strcmp(mode, "session")) rc = run_session();
    else { usage(); rc = 1; }
    if (dpy) XCloseDisplay(dpy);
    return rc;
}
