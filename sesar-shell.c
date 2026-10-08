/*
 * sesar-shell  —  SESAR DE en C puro (Xlib + FreeType). Sin Python, sin GTK.
 *
 * Pensado para correr dentro de Gladiator (Ubuntu completo en el teléfono), sobre
 * JWM + xterm. No agrega dependencias nuevas: xrandr / pactl / amixer se usan solo
 * si existen (la función correspondiente se desactiva sola si no están).
 *
 * Compilar (Termux o Linux):
 *   cc -O2 -o sesar-shell sesar-shell.c $(pkg-config --cflags --libs x11 xext freetype2) -lm
 *
 * Uso:
 *   sesar-shell setup        genera ~/.jwmrc (barra, tema, atajos) desde la config
 *   sesar-shell session      aplica recursos X y ejecuta jwm
 *   sesar-shell menu         menú de inicio (Super+Espacio / botón SESAR)
 *   sesar-shell settings [p] ajustes: apariencia, iconos, barra, pantalla, atajos
 *   sesar-shell panel        panel rápido (volumen, escritorios, pantalla completa...)
 *   sesar-shell power        diálogo de energía / sesión      (Super+Esc)
 *   sesar-shell desktop      fondo + iconos del escritorio (~/Desktop)
 *   sesar-shell hud          HUD de sistema (CPU / RAM / batería)
 *   sesar-shell games        juegos soportados (prefixes de Gladiator)
 *   sesar-shell fullscreen   alterna pantalla completa de la ventana activa
 *   sesar-shell goto N       cambia al escritorio N      (también: next / prev)
 *   sesar-shell showdesktop  minimiza/restaura todas las ventanas
 *   sesar-shell reload       recarga fondo/iconos/HUD sin reiniciar la sesión
 *   sesar-shell autostart    lanza ~/.config/sesar/autostart y ~/.config/autostart
 *   sesar-shell xres | appmenu | files | wallpaper | uninstall
 *
 * Config: ~/.config/sesar/{config,keys,favorites,autostart,iconpos}
 * Variables: SESAR_SCALE (escala de UI), SESAR_FONT / SESAR_FONT_BOLD (rutas TTF),
 *            SESAR_TERM (terminal), SESAR_TRAY (alto de la barra en px).
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
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MARK "sesar-de"

/* ------------------------------------------------------------------ tema */
typedef struct {
    uint32_t bg, field, p1, p2, text, dim, muted, line, btn1, btn2;
    uint32_t a1, a2, a1_hi, a1_lo, a1_mid;
    int light;
    float glow;
} Theme;
static Theme T;

#define C_BG      (T.bg)
#define C_FIELD   (T.field)
#define C_PANEL1  (T.p1)
#define C_PANEL2  (T.p2)
#define C_CYAN    (T.a1)       /* acento principal */
#define C_MAGENTA (T.a2)       /* acento secundario */
#define A1_HI     (T.a1_hi)
#define A1_MID    (T.a1_mid)
#define A1_LO     (T.a1_lo)
#define C_BTN1    (T.btn1)
#define C_BTN2    (T.btn2)
#define C_GREEN   0x39FF88
#define C_AMBER   0xFFB020
#define C_RED     0xFF3B5C
#define C_VIOLET  0x7C4DFF
#define C_TEXT    (T.text)
#define C_DIM     (T.dim)
#define C_MUTED   (T.muted)
#define C_LINE    (T.line)

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
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void die(const char *msg) { fprintf(stderr, "sesar-shell: %s\n", msg); exit(1); }

static char *xstrdup(const char *s) {
    char *p = strdup(s ? s : "");
    if (!p) die("sin memoria");
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("sin memoria");
    return q;
}

static int file_exists(const char *p) { return access(p, F_OK) == 0; }
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }

static void scopy(char *dst, size_t n, const char *src) {
    if (!n) return;
    snprintf(dst, n, "%s", src ? src : "");
}

static uint32_t mix(uint32_t d, uint32_t s, float a);

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
        signal(SIGUSR1, SIG_DFL);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        if (dpy) close(ConnectionNumber(dpy));
        execlp("sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void spawn_fmt(const char *fmt, ...) {
    char buf[3000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    spawn_cmd(buf);
}

static void reap(void) { while (waitpid(-1, NULL, WNOHANG) > 0) {} }

/* Ejecuta un comando y devuelve su salida (sin el \n final). 0 si falló. */
static int cmd_output(const char *cmd, char *out, size_t n) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    size_t got = fread(out, 1, n - 1, p);
    out[got] = 0;
    int rc = pclose(p);
    while (got && (out[got - 1] == '\n' || out[got - 1] == '\r')) out[--got] = 0;
    return rc == 0;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

static unsigned char *read_bin(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fputs(data, f);
    fclose(f);
    return 1;
}

static int copy_file(const char *from, const char *to) {
    char *d = read_file(from);
    if (!d) return 0;
    int ok = write_file(to, d);
    free(d);
    return ok;
}

static void mkdir_p(const char *path) {
    char tmp[1400];
    scopy(tmp, sizeof tmp, path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    return h && *h ? h : "/tmp";
}

static void cfg_path(const char *file, char *out, size_t n) {
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) snprintf(out, n, "%s/sesar/%s", x, file);
    else snprintf(out, n, "%s/.config/sesar/%s", home_dir(), file);
}

static void ensure_cfg_dir(void) {
    char p[1400];
    cfg_path("x", p, sizeof p);
    char *s = strrchr(p, '/');
    if (s) { *s = 0; mkdir_p(p); }
}

static void desktop_dir(char *out, size_t n) {
    snprintf(out, n, "%s/Desktop", home_dir());
    if (!is_dir(out)) {
        char alt[1400];
        snprintf(alt, sizeof alt, "%s/Escritorio", home_dir());
        if (is_dir(alt)) scopy(out, n, alt);
    }
}

/* una sola instancia por nombre; guarda el pid para poder señalizarla (reload) */
static int single_instance(const char *name) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char path[1100];
    snprintf(path, sizeof path, "%s/sesar-%s-%d.lock", dir, name, (int)getuid());
    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return 1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) return 0;
    if (ftruncate(fd, 0) == 0) { char b[32]; int L = snprintf(b, sizeof b, "%d\n", (int)getpid()); if (write(fd, b, (size_t)L) < 0) {} }
    return 1; /* el fd queda abierto mientras viva el proceso */
}

static void signal_instance(const char *name, int sig) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char path[1100];
    snprintf(path, sizeof path, "%s/sesar-%s-%d.lock", dir, name, (int)getuid());
    char *s = read_file(path);
    if (!s) return;
    int pid = atoi(s);
    free(s);
    if (pid > 1) kill((pid_t)pid, sig);
}

static volatile sig_atomic_t g_reload = 0;
static void on_usr1(int s) { (void)s; g_reload = 1; }
static void install_reload_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);   /* sin SA_RESTART: interrumpe select() */
}

/* ------------------------------------------------------------------ configuración */
typedef struct {
    int theme, accent, wall;
    char wall_img[512];
    int scale, tray_size, tray_top, tray_autohide;
    int clock24, clock_date, ndesk;
    int hud, hud_pos;
    int icons_real, icon_shape, icon_size;
    char icon_theme[64];
    int desk_icons, desk_defaults, single_click;
    int win_border, win_maximize, term_font;
    char term[64];
} Cfg;
static Cfg cfg;

static void cfg_defaults(void) {
    memset(&cfg, 0, sizeof cfg);
    cfg.theme = 0; cfg.accent = 0; cfg.wall = 0;
    cfg.scale = 0; cfg.tray_size = 0; cfg.tray_top = 0; cfg.tray_autohide = 0;
    cfg.clock24 = 1; cfg.clock_date = 1; cfg.ndesk = 4;
    cfg.hud = 1; cfg.hud_pos = 1;
    cfg.icons_real = 1; cfg.icon_shape = 0; cfg.icon_size = 1;
    cfg.desk_icons = 1; cfg.desk_defaults = 1; cfg.single_click = 1;
    cfg.win_border = 1; cfg.win_maximize = 0; cfg.term_font = 11;
}

typedef struct { const char *k; int *i; char *s; size_t n; } CfgKey;
static int cfg_table(CfgKey *t) {
    int n = 0;
#define KI(name, field) t[n++] = (CfgKey){name, &cfg.field, NULL, 0}
#define KS(name, field) t[n++] = (CfgKey){name, NULL, cfg.field, sizeof cfg.field}
    KI("theme", theme); KI("accent", accent); KI("wallpaper", wall); KS("wallpaper_image", wall_img);
    KI("scale", scale); KI("tray_size", tray_size); KI("tray_top", tray_top); KI("tray_autohide", tray_autohide);
    KI("clock24", clock24); KI("clock_date", clock_date); KI("desktops", ndesk);
    KI("hud", hud); KI("hud_pos", hud_pos);
    KI("icons_real", icons_real); KI("icon_shape", icon_shape); KI("icon_size", icon_size); KS("icon_theme", icon_theme);
    KI("desktop_icons", desk_icons); KI("desktop_defaults", desk_defaults); KI("single_click", single_click);
    KI("win_border", win_border); KI("win_maximize", win_maximize); KI("term_font", term_font); KS("terminal", term);
#undef KI
#undef KS
    return n;
}

static void cfg_load(void) {
    cfg_defaults();
    char p[1400];
    cfg_path("config", p, sizeof p);
    char *txt = read_file(p);
    if (!txt) return;
    CfgKey t[40];
    int nt = cfg_table(t);
    char *save = NULL;
    for (char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        while (*ln == ' ') ln++;
        if (*ln == '#' || !*ln) continue;
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        size_t L = strlen(v);
        while (L && (v[L - 1] == '\r' || v[L - 1] == ' ')) v[--L] = 0;
        for (int i = 0; i < nt; i++)
            if (!strcmp(t[i].k, ln)) {
                if (t[i].i) *t[i].i = atoi(v);
                else scopy(t[i].s, t[i].n, v);
            }
    }
    free(txt);
    cfg.theme = iclamp(cfg.theme, 0, 2);
    cfg.accent = iclamp(cfg.accent, 0, 6);
    cfg.wall = iclamp(cfg.wall, 0, 4);
    cfg.ndesk = iclamp(cfg.ndesk, 1, 6);
    cfg.tray_size = iclamp(cfg.tray_size, 0, 3);
    cfg.icon_shape = iclamp(cfg.icon_shape, 0, 3);
    cfg.icon_size = iclamp(cfg.icon_size, 0, 2);
    cfg.win_border = iclamp(cfg.win_border, 0, 2);
    cfg.hud_pos = iclamp(cfg.hud_pos, 0, 3);
    cfg.term_font = iclamp(cfg.term_font, 6, 32);
}

static int cfg_save(void) {
    ensure_cfg_dir();
    char p[1400];
    cfg_path("config", p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    fputs("# sesar-shell: configuración (se edita desde 'sesar-shell settings')\n", f);
    CfgKey t[40];
    int nt = cfg_table(t);
    for (int i = 0; i < nt; i++) {
        if (t[i].i) fprintf(f, "%s=%d\n", t[i].k, *t[i].i);
        else fprintf(f, "%s=%s\n", t[i].k, t[i].s);
    }
    fclose(f);
    return 1;
}

static const char *ACCENT_NAMES[] = {"Cian", "Magenta", "Verde", "Ámbar", "Violeta", "Azul", "Rojo"};
static const uint32_t ACCENT_A1[] = {0x00E5FF, 0xFF2BD6, 0x39FF88, 0xFFB020, 0x7C4DFF, 0x4D7CFF, 0xFF3B5C};
static const uint32_t ACCENT_A2[] = {0xFF2BD6, 0x00E5FF, 0x00E5FF, 0xFF3B5C, 0x00E5FF, 0xFF2BD6, 0xFFB020};
static const char *THEME_NAMES[] = {"Neón", "Carbón", "Claro"};

static void theme_apply(void) {
    int light = cfg.theme == 2;
    if (cfg.theme == 0) {
        T = (Theme){0x04060D, 0x070C1A, 0x0E1730, 0x090F22, 0xE8F4FF, 0xB4C4E0, 0x6F82A8, 0x24365E,
                    0x0E1830, 0x0A1226, 0, 0, 0, 0, 0, 0, 1.0f};
    } else if (cfg.theme == 1) {
        T = (Theme){0x0A0A0C, 0x101013, 0x1C1C22, 0x131317, 0xF0F0F4, 0xC0C0CA, 0x7C7C88, 0x34343E,
                    0x1E1E25, 0x16161B, 0, 0, 0, 0, 0, 0, 0.55f};
    } else {
        T = (Theme){0xE9EDF5, 0xFFFFFF, 0xFFFFFF, 0xE3E8F2, 0x121A2E, 0x3A4660, 0x66748F, 0xC2CADA,
                    0xF4F6FB, 0xE0E5EF, 0, 0, 0, 0, 0, 0, 0.18f};
    }
    uint32_t a1 = ACCENT_A1[cfg.accent], a2 = ACCENT_A2[cfg.accent];
    if (light) { a1 = mix(a1, 0x000000, 0.38f); a2 = mix(a2, 0x000000, 0.38f); }
    T.a1 = a1; T.a2 = a2; T.light = light;
    T.a1_hi = mix(a1, 0xFFFFFF, 0.45f);
    T.a1_lo = mix(a1, 0x000000, 0.35f);
    T.a1_mid = mix(a1, 0x000000, 0.12f);
}

static void scale_apply(int sw, int sh) {
    float s = clampf((float)imin(sw, sh) / 720.f, 1.f, 3.f);
    if (cfg.scale > 0) s = cfg.scale / 100.f;
    if (getenv("SESAR_SCALE")) s = (float)atof(getenv("SESAR_SCALE"));
    S = clampf(s, 0.5f, 4.f);
}

static int tray_height(void) {
    const char *e = getenv("SESAR_TRAY");
    if (e && atoi(e) > 0) return atoi(e);
    static const int base[] = {40, 32, 40, 54};
    return sc((float)base[cfg.tray_size]);
}

static const char *terminal_cmd(void) {
    const char *t = getenv("SESAR_TERM");
    if (t && *t) return t;
    if (cfg.term[0]) return cfg.term;
    if (!which("xterm", NULL, 0) && which("aterm", NULL, 0)) return "aterm";
    return "xterm";
}

/* constructor de cadenas */
typedef struct { char *p; size_t n, cap; } Sb;
static void sb_add(Sb *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int w = vsnprintf(b->p ? b->p + b->n : NULL, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (w < 0) return;
        if ((size_t)w < b->cap - b->n) { b->n += (size_t)w; return; }
        b->cap = b->cap ? b->cap * 2 + (size_t)w : 4096 + (size_t)w;
        b->p = xrealloc(b->p, b->cap);
    }
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



/* ------------------------------------------------------------------ formas de icono */
static float sdf_rrect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float dx = fabsf(px - cx) - (hw - r), dy = fabsf(py - cy) - (hh - r);
    float ox = fmaxf(dx, 0.f), oy = fmaxf(dy, 0.f);
    return sqrtf(ox * ox + oy * oy) + fminf(fmaxf(dx, dy), 0.f) - r;
}

/* shape: 0 hexágono, 1 cuadrado redondeado, 2 círculo, 3 cuadrado biselado */
static float sdf_badge(int shape, float px, float py, float cx, float cy, float R) {
    switch (shape) {
    case 1: return sdf_rrect(px, py, cx, cy, R * 0.92f, R * 0.92f, R * 0.34f);
    case 2: return sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy)) - R;
    case 3: return sdf_chamfer(px, py, cx - R * 0.92f, cy - R * 0.92f, R * 1.84f, R * 1.84f, R * 0.55f);
    default: return sdf_hex(px - cx, py - cy, R * 0.8660254f);
    }
}

static void draw_badge(Canvas *c, float cx, float cy, float R, int shape, uint32_t col,
                       float fill_a, float sw, float stroke_a, float glow_a, float gw) {
    int x0 = imax((int)floorf(cx - R - gw) - 1, c->cx0), x1 = imin((int)ceilf(cx + R + gw) + 1, c->cx1);
    int y0 = imax((int)floorf(cy - R - gw) - 1, c->cy0), y1 = imin((int)ceilf(cy + R + gw) + 1, c->cy1);
    for (int iy = y0; iy < y1; iy++)
        for (int ix = x0; ix < x1; ix++) {
            float d = sdf_badge(shape, ix + 0.5f, iy + 0.5f, cx, cy, R);
            if (d > gw) continue;
            if (d > 0.f) {
                if (glow_a > 0.f) { float g = 1.f - d / gw; put(c, ix, iy, col, g * g * glow_a); }
                continue;
            }
            float cov = clamp01(0.5f - d);
            if (fill_a > 0.f) put(c, ix, iy, col, cov * fill_a);
            if (glow_a > 0.f && gw > 0.f) { float g = clamp01(1.f + d / gw); put(c, ix, iy, col, g * g * glow_a * 0.6f); }
            if (sw > 0.f) {
                float sc2 = clamp01(0.5f - (fabsf(d + sw * 0.5f) - sw * 0.5f));
                if (sc2 > 0.f) put(c, ix, iy, col, sc2 * stroke_a);
            }
        }
}

/* ------------------------------------------------------------------ imágenes (PNG propio, sin dependencias) */
typedef struct { int w, h; uint32_t *px; } Img;   /* 0xAARRGGBB, alfa directo */

typedef struct {
    const unsigned char *src; size_t len, pos;
    uint32_t bb; int bc;
    unsigned char *out; size_t n, cap;
    int err;
} Inf;
typedef struct { uint16_t count[16], sym[320]; } Huff;

static int inf_bits(Inf *s, int n) {
    while (s->bc < n) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        s->bb |= (uint32_t)s->src[s->pos++] << s->bc;
        s->bc += 8;
    }
    int v = (int)(s->bb & ((1u << n) - 1));
    s->bb >>= n; s->bc -= n;
    return v;
}

static void huff_build(Huff *h, const unsigned char *len, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++) if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
}

static int huff_decode(Inf *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= inf_bits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    s->err = 1;
    return -1;
}

static void inf_put(Inf *s, unsigned char b) {
    if (s->n == s->cap) {
        if (s->cap > ((size_t)1 << 30)) { s->err = 1; return; }
        s->cap = s->cap ? s->cap * 2 : 1 << 16;
        s->out = xrealloc(s->out, s->cap);
    }
    s->out[s->n++] = b;
}

static int inf_codes(Inf *s, const Huff *lc, const Huff *dc) {
    static const uint16_t lb[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint16_t le[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t db[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint16_t de[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = huff_decode(s, lc);
        if (s->err || sym < 0) return -1;
        if (sym < 256) inf_put(s, (unsigned char)sym);
        else if (sym == 256) return 0;
        else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = lb[sym] + inf_bits(s, le[sym]);
            int ds = huff_decode(s, dc);
            if (s->err || ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)db[ds] + (size_t)inf_bits(s, de[ds]);
            if (dist > s->n) return -1;
            for (int i = 0; i < len; i++) inf_put(s, s->out[s->n - dist]);
        }
        if (s->err) return -1;
    }
}

static unsigned char *zlib_inflate(const unsigned char *src, size_t len, size_t *outlen) {
    Inf s;
    memset(&s, 0, sizeof s);
    if (len < 2) return NULL;
    s.src = src + 2; s.len = len - 2;
    int last;
    do {
        last = inf_bits(&s, 1);
        int type = inf_bits(&s, 2);
        if (s.err) break;
        if (type == 0) {
            s.bb = 0; s.bc = 0;
            if (s.pos + 4 > s.len) { s.err = 1; break; }
            unsigned L = s.src[s.pos] | (unsigned)(s.src[s.pos + 1] << 8);
            s.pos += 4;
            if (s.pos + L > s.len) { s.err = 1; break; }
            for (unsigned i = 0; i < L; i++) inf_put(&s, s.src[s.pos++]);
        } else if (type == 1) {
            unsigned char l[320];
            Huff lc, dc;
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(&lc, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dc, l, 30);
            if (inf_codes(&s, &lc, &dc)) s.err = 1;
        } else if (type == 2) {
            static const unsigned char ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            unsigned char l[320];
            Huff lc, dc;
            int nl = inf_bits(&s, 5) + 257, nd = inf_bits(&s, 5) + 1, nc = inf_bits(&s, 4) + 4;
            if (nl > 286 || nd > 30) { s.err = 1; break; }
            memset(l, 0, sizeof l);
            for (int i = 0; i < nc; i++) l[ord[i]] = (unsigned char)inf_bits(&s, 3);
            huff_build(&lc, l, 19);
            unsigned char ll[320];
            memset(ll, 0, sizeof ll);
            int idx = 0;
            while (idx < nl + nd && !s.err) {
                int sym = huff_decode(&s, &lc);
                if (sym < 0) break;
                if (sym < 16) ll[idx++] = (unsigned char)sym;
                else {
                    int rep, val = 0;
                    if (sym == 16) { if (!idx) { s.err = 1; break; } val = ll[idx - 1]; rep = 3 + inf_bits(&s, 2); }
                    else if (sym == 17) rep = 3 + inf_bits(&s, 3);
                    else rep = 11 + inf_bits(&s, 7);
                    if (idx + rep > nl + nd) { s.err = 1; break; }
                    while (rep--) ll[idx++] = (unsigned char)val;
                }
            }
            if (s.err) break;
            huff_build(&lc, ll, nl);
            huff_build(&dc, ll + nl, nd);
            if (inf_codes(&s, &lc, &dc)) s.err = 1;
        } else s.err = 1;
    } while (!last && !s.err);
    if (s.err) { free(s.out); return NULL; }
    *outlen = s.n;
    return s.out;
}

static uint32_t be32(const unsigned char *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static Img *png_load(const char *path) {
    size_t len = 0;
    unsigned char *d = read_bin(path, &len);
    if (!d) return NULL;
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (len < 33 || memcmp(d, sig, 8)) { free(d); return NULL; }
    uint32_t w = 0, h = 0;
    int depth = 0, ct = 0, il = 0;
    unsigned char pal[256][4];
    int npal = 0;
    for (int i = 0; i < 256; i++) { pal[i][0] = pal[i][1] = pal[i][2] = 0; pal[i][3] = 255; }
    unsigned char *idat = NULL;
    size_t nidat = 0;
    size_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t cl = be32(d + pos);
        const unsigned char *t = d + pos + 4, *cd = d + pos + 8;
        if (pos + 12 + cl > len) break;
        if (!memcmp(t, "IHDR", 4) && cl >= 13) {
            w = be32(cd); h = be32(cd + 4); depth = cd[8]; ct = cd[9]; il = cd[12];
        } else if (!memcmp(t, "PLTE", 4)) {
            npal = (int)imin((int)(cl / 3), 256);
            for (int i = 0; i < npal; i++) { pal[i][0] = cd[i * 3]; pal[i][1] = cd[i * 3 + 1]; pal[i][2] = cd[i * 3 + 2]; }
        } else if (!memcmp(t, "tRNS", 4) && ct == 3) {
            for (uint32_t i = 0; i < cl && i < 256; i++) pal[i][3] = cd[i];
        } else if (!memcmp(t, "IDAT", 4)) {
            idat = xrealloc(idat, nidat + cl);
            memcpy(idat + nidat, cd, cl);
            nidat += cl;
        } else if (!memcmp(t, "IEND", 4)) break;
        pos += 12 + cl;
    }
    free(d);
    Img *im = NULL;
    unsigned char *raw = NULL;
    if (!w || !h || w > 16384 || h > 16384 || (uint64_t)w * h > 64000000ULL || il != 0 || !idat) goto done;
    int ch = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
    if (!ch || (depth != 8 && depth != 16 && !(ct == 0 || ct == 3))) goto done;
    size_t rawn = 0;
    raw = zlib_inflate(idat, nidat, &rawn);
    if (!raw) goto done;
    size_t bpp = (size_t)imax(1, ch * depth / 8);
    size_t stride = ((size_t)w * (size_t)ch * (size_t)depth + 7) / 8;
    if (rawn < (stride + 1) * h) goto done;
    unsigned char *prev = calloc(stride, 1), *cur = calloc(stride, 1);
    im = malloc(sizeof *im);
    im->w = (int)w; im->h = (int)h;
    im->px = malloc((size_t)w * h * 4);
    if (!prev || !cur || !im->px) { free(prev); free(cur); if (im) { free(im->px); free(im); im = NULL; } goto done; }
    for (uint32_t y = 0; y < h; y++) {
        const unsigned char *row = raw + (size_t)y * (stride + 1);
        int ft = row[0];
        memcpy(cur, row + 1, stride);
        for (size_t i = 0; i < stride; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, v = cur[i];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                      v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
            default: break;
            }
            cur[i] = (unsigned char)v;
        }
        for (uint32_t x = 0; x < w; x++) {
            unsigned r = 0, g = 0, b = 0, a = 255;
            if (depth == 8 || depth == 16) {
                int step = depth / 8;
                const unsigned char *p = cur + (size_t)x * (size_t)ch * (size_t)step;
                if (ct == 0) r = g = b = p[0];
                else if (ct == 2) { r = p[0]; g = p[step]; b = p[2 * step]; }
                else if (ct == 3) { const unsigned char *q = pal[p[0]]; r = q[0]; g = q[1]; b = q[2]; a = q[3]; }
                else if (ct == 4) { r = g = b = p[0]; a = p[step]; }
                else { r = p[0]; g = p[step]; b = p[2 * step]; a = p[3 * step]; }
            } else {
                size_t bit = (size_t)x * (size_t)depth;
                unsigned v = (cur[bit / 8] >> (8 - depth - (int)(bit % 8))) & ((1u << depth) - 1);
                if (ct == 3) { const unsigned char *q = pal[v]; r = q[0]; g = q[1]; b = q[2]; a = q[3]; }
                else { r = g = b = v * 255 / ((1u << depth) - 1); }
            }
            im->px[(size_t)y * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        unsigned char *t2 = prev; prev = cur; cur = t2;
    }
    free(prev); free(cur);
done:
    free(raw); free(idat);
    return im;
}

static void img_free(Img *im) { if (im) { free(im->px); free(im); } }

/* Remuestrea el rectángulo (sx,sy,sw,sh) de src a dw x dh (filtro de caja / bilineal). */
static Img *img_resample(const Img *src, float sx, float sy, float sw, float sh, int dw, int dh) {
    if (dw < 1 || dh < 1) return NULL;
    Img *o = malloc(sizeof *o);
    o->w = dw; o->h = dh;
    o->px = malloc((size_t)dw * (size_t)dh * 4);
    float kx = sw / dw, ky = sh / dh;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            float x0 = sx + x * kx, x1 = x0 + kx, y0 = sy + y * ky, y1 = y0 + ky;
            float ar = 0, ag = 0, ab = 0, aa = 0, wsum = 0;
            if (kx <= 1.f && ky <= 1.f) {   /* ampliación: bilineal */
                float fx = clampf(x0 + kx * 0.5f - 0.5f, 0, (float)src->w - 1), fy = clampf(y0 + ky * 0.5f - 0.5f, 0, (float)src->h - 1);
                int ix = (int)fx, iy = (int)fy, jx = imin(ix + 1, src->w - 1), jy = imin(iy + 1, src->h - 1);
                float tx = fx - ix, ty = fy - iy;
                int px[4] = {iy * src->w + ix, iy * src->w + jx, jy * src->w + ix, jy * src->w + jx};
                float wt[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                for (int k = 0; k < 4; k++) {
                    uint32_t p = src->px[px[k]];
                    float a = (p >> 24) * wt[k];
                    aa += a; ar += ((p >> 16) & 255) * a; ag += ((p >> 8) & 255) * a; ab += (p & 255) * a;
                }
                wsum = 1.f;
            } else {
                int ix0 = (int)floorf(x0), ix1 = (int)ceilf(x1), iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
                ix0 = imax(ix0, 0); iy0 = imax(iy0, 0); ix1 = imin(ix1, src->w); iy1 = imin(iy1, src->h);
                for (int j = iy0; j < iy1; j++) {
                    float wy = fminf(y1, j + 1.f) - fmaxf(y0, (float)j);
                    for (int i = ix0; i < ix1; i++) {
                        float wx = fminf(x1, i + 1.f) - fmaxf(x0, (float)i), wgt = wx * wy;
                        uint32_t p = src->px[(size_t)j * src->w + i];
                        float a = (p >> 24) * wgt;
                        aa += a; ar += ((p >> 16) & 255) * a; ag += ((p >> 8) & 255) * a; ab += (p & 255) * a;
                        wsum += wgt;
                    }
                }
            }
            uint32_t r = 0, g = 0, b = 0, a = 0;
            if (aa > 0.f && wsum > 0.f) {
                r = (uint32_t)lrintf(ar / aa); g = (uint32_t)lrintf(ag / aa); b = (uint32_t)lrintf(ab / aa);
                a = (uint32_t)lrintf(aa / wsum);
            }
            o->px[(size_t)y * dw + x] = (imin((int)a, 255) << 24) | ((uint32_t)imin((int)r, 255) << 16) | ((uint32_t)imin((int)g, 255) << 8) | (uint32_t)imin((int)b, 255);
        }
    return o;
}

static Img *img_fit(const Img *src, int box) {
    float k = (float)box / (float)imax(src->w, src->h);
    return img_resample(src, 0, 0, (float)src->w, (float)src->h, imax(1, (int)lrintf(src->w * k)), imax(1, (int)lrintf(src->h * k)));
}

/* escala para cubrir dw x dh recortando el exceso */
static Img *img_cover(const Img *src, int dw, int dh) {
    float sa = (float)src->w / src->h, da = (float)dw / dh, sx = 0, sy = 0, sw = (float)src->w, sh = (float)src->h;
    if (sa > da) { sw = src->h * da; sx = (src->w - sw) / 2.f; }
    else { sh = src->w / da; sy = (src->h - sh) / 2.f; }
    return img_resample(src, sx, sy, sw, sh, dw, dh);
}

static void blit_img(Canvas *c, const Img *im, int x, int y, float alpha) {
    for (int iy = 0; iy < im->h; iy++) {
        int py = y + iy;
        if (py < c->cy0 || py >= c->cy1) continue;
        for (int ix = 0; ix < im->w; ix++) {
            uint32_t p = im->px[(size_t)iy * im->w + ix];
            uint32_t a = p >> 24;
            if (a) put(c, x + ix, py, p & 0xFFFFFF, (a / 255.f) * alpha);
        }
    }
}

/* ------------------------------------------------------------------ iconos del sistema */
static char *icon_roots[8];
static int nicon_roots;

static void icon_roots_init(void) {
    if (nicon_roots) return;
    char b[1400];
    const char *xh = getenv("XDG_DATA_HOME"), *xd = getenv("XDG_DATA_DIRS");
    if (xh && *xh) snprintf(b, sizeof b, "%s/icons", xh); else snprintf(b, sizeof b, "%s/.local/share/icons", home_dir());
    icon_roots[nicon_roots++] = xstrdup(b);
    snprintf(b, sizeof b, "%s/.icons", home_dir());
    icon_roots[nicon_roots++] = xstrdup(b);
    char *copy = xstrdup(xd && *xd ? xd : "/usr/local/share:/usr/share"), *save = NULL;
    for (char *t = strtok_r(copy, ":", &save); t && nicon_roots < 6; t = strtok_r(NULL, ":", &save)) {
        snprintf(b, sizeof b, "%s/icons", t);
        icon_roots[nicon_roots++] = xstrdup(b);
    }
    free(copy);
    if (prefix_dir[0]) { snprintf(b, sizeof b, "%s/share/icons", prefix_dir); icon_roots[nicon_roots++] = xstrdup(b); }
    icon_roots[nicon_roots++] = xstrdup("/usr/share/icons");
}

static int has_png_ext(const char *s) { size_t L = strlen(s); return L > 4 && !strcasecmp(s + L - 4, ".png"); }

/* kind 0: aplicaciones, 1: lugares/archivos */
static char *icon_find(const char *name, int want, int kind) {
    if (!name || !*name) return NULL;
    if (name[0] == '/') return file_exists(name) && has_png_ext(name) ? xstrdup(name) : NULL;
    icon_roots_init();
    char base[256];
    scopy(base, sizeof base, name);
    if (has_png_ext(base)) base[strlen(base) - 4] = 0;
    static const int sizes[] = {16, 22, 24, 32, 36, 48, 64, 72, 96, 128, 192, 256, 512};
    int order[13], n = 13;
    for (int i = 0; i < n; i++) order[i] = sizes[i];
    for (int i = 0; i < n; i++)    /* más cercano primero, preferiendo >= want */
        for (int j = i + 1; j < n; j++) {
            int di = abs(order[i] - want) + (order[i] < want ? 1000 : 0), dj = abs(order[j] - want) + (order[j] < want ? 1000 : 0);
            if (dj < di) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }
    const char *themes[10];
    int nt = 0;
    if (cfg.icon_theme[0]) themes[nt++] = cfg.icon_theme;
    static const char *std[] = {"hicolor", "Yaru", "Adwaita", "gnome", "Papirus", "breeze", "oxygen", "elementary"};
    for (int i = 0; i < 8; i++) themes[nt++] = std[i];
    static const char *ctx0[] = {"apps", "categories", NULL};
    static const char *ctx1[] = {"places", "mimetypes", "devices", NULL};
    const char *const *ctx = kind ? ctx1 : ctx0;
    char p[1600];
    for (int t = 0; t < nt; t++)
        for (int r = 0; r < nicon_roots; r++) {
            snprintf(p, sizeof p, "%s/%s", icon_roots[r], themes[t]);
            if (!is_dir(p)) continue;
            for (int s = 0; s < n; s++)
                for (int c = 0; ctx[c]; c++) {
                    snprintf(p, sizeof p, "%s/%s/%dx%d/%s/%s.png", icon_roots[r], themes[t], order[s], order[s], ctx[c], base);
                    if (file_exists(p)) return xstrdup(p);
                    snprintf(p, sizeof p, "%s/%s/%s/%d/%s.png", icon_roots[r], themes[t], ctx[c], order[s], base);
                    if (file_exists(p)) return xstrdup(p);
                }
        }
    snprintf(p, sizeof p, "/usr/share/pixmaps/%s.png", base);
    if (file_exists(p)) return xstrdup(p);
    if (prefix_dir[0]) {
        snprintf(p, sizeof p, "%s/share/pixmaps/%s.png", prefix_dir, base);
        if (file_exists(p)) return xstrdup(p);
    }
    return NULL;
}

typedef struct { char *key; Img *img; } IconC;
static IconC *icache;
static int nicache;

static Img *icon_get(const char *name, int px, int kind) {
    if (!cfg.icons_real || !name || !*name || px < 8) return NULL;
    char key[400];
    snprintf(key, sizeof key, "%s|%d|%d|%s", name, px, kind, cfg.icon_theme);
    for (int i = 0; i < nicache; i++) if (!strcmp(icache[i].key, key)) return icache[i].img;
    Img *out = NULL;
    char *path = icon_find(name, px, kind);
    if (path) {
        Img *src = png_load(path);
        if (src) { out = img_fit(src, px); img_free(src); }
        free(path);
    }
    icache = xrealloc(icache, sizeof *icache * (size_t)(nicache + 1));
    icache[nicache].key = xstrdup(key);
    icache[nicache].img = out;
    nicache++;
    return out;
}

static void icon_cache_clear(void) {
    for (int i = 0; i < nicache; i++) { free(icache[i].key); img_free(icache[i].img); }
    free(icache); icache = NULL; nicache = 0;
}

/* lista de temas de iconos instalados */
static int list_icon_themes(char names[][64], int max) {
    icon_roots_init();
    int n = 0;
    for (int r = 0; r < nicon_roots && n < max; r++) {
        DIR *d = opendir(icon_roots[r]);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            if (e->d_name[0] == '.') continue;
            char p[1600];
            snprintf(p, sizeof p, "%s/%s/index.theme", icon_roots[r], e->d_name);
            if (!file_exists(p) || !strcmp(e->d_name, "default")) continue;
            int dup = 0;
            for (int i = 0; i < n; i++) if (!strcmp(names[i], e->d_name)) dup = 1;
            if (!dup) scopy(names[n++], 64, e->d_name);
        }
        closedir(d);
    }
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (strcasecmp(names[j], names[i]) < 0) { char t[64]; scopy(t, 64, names[i]); scopy(names[i], 64, names[j]); scopy(names[j], 64, t); }
    return n;
}


static int screen_w(void);
static int screen_h(void);
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
static Atom A_PROTOCOLS, A_DELETE, A_NET_ACTIVE, A_NET_CURDESK, A_NET_NUMDESK, A_NET_STATE, A_NET_FULL,
            A_NET_SHOWDESK, A_NET_WMTYPE, A_TYPE_DIALOG, A_NET_WMNAME, A_UTF8;

static void x_init(void) {
    vis = DefaultVisual(dpy, scr);
    depth = DefaultDepth(dpy, scr);
    std_masks = (vis->red_mask == 0xFF0000 && vis->green_mask == 0x00FF00 && vis->blue_mask == 0x0000FF);
    A_PROTOCOLS = XInternAtom(dpy, "WM_PROTOCOLS", False);
    A_DELETE = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    A_NET_ACTIVE = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_NET_CURDESK = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);
    A_NET_NUMDESK = XInternAtom(dpy, "_NET_NUMBER_OF_DESKTOPS", False);
    A_NET_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_NET_FULL = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    A_NET_SHOWDESK = XInternAtom(dpy, "_NET_SHOWING_DESKTOP", False);
    A_NET_WMTYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    A_TYPE_DIALOG = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    A_NET_WMNAME = XInternAtom(dpy, "_NET_WM_NAME", False);
    A_UTF8 = XInternAtom(dpy, "UTF8_STRING", False);
}

static void win_make_buffers(Win *W) {
    W->cv = canvas_new(W->w, W->h);
    W->conv = std_masks ? NULL : calloc((size_t)W->w * (size_t)W->h, 4);
    W->img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0,
                          (char *)(std_masks ? W->cv.px : W->conv), (unsigned)W->w, (unsigned)W->h, 32, 0);
}

static void win_free_buffers(Win *W) {
    if (W->img) { W->img->data = NULL; XDestroyImage(W->img); W->img = NULL; }
    free(W->cv.px); W->cv.px = NULL;
    free(W->conv); W->conv = NULL;
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
    if (!override) XSetWMProtocols(dpy, W->win, &A_DELETE, 1);
    W->w = w; W->h = h;
    win_make_buffers(W);
    W->gc = XCreateGC(dpy, W->win, 0, NULL);
    return W;
}

/* ventana normal, administrada por JWM (con título y redimensionable) */
static Win *win_create_managed(int w, int h, const char *title, const char *cls) {
    int sw = screen_w(), sh = screen_h();
    w = imin(w, sw); h = imin(h, sh);
    Win *W = win_create((sw - w) / 2, (sh - h) / 2, w, h, 0, cls);
    XStoreName(dpy, W->win, title);
    XChangeProperty(dpy, W->win, A_NET_WMNAME, A_UTF8, 8, PropModeReplace, (unsigned char *)title, (int)strlen(title));
    XSizeHints sz;
    memset(&sz, 0, sizeof sz);
    sz.flags = PPosition | PSize | PMinSize;
    sz.x = (sw - w) / 2; sz.y = (sh - h) / 2; sz.width = w; sz.height = h;
    sz.min_width = imin(sc(320), sw); sz.min_height = imin(sc(300), sh);
    XSetWMNormalHints(dpy, W->win, &sz);
    return W;
}

static void win_resize_buffers(Win *W, int w, int h) {
    if (w < 8 || h < 8 || (w == W->w && h == W->h)) return;
    win_free_buffers(W);
    W->w = w; W->h = h;
    win_make_buffers(W);
}

static unsigned mask_shift(unsigned long m) { unsigned s = 0; while (m && !(m & 1)) { m >>= 1; s++; } return s; }
static unsigned mask_bits(unsigned long m) { unsigned n = 0; while (m & 1) { m >>= 1; n++; } return n; }

static void canvas_convert(const uint32_t *src, uint32_t *dst, size_t count) {
    unsigned rs = mask_shift(vis->red_mask), gs = mask_shift(vis->green_mask), bs = mask_shift(vis->blue_mask);
    unsigned rb = mask_bits(vis->red_mask >> rs), gb = mask_bits(vis->green_mask >> gs), bb = mask_bits(vis->blue_mask >> bs);
    for (size_t i = 0; i < count; i++) {
        uint32_t p = src[i];
        uint32_t r = ((p >> 16) & 255) >> (8 - rb), g = ((p >> 8) & 255) >> (8 - gb), b = (p & 255) >> (8 - bb);
        dst[i] = (r << rs) | (g << gs) | (b << bs);
    }
}

static void win_present(Win *W) {
    if (!std_masks) canvas_convert(W->cv.px, W->conv, (size_t)W->w * (size_t)W->h);
#ifdef SESAR_DUMP
    if (getenv("SESAR_DUMP")) {
        FILE *f = fopen(getenv("SESAR_DUMP"), "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", W->w, W->h);
            for (size_t i = 0; i < (size_t)W->w * W->h; i++) {
                uint32_t p = W->cv.px[i];
                fputc((p >> 16) & 255, f); fputc((p >> 8) & 255, f); fputc(p & 255, f);
            }
            fclose(f);
        }
    }
#endif
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

static void ungrab_input(void) {
    XUngrabKeyboard(dpy, CurrentTime);
    XUngrabPointer(dpy, CurrentTime);
    XSync(dpy, False);
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
    void (*resize)(App *, int w, int h);       /* la ventana cambió de tamaño */
    void (*root)(App *, int w, int h);         /* cambió la resolución de la pantalla */
    void (*reload)(App *);                     /* llegó SIGUSR1 */
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
            case Expose: if (ev.xexpose.count == 0 && ev.xexpose.window == a->w->win) a->dirty = 1; break;
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
            case ConfigureNotify:
                if (ev.xconfigure.window == RootWindow(dpy, scr)) {
                    if (a->root) a->root(a, ev.xconfigure.width, ev.xconfigure.height);
                } else if (ev.xconfigure.window == a->w->win &&
                           (ev.xconfigure.width != a->w->w || ev.xconfigure.height != a->w->h)) {
                    win_resize_buffers(a->w, ev.xconfigure.width, ev.xconfigure.height);
                    if (a->resize) a->resize(a, a->w->w, a->w->h);
                    a->dirty = 1;
                }
                break;
            case ClientMessage:
                if (ev.xclient.message_type == A_PROTOCOLS && (Atom)ev.xclient.data.l[0] == A_DELETE) a->quit = 1;
                break;
            default: break;
            }
        }
        if (g_reload) { g_reload = 0; if (a->reload) a->reload(a); }
        if (a->quit) break;
        if (a->dirty) { a->dirty = 0; a->draw(a); win_present(a->w); }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        struct timeval tv = { a->tick_ms / 1000, (a->tick_ms % 1000) * 1000 };
        int r = select(fd + 1, &fds, NULL, NULL, a->tick_ms > 0 ? &tv : NULL);
        if (r == 0 && a->tick) a->tick(a);
    }
}

/* mensajes EWMH a la raíz (JWM los atiende) */
static void root_msg(Window w, Atom type, long d0, long d1, long d2) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = d0; e.xclient.data.l[1] = d1; e.xclient.data.l[2] = d2;
    e.xclient.data.l[3] = 1;
    XSendEvent(dpy, RootWindow(dpy, scr), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(dpy);
}

static long get_cardinal(Window w, Atom prop, long def) {
    Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
    long v = def;
    if (XGetWindowProperty(dpy, w, prop, 0, 1, False, XA_CARDINAL, &t, &f, &n, &after, &d) == Success && d) {
        if (n > 0) v = *(long *)d;
        XFree(d);
    }
    return v;
}

static Window active_window(void) {
    Atom t; int f; unsigned long n, after; unsigned char *d = NULL;
    Window w = None;
    if (XGetWindowProperty(dpy, RootWindow(dpy, scr), A_NET_ACTIVE, 0, 1, False, XA_WINDOW, &t, &f, &n, &after, &d) == Success && d) {
        if (n > 0) w = *(Window *)d;
        XFree(d);
    }
    return w;
}

static void goto_desktop(int n) {
    int nd = (int)get_cardinal(RootWindow(dpy, scr), A_NET_NUMDESK, cfg.ndesk);
    if (nd < 1) nd = 1;
    n = ((n % nd) + nd) % nd;
    root_msg(RootWindow(dpy, scr), A_NET_CURDESK, n, CurrentTime, 0);
}

static int current_desktop(void) { return (int)get_cardinal(RootWindow(dpy, scr), A_NET_CURDESK, 0); }

static void toggle_fullscreen_active(void) {
    Window w = active_window();
    if (w == None) return;
    root_msg(w, A_NET_STATE, 2 /* _NET_WM_STATE_TOGGLE */, (long)A_NET_FULL, 0);
}

static void toggle_show_desktop(void) {
    long cur = get_cardinal(RootWindow(dpy, scr), A_NET_SHOWDESK, 0);
    root_msg(RootWindow(dpy, scr), A_NET_SHOWDESK, cur ? 0 : 1, 0, 0);
}

/* ------------------------------------------------------------------ widgets comunes */
typedef struct { int x, y, w, h; } Rect;
static int in_rect(Rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

static Shape panel_shape(void) {
    Shape s;
    memset(&s, 0, sizeof s);
    s.cut = (float)sc(18); s.top = C_PANEL1; s.bot = C_PANEL2; s.fa = 0.98f;
    s.stroke = C_CYAN; s.sa = 0.60f; s.sw = fmaxf(1.f, S * 1.2f);
    s.glow = C_CYAN; s.ga = 0.30f * T.glow; s.gw = (float)sc(12);
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
        s.top = hover ? A1_HI : C_CYAN; s.bot = hover ? A1_MID : A1_LO; s.fa = 1.f; txt = C_BG;
    } else {
        s.top = hover ? col : C_BTN1; s.bot = hover ? col : C_BTN2; s.fa = hover ? 0.28f : 1.f;
    }
    draw_chamfer(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, &s);
    draw_text_c(c, F->uib, r.x + r.w / 2, r.y + (r.h - F->uib->height) / 2, label, txt, 1.f, S);
}



/* ------------------------------------------------------------------ fuentes compartidas */
static int screen_w(void);
static int screen_h(void);
static Fonts *g_F;
static Fonts *shared_fonts(void) { if (!g_F) g_F = fonts_open(); return g_F; }
static void shared_fonts_reset(void) { g_F = NULL; }   /* (se pierden a propósito: son pocas) */

static void wrap_text(SFont *f, const char *s, int maxw, char lines[][200], int maxl, int *nl) {
    *nl = 0;
    char cur[200] = "";
    char *copy = xstrdup(s), *save = NULL;
    for (char *w = strtok_r(copy, " ", &save); w && *nl < maxl; w = strtok_r(NULL, " ", &save)) {
        char t[200];
        snprintf(t, sizeof t, "%s%s%s", cur, cur[0] ? " " : "", w);
        if (cur[0] && text_width(f, t, 0) > maxw) { scopy(lines[(*nl)++], 200, cur); scopy(cur, sizeof cur, w); }
        else scopy(cur, sizeof cur, t);
    }
    if (cur[0] && *nl < maxl) scopy(lines[(*nl)++], 200, cur);
    free(copy);
}

/* ------------------------------------------------------------------ menú emergente */
typedef struct { const char *label; int style; } MItem;   /* style: 0 normal, 1 peligro, 2 separador sobre el item */
typedef struct { Fonts *F; const MItem *it; int n, sel, hover, W, H, rowH, pad, result; } PopM;

static void popm_draw(App *a) {
    PopM *p = a->u;
    Canvas *c = &a->w->cv;
    fill_rect(c, 0, 0, p->W, p->H, C_BG, 1.f);
    Shape ps = panel_shape();
    ps.cut = (float)sc(10); ps.ga = 0.22f * T.glow; ps.gw = (float)sc(8);
    draw_chamfer(c, 0, 0, (float)p->W, (float)p->H, &ps);
    for (int i = 0; i < p->n; i++) {
        int ry = p->pad + i * p->rowH;
        int on = (i == p->sel || i == p->hover);
        uint32_t col = p->it[i].style == 1 ? C_RED : C_CYAN;
        if (p->it[i].style == 2 && i > 0) fill_rect(c, p->pad, ry - 1, p->W - p->pad * 2, imax(1, sc(1)), C_LINE, 0.9f);
        if (on) {
            Shape rs;
            memset(&rs, 0, sizeof rs);
            rs.cut = (float)sc(6); rs.top = col; rs.bot = col; rs.fa = 0.2f;
            rs.stroke = col; rs.sa = 0.8f; rs.sw = fmaxf(1.f, S);
            draw_chamfer(c, (float)p->pad, (float)(ry + 2), (float)(p->W - p->pad * 2), (float)(p->rowH - 4), &rs);
        }
        draw_text(c, p->F->uib, p->pad + sc(12), ry + (p->rowH - p->F->uib->height) / 2, p->it[i].label,
                  p->it[i].style == 1 ? C_RED : (on ? col : C_TEXT), 1.f, 0, p->W - p->pad * 2 - sc(20));
    }
}

static int popm_at(PopM *p, int x, int y) {
    if (x < p->pad || x >= p->W - p->pad || y < p->pad) return -1;
    int i = (y - p->pad) / p->rowH;
    return i >= 0 && i < p->n ? i : -1;
}

static void popm_key(App *a, KeySym ks, int cp, unsigned st) {
    PopM *p = a->u;
    (void)cp; (void)st;
    a->dirty = 1;
    if (ks == XK_Escape) { p->result = -1; a->quit = 1; }
    else if (ks == XK_Up) p->sel = (p->sel + p->n - 1) % p->n;
    else if (ks == XK_Down || ks == XK_Tab) p->sel = (p->sel + 1) % p->n;
    else if (ks == XK_Return || ks == XK_KP_Enter) { p->result = p->sel; a->quit = 1; }
}

static void popm_button(App *a, int x, int y, int button, int press) {
    PopM *p = a->u;
    if (!press || button > 3) return;
    int i = popm_at(p, x, y);
    p->result = i;
    a->quit = 1;
}

static void popm_motion(App *a, int x, int y) {
    PopM *p = a->u;
    int h = popm_at(p, x, y);
    if (h != p->hover) { p->hover = h; if (h >= 0) p->sel = h; a->dirty = 1; }
}

/* devuelve el índice elegido o -1. Mientras está abierto captura teclado y puntero. */
static int popup_menu(int x, int y, const MItem *items, int n) {
    PopM p;
    memset(&p, 0, sizeof p);
    p.F = shared_fonts(); p.it = items; p.n = n; p.hover = -1; p.result = -1;
    p.pad = sc(8); p.rowH = sc(42);
    int w = 0;
    for (int i = 0; i < n; i++) w = imax(w, text_width(p.F->uib, items[i].label, 0));
    int sw = screen_w(), sh = screen_h();
    p.W = imin(w + p.pad * 2 + sc(40), sw - sc(8));
    p.W = imax(p.W, imin(sc(200), sw - sc(8)));
    p.H = p.pad * 2 + n * p.rowH;
    x = iclamp(x, 4, imax(4, sw - p.W - 4));
    y = iclamp(y, 4, imax(4, sh - p.H - 4));
    Win *W = win_create(x, y, p.W, p.H, 1, "sesar-popup");
    win_shape_chamfer(W, sc(10));
    XMapRaised(dpy, W->win);
    XSync(dpy, False);
    grab_input(W);
    App a = {0};
    a.w = W; a.u = &p;
    a.draw = popm_draw; a.key = popm_key; a.button = popm_button; a.motion = popm_motion;
    run_app(&a);
    ungrab_input();
    XDestroyWindow(dpy, W->win);
    win_free_buffers(W);
    free(W);
    XSync(dpy, False);
    return p.result;
}

/* ------------------------------------------------------------------ diálogo de formulario */
typedef struct { const char *label; char *buf; size_t cap; int toggle; } FField;   /* toggle: buf[0] = '0'/'1' */
typedef struct {
    Fonts *F;
    FField *f; int nf, sel, W, H, pad, blink, accepted, hover;
    const char *title, *msg, *ok;
    char lines[8][200]; int nlines;
    Rect fr[8], bOk, bCancel;
    int timeout, remaining;
} Form;

static void form_layout(Form *d) {
    d->pad = sc(20);
    int y = d->pad + d->F->title->height + sc(12);
    int iw = d->W - d->pad * 2;
    if (d->msg && d->msg[0]) {
        wrap_text(d->F->ui, d->msg, iw, d->lines, 8, &d->nlines);
        y += d->nlines * (d->F->ui->height + sc(2)) + sc(10);
    }
    for (int i = 0; i < d->nf; i++) {
        y += d->F->sm->height + sc(4);
        d->fr[i] = (Rect){d->pad, y, iw, sc(40)};
        y += sc(40) + sc(10);
    }
    y += sc(4);
    int bw = (iw - sc(10)) / 2;
    d->bCancel = (Rect){d->pad, y, bw, sc(44)};
    d->bOk = (Rect){d->pad + bw + sc(10), y, bw, sc(44)};
    d->H = y + sc(44) + d->pad;
}

static void form_draw(App *a) {
    Form *d = a->u;
    Canvas *c = &a->w->cv;
    Fonts *F = d->F;
    fill_rect(c, 0, 0, d->W, d->H, C_BG, 1.f);
    Shape ps = panel_shape();
    draw_chamfer(c, 0, 0, (float)d->W, (float)d->H, &ps);
    int y = d->pad;
    draw_text(c, F->title, d->pad, y, d->title, C_TEXT, 1.f, S, d->W - d->pad * 2);
    y += F->title->height + sc(12);
    for (int i = 0; i < d->nlines; i++) {
        draw_text(c, F->ui, d->pad, y, d->lines[i], C_DIM, 1.f, 0, 0);
        y += F->ui->height + sc(2);
    }
    for (int i = 0; i < d->nf; i++) {
        Rect r = d->fr[i];
        draw_text(c, F->smb, r.x, r.y - F->sm->height - sc(3), d->f[i].label, C_MUTED, 1.f, S * 0.5f, r.w);
        Shape fs;
        memset(&fs, 0, sizeof fs);
        fs.cut = (float)sc(8); fs.top = fs.bot = C_FIELD; fs.fa = 1.f;
        fs.stroke = C_CYAN; fs.sa = i == d->sel ? 0.95f : 0.4f; fs.sw = fmaxf(1.f, S * 1.2f);
        draw_chamfer(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, &fs);
        int ty = r.y + (r.h - F->uib->height) / 2;
        if (d->f[i].toggle) {
            int on = d->f[i].buf[0] == '1';
            draw_text(c, F->uib, r.x + sc(12), ty, on ? "SÍ" : "NO", on ? C_GREEN : C_MUTED, 1.f, 0, 0);
        } else {
            draw_text(c, F->uib, r.x + sc(12), ty, d->f[i].buf, C_TEXT, 1.f, 0, r.w - sc(30));
            if (i == d->sel && d->blink) {
                int w = imin(text_width(F->uib, d->f[i].buf, 0), r.w - sc(30));
                fill_rect(c, r.x + sc(12) + w + sc(2), ty, imax(2, sc(2)), F->uib->height, C_CYAN, 1.f);
            }
        }
    }
    char ok[96];
    if (d->timeout > 0) snprintf(ok, sizeof ok, "%s (%d)", d->ok, d->remaining);
    else snprintf(ok, sizeof ok, "%s", d->ok);
    draw_button(c, F, d->bCancel, "CANCELAR", BTN_NORMAL, d->hover == 1);
    draw_button(c, F, d->bOk, ok, BTN_PRIMARY, d->hover == 2);
}

static void form_next(Form *d, int dir) { if (d->nf) d->sel = (d->sel + dir + d->nf) % d->nf; }

static void form_key(App *a, KeySym ks, int cp, unsigned st) {
    Form *d = a->u;
    a->dirty = 1;
    d->blink = 1;
    if (ks == XK_Escape) { a->quit = 1; return; }
    if (ks == XK_Return || ks == XK_KP_Enter) {
        if (d->nf > 1 && d->sel < d->nf - 1) d->sel++;
        else { d->accepted = 1; a->quit = 1; }
        return;
    }
    if (ks == XK_Tab || ks == XK_Down) { form_next(d, (st & ShiftMask) ? -1 : 1); return; }
    if (ks == XK_ISO_Left_Tab || ks == XK_Up) { form_next(d, -1); return; }
    if (!d->nf) return;
    FField *f = &d->f[d->sel];
    if (f->toggle) { if (ks == XK_space) f->buf[0] = f->buf[0] == '1' ? '0' : '1'; return; }
    size_t L = strlen(f->buf);
    if (ks == XK_BackSpace) {
        if (L) { do { L--; } while (L && (f->buf[L] & 0xC0) == 0x80); f->buf[L] = 0; }
    } else if ((st & ControlMask) && (ks == XK_u || ks == XK_U)) f->buf[0] = 0;
    else if (cp >= 32 && L + 6 < f->cap) { L += (size_t)utf8_put(f->buf + L, cp); f->buf[L] = 0; }
}

static void form_button(App *a, int x, int y, int button, int press) {
    Form *d = a->u;
    if (button != 1 || !press) return;
    if (in_rect(d->bOk, x, y)) { d->accepted = 1; a->quit = 1; return; }
    if (in_rect(d->bCancel, x, y) || x < 0 || y < 0 || x >= d->W || y >= d->H) { a->quit = 1; return; }
    for (int i = 0; i < d->nf; i++)
        if (in_rect(d->fr[i], x, y)) {
            d->sel = i;
            if (d->f[i].toggle) d->f[i].buf[0] = d->f[i].buf[0] == '1' ? '0' : '1';
            a->dirty = 1;
        }
}

static void form_motion(App *a, int x, int y) {
    Form *d = a->u;
    int h = in_rect(d->bCancel, x, y) ? 1 : (in_rect(d->bOk, x, y) ? 2 : 0);
    if (h != d->hover) { d->hover = h; a->dirty = 1; }
}

static void form_tick(App *a) {
    Form *d = a->u;
    d->blink = !d->blink;
    if (d->timeout > 0) {
        d->remaining--;
        if (d->remaining <= 0) { d->accepted = 0; a->quit = 1; }
    }
    a->dirty = 1;
}

/* 1 si el usuario aceptó. timeout_s > 0: cuenta regresiva que cancela sola (para confirmaciones). */
static int form_dialog(const char *title, const char *msg, FField *f, int nf, const char *ok, int timeout_s) {
    Form d;
    memset(&d, 0, sizeof d);
    d.F = shared_fonts(); d.f = f; d.nf = imin(nf, 8); d.title = title; d.msg = msg; d.ok = ok;
    d.timeout = timeout_s; d.remaining = timeout_s; d.blink = 1;
    int sw = screen_w(), sh = screen_h();
    d.W = imin(sc(480), sw - sc(12));
    form_layout(&d);
    Win *W = win_create((sw - d.W) / 2, imax(sc(10), (sh - d.H) / 3), d.W, d.H, 1, "sesar-dialog");
    win_shape_chamfer(W, sc(18));
    XMapRaised(dpy, W->win);
    XSync(dpy, False);
    grab_input(W);
    App a = {0};
    a.w = W; a.u = &d; a.tick_ms = timeout_s > 0 ? 1000 : 530;
    a.draw = form_draw; a.key = form_key; a.button = form_button; a.motion = form_motion; a.tick = form_tick;
    run_app(&a);
    ungrab_input();
    XDestroyWindow(dpy, W->win);
    win_free_buffers(W);
    free(W);
    XSync(dpy, False);
    return d.accepted;
}

static void message_box(const char *title, const char *msg) {
    form_dialog(title, msg, NULL, 0, "ACEPTAR", 0);
}

/* ------------------------------------------------------------------ aplicaciones (.desktop) */
static const char *CAT_NAMES[] = {"Internet", "Oficina", "Multimedia", "Gráficos", "Desarrollo",
                                  "Juegos", "Sistema", "Accesorios", "Otros"};
#define NCATS 9

typedef struct {
    char *name, *lname, *exec, *comment, *icon, *id, *path;
    int fav;
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

static void lang_code(char *out, size_t n, char *full, size_t nf) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    out[0] = 0; full[0] = 0;
    if (l && strlen(l) >= 2 && isalpha((unsigned char)l[0]) && isalpha((unsigned char)l[1])) {
        snprintf(out, n, "Name[%c%c]", l[0], l[1]);
        if (l[2] == '_' && isalpha((unsigned char)l[3]) && isalpha((unsigned char)l[4]))
            snprintf(full, nf, "Name[%c%c_%c%c]", l[0], l[1], l[3], l[4]);
    }
}

static void load_desktop_file(const char *path, const char *id) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048], name[256] = "", namel[256] = "", namef[256] = "", exec[1024] = "", comment[256] = "", cats[512] = "", icon[256] = "";
    char lkey[16], lfull[16];
    lang_code(lkey, sizeof lkey, lfull, sizeof lfull);
    int in = 0, term = 0, nodisp = 0, hidden = 0, isapp = 1, onlyin = 0;
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
        else if (lfull[0] && !strcmp(k, lfull)) snprintf(namef, sizeof namef, "%s", v);
        else if (lkey[0] && !strcmp(k, lkey)) snprintf(namel, sizeof namel, "%s", v);
        else if (!strcmp(k, "Icon")) snprintf(icon, sizeof icon, "%s", v);
        else if (!strcmp(k, "OnlyShowIn")) onlyin = v[0] != 0;
        else if (!strcmp(k, "Exec")) snprintf(exec, sizeof exec, "%s", v);
        else if (!strcmp(k, "Comment")) snprintf(comment, sizeof comment, "%s", v);
        else if (!strcmp(k, "Categories")) snprintf(cats, sizeof cats, "%s", v);
        else if (!strcmp(k, "Terminal")) term = !strcmp(v, "true");
        else if (!strcmp(k, "NoDisplay")) nodisp = !strcmp(v, "true");
        else if (!strcmp(k, "Hidden")) hidden = !strcmp(v, "true");
        else if (!strcmp(k, "Type")) isapp = !strcmp(v, "Application");
    }
    fclose(f);
    if (namef[0]) snprintf(namel, sizeof namel, "%s", namef);
    if (!isapp || nodisp || hidden || onlyin || !exec[0] || !(namel[0] || name[0])) return;
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
    a->icon = xstrdup(icon);
    a->id = xstrdup(id);
    a->path = xstrdup(path);
    a->fav = 0;
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

static void favs_apply(void);
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
    favs_apply();
}

/* favoritos: ids de .desktop, uno por línea, en ~/.config/sesar/favorites */
static void favs_apply(void) {
    char p[1400];
    cfg_path("favorites", p, sizeof p);
    char *txt = read_file(p);
    for (int i = 0; i < napps; i++) apps[i].fav = 0;
    if (!txt) return;
    char *save = NULL;
    for (char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save))
        for (int i = 0; i < napps; i++) if (!strcmp(apps[i].id, ln)) apps[i].fav = 1;
    free(txt);
}

static void favs_save(void) {
    ensure_cfg_dir();
    char p[1400];
    cfg_path("favorites", p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    for (int i = 0; i < napps; i++) if (apps[i].fav) fprintf(f, "%s\n", apps[i].id);
    fclose(f);
}

/* copia el lanzador al escritorio (~/Desktop). 1 si se creó. */
static int app_to_desktop(const AppEntry *a) {
    char dd[1200], dst[1400];
    snprintf(dd, sizeof dd, "%s/Desktop", home_dir());
    if (!is_dir(dd)) desktop_dir(dd, sizeof dd);
    mkdir_p(dd);
    snprintf(dst, sizeof dst, "%s/%s", dd, a->id);
    if (!copy_file(a->path, dst)) return 0;
    chmod(dst, 0755);
    return 1;
}

static void launch_app_ex(const AppEntry *a);
static void launch_app(const AppEntry *a) { launch_app_ex(a); }


/* ------------------------------------------------------------------ acciones */
enum { ACT_TERM, ACT_FILES, ACT_RESTART, ACT_LOGOUT, ACT_SETTINGS, ACT_PANEL, ACT_POWER };

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
    case ACT_SETTINGS: spawn_fmt("'%s' settings", self_path); break;
    case ACT_PANEL: spawn_fmt("'%s' panel", self_path); break;
    case ACT_POWER: spawn_fmt("'%s' power", self_path); break;
    }
}

/* ------------------------------------------------------------------ MENU DE INICIO */
typedef struct {
    Fonts *F;
    int W, H, pad, rowH, listX, listY, listW, listH, footY, rightX, rightW, searchY, searchH;
    char query[128];
    int qlen, cat, sel, scroll, hover, blink;
    int *flt, nflt;
    const char *tabLabel[NCATS + 2];
    int tabCat[NCATS + 2], ntabs;
    Rect tabR[NCATS + 2], searchR, infoR;
    Rect btnR[6];
    const char *btnLabel[6];
    int btnAct[6], btnStyle[6], nbtn;
    int press, px, py, scroll0, moved, press_ticks, tick_n;
} Menu;

static int menu_matches(Menu *m, const AppEntry *a) {
    if (m->cat == -2 && !a->fav) return 0;
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
    m->tabLabel[m->ntabs] = "FAVORITOS"; m->tabCat[m->ntabs++] = -2;
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
        m->btnLabel[m->nbtn] = "AJUSTES"; m->btnAct[m->nbtn] = ACT_SETTINGS; m->btnStyle[m->nbtn] = BTN_NORMAL;
        m->btnR[m->nbtn++] = (Rect){bx, by, bw, bh}; by += bh + sc(8);
        m->btnLabel[m->nbtn] = "PANEL RÁPIDO"; m->btnAct[m->nbtn] = ACT_PANEL; m->btnStyle[m->nbtn] = BTN_NORMAL;
        m->btnR[m->nbtn++] = (Rect){bx, by, bw, bh};
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
        int hr = sc(17);
        {
            float bcx = (float)(m->listX + sc(10) + hr), bcy = (float)(ry + m->rowH / 2);
            Img *im = icon_get(e->icon, hr * 2 - sc(10), 0);
            draw_badge(c, bcx, bcy, (float)hr, cfg.icon_shape, e->color, 0.12f, fmaxf(1.f, S * 1.3f), 1.f, 0.f, 1.f);
            if (im) blit_img(c, im, (int)bcx - im->w / 2, (int)bcy - im->h / 2, 1.f);
            else {
                char ini[8] = {0};
                const char *p = e->name; int cp = utf8_next(&p); if (cp >= 'a' && cp <= 'z') cp -= 32; utf8_put(ini, cp);
                draw_text_c(c, F->smb, (int)bcx, (int)bcy - F->smb->height / 2, ini, e->color, 1.f, 0);
            }
            if (e->fav) fill_rect(c, m->listX + sc(10) + hr * 2 - sc(5), ry + m->rowH / 2 - hr, sc(6), sc(6), C_AMBER, 1.f);
        }
        int nx = m->listX + sc(10) + hr * 2 + sc(14), maxw = m->listW - (nx - m->listX) - sc(16);
        if (e->comment[0]) {
            draw_text(c, F->uib, nx, ry + sc(7), e->name, selected ? C_CYAN : C_TEXT, 1.f, 0, maxw);
            draw_text(c, F->sm, nx, ry + sc(7) + F->uib->height + sc(1), e->comment, C_MUTED, 1.f, 0, maxw);
        } else {
            draw_text(c, F->uib, nx, ry + (m->rowH - F->uib->height) / 2, e->name, selected ? C_CYAN : C_TEXT, 1.f, 0, maxw);
        }
    }
    if (m->nflt == 0)
    {
        if (m->qlen > 0) {
            char hint[300];
            snprintf(hint, sizeof hint, "ENTER: ejecutar \"%s\"", m->query);
            draw_text(c, F->uib, m->listX + sc(10), m->listY + sc(14), hint, C_CYAN, 1.f, 0, m->listW - sc(20));
        } else draw_text_c(c, F->ui, m->listX + m->listW / 2, m->listY + sc(30),
                           m->cat == -2 ? "Sin favoritos: clic derecho sobre una app" : "Sin resultados", C_MUTED, 1.f, S);
    }
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
    draw_text(c, F->sm, m->pad, m->footY, "ENTER lanzar  ·  TAB categoría  ·  CLIC DER. opciones  ·  ESC", C_MUTED, 1.f, S * 0.5f, m->W - m->pad * 2);
}

static void menu_key(App *a, KeySym ks, int cp, unsigned state) {
    Menu *m = a->u;
    int page = imax(1, m->listH / m->rowH - 1);
    a->dirty = 1;
    switch (ks) {
    case XK_Escape: a->quit = 1; return;
    case XK_Return: case XK_KP_Enter:
        if (m->nflt > 0) { launch_app(&apps[m->flt[m->sel]]); a->quit = 1; }
        else if (m->qlen > 0) { spawn_cmd(m->query); a->quit = 1; }
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

static void menu_context(App *a, int row, int x, int y) {
    Menu *m = a->u;
    AppEntry *e = &apps[m->flt[row]];
    MItem it[3] = {{"Abrir", 0}, {"Agregar al escritorio", 0}, {e->fav ? "Quitar de favoritos" : "Fijar en favoritos", 0}};
    int r = popup_menu(x, y, it, 3);
    grab_input(a->w);
    a->dirty = 1;
    m->press = 0;
    if (r == 0) { launch_app(e); a->quit = 1; }
    else if (r == 1) { app_to_desktop(e); signal_instance("desktop", SIGUSR1); }
    else if (r == 2) { e->fav = !e->fav; favs_save(); if (m->cat == -2) menu_refilter(m); }
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
    if (button == 3 && press) {
        int h3 = menu_hit(m, x, y);
        if (h3 >= 1000 && h3 < 2000) menu_context(a, h3 - 1000, x, y);
        return;
    }
    if (button != 1) return;
    if (press) { m->press = 1; m->px = x; m->py = y; m->scroll0 = m->scroll; m->moved = 0; m->press_ticks = 0; return; }
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
    if (++m->tick_n % 3 == 0) { m->blink = !m->blink; a->dirty = 1; }
    if (m->press && !m->moved && ++m->press_ticks >= 4) {      /* pulsación larga = clic derecho */
        int h = menu_hit(m, m->px, m->py);
        if (h >= 1000 && h < 2000) menu_context(a, h - 1000, m->px, m->py);
        m->press = 0;
    }
}

static int run_menu(int argc, char **argv) {
    if (!single_instance("menu")) return 0;
    int tray = tray_height();
    int top = cfg.tray_top;
    for (int i = 2; i < argc; i++) if (!strcmp(argv[i], "--top")) top = 1;
    int sw = screen_w(), sh = screen_h();
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
    a.w = w; a.u = m; a.tick_ms = 180;
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
    ps.accent = C_MAGENTA; ps.stroke = C_MAGENTA; ps.glow = C_MAGENTA; ps.ga *= T.glow;
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
    p->W = imin(sc(380), screen_w() - sc(16));
    p->label[0] = "REINICIAR ESCRITORIO"; p->act[0] = ACT_RESTART; p->style[0] = BTN_NORMAL;
    p->label[1] = "CERRAR SESIÓN"; p->act[1] = ACT_LOGOUT; p->style[1] = BTN_DANGER;
    p->label[2] = "CANCELAR"; p->act[2] = -1; p->style[2] = BTN_NORMAL;
    p->n = 3;
    int y = pad + sc(58);
    for (int i = 0; i < p->n; i++) { p->r[i] = (Rect){pad, y, p->W - pad * 2, bh}; y += bh + gap; }
    p->H = y - gap + pad;
    p->hover = -1;
    int x = (screen_w() - p->W) / 2, yy = (screen_h() - p->H) / 2;
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
    s.cut = (float)(hgt / 2); s.top = C_BTN2; s.bot = C_BTN2; s.fa = 1.f;
    s.stroke = C_LINE; s.sa = 1.f; s.sw = fmaxf(1.f, S);
    draw_chamfer(c, (float)x, (float)y, (float)w, (float)hgt, &s);
    int fw = (int)((w - 2) * clamp01(frac));
    for (int ix = 0; ix < fw; ix++) {
        uint32_t col = lerp_col(C_CYAN, C_MAGENTA, (float)ix / (float)imax(1, w - 2));
        fill_rect(c, x + 1 + ix, y + 1, 1, hgt - 2, col, 1.f);
    }
    int seg = sc(8);
    for (int ix = seg; ix < w; ix += seg) fill_rect(c, x + ix, y + 1, imax(1, sc(1)), hgt - 2, C_FIELD, 0.9f);
}

static void hud_draw(App *a) {
    Hud *h = a->u;
    Canvas *c = &a->w->cv;
    Fonts *F = h->F;
    fill_rect(c, 0, 0, h->W, h->H, C_BG, 1.f);
    Shape ps = panel_shape();
    ps.cut = (float)sc(12); ps.accent = C_MAGENTA; ps.stroke = C_MAGENTA; ps.glow = C_MAGENTA; ps.ga = 0.2f * T.glow; ps.gw = (float)sc(8);
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

typedef struct { Hud *h; Win *w; int visible; } HudCtx;

static void hud_place(HudCtx *x) {
    Hud *h = x->h;
    int sw = screen_w(), sh = screen_h(), g = sc(12), tr = tray_height();
    int left = (cfg.hud_pos == 0 || cfg.hud_pos == 2), bottom = (cfg.hud_pos >= 2);
    int px = left ? g : sw - h->W - g;
    int py = bottom ? sh - h->H - g - (cfg.tray_top ? 0 : tr) : g + (cfg.tray_top ? tr : 0);
    XMoveWindow(dpy, x->w->win, px, py);
}

static void hud_reload(App *a) {
    HudCtx *x = a->u;
    Hud *h = x->h;
    cfg_load();
    theme_apply();
    hud_place(x);
    if (cfg.hud && !x->visible) { XMapRaised(dpy, x->w->win); x->visible = 1; }
    if (!cfg.hud && x->visible) { XUnmapWindow(dpy, x->w->win); x->visible = 0; }
    (void)h;
    a->dirty = 1;
}

static void hud_draw2(App *a) { HudCtx *x = a->u; a->u = x->h; hud_draw(a); a->u = x; }
static void hud_tick2(App *a) { HudCtx *x = a->u; a->u = x->h; hud_tick(a); a->u = x; if (!x->visible) a->dirty = 0; }

static int run_hud(void) {
    if (!single_instance("hud")) return 0;
    install_reload_handler();
    Hud *h = calloc(1, sizeof *h);
    h->F = fonts_open();
    snprintf(h->ramtxt, sizeof h->ramtxt, "--");
    hud_update(h);
    h->W = sc(240);
    h->H = sc(14) * 2 + h->F->uib->height + sc(12) + (h->F->sm->height + sc(4) + sc(9) + sc(10)) * (h->has_bat ? 3 : 2) - sc(6);
    Win *w = win_create(0, 0, h->W, h->H, 1, "sesar-hud");
    win_shape_chamfer(w, sc(12));
    HudCtx x = {h, w, 0};
    hud_place(&x);
    if (cfg.hud) { XMapRaised(dpy, w->win); x.visible = 1; }
    App a = {0};
    a.w = w; a.u = &x; a.tick_ms = 1000;
    a.draw = hud_draw2; a.tick = hud_tick2; a.reload = hud_reload;
    run_app(&a);
    return 0;
}


/* ------------------------------------------------------------------ pantalla (tamaño real, sigue a RandR) */
static void screen_size(int *w, int *h) {
    Window r; int x, y; unsigned ww, hh, bw, d;
    if (XGetGeometry(dpy, RootWindow(dpy, scr), &r, &x, &y, &ww, &hh, &bw, &d)) { *w = (int)ww; *h = (int)hh; }
    else { *w = DisplayWidth(dpy, scr); *h = DisplayHeight(dpy, scr); }
}
static int screen_w(void) { int w, h; screen_size(&w, &h); return w; }
static int screen_h(void) { int w, h; screen_size(&w, &h); return h; }

/* ------------------------------------------------------------------ FONDO */
static const char *WALL_NAMES[] = {"Synthwave", "Degradado", "Cuadrícula", "Sólido", "Imagen"};

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

static void wall_vgrad(Canvas *c, uint32_t top, uint32_t mid, uint32_t bot, float split) {
    for (int y = 0; y < c->h; y++) {
        float t = (float)y / (float)c->h;
        uint32_t col = t < split ? lerp_col(top, mid, t / split) : lerp_col(mid, bot, (t - split) / (1.f - split));
        for (int x = 0; x < c->w; x++) c->px[(size_t)y * c->w + x] = col;
    }
}

static void wall_vignette(Canvas *c, float strength) {
    int w = c->w, h = c->h;
    float rin = fminf((float)w, (float)h) * 0.35f, rout = (float)imax(w, h) * 0.75f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float dx = x - w / 2.f, dy = y - h / 2.f, d = sqrtf(dx * dx + dy * dy);
            if (d <= rin) continue;
            float v = clamp01((d - rin) / (rout - rin)) * strength;
            uint32_t *p = &c->px[(size_t)y * w + x];
            *p = mix(*p, T.light ? 0xFFFFFF : 0x000000, v);
        }
}

static void wall_synthwave(Canvas *c) {
    int w = c->w, h = c->h;
    float hy = h * 0.66f;
    uint32_t top = mix(T.bg, 0x000000, T.light ? 0.f : 0.35f), mid = mix(T.bg, T.a1, T.light ? 0.16f : 0.10f), bot = mix(T.bg, T.a2, T.light ? 0.22f : 0.14f);
    wall_vgrad(c, top, mid, bot, 0.62f);
    float big = (float)imax(w, h);
    radial_glow(c, w * 0.12f, h * 0.06f, big * 0.60f, T.a2, T.light ? 0.20f : 0.26f);
    radial_glow(c, w * 0.95f, h * 0.22f, big * 0.55f, T.a1, T.light ? 0.16f : 0.20f);
    if (!T.light) {
        unsigned seed = 7;
        int stars = w * h / 9000;
        for (int i = 0; i < stars; i++) {
            seed = seed * 1664525u + 1013904223u; int sx = (int)(seed % (unsigned)w);
            seed = seed * 1664525u + 1013904223u; int sy = (int)(seed % (unsigned)(hy * 0.95f));
            seed = seed * 1664525u + 1013904223u; float a = 0.15f + (seed % 1000) / 1800.f;
            put(c, sx, sy, 0xE6F2FF, a);
            if (a > 0.5f) { put(c, sx + 1, sy, 0xE6F2FF, a * 0.5f); put(c, sx, sy + 1, 0xE6F2FF, a * 0.5f); }
        }
    }
    for (int y = (int)(hy - h * 0.14f); y < (int)hy; y++) {
        float t = (y - (hy - h * 0.14f)) / (h * 0.14f);
        fill_rect(c, 0, y, w, 1, T.a2, t * t * 0.28f);
    }
    float cx = w / 2.f, cy = hy * 0.46f, R = h * 0.17f;
    draw_hex(c, cx, cy, R, T.a1, 0.05f, fmaxf(2.f, h / 360.f), 0.30f, 0.15f * T.glow, R * 0.25f);
    draw_line_a(c, 0, hy, (float)w, hy, fmaxf(1.5f, h / 540.f), T.a2, 0.55f, 0.55f);
    float lw = fmaxf(1.f, h / 720.f), step = w / 7.f;
    for (int i = -14; i <= 14; i++) draw_line_a(c, w / 2.f, hy, w / 2.f + i * step, (float)h, lw, T.a1, 0.f, 0.55f);
    int rows = 13;
    for (int k = 1; k <= rows; k++) {
        float z = (float)k / rows, y = hy + (h - hy) * z * z;
        draw_line_a(c, 0, y, (float)w, y, lw, T.a1, 0.06f + 0.5f * z, 0.06f + 0.5f * z);
    }
    wall_vignette(c, T.light ? 0.25f : 0.55f);
    char bp[1400];
    if (find_font(1, bp, sizeof bp) || find_font(0, bp, sizeof bp)) {
        SFont *f = font_open(bp, imax(12, (int)(h * 0.045f)), 0);
        if (f) draw_text_c(c, f, w / 2, (int)(hy * 0.46f + h * 0.17f + h * 0.04f), "S E S A R   D E", T.a1, 0.5f, 0);
    }
}

static void wall_gradient(Canvas *c) {
    int w = c->w, h = c->h;
    wall_vgrad(c, mix(T.bg, T.a1, T.light ? 0.30f : 0.22f), mix(T.bg, T.a1, 0.08f), mix(T.bg, T.a2, T.light ? 0.30f : 0.24f), 0.5f);
    radial_glow(c, w * 0.2f, h * 0.15f, (float)imax(w, h) * 0.6f, T.a1, 0.22f);
    radial_glow(c, w * 0.85f, h * 0.85f, (float)imax(w, h) * 0.6f, T.a2, 0.22f);
    wall_vignette(c, 0.35f);
}

static void wall_grid(Canvas *c) {
    int w = c->w, h = c->h;
    wall_vgrad(c, mix(T.bg, 0x000000, T.light ? 0.f : 0.2f), T.bg, mix(T.bg, T.a1, 0.10f), 0.6f);
    int step = imax(24, sc(48));
    for (int x = 0; x < w; x += step) fill_rect(c, x, 0, 1, h, T.a1, 0.10f);
    for (int y = 0; y < h; y += step) fill_rect(c, 0, y, w, 1, T.a1, 0.10f);
    for (int y = 0; y < h; y += step * 4) fill_rect(c, 0, y, w, 1, T.a1, 0.16f);
    for (int x = 0; x < w; x += step * 4) fill_rect(c, x, 0, 1, h, T.a1, 0.16f);
    radial_glow(c, w / 2.f, h / 2.f, (float)imax(w, h) * 0.5f, T.a1, 0.12f);
    wall_vignette(c, 0.4f);
}

static int wall_image(Canvas *c) {
    if (!cfg.wall_img[0]) return 0;
    Img *src = png_load(cfg.wall_img);
    if (!src) return 0;
    Img *cov = img_cover(src, c->w, c->h);
    img_free(src);
    if (!cov) return 0;
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) c->px[i] = cov->px[i] & 0xFFFFFF;
    img_free(cov);
    return 1;
}

static void render_wallpaper(Canvas *c) {
    switch (cfg.wall) {
    case 1: wall_gradient(c); break;
    case 2: wall_grid(c); break;
    case 3: for (size_t i = 0; i < (size_t)c->w * c->h; i++) c->px[i] = T.p2; break;
    case 4: if (wall_image(c)) break; /* sin imagen válida: cae a synthwave */
    default: wall_synthwave(c); break;
    }
}

/* ------------------------------------------------------------------ iconos de escritorio */
typedef struct {
    char name[128], exec[1024], icon[256], path[1100];
    int kind;                 /* 0 interno, 1 .desktop, 2 archivo, 3 carpeta */
    int act;                  /* kind 0: acción */
    int term, col, row, placed;
    uint32_t color;
} DItem;

enum { DA_TERM, DA_FILES, DA_MENU, DA_SETTINGS, DA_GAMES };

typedef struct {
    Fonts *F;
    Canvas base;
    Pixmap root_pm;
    DItem *it; int n;
    int cw, ch, cols, rows, ox, oy;      /* celda y área */
    int sel, hover, drag, press, px, py, ptick, moved, dx, dy;
    long last_click_ms; int last_click;
    time_t dir_mtime;
    int sw, sh, has_win, mapped, dark_bg;
} Desk;

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static uint32_t name_color(const char *s) {
    uint32_t pal[] = {C_CYAN, C_MAGENTA, C_GREEN, C_AMBER, C_VIOLET, 0x4D7CFF};
    unsigned h = 5381;
    for (const char *p = s; *p; p++) h = h * 33 + (unsigned char)*p;
    return pal[h % 6];
}

static int parse_desktop_item(const char *path, DItem *d) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[2048], name[128] = "", namel[128] = "", lkey[16], lfull[16];
    lang_code(lkey, sizeof lkey, lfull, sizeof lfull);
    int in = 0, isapp = 1;
    d->term = 0;
    while (fgets(line, sizeof line, f)) {
        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (line[0] == '[') { in = !strcmp(line, "[Desktop Entry]"); continue; }
        if (!in || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "Name")) scopy(name, sizeof name, v);
        else if ((lfull[0] && !strcmp(k, lfull)) || (lkey[0] && !strcmp(k, lkey) && !namel[0])) scopy(namel, sizeof namel, v);
        else if (!strcmp(k, "Exec")) scopy(d->exec, sizeof d->exec, v);
        else if (!strcmp(k, "Icon")) scopy(d->icon, sizeof d->icon, v);
        else if (!strcmp(k, "Terminal")) d->term = !strcmp(v, "true");
        else if (!strcmp(k, "Type")) isapp = !strcmp(v, "Application");
    }
    fclose(f);
    if (!isapp || !d->exec[0]) return 0;
    scopy(d->name, sizeof d->name, namel[0] ? namel : name);
    if (!d->name[0]) return 0;
    char cl[1024];
    clean_exec(d->exec, cl, sizeof cl);
    scopy(d->exec, sizeof d->exec, cl);
    return 1;
}

static int ditem_cmp(const void *a, const void *b) { return strcasecmp(((const DItem *)a)->name, ((const DItem *)b)->name); }

static void desk_add(Desk *D, const DItem *d) {
    D->it = xrealloc(D->it, sizeof *D->it * (size_t)(D->n + 1));
    D->it[D->n++] = *d;
}

static void desk_load_items(Desk *D) {
    free(D->it); D->it = NULL; D->n = 0;
    char tmp[300];
    if (cfg.desk_defaults) {
        DItem d;
        memset(&d, 0, sizeof d);
        scopy(d.name, sizeof d.name, "Terminal"); scopy(d.icon, sizeof d.icon, "utilities-terminal"); d.act = DA_TERM; desk_add(D, &d);
        if (files_cmd(tmp, sizeof tmp)) { scopy(d.name, sizeof d.name, "Archivos"); scopy(d.icon, sizeof d.icon, "system-file-manager"); d.act = DA_FILES; desk_add(D, &d); }
        scopy(d.name, sizeof d.name, "Aplicaciones"); scopy(d.icon, sizeof d.icon, "applications-other"); d.act = DA_MENU; desk_add(D, &d);
        scopy(d.name, sizeof d.name, "Juegos"); scopy(d.icon, sizeof d.icon, "applications-games"); d.act = DA_GAMES; desk_add(D, &d);
        scopy(d.name, sizeof d.name, "Ajustes"); scopy(d.icon, sizeof d.icon, "preferences-system"); d.act = DA_SETTINGS; desk_add(D, &d);
    }
    int nbuiltin = D->n;
    char dd[1400];
    desktop_dir(dd, sizeof dd);
    DIR *dir = opendir(dd);
    if (dir) {
        struct dirent *e;
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.') continue;
            DItem d;
            memset(&d, 0, sizeof d);
            snprintf(d.path, sizeof d.path, "%s/%s", dd, e->d_name);
            size_t L = strlen(e->d_name);
            if (L > 8 && !strcmp(e->d_name + L - 8, ".desktop")) {
                if (!parse_desktop_item(d.path, &d)) continue;
                d.kind = 1;
            } else if (is_dir(d.path)) { d.kind = 3; scopy(d.name, sizeof d.name, e->d_name); scopy(d.icon, sizeof d.icon, "folder"); }
            else { d.kind = 2; scopy(d.name, sizeof d.name, e->d_name); scopy(d.icon, sizeof d.icon, "text-x-generic"); }
            desk_add(D, &d);
        }
        closedir(dir);
    }
    qsort(D->it + nbuiltin, (size_t)(D->n - nbuiltin), sizeof *D->it, ditem_cmp);
    for (int i = 0; i < D->n; i++) D->it[i].color = name_color(D->it[i].name);
    struct stat st;
    D->dir_mtime = stat(dd, &st) == 0 ? st.st_mtime : 0;
}

static void desk_geometry(Desk *D) {
    static const float cell[] = {84, 104, 128};
    D->cw = sc(cell[cfg.icon_size]);
    D->ch = D->cw + sc(30);
    int tr = tray_height(), g = sc(10);
    D->ox = g + ((cfg.hud && (cfg.hud_pos == 0 || cfg.hud_pos == 2)) ? sc(256) : 0);
    D->oy = g + (cfg.tray_top ? tr : 0);
    D->cols = imax(1, (D->sw - D->ox - g) / D->cw);
    D->rows = imax(1, (D->sh - D->oy - g - (cfg.tray_top ? 0 : tr)) / D->ch);
}

static int cell_free(Desk *D, int col, int row, int except) {
    if (col < 0 || row < 0 || col >= D->cols || row >= D->rows) return 0;
    for (int i = 0; i < D->n; i++) if (i != except && D->it[i].placed && D->it[i].col == col && D->it[i].row == row) return 0;
    return 1;
}

static void cell_nearest(Desk *D, int col, int row, int except, int *oc, int *or_) {
    col = iclamp(col, 0, D->cols - 1); row = iclamp(row, 0, D->rows - 1);
    for (int r = 0; r < D->cols + D->rows; r++)
        for (int dr = -r; dr <= r; dr++)
            for (int dc = -r; dc <= r; dc++) {
                if (imax(abs(dr), abs(dc)) != r) continue;
                if (cell_free(D, col + dc, row + dr, except)) { *oc = col + dc; *or_ = row + dr; return; }
            }
    *oc = 0; *or_ = 0;
}

static void desk_positions_save(Desk *D) {
    ensure_cfg_dir();
    char p[1400];
    cfg_path("iconpos", p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    for (int i = 0; i < D->n; i++) if (D->it[i].placed) fprintf(f, "%d\t%d\t%s\n", D->it[i].col, D->it[i].row, D->it[i].name);
    fclose(f);
}

static void desk_layout(Desk *D, int reset) {
    char p[1400];
    cfg_path("iconpos", p, sizeof p);
    for (int i = 0; i < D->n; i++) D->it[i].placed = 0;
    char *txt = reset ? NULL : read_file(p);
    if (txt) {
        char *save = NULL;
        for (char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            int c, r, off = 0;
            if (sscanf(ln, "%d\t%d\t%n", &c, &r, &off) < 2 || !off) continue;
            for (int i = 0; i < D->n; i++)
                if (!D->it[i].placed && !strcmp(D->it[i].name, ln + off) && cell_free(D, c, r, i)) {
                    D->it[i].col = c; D->it[i].row = r; D->it[i].placed = 1;
                }
        }
        free(txt);
    }
    for (int i = 0; i < D->n; i++) {
        if (D->it[i].placed) continue;
        for (int c = 0; c < D->cols && !D->it[i].placed; c++)
            for (int r = 0; r < D->rows && !D->it[i].placed; r++)
                if (cell_free(D, c, r, i)) { D->it[i].col = c; D->it[i].row = r; D->it[i].placed = 1; }
    }
    if (reset) desk_positions_save(D);
}

static Rect desk_cell_rect(Desk *D, int i) {
    return (Rect){D->ox + D->it[i].col * D->cw, D->oy + D->it[i].row * D->ch, D->cw, D->ch};
}

static int desk_hit(Desk *D, int x, int y) {
    for (int i = 0; i < D->n; i++) if (D->it[i].placed && in_rect(desk_cell_rect(D, i), x, y)) return i;
    return -1;
}

static void desk_draw_item(Desk *D, Canvas *c, int i, int cx, int cy, int selected, int hover, float alpha) {
    DItem *it = &D->it[i];
    Fonts *F = D->F;
    int R = (int)(D->cw * 0.31f);
    int bx = cx + D->cw / 2, by = cy + sc(8) + R;
    if (selected || hover) {
        Shape rs;
        memset(&rs, 0, sizeof rs);
        rs.cut = (float)sc(8); rs.top = rs.bot = C_CYAN; rs.fa = (selected ? 0.20f : 0.10f) * alpha;
        rs.stroke = C_CYAN; rs.sa = (selected ? 0.9f : 0.4f) * alpha; rs.sw = fmaxf(1.f, S);
        draw_chamfer(c, (float)(cx + sc(2)), (float)(cy + sc(2)), (float)(D->cw - sc(4)), (float)(D->ch - sc(4)), &rs);
    }
    uint32_t tile = T.light ? 0xFFFFFF : C_PANEL1;
    draw_badge(c, (float)bx, (float)by, (float)R, cfg.icon_shape, tile, 0.80f * alpha, 0.f, 0.f, 0.f, 1.f);
    draw_badge(c, (float)bx, (float)by, (float)R, cfg.icon_shape, it->color, 0.10f * alpha, fmaxf(1.f, S * 1.4f), 0.9f * alpha, 0.35f * T.glow * alpha, (float)sc(7));
    Img *im = icon_get(it->icon, (int)(R * 1.12f), it->kind == 3 || it->kind == 2 ? 1 : 0);
    if (im) blit_img(c, im, bx - im->w / 2, by - im->h / 2, alpha);
    else {
        char ini[8] = {0};
        const char *p = it->name; int cp = utf8_next(&p); if (cp >= 'a' && cp <= 'z') cp -= 32; utf8_put(ini, cp);
        draw_text_c(c, F->title, bx, by - F->title->height / 2, ini, it->color, alpha, 0);
    }
    if (it->kind == 1) {   /* flechita de acceso directo */
        int s = sc(9), ax = bx + R - s - sc(2), ay = by + R - s - sc(2);
        fill_rect(c, ax, ay, s, s, C_BG, 0.85f * alpha);
        draw_line_a(c, (float)ax + 2, (float)(ay + s - 2), (float)(ax + s - 2), (float)ay + 2, fmaxf(1.f, S), C_CYAN, alpha, alpha);
    }
    char lines[2][200];
    int nl = 0;
    int maxw = D->cw - sc(8);
    wrap_text(F->sm, it->name, maxw, lines, 2, &nl);
    if (nl == 0) { lines[0][0] = 0; nl = 1; }
    uint32_t tcol = D->dark_bg ? 0xFFFFFF : 0x121A2E, scol = D->dark_bg ? 0x000000 : 0xFFFFFF;
    int ty = by + R + sc(6);
    for (int k = 0; k < nl; k++) {
        char *ln = lines[k];
        if (k == 1 && text_width(F->sm, ln, 0) > maxw) { /* se elide en draw_text */ }
        int tw = imin(text_width(F->sm, ln, 0), maxw);
        int tx = cx + (D->cw - tw) / 2;
        draw_text(c, F->sm, tx + 1, ty + 1, ln, scol, 0.7f * alpha, 0, maxw);
        draw_text(c, F->sm, tx, ty, ln, tcol, alpha, 0, maxw);
        ty += F->sm->height + sc(1);
    }
}

static void desk_draw(App *a) {
    Desk *D = a->u;
    Canvas *c = &a->w->cv;
    if (c->w == D->base.w && c->h == D->base.h) memcpy(c->px, D->base.px, (size_t)c->w * (size_t)c->h * 4);
    clip_reset(c);
    for (int i = 0; i < D->n; i++) {
        if (!D->it[i].placed || (D->drag && i == D->sel && D->moved)) continue;
        Rect r = desk_cell_rect(D, i);
        desk_draw_item(D, c, i, r.x, r.y, i == D->sel, i == D->hover, 1.f);
    }
    if (D->drag && D->moved && D->sel >= 0) {   /* destino + icono fantasma */
        int tc = iclamp((D->dx - D->ox) / D->cw, 0, D->cols - 1), tr = iclamp((D->dy - D->oy) / D->ch, 0, D->rows - 1);
        fill_rect(c, D->ox + tc * D->cw + sc(3), D->oy + tr * D->ch + sc(3), D->cw - sc(6), D->ch - sc(6), C_CYAN, 0.12f);
        desk_draw_item(D, c, D->sel, D->dx - D->cw / 2, D->dy - D->ch / 2, 1, 0, 0.9f);
    }
}

static void open_ditem(DItem *it) {
    char tmp[300];
    if (it->kind == 0) {
        switch (it->act) {
        case DA_TERM: spawn_cmd(terminal_cmd()); break;
        case DA_FILES: if (files_cmd(tmp, sizeof tmp)) spawn_cmd(tmp); break;
        case DA_MENU: spawn_fmt("'%s' menu", self_path); break;
        case DA_SETTINGS: spawn_fmt("'%s' settings", self_path); break;
        case DA_GAMES: spawn_fmt("'%s' games", self_path); break;
        }
    } else if (it->kind == 1) {
        AppEntry ae;
        memset(&ae, 0, sizeof ae);
        ae.exec = it->exec; ae.term = it->term;
        launch_app_ex(&ae);
    } else if (it->kind == 3 && files_cmd(tmp, sizeof tmp)) {
        spawn_fmt("%s '%s'", tmp, it->path);
    } else if (which("xdg-open", NULL, 0)) {
        spawn_fmt("xdg-open '%s'", it->path);
    } else if (files_cmd(tmp, sizeof tmp)) {
        spawn_fmt("%s '%s'", tmp, it->path);
    }
}

static void desk_reload_items(App *a) {
    Desk *D = a->u;
    desk_load_items(D);
    desk_geometry(D);
    desk_layout(D, 0);
    D->sel = D->hover = -1;
    a->dirty = 1;
}

static void shortcut_dialog(void) {
    char name[128] = "", cmd[512] = "", icon[128] = "", term[2] = "0";
    FField f[4] = {{"NOMBRE", name, sizeof name, 0}, {"COMANDO", cmd, sizeof cmd, 0},
                   {"ICONO (nombre o ruta .png, opcional)", icon, sizeof icon, 0}, {"EJECUTAR EN TERMINAL", term, sizeof term, 1}};
    if (!form_dialog("NUEVO ACCESO DIRECTO", NULL, f, 4, "CREAR", 0)) return;
    if (!name[0] || !cmd[0]) { message_box("ACCESO DIRECTO", "Hace falta un nombre y un comando."); return; }
    char dd[1400], safe[160], path[1700];
    desktop_dir(dd, sizeof dd);
    if (!is_dir(dd)) snprintf(dd, sizeof dd, "%s/Desktop", home_dir());
    mkdir_p(dd);
    size_t k = 0;
    for (const char *p = name; *p && k < sizeof safe - 9; p++) safe[k++] = (isalnum((unsigned char)*p) || (unsigned char)*p >= 0x80) ? *p : '_';
    safe[k] = 0;
    snprintf(path, sizeof path, "%s/%s.desktop", dd, safe);
    FILE *fp = fopen(path, "wb");
    if (!fp) { message_box("ACCESO DIRECTO", "No pude escribir en el escritorio."); return; }
    fprintf(fp, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nTerminal=%s\n", name, cmd, term[0] == '1' ? "true" : "false");
    if (icon[0]) fprintf(fp, "Icon=%s\n", icon);
    fclose(fp);
    chmod(path, 0755);
}

static void new_folder(void) {
    char dd[1400], p[1700];
    desktop_dir(dd, sizeof dd);
    if (!is_dir(dd)) snprintf(dd, sizeof dd, "%s/Desktop", home_dir());
    mkdir_p(dd);
    for (int i = 1; i < 100; i++) {
        if (i == 1) snprintf(p, sizeof p, "%s/Nueva carpeta", dd);
        else snprintf(p, sizeof p, "%s/Nueva carpeta %d", dd, i);
        if (!file_exists(p)) { mkdir(p, 0755); return; }
    }
}

static void desk_rebuild(App *a);

static void desk_context(App *a, int x, int y, int item) {
    Desk *D = a->u;
    if (item >= 0) {
        DItem *it = &D->it[item];
        MItem m[2] = {{"Abrir", 0}, {"Quitar del escritorio", 1}};
        int r = popup_menu(x, y, m, it->kind == 1 ? 2 : 1);
        if (r == 0) open_ditem(it);
        else if (r == 1) { unlink(it->path); desk_reload_items(a); }
    } else {
        MItem m[8] = {{"Nuevo acceso directo...", 0}, {"Nueva carpeta", 0}, {"Ordenar iconos", 0}, {"Cambiar fondo...", 0},
                      {"Terminal aquí", 2}, {"Ajustes de apariencia", 0}, {"Ajustes de pantalla", 0}, {"Panel rápido", 0}};
        int r = popup_menu(x, y, m, 8);
        a->dirty = 1;
        if (r == 0) { shortcut_dialog(); desk_reload_items(a); }
        else if (r == 1) { new_folder(); desk_reload_items(a); }
        else if (r == 2) { desk_layout(D, 1); }
        else if (r == 3) {
            MItem w[5];
            int nw = 0, map[5];
            for (int i = 0; i < 5; i++) { if (i == 4 && !cfg.wall_img[0]) continue; w[nw].label = WALL_NAMES[i]; w[nw].style = 0; map[nw++] = i; }
            int s = popup_menu(x, y, w, nw);
            if (s >= 0) { cfg.wall = map[s]; cfg_save(); desk_rebuild(a); }
        }
        else if (r == 4) spawn_cmd(terminal_cmd());
        else if (r == 5) spawn_fmt("'%s' settings appearance", self_path);
        else if (r == 6) spawn_fmt("'%s' settings display", self_path);
        else if (r == 7) spawn_fmt("'%s' panel", self_path);
    }
    a->dirty = 1;
}

static void desk_button(App *a, int x, int y, int button, int press) {
    Desk *D = a->u;
    if (button == 4 || button == 5) { if (press) goto_desktop(current_desktop() + (button == 5 ? 1 : -1)); return; }
    if (button == 3 && press) { desk_context(a, x, y, desk_hit(D, x, y)); D->press = 0; return; }
    if (button != 1) return;
    if (press) {
        D->press = 1; D->px = x; D->py = y; D->ptick = 0; D->moved = 0; D->drag = 0;
        int h = desk_hit(D, x, y);
        D->sel = h;
        D->drag = h >= 0;
        a->tick_ms = 100;
        a->dirty = 1;
        return;
    }
    if (!D->press) return;
    D->press = 0;
    a->tick_ms = 1000;
    a->dirty = 1;
    if (D->drag && D->moved && D->sel >= 0) {
        int tc = iclamp((x - D->ox) / D->cw, 0, D->cols - 1), tr = iclamp((y - D->oy) / D->ch, 0, D->rows - 1), oc, orow;
        cell_nearest(D, tc, tr, D->sel, &oc, &orow);
        D->it[D->sel].col = oc; D->it[D->sel].row = orow;
        desk_positions_save(D);
        D->drag = 0; D->moved = 0;
        return;
    }
    D->drag = 0;
    if (D->moved) return;
    if (D->sel < 0) return;
    long t = now_ms();
    if (cfg.single_click || (D->last_click == D->sel && t - D->last_click_ms < 450)) { open_ditem(&D->it[D->sel]); D->last_click = -1; }
    else { D->last_click = D->sel; D->last_click_ms = t; }
}

static void desk_motion(App *a, int x, int y) {
    Desk *D = a->u;
    if (D->press && D->drag) {
        if (D->moved || abs(x - D->px) > sc(10) || abs(y - D->py) > sc(10)) { D->moved = 1; D->dx = x; D->dy = y; a->dirty = 1; }
        return;
    }
    int h = desk_hit(D, x, y);
    if (h != D->hover) { D->hover = h; a->dirty = 1; }
}

static void desk_tick(App *a) {
    Desk *D = a->u;
    reap();
    if (D->press && !D->moved && ++D->ptick >= 6) {        /* pulsación larga ~600 ms = clic derecho */
        D->press = 0; a->tick_ms = 1000;
        desk_context(a, D->px, D->py, D->sel >= 0 ? D->sel : -1);
        return;
    }
    char dd[1400];
    desktop_dir(dd, sizeof dd);
    struct stat st;
    if (stat(dd, &st) == 0 && st.st_mtime != D->dir_mtime) desk_reload_items(a);
}

static void desk_key(App *a, KeySym ks, int cp, unsigned st) {
    Desk *D = a->u;
    (void)cp; (void)st;
    if (ks == XK_Return && D->sel >= 0) open_ditem(&D->it[D->sel]);
    a->dirty = 1;
}

static void desk_set_root(Desk *D) {
    Window root = RootWindow(dpy, scr);
    if (D->root_pm != None) { XFreePixmap(dpy, D->root_pm); D->root_pm = None; }
    int w = D->base.w, h = D->base.h;
    Pixmap pm = XCreatePixmap(dpy, root, (unsigned)w, (unsigned)h, (unsigned)depth);
    uint32_t *data = D->base.px, *conv = NULL;
    if (!std_masks) { conv = malloc((size_t)w * h * 4); canvas_convert(D->base.px, conv, (size_t)w * h); data = conv; }
    XImage *img = XCreateImage(dpy, vis, (unsigned)depth, ZPixmap, 0, (char *)data, (unsigned)w, (unsigned)h, 32, 0);
    GC gc = XCreateGC(dpy, pm, 0, NULL);
    XPutImage(dpy, pm, gc, img, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    img->data = NULL; XDestroyImage(img);
    free(conv);
    XFreeGC(dpy, gc);
    XSetWindowBackgroundPixmap(dpy, root, pm);
    XClearWindow(dpy, root);
    D->root_pm = pm;   /* no liberar: la raíz lo referencia; sin este proceso el fondo quedaría negro */
    XSync(dpy, False);
}

typedef struct { Desk *D; Win *w; } DeskCtx;
static DeskCtx g_dctx;

/* se vuelve a leer config/tema y se repinta todo (reload, cambio de resolución, cambio de fondo) */
static void desk_rebuild(App *a) {
    Desk *D = a->u;
    cfg_load();
    theme_apply();
    int sw, sh;
    screen_size(&sw, &sh);
    scale_apply(sw, sh);
    icon_cache_clear();
    D->F = fonts_open();
    D->sw = sw; D->sh = sh;
    free(D->base.px);
    D->base = canvas_new(sw, sh);
    render_wallpaper(&D->base);
    {   /* luminosidad media de la zona de iconos -> color de etiquetas legible */
        long acc = 0; int cnt = 0;
        for (int yy = 0; yy < sh; yy += imax(1, sh / 12))
            for (int xx = 0; xx < imin(sw, sw / 3 + 1); xx += imax(1, sw / 24)) {
                uint32_t p = D->base.px[(size_t)yy * sw + xx];
                acc += (long)(((p >> 16) & 255) * 30 + ((p >> 8) & 255) * 59 + (p & 255) * 11) / 100; cnt++;
            }
        D->dark_bg = cnt ? (acc / cnt) < 140 : 1;
    }
    desk_set_root(D);
    Win *W = g_dctx.w;
    if (cfg.desk_icons) {
        XMoveResizeWindow(dpy, W->win, 0, 0, (unsigned)sw, (unsigned)sh);
        if (!D->mapped) { XMapWindow(dpy, W->win); D->mapped = 1; }
        XLowerWindow(dpy, W->win);
        win_resize_buffers(W, sw, sh);
        XSetWindowBackgroundPixmap(dpy, W->win, D->root_pm);
    } else if (D->mapped) { XUnmapWindow(dpy, W->win); D->mapped = 0; }
    desk_load_items(D);
    desk_geometry(D);
    desk_layout(D, 0);
    D->sel = D->hover = -1;
    a->dirty = cfg.desk_icons;
    XSync(dpy, False);
}

static void desk_reload_cb(App *a) { desk_rebuild(a); }
static void desk_root_cb(App *a, int w, int h) { Desk *D = a->u; if (w != D->sw || h != D->sh) desk_rebuild(a); }

static int run_desktop(void) {
    if (!single_instance("desktop")) return 0;
    install_reload_handler();
    Desk *D = calloc(1, sizeof *D);
    D->sel = D->hover = -1; D->last_click = -1; D->root_pm = None;
    int sw, sh;
    screen_size(&sw, &sh);
    Win *W = win_create(0, 0, sw, sh, 1, "sesar-desktop");
    g_dctx.D = D; g_dctx.w = W;
    XSelectInput(dpy, RootWindow(dpy, scr), StructureNotifyMask);
    App a = {0};
    a.w = W; a.u = D; a.tick_ms = 1000;
    a.draw = desk_draw; a.key = desk_key; a.button = desk_button; a.motion = desk_motion; a.tick = desk_tick;
    a.reload = desk_reload_cb; a.root = desk_root_cb;
    desk_rebuild(&a);
    run_app(&a);
    return 0;
}


/* ------------------------------------------------------------------ PANEL RÁPIDO */
typedef struct {
    Fonts *F;
    int W, H, pad, backend, vol, muted, ndesk, curdesk, hover, dragvol, last_set;
    Rect volR, muteR, deskR[6], togR[3], btnR[6];
} Panel;

static const char *PANEL_BTN[] = {"PANTALLA COMPLETA", "ESCRITORIO", "AJUSTES", "ENERGÍA", "ATAJOS", "TERMINAL"};
static const char *PANEL_TOG[] = {"HUD", "ICONOS", "1 TOQUE"};

static void volume_read(Panel *p) {
    char out[512];
    p->vol = 0; p->muted = 0;
    if (p->backend == 1) {
        if (cmd_output("pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null", out, sizeof out)) {
            char *pc = strchr(out, '%');
            if (pc) { char *q = pc; while (q > out && isdigit((unsigned char)q[-1])) q--; p->vol = atoi(q); }
        }
        if (cmd_output("pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null", out, sizeof out)) p->muted = strstr(out, "yes") != NULL;
    } else if (p->backend == 2) {
        if (cmd_output("amixer sget Master 2>/dev/null", out, sizeof out)) {
            char *b = strchr(out, '[');
            if (b) p->vol = atoi(b + 1);
            p->muted = strstr(out, "[off]") != NULL;
        }
    }
    p->vol = iclamp(p->vol, 0, 150);
}

static void volume_set(Panel *p, int v) {
    v = iclamp(v, 0, 100);
    if (p->backend == 1) spawn_fmt("pactl set-sink-volume @DEFAULT_SINK@ %d%%", v);
    else if (p->backend == 2) spawn_fmt("amixer -q sset Master %d%%", v);
    p->vol = v;
}

static void panel_layout(Panel *p) {
    int y = p->pad + p->F->title->height + sc(14);
    int iw = p->W - p->pad * 2;
    if (p->backend) {
        y += sc(20);
        p->volR = (Rect){p->pad, y, iw - sc(60), sc(34)};
        p->muteR = (Rect){p->pad + iw - sc(52), y, sc(52), sc(34)};
        y += sc(34) + sc(14);
    }
    y += sc(20);
    int gap = sc(6), dw = (iw - gap * (p->ndesk - 1)) / p->ndesk;
    for (int i = 0; i < p->ndesk; i++) p->deskR[i] = (Rect){p->pad + i * (dw + gap), y, dw, sc(38)};
    y += sc(38) + sc(14);
    int tw = (iw - gap * 2) / 3;
    for (int i = 0; i < 3; i++) p->togR[i] = (Rect){p->pad + i * (tw + gap), y, tw, sc(36)};
    y += sc(36) + sc(14);
    int bw = (iw - gap) / 2;
    for (int i = 0; i < 6; i++) p->btnR[i] = (Rect){p->pad + (i % 2) * (bw + gap), y + (i / 2) * (sc(42) + gap), bw, sc(42)};
    y += 3 * (sc(42) + gap);
    p->H = y - gap + p->pad;
}

static int panel_hit(Panel *p, int x, int y) {
    if (p->backend && in_rect(p->volR, x, y)) return 1;
    if (p->backend && in_rect(p->muteR, x, y)) return 2;
    for (int i = 0; i < p->ndesk; i++) if (in_rect(p->deskR[i], x, y)) return 10 + i;
    for (int i = 0; i < 3; i++) if (in_rect(p->togR[i], x, y)) return 20 + i;
    for (int i = 0; i < 6; i++) if (in_rect(p->btnR[i], x, y)) return 30 + i;
    return 0;
}

static void panel_chip(Canvas *c, Fonts *F, Rect r, const char *label, int active, int hover, uint32_t col) {
    Shape s;
    memset(&s, 0, sizeof s);
    s.cut = (float)sc(7); s.sw = fmaxf(1.f, S * 1.2f);
    s.top = s.bot = active ? col : C_BTN1; s.fa = active ? 0.26f : (hover ? 0.6f : 1.f);
    s.stroke = active ? col : (hover ? C_MUTED : C_LINE); s.sa = 1.f;
    draw_chamfer(c, (float)r.x, (float)r.y, (float)r.w, (float)r.h, &s);
    draw_text_c(c, F->smb, r.x + r.w / 2, r.y + (r.h - F->smb->height) / 2, label, active ? col : C_DIM, 1.f, S * 0.5f);
}

static void panel_draw(App *a) {
    Panel *p = a->u;
    Canvas *c = &a->w->cv;
    Fonts *F = p->F;
    fill_rect(c, 0, 0, p->W, p->H, C_BG, 1.f);
    Shape ps = panel_shape();
    ps.ga *= T.glow;
    draw_chamfer(c, 0, 0, (float)p->W, (float)p->H, &ps);
    draw_text(c, F->title, p->pad, p->pad, "PANEL", C_TEXT, 1.f, S * 2.f, 0);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[64], ds[64];
    strftime(ts, sizeof ts, cfg.clock24 ? "%H:%M" : "%I:%M %p", &tm);
    strftime(ds, sizeof ds, "%a %d %b", &tm);
    int tw = text_width(F->title, ts, 0);
    draw_text(c, F->title, p->W - p->pad - tw, p->pad, ts, C_CYAN, 1.f, 0, 0);
    draw_text(c, F->sm, p->W - p->pad - text_width(F->sm, ds, 0), p->pad + F->title->height, ds, C_MUTED, 1.f, 0, 0);
    if (p->backend) {
        draw_text(c, F->smb, p->pad, p->volR.y - sc(18), "VOLUMEN", C_MUTED, 1.f, S * 1.5f, 0);
        char vs[16];
        snprintf(vs, sizeof vs, "%d%%", p->vol);
        draw_text(c, F->smb, p->volR.x + p->volR.w - text_width(F->smb, vs, 0), p->volR.y - sc(18), vs, C_CYAN, 1.f, 0, 0);
        hud_bar(c, p->volR.x, p->volR.y + sc(8), p->volR.w, sc(18), p->muted ? 0.f : (float)p->vol / 100.f);
        panel_chip(c, F, p->muteR, p->muted ? "MUDO" : "SON", p->muted, p->hover == 2, C_RED);
    }
    draw_text(c, F->smb, p->pad, p->deskR[0].y - sc(18), "ESCRITORIOS", C_MUTED, 1.f, S * 1.5f, 0);
    for (int i = 0; i < p->ndesk; i++) {
        char n[8];
        snprintf(n, sizeof n, "%d", i + 1);
        panel_chip(c, F, p->deskR[i], n, i == p->curdesk, p->hover == 10 + i, C_MAGENTA);
    }
    int on[3] = {cfg.hud, cfg.desk_icons, cfg.single_click};
    for (int i = 0; i < 3; i++) panel_chip(c, F, p->togR[i], PANEL_TOG[i], on[i], p->hover == 20 + i, C_GREEN);
    for (int i = 0; i < 6; i++) draw_button(c, F, p->btnR[i], PANEL_BTN[i], BTN_NORMAL, p->hover == 30 + i);
}

static void panel_set_vol_from_x(App *a, int x) {
    Panel *p = a->u;
    int v = (int)lrintf(clamp01((float)(x - p->volR.x) / (float)p->volR.w) * 100.f);
    if (abs(v - p->last_set) >= 2 || v == 0 || v == 100) { volume_set(p, v); p->last_set = v; p->muted = 0; }
    p->vol = v;
    a->dirty = 1;
}

static void panel_activate(App *a, int h) {
    Panel *p = a->u;
    if (h == 2) { spawn_cmd(p->backend == 1 ? "pactl set-sink-mute @DEFAULT_SINK@ toggle" : "amixer -q sset Master toggle"); p->muted = !p->muted; }
    else if (h >= 10 && h < 20) { goto_desktop(h - 10); p->curdesk = h - 10; }
    else if (h >= 20 && h < 30) {
        if (h == 20) cfg.hud = !cfg.hud;
        else if (h == 21) cfg.desk_icons = !cfg.desk_icons;
        else cfg.single_click = !cfg.single_click;
        cfg_save();
        signal_instance("desktop", SIGUSR1);
        signal_instance("hud", SIGUSR1);
    } else if (h >= 30) {
        a->quit = 1;
        ungrab_input();
        XSync(dpy, False);
        switch (h - 30) {
        case 0: usleep(120000); toggle_fullscreen_active(); break;
        case 1: usleep(120000); toggle_show_desktop(); break;
        case 2: run_action(ACT_SETTINGS); break;
        case 3: run_action(ACT_POWER); break;
        case 4: spawn_fmt("'%s' settings keys", self_path); break;
        case 5: run_action(ACT_TERM); break;
        }
    }
    a->dirty = 1;
}

static void panel_button(App *a, int x, int y, int button, int press) {
    Panel *p = a->u;
    if (button != 1) return;
    if (!press) { p->dragvol = 0; return; }
    if (x < 0 || y < 0 || x >= p->W || y >= p->H) { a->quit = 1; return; }
    int h = panel_hit(p, x, y);
    if (h == 1) { p->dragvol = 1; p->last_set = -9; panel_set_vol_from_x(a, x); }
    else if (h) panel_activate(a, h);
}

static void panel_motion(App *a, int x, int y) {
    Panel *p = a->u;
    if (p->dragvol) { panel_set_vol_from_x(a, x); return; }
    int h = panel_hit(p, x, y);
    if (h != p->hover) { p->hover = h; a->dirty = 1; }
}

static void panel_key(App *a, KeySym ks, int cp, unsigned st) { (void)cp; (void)st; if (ks == XK_Escape) a->quit = 1; }

static void panel_tick(App *a) {
    Panel *p = a->u;
    p->curdesk = current_desktop();
    if (!p->dragvol) volume_read(p);
    a->dirty = 1;
}

static int run_panel(void) {
    if (!single_instance("panel")) return 0;
    Panel *p = calloc(1, sizeof *p);
    p->F = fonts_open();
    p->pad = sc(18);
    p->backend = which("pactl", NULL, 0) ? 1 : (which("amixer", NULL, 0) ? 2 : 0);
    p->ndesk = iclamp((int)get_cardinal(RootWindow(dpy, scr), A_NET_NUMDESK, cfg.ndesk), 1, 6);
    p->curdesk = current_desktop();
    p->hover = 0;
    if (p->backend) volume_read(p);
    int sw = screen_w(), sh = screen_h();
    p->W = imin(sc(380), sw - sc(12));
    panel_layout(p);
    int tr = tray_height();
    int x = sw - p->W - sc(8), y = cfg.tray_top ? tr + sc(8) : sh - tr - p->H - sc(8);
    Win *w = win_create(x, imax(4, y), p->W, p->H, 1, "sesar-panel");
    win_shape_chamfer(w, sc(18));
    XMapRaised(dpy, w->win);
    XSync(dpy, False);
    grab_input(w);
    App a = {0};
    a.w = w; a.u = p; a.tick_ms = 2000;
    a.draw = panel_draw; a.key = panel_key; a.button = panel_button; a.motion = panel_motion; a.tick = panel_tick;
    run_app(&a);
    ungrab_input();
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



/* ------------------------------------------------------------------ atajos de teclado */
typedef struct { const char *combo, *mask, *key, *action, *desc; } KeyDef;
static const KeyDef KEYS[] = {
    {"Ctrl+Alt+T",        "CA",  "t",       "exec:@TERM@",             "Abrir terminal"},
    {"Super+Enter",       "4",   "Return",  "exec:@TERM@",             "Abrir terminal"},
    {"Super+Espacio",     "4",   "space",   "exec:@SELF@ menu",        "Menú de inicio"},
    {"Alt+F1",            "A",   "F1",      "exec:@SELF@ menu",        "Menú de inicio"},
    {"Alt+F2",            "A",   "F2",      "exec:@SELF@ menu",        "Ejecutar comando (escribí y ENTER)"},
    {"Super+E",           "4",   "e",       "exec:@SELF@ files",       "Gestor de archivos"},
    {"Super+S",           "4",   "s",       "exec:@SELF@ settings",    "Ajustes"},
    {"Super+P",           "4",   "p",       "exec:@SELF@ panel",       "Panel rápido"},
    {"Super+/",           "4",   "slash",   "exec:@SELF@ settings keys", "Lista de atajos"},
    {"Super+Esc",         "4",   "Escape",  "exec:@SELF@ power",       "Energía / sesión"},
    {"Ctrl+Alt+Supr",     "CA",  "Delete",  "exec:@SELF@ power",       "Energía / sesión"},
    {"Alt+Tab",           "A",   "Tab",     "next",                    "Ventana siguiente"},
    {"Alt+Shift+Tab",     "AS",  "Tab",     "prev",                    "Ventana anterior"},
    {"Alt+Espacio",       "A",   "space",   "window",                  "Menú de la ventana"},
    {"Alt+F4",            "A",   "F4",      "close",                   "Cerrar ventana"},
    {"Super+Q",           "4",   "q",       "close",                   "Cerrar ventana"},
    {"Super+F",           "4",   "f",       "fullscreen",              "Pantalla completa"},
    {"Alt+F11",           "A",   "F11",     "fullscreen",              "Pantalla completa"},
    {"Super+M",           "4",   "m",       "maximize",                "Maximizar / restaurar"},
    {"Super+↑",           "4",   "Up",      "maximize",                "Maximizar / restaurar"},
    {"Alt+F10",           "A",   "F10",     "maximize",                "Maximizar / restaurar"},
    {"Super+↓",           "4",   "Down",    "minimize",                "Minimizar"},
    {"Super+D",           "4",   "d",       "showdesktop",             "Mostrar escritorio"},
    {"Ctrl+Alt+D",        "CA",  "d",       "showdesktop",             "Mostrar escritorio"},
    {"Super+←",           "4",   "Left",    "ldesktop",                "Escritorio anterior"},
    {"Super+→",           "4",   "Right",   "rdesktop",                "Escritorio siguiente"},
    {"Ctrl+Alt+←",        "CA",  "Left",    "ldesktop",                "Escritorio anterior"},
    {"Ctrl+Alt+→",        "CA",  "Right",   "rdesktop",                "Escritorio siguiente"},
    {"Ctrl+Alt+1…9",      "CA",  "#",       "desktop#",                "Ir al escritorio N"},
    {"Ctrl+Alt+Shift+1…9","CAS", "#",       "send#",                   "Mover ventana al escritorio N"},
};
#define NKEYS ((int)(sizeof KEYS / sizeof KEYS[0]))

typedef struct { char combo[64], cmd[512]; } CustomKey;
static CustomKey *ckeys;
static int nckeys;

static void ckeys_load(void) {
    free(ckeys); ckeys = NULL; nckeys = 0;
    char p[1400];
    cfg_path("keys", p, sizeof p);
    char *txt = read_file(p);
    if (!txt) return;
    char *save = NULL;
    for (char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        while (*ln == ' ' || *ln == '\t') ln++;
        if (*ln == '#' || !*ln) continue;
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = ln, *v = eq + 1;
        size_t kl = strlen(k);
        while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t')) k[--kl] = 0;
        while (*v == ' ' || *v == '\t') v++;
        size_t vl = strlen(v);
        while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' ')) v[--vl] = 0;
        if (!*k || !*v) continue;
        ckeys = xrealloc(ckeys, sizeof *ckeys * (size_t)(nckeys + 1));
        scopy(ckeys[nckeys].combo, sizeof ckeys[nckeys].combo, k);
        scopy(ckeys[nckeys].cmd, sizeof ckeys[nckeys].cmd, v);
        nckeys++;
    }
    free(txt);
}

static void ckeys_save(void) {
    ensure_cfg_dir();
    char p[1400];
    cfg_path("keys", p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fputs("# Atajos personalizados: Combinación = comando   (ej.  Ctrl+Alt+B = firefox)\n", f);
    for (int i = 0; i < nckeys; i++) fprintf(f, "%s = %s\n", ckeys[i].combo, ckeys[i].cmd);
    fclose(f);
}

/* "Ctrl+Alt+B" -> mask "CA", key "b" (nombres de JWM / X11) */
static int parse_combo(const char *combo, char *mask, size_t mn, char *key, size_t kn) {
    char *copy = xstrdup(combo), *save = NULL;
    mask[0] = 0; key[0] = 0;
    size_t m = 0;
    for (char *t = strtok_r(copy, "+", &save); t; t = strtok_r(NULL, "+", &save)) {
        while (*t == ' ') t++;
        size_t L = strlen(t);
        while (L && t[L - 1] == ' ') t[--L] = 0;
        if (!L) continue;
        char lo[32];
        scopy(lo, sizeof lo, t);
        for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
        char c = 0;
        if (!strcmp(lo, "ctrl") || !strcmp(lo, "control")) c = 'C';
        else if (!strcmp(lo, "alt")) c = 'A';
        else if (!strcmp(lo, "shift") || !strcmp(lo, "mayús")) c = 'S';
        else if (!strcmp(lo, "super") || !strcmp(lo, "win") || !strcmp(lo, "meta") || !strcmp(lo, "mod4")) c = '4';
        if (c) { if (m + 1 < mn && !strchr(mask, c)) { mask[m++] = c; mask[m] = 0; } continue; }
        if (!strcmp(lo, "enter")) scopy(key, kn, "Return");
        else if (!strcmp(lo, "esc")) scopy(key, kn, "Escape");
        else if (!strcmp(lo, "space") || !strcmp(lo, "espacio")) scopy(key, kn, "space");
        else if (!strcmp(lo, "tab")) scopy(key, kn, "Tab");
        else if (!strcmp(lo, "del") || !strcmp(lo, "supr") || !strcmp(lo, "delete")) scopy(key, kn, "Delete");
        else if (!strcmp(lo, "up")) scopy(key, kn, "Up");
        else if (!strcmp(lo, "down")) scopy(key, kn, "Down");
        else if (!strcmp(lo, "left")) scopy(key, kn, "Left");
        else if (!strcmp(lo, "right")) scopy(key, kn, "Right");
        else if (L == 1) { key[0] = (char)tolower((unsigned char)t[0]); key[1] = 0; }
        else scopy(key, kn, t);
    }
    free(copy);
    return key[0] != 0;
}

/* ------------------------------------------------------------------ recursos de X (xterm, DPI) */
static void hex6(char *out, uint32_t c) { snprintf(out, 8, "#%06X", (unsigned)(c & 0xFFFFFF)); }

static char *xres_block(void) {
    Sb b = {0};
    char h[8], h2[8];
    sb_add(&b, "! BEGIN " MARK "\n");
    hex6(h, T.bg); sb_add(&b, "XTerm*background: %s\n", h);
    hex6(h, T.text); sb_add(&b, "XTerm*foreground: %s\n", h);
    hex6(h, T.a1); sb_add(&b, "XTerm*cursorColor: %s\nXTerm*pointerColor: %s\n", h, h);
    hex6(h, T.bg); sb_add(&b, "XTerm*pointerColorBackground: %s\n", h);
    if (T.light)
        sb_add(&b, "XTerm*color0: #1A2238\nXTerm*color1: #C62840\nXTerm*color2: #12803F\nXTerm*color3: #A86400\n"
                   "XTerm*color4: #2A4FC4\nXTerm*color5: #A5189A\nXTerm*color6: #0A7F96\nXTerm*color7: #5A6682\n"
                   "XTerm*color8: #3A4660\nXTerm*color9: #E0364F\nXTerm*color10: #1C9E52\nXTerm*color11: #C27A00\n"
                   "XTerm*color12: #4468E0\nXTerm*color13: #C52DB8\nXTerm*color14: #1596B0\nXTerm*color15: #8591AD\n");
    else
        sb_add(&b, "XTerm*color0: #0B1224\nXTerm*color1: #FF3B5C\nXTerm*color2: #39FF88\nXTerm*color3: #FFB020\n"
                   "XTerm*color4: #4D7CFF\nXTerm*color5: #FF2BD6\nXTerm*color6: #00E5FF\nXTerm*color7: #B4C4E0\n"
                   "XTerm*color8: #24365E\nXTerm*color9: #FF6B85\nXTerm*color10: #7DFFB0\nXTerm*color11: #FFD166\n"
                   "XTerm*color12: #7FA2FF\nXTerm*color13: #FF7BE8\nXTerm*color14: #7DF3FF\nXTerm*color15: #E8F4FF\n");
    hex6(h2, T.line);
    (void)h2;
    sb_add(&b, "XTerm*faceName: Monospace\nXTerm*faceSize: %d\nXTerm*scrollBar: false\nXTerm*internalBorder: %d\n"
               "XTerm*borderWidth: 0\nXTerm*saveLines: 10000\nXTerm*cursorBlink: true\nXTerm*selectToClipboard: true\n"
               "XTerm*metaSendsEscape: true\nXTerm*termName: xterm-256color\nXTerm*utf8: 1\n",
           cfg.term_font, sc(10));
    sb_add(&b, "XTerm*vt100.translations: #override \\\n"
               "    Ctrl Shift <Key>C: copy-selection(CLIPBOARD) \\n\\\n"
               "    Ctrl Shift <Key>V: insert-selection(CLIPBOARD) \\n\\\n"
               "    Ctrl <Key>plus: larger-vt-font() \\n\\\n"
               "    Ctrl <Key>equal: larger-vt-font() \\n\\\n"
               "    Ctrl <Key>minus: smaller-vt-font()\n");
    sb_add(&b, "Xft.dpi: %d\nXft.antialias: 1\nXft.hinting: 1\nXft.hintstyle: hintslight\nXft.rgba: none\nXcursor.size: %d\n",
           (int)lrintf(96.f * S), sc(24));
    sb_add(&b, "! END " MARK "\n");
    return b.p;
}

static int run_xres(void) {
    char *blk = xres_block();
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
    char *out = calloc(1, strlen(old) + strlen(blk) + 8);
    if (b && e) {
        e = strchr(e, '\n');
        memcpy(out, old, (size_t)(b - old));
        strcat(out, e ? e + 1 : "");
    } else strcat(out, old);
    size_t L = strlen(out);
    if (L && out[L - 1] != '\n') strcat(out, "\n");
    strcat(out, blk);
    XChangeProperty(dpy, root, XA_RESOURCE_MANAGER, XA_STRING, 8, PropModeReplace, (unsigned char *)out, (int)strlen(out));
    XSync(dpy, False);
    free(old); free(out); free(blk);
    return 0;
}

/* ------------------------------------------------------------------ configuración de JWM */
static void sb_xml(Sb *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_add(b, "&amp;"); break;
        case '<': sb_add(b, "&lt;"); break;
        case '>': sb_add(b, "&gt;"); break;
        case '"': sb_add(b, "&quot;"); break;
        default: sb_add(b, "%c", *s);
        }
    }
}

static void sb_key(Sb *b, const char *mask, const char *key, const char *action) {
    char act[1400];
    scopy(act, sizeof act, action);
    Sb t = {0};
    const char *p = act;
    while (*p) {
        if (!strncmp(p, "@SELF@", 6)) { sb_add(&t, "'%s'", self_path); p += 6; }
        else if (!strncmp(p, "@TERM@", 6)) { sb_add(&t, "%s", terminal_cmd()); p += 6; }
        else { sb_add(&t, "%c", *p); p++; }
    }
    if (mask && *mask) sb_add(b, "  <Key mask=\"%s\" key=\"", mask); else sb_add(b, "  <Key key=\"");
    sb_xml(b, key);
    sb_add(b, "\">");
    sb_xml(b, t.p ? t.p : "");
    sb_add(b, "</Key>\n");
    free(t.p);
}

static char *gen_jwmrc(void) {
    Sb b = {0};
    char c1[8], c2[8], c3[8], c4[8], c5[8], c6[8], c7[8], c8[8], c9[8];
    const char *font = "DejaVu Sans Mono";
    int tray_h = tray_height();
    static const float title_h[] = {22, 28, 36}, border_w[] = {2, 3, 7};
    int th = sc(title_h[cfg.win_border]), bw = imax(2, sc(border_w[cfg.win_border]));
    int term_w = sc(240);
    const char *selfq = self_path;
    (void)selfq;

    sb_add(&b, "<?xml version=\"1.0\"?>\n<!-- " MARK ": generado por 'sesar-shell setup'; se regenera al iniciar sesión -->\n<JWM>\n");
    sb_add(&b, "  <StartupCommand>'%s' xres</StartupCommand>\n", self_path);
    sb_add(&b, "  <StartupCommand>'%s' desktop</StartupCommand>\n", self_path);
    sb_add(&b, "  <StartupCommand>'%s' hud</StartupCommand>\n", self_path);
    sb_add(&b, "  <StartupCommand>'%s' autostart</StartupCommand>\n\n", self_path);

    sb_add(&b, "  <RootMenu onroot=\"3\">\n");
    sb_add(&b, "    <Program label=\"Inicio\">'%s' menu</Program>\n", self_path);
    sb_add(&b, "    <Program label=\"Juegos soportados\">'%s' games</Program>\n", self_path);
    sb_add(&b, "    <Program label=\"Terminal\">%s</Program>\n", terminal_cmd());
    sb_add(&b, "    <Program label=\"Archivos\">'%s' files</Program>\n    <Separator/>\n", self_path);
    sb_add(&b, "    <Include>exec:'%s' appmenu</Include>\n    <Separator/>\n", self_path);
    sb_add(&b, "    <Program label=\"Ajustes\">'%s' settings</Program>\n", self_path);
    sb_add(&b, "    <Program label=\"Energía\">'%s' power</Program>\n", self_path);
    sb_add(&b, "    <Restart label=\"Reiniciar JWM\"/>\n    <Exit label=\"Salir\" confirm=\"false\"/>\n  </RootMenu>\n\n");

    sb_add(&b, "  <Tray x=\"0\" y=\"%d\" height=\"%d\" layout=\"horizontal\" autohide=\"%s\" layer=\"above\">\n",
           cfg.tray_top ? 0 : -1, tray_h, cfg.tray_autohide ? "on" : "off");
    sb_add(&b, "    <TrayButton label=\"  SESAR  \" popup=\"Inicio\">exec:'%s' menu</TrayButton>\n", self_path);
    sb_add(&b, "    <Pager labeled=\"true\"/>\n    <TaskList maxwidth=\"%d\"/>\n    <Dock/>\n", term_w);
    sb_add(&b, "    <TrayButton label=\" SYS \" popup=\"Panel rápido\">exec:'%s' panel</TrayButton>\n", self_path);
    sb_add(&b, "    <Clock format=\"%s\"/>\n  </Tray>\n\n",
           cfg.clock24 ? (cfg.clock_date ? "%a %d %b  %H:%M" : "%H:%M") : (cfg.clock_date ? "%a %d %b  %I:%M %p" : "%I:%M %p"));

    sb_add(&b, "  <Desktops width=\"%d\" height=\"1\">\n", cfg.ndesk);
    for (int i = 1; i <= cfg.ndesk; i++) sb_add(&b, "    <Desktop name=\"%02d\"/>\n", i);
    sb_add(&b, "  </Desktops>\n\n");

    hex6(c1, T.muted); hex6(c2, T.p2); hex6(c3, T.line); hex6(c4, T.bg); hex6(c5, T.a1); hex6(c6, T.a1_lo);
    hex6(c7, T.text); hex6(c8, T.p1); hex6(c9, T.field);
    sb_add(&b, "  <WindowStyle decorations=\"flat\">\n    <Font>%s-9:bold</Font>\n    <Width>%d</Width>\n    <Height>%d</Height>\n    <Corner>0</Corner>\n", font, bw, th);
    sb_add(&b, "    <Foreground>%s</Foreground>\n    <Background>%s</Background>\n    <Outline>%s</Outline>\n    <Opacity>1.0</Opacity>\n", c1, c2, c3);
    sb_add(&b, "    <Active>\n      <Foreground>%s</Foreground>\n      <Background>%s:%s</Background>\n      <Outline>%s</Outline>\n      <Opacity>1.0</Opacity>\n    </Active>\n  </WindowStyle>\n\n", c4, c5, c6, c5);
    sb_add(&b, "  <TrayStyle>\n    <Font>%s-9:bold</Font>\n    <Background>%s:%s</Background>\n    <Foreground>%s</Foreground>\n    <Outline>%s</Outline>\n    <Opacity>0.96</Opacity>\n  </TrayStyle>\n\n", font, c8, c9, c7, c5);
    sb_add(&b, "  <TrayButtonStyle>\n    <Font>%s-9:bold</Font>\n    <Foreground>%s</Foreground>\n    <Background>%s</Background>\n    <Active>\n      <Foreground>%s</Foreground>\n      <Background>%s</Background>\n    </Active>\n  </TrayButtonStyle>\n\n", font, c5, c9, c4, c5);
    sb_add(&b, "  <TaskListStyle>\n    <Font>%s-8</Font>\n    <Foreground>%s</Foreground>\n    <Background>%s</Background>\n    <Active>\n      <Foreground>%s</Foreground>\n      <Background>%s:%s</Background>\n    </Active>\n  </TaskListStyle>\n\n", font, c1, c2, c4, c5, c6);
    char c10[8];
    hex6(c10, T.a2);
    sb_add(&b, "  <PagerStyle>\n    <Outline>%s</Outline>\n    <Foreground>%s</Foreground>\n    <Background>%s</Background>\n    <Text>%s</Text>\n    <Active>\n      <Foreground>%s</Foreground>\n      <Background>%s</Background>\n    </Active>\n  </PagerStyle>\n\n", c3, c9, c2, c1, c10, c5);
    sb_add(&b, "  <ClockStyle>\n    <Font>%s-9:bold</Font>\n    <Foreground>%s</Foreground>\n    <Background>%s</Background>\n  </ClockStyle>\n\n", font, c5, c9);
    sb_add(&b, "  <MenuStyle>\n    <Font>%s-9:bold</Font>\n    <Foreground>%s</Foreground>\n    <Background>%s</Background>\n    <Outline>%s</Outline>\n    <Opacity>0.98</Opacity>\n    <Active>\n      <Foreground>%s</Foreground>\n      <Background>%s:%s</Background>\n    </Active>\n  </MenuStyle>\n\n", font, c7, c2, c5, c4, c5, c6);
    sb_add(&b, "  <PopupStyle enabled=\"true\" delay=\"500\">\n    <Font>%s-8</Font>\n    <Outline>%s</Outline>\n    <Foreground>%s</Foreground>\n    <Background>%s</Background>\n  </PopupStyle>\n\n", font, c5, c7, c9);

    if (cfg.win_maximize)
        sb_add(&b, "  <Group>\n    <Class>*</Class>\n    <Option>maximized</Option>\n  </Group>\n\n");

    sb_add(&b, "  <FocusModel>click</FocusModel>\n  <SnapMode distance=\"%d\">border</SnapMode>\n  <MoveMode>opaque</MoveMode>\n  <ResizeMode>opaque</ResizeMode>\n"
               "  <DoubleClickSpeed>400</DoubleClickSpeed>\n  <DoubleClickDelta>%d</DoubleClickDelta>\n\n", sc(10), sc(6));

    sb_add(&b, "  <Key key=\"Up\">up</Key>\n  <Key key=\"Down\">down</Key>\n  <Key key=\"Right\">right</Key>\n  <Key key=\"Left\">left</Key>\n"
               "  <Key key=\"Return\">select</Key>\n  <Key key=\"Escape\">escape</Key>\n");
    for (int i = 0; i < NKEYS; i++) sb_key(&b, KEYS[i].mask, KEYS[i].key, KEYS[i].action);
    ckeys_load();
    for (int i = 0; i < nckeys; i++) {
        char mask[8], key[32];
        if (!parse_combo(ckeys[i].combo, mask, sizeof mask, key, sizeof key)) continue;
        char act[560];
        snprintf(act, sizeof act, "exec:%s", ckeys[i].cmd);
        sb_key(&b, mask, key, act);
    }
    sb_add(&b, "\n  <Mouse context=\"title\" button=\"1\">move</Mouse>\n  <Mouse context=\"title\" button=\"2\">move</Mouse>\n"
               "  <Mouse context=\"title\" button=\"3\">window</Mouse>\n  <Mouse context=\"title\" button=\"4\">shade</Mouse>\n"
               "  <Mouse context=\"title\" button=\"5\">shade</Mouse>\n  <Mouse context=\"title\" button=\"11\">maximize</Mouse>\n"
               "  <Mouse context=\"icon\" button=\"1\">window</Mouse>\n  <Mouse context=\"icon\" button=\"3\">window</Mouse>\n"
               "  <Mouse context=\"border\" button=\"1\">resize</Mouse>\n  <Mouse context=\"border\" button=\"2\">move</Mouse>\n"
               "  <Mouse context=\"border\" button=\"3\">window</Mouse>\n  <Mouse context=\"close\" button=\"-1\">close</Mouse>\n"
               "  <Mouse context=\"close\" button=\"2\">kill</Mouse>\n  <Mouse context=\"maximize\" button=\"-1\">maximize</Mouse>\n"
               "  <Mouse context=\"minimize\" button=\"-1\">minimize</Mouse>\n  <Mouse context=\"root\" button=\"4\">ldesktop</Mouse>\n"
               "  <Mouse context=\"root\" button=\"5\">rdesktop</Mouse>\n</JWM>\n");
    return b.p;
}

/* escribe ~/.jwmrc (con copia de seguridad de una config ajena). verbose: mensajes por stdout */
static int jwm_write(int verbose) {
    char path[1400], bak[1500];
    snprintf(path, sizeof path, "%s/.jwmrc", home_dir());
    snprintf(bak, sizeof bak, "%s.pre-sesar", path);
    char *old = read_file(path);
    if (old && !strstr(old, MARK) && !strstr(old, "gladiator-desktop") && !file_exists(bak)) {
        if (copy_file(path, bak) && verbose) printf("[ok] copia de tu configuración anterior -> %s\n", bak);
    }
    char *cfgtxt = gen_jwmrc();
    if (!write_file(path, cfgtxt)) { free(old); free(cfgtxt); if (verbose) fprintf(stderr, "no puedo escribir %s\n", path); return 0; }
    free(cfgtxt);
    int ok = 1;
    if (which("jwm", NULL, 0)) {
        char out[2048];
        if (!cmd_output("jwm -p 2>&1", out, sizeof out)) {
            ok = 0;
            if (verbose) fprintf(stderr, "[!!] jwm -p rechazó la configuración generada:\n%s\n", out);
            if (old) write_file(path, old);
        } else if (verbose && out[0]) printf("[jwm] %s\n", out);
    }
    free(old);
    if (verbose && ok) printf("[ok] configuración JWM -> %s\n", path);
    return ok;
}

static int run_setup(void) {
    jwm_write(1);
    if (!which("jwm", NULL, 0)) printf("[!!] jwm no está instalado (pkg install jwm)\n");
    if (!which(terminal_cmd(), NULL, 0)) printf("[!!] no encuentro la terminal '%s'\n", terminal_cmd());
    printf("Listo. Iniciá el escritorio con:  %s session\n", self_path);
    return 0;
}

static int run_uninstall(void) {
    char path[1400], b1[1500], b2[1500];
    snprintf(path, sizeof path, "%s/.jwmrc", home_dir());
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

/* aplica la config: regenera JWM, recursos X, y avisa a los procesos vivos */
static void apply_all(int restart_jwm) {
    cfg_save();
    jwm_write(0);
    run_xres();
    signal_instance("desktop", SIGUSR1);
    signal_instance("hud", SIGUSR1);
    if (restart_jwm && which("jwm", NULL, 0)) spawn_cmd("jwm -restart");
}

static int run_session(void) {
    if (!getenv("DISPLAY")) die("no hay DISPLAY (abrí el servidor X y exportá DISPLAY=:0)");
    if (!which("jwm", NULL, 0)) die("jwm no está instalado (pkg install jwm)");
    jwm_write(0);
    run_xres();
    XCloseDisplay(dpy);
    dpy = NULL;
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

static int run_autostart(void) {
    char p[1400];
    cfg_path("autostart", p, sizeof p);
    char *txt = read_file(p);
    if (txt) {
        char *save = NULL;
        for (char *ln = strtok_r(txt, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            while (*ln == ' ' || *ln == '\t') ln++;
            if (*ln && *ln != '#') spawn_cmd(ln);
        }
        free(txt);
    }
    char dir[1400];
    snprintf(dir, sizeof dir, "%s/.config/autostart", home_dir());
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t L = strlen(e->d_name);
            if (L < 9 || strcmp(e->d_name + L - 8, ".desktop")) continue;
            char full[1700];
            snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
            char *c = read_file(full);
            if (!c) continue;
            int skip = strstr(c, "Hidden=true") || strstr(c, "X-GNOME-Autostart-enabled=false") || strstr(c, "OnlyShowIn=");
            if (!skip) { DItem it; memset(&it, 0, sizeof it); if (parse_desktop_item(full, &it)) { AppEntry ae; memset(&ae, 0, sizeof ae); ae.exec = it.exec; ae.term = it.term; launch_app_ex(&ae); } }
            free(c);
        }
        closedir(d);
    }
    usleep(200000);
    return 0;
}


/* ------------------------------------------------------------------ AJUSTES */
enum { R_HEAD, R_CHOICE, R_TOGGLE, R_SWATCH, R_BUTTON, R_CHSTR, R_INFO, R_KEY };
enum { B_NONE, B_WALLIMG, B_NEWSHORT, B_ORDER, B_APPLYRES, B_CUSTOMRES, B_ADDKEY, B_DELKEY };
enum { P_APPEAR, P_ICONS, P_BAR, P_DISPLAY, P_KEYS, P_ABOUT, NPAGES };

typedef struct {
    int kind;
    char label[220], text2[220];
    int *val; int base; const int *vals;
    char *sval; size_t scap;
    const char *const *opts; int nopts;
    int cmd, arg;
    int y, h, nchip;
    Rect chip[24];
} Row;

typedef struct {
    Fonts *F;
    int W, H, pad, page, scroll, press, px, py, scroll0, moved, hover;
    Row rows[110]; int nrows, content_h;
    Rect tabR[NPAGES], btnApply, btnClose;
    int viewY, viewH;
    const char *th_opts[34]; char themes[33][64]; int nthemes;
    char xr_out[64], xr_cur[24], xr_modes[40][24]; const char *xr_opts[40]; int nxr, xr_sel, xr_rot, xr_currot;
    int has_xrandr, modified, msg_ticks;
    char msg[160];
} Set;

static const char *PAGE_NAMES[NPAGES] = {"APARIENCIA", "ICONOS", "BARRA", "PANTALLA", "ATAJOS", "ACERCA DE"};
static const char *SCALE_OPTS[] = {"Auto", "100%", "125%", "150%", "175%", "200%", "250%"};
static const int SCALE_VALS[] = {0, 100, 125, 150, 175, 200, 250};
static const char *BORDER_OPTS[] = {"Fino", "Normal", "Grueso (táctil)"};
static const char *TERMF_OPTS[] = {"9", "10", "11", "12", "14", "16", "18"};
static const int TERMF_VALS[] = {9, 10, 11, 12, 14, 16, 18};
static const char *SHAPE_OPTS[] = {"Hexágono", "Redondeado", "Círculo", "Biselado"};
static const char *ISIZE_OPTS[] = {"Pequeño", "Mediano", "Grande"};
static const char *TRAYSZ_OPTS[] = {"Auto", "Compacta", "Normal", "Grande (táctil)"};
static const int TRAYSZ_VALS[] = {0, 1, 2, 3};
static const char *TRAYPOS_OPTS[] = {"Abajo", "Arriba"};
static const char *NDESK_OPTS[] = {"1", "2", "3", "4", "5", "6"};
static const char *HUDPOS_OPTS[] = {"Arriba izq.", "Arriba der.", "Abajo izq.", "Abajo der."};
static const char *ROT_OPTS[] = {"Normal", "Izquierda", "Derecha", "Invertida"};
static const char *ROT_NAMES[] = {"normal", "left", "right", "inverted"};

static Row *row_add(Set *s, int kind, const char *label) {
    if (s->nrows >= (int)(sizeof s->rows / sizeof s->rows[0])) return &s->rows[s->nrows - 1];
    Row *r = &s->rows[s->nrows++];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    scopy(r->label, sizeof r->label, label);
    return r;
}
static void add_head(Set *s, const char *t) { row_add(s, R_HEAD, t); }
static void add_info(Set *s, const char *t) { row_add(s, R_INFO, t); }
static void add_toggle(Set *s, const char *t, int *v) { row_add(s, R_TOGGLE, t)->val = v; }
static void add_button(Set *s, const char *t, int cmd, int arg) { Row *r = row_add(s, R_BUTTON, t); r->cmd = cmd; r->arg = arg; }
static void add_choice(Set *s, const char *t, int *v, int base, const int *vals, const char *const *o, int n) {
    Row *r = row_add(s, R_CHOICE, t);
    r->val = v; r->base = base; r->vals = vals; r->opts = o; r->nopts = n;
}

static int row_index(const Row *r) {
    if (r->kind == R_CHSTR) {
        for (int i = 1; i < r->nopts; i++) if (!strcmp(r->sval, r->opts[i])) return i;
        return 0;
    }
    if (r->kind == R_SWATCH) return *r->val;
    if (r->vals) { for (int i = 0; i < r->nopts; i++) if (r->vals[i] == *r->val) return i; return -1; }
    return *r->val - r->base;
}

static void row_choose(Row *r, int i) {
    if (r->kind == R_CHSTR) scopy(r->sval, r->scap, i == 0 ? "" : r->opts[i]);
    else *r->val = r->vals ? r->vals[i] : i + r->base;
}

static void xr_probe(Set *s) {
    s->nxr = 0; s->has_xrandr = which("xrandr", NULL, 0);
    s->xr_out[0] = 0; s->xr_cur[0] = 0; s->xr_currot = 0;
    if (!s->has_xrandr) return;
    FILE *p = popen("xrandr -q 2>/dev/null", "r");
    if (!p) return;
    char line[512];
    int in_out = 0;
    while (fgets(line, sizeof line, p)) {
        if (line[0] != ' ') {
            in_out = 0;
            char *c = strstr(line, " connected");
            if (c && !s->xr_out[0]) {
                in_out = 1;
                size_t n = (size_t)(c - line);
                if (n >= sizeof s->xr_out) n = sizeof s->xr_out - 1;
                memcpy(s->xr_out, line, n); s->xr_out[n] = 0;
                char *g = c + 10;
                while (*g == ' ' || (!isdigit((unsigned char)*g) && *g && *g != '(')) { if (!strncmp(g, "primary", 7)) g += 7; else g++; }
                while (*g == ' ') g++;
                for (char *q = g; *q && *q != '\n'; q++) {   /* rotación actual */
                    if (!strncmp(q, " left ", 6)) { s->xr_currot = 1; break; }
                    if (!strncmp(q, " right ", 7)) { s->xr_currot = 2; break; }
                    if (!strncmp(q, " inverted ", 10)) { s->xr_currot = 3; break; }
                    if (*q == '(') break;
                }
            }
            continue;
        }
        if (!in_out) continue;
        int w, h;
        if (sscanf(line, " %dx%d", &w, &h) == 2 && s->nxr < 40) {
            char m[24];
            snprintf(m, sizeof m, "%dx%d", w, h);
            int dup = 0;
            for (int i = 0; i < s->nxr; i++) if (!strcmp(s->xr_modes[i], m)) dup = 1;
            if (!dup) { scopy(s->xr_modes[s->nxr], 24, m); s->xr_opts[s->nxr] = s->xr_modes[s->nxr]; s->nxr++; }
            if (strchr(line, '*')) scopy(s->xr_cur, sizeof s->xr_cur, m);
        }
    }
    pclose(p);
    s->xr_sel = 0;
    for (int i = 0; i < s->nxr; i++) if (!strcmp(s->xr_modes[i], s->xr_cur)) s->xr_sel = i;
    s->xr_rot = s->xr_currot;
}

static void set_build(Set *s) {
    s->nrows = 0;
    char t[220];
    switch (s->page) {
    case P_APPEAR: {
        add_head(s, "TEMA");
        add_choice(s, "Estilo", &cfg.theme, 0, NULL, THEME_NAMES, 3);
        Row *r = row_add(s, R_SWATCH, "Color de acento"); r->val = &cfg.accent; r->opts = ACCENT_NAMES; r->nopts = 7;
        add_head(s, "FONDO DE PANTALLA");
        add_choice(s, "Estilo de fondo", &cfg.wall, 0, NULL, WALL_NAMES, 5);
        snprintf(t, sizeof t, "Imagen PNG personalizada%s", cfg.wall_img[0] ? ": " : "");
        if (cfg.wall_img[0]) { size_t L = strlen(t); scopy(t + L, sizeof t - L, cfg.wall_img); }
        add_button(s, cfg.wall_img[0] ? "Cambiar imagen PNG..." : "Elegir imagen PNG...", B_WALLIMG, 0);
        if (cfg.wall_img[0]) add_info(s, cfg.wall_img);
        add_head(s, "TAMAÑO DE LA INTERFAZ");
        add_choice(s, "Escala (afecta barra, menús, ventanas y fuentes)", &cfg.scale, 0, SCALE_VALS, SCALE_OPTS, 7);
        add_head(s, "VENTANAS");
        add_choice(s, "Borde y barra de título", &cfg.win_border, 0, NULL, BORDER_OPTS, 3);
        add_toggle(s, "Abrir ventanas maximizadas (pantallas chicas)", &cfg.win_maximize);
        add_head(s, "TERMINAL");
        add_choice(s, "Tamaño de fuente (Ctrl +/- para zoom)", &cfg.term_font, 0, TERMF_VALS, TERMF_OPTS, 7);
        break;
    }
    case P_ICONS: {
        add_head(s, "ICONOS DE APLICACIONES");
        add_toggle(s, "Usar iconos del sistema (PNG)", &cfg.icons_real);
        Row *r = row_add(s, R_CHSTR, "Tema de iconos");
        r->sval = cfg.icon_theme; r->scap = sizeof cfg.icon_theme; r->opts = s->th_opts; r->nopts = s->nthemes + 1;
        add_choice(s, "Forma del icono", &cfg.icon_shape, 0, NULL, SHAPE_OPTS, 4);
        add_head(s, "ESCRITORIO");
        add_toggle(s, "Mostrar iconos en el escritorio", &cfg.desk_icons);
        add_toggle(s, "Incluir accesos integrados (Terminal, Archivos...)", &cfg.desk_defaults);
        add_toggle(s, "Un solo toque abre (táctil)", &cfg.single_click);
        add_choice(s, "Tamaño de iconos del escritorio", &cfg.icon_size, 0, NULL, ISIZE_OPTS, 3);
        add_button(s, "Nuevo acceso directo...", B_NEWSHORT, 0);
        add_button(s, "Reordenar iconos", B_ORDER, 0);
        add_info(s, "Los accesos del escritorio viven en ~/Desktop (archivos .desktop). Arrastrá un icono para moverlo; clic derecho o mantener presionado abre su menú.");
        break;
    }
    case P_BAR: {
        add_head(s, "BARRA");
        add_choice(s, "Tamaño", &cfg.tray_size, 0, TRAYSZ_VALS, TRAYSZ_OPTS, 4);
        add_choice(s, "Posición", &cfg.tray_top, 0, NULL, TRAYPOS_OPTS, 2);
        add_toggle(s, "Ocultar automáticamente", &cfg.tray_autohide);
        add_head(s, "RELOJ");
        add_toggle(s, "Formato 24 horas", &cfg.clock24);
        add_toggle(s, "Mostrar fecha", &cfg.clock_date);
        add_head(s, "ESCRITORIOS VIRTUALES");
        add_choice(s, "Cantidad", &cfg.ndesk, 1, NULL, NDESK_OPTS, 6);
        add_head(s, "MONITOR DEL SISTEMA (HUD)");
        add_toggle(s, "Mostrar HUD de CPU / RAM / batería", &cfg.hud);
        add_choice(s, "Posición del HUD", &cfg.hud_pos, 0, NULL, HUDPOS_OPTS, 4);
        break;
    }
    case P_DISPLAY: {
        int sw, sh;
        screen_size(&sw, &sh);
        snprintf(t, sizeof t, "Pantalla actual: %d x %d   ·   escala de interfaz: %.2fx", sw, sh, S);
        add_info(s, t);
        xr_probe(s);
        if (s->has_xrandr && s->nxr) {
            add_head(s, "RESOLUCIÓN");
            Row *r = row_add(s, R_CHOICE, s->xr_out[0] ? s->xr_out : "Salida");
            r->val = &s->xr_sel; r->opts = s->xr_opts; r->nopts = s->nxr;
            add_choice(s, "Giro", &s->xr_rot, 0, NULL, ROT_OPTS, 4);
            add_button(s, "Aplicar resolución y giro", B_APPLYRES, 0);
        }
        if (s->has_xrandr) add_button(s, "Resolución personalizada...", B_CUSTOMRES, 0);
        else add_info(s, "No encontré 'xrandr' (paquete x11-xserver-utils), así que acá no se puede cambiar la resolución. Si usás Termux:X11 se cambia desde sus preferencias o desde Gladiator.");
        add_info(s, "Si el cambio deja la pantalla ilegible, la resolución anterior vuelve sola a los 10 segundos. Para agrandar o achicar todo sin cambiar la resolución usá Apariencia > Escala.");
        break;
    }
    case P_KEYS: {
        add_info(s, "Atajos del sistema. Super = tecla Windows / Meta.");
        for (int i = 0; i < NKEYS; i++) { Row *r = row_add(s, R_KEY, KEYS[i].combo); scopy(r->text2, sizeof r->text2, KEYS[i].desc); r->cmd = B_NONE; }
        add_head(s, "PERSONALIZADOS (tocá uno para eliminarlo)");
        ckeys_load();
        for (int i = 0; i < nckeys; i++) { Row *r = row_add(s, R_KEY, ckeys[i].combo); scopy(r->text2, sizeof r->text2, ckeys[i].cmd); r->cmd = B_DELKEY; r->arg = i; }
        add_button(s, "Agregar atajo...", B_ADDKEY, 0);
        add_info(s, "Formato: Ctrl+Alt+B, Super+Enter, Alt+F5... Se guardan en ~/.config/sesar/keys. Apretá APLICAR para activarlos.");
        break;
    }
    default: {
        add_head(s, "SESAR DE");
        add_info(s, "Entorno de escritorio liviano en C puro (Xlib + FreeType) sobre JWM y xterm, pensado para correr en el teléfono dentro de Gladiator.");
        add_info(s, "Configuración: ~/.config/sesar/config   ·   atajos: ~/.config/sesar/keys   ·   autostart: ~/.config/sesar/autostart");
        add_info(s, "Dependencias: JWM y xterm. Opcionales (se detectan solas): xrandr para la resolución, pactl o amixer para el volumen.");
        break;
    }
    }
}

static void set_layout(Set *s) {
    Fonts *F = s->F;
    s->pad = sc(16);
    int x = s->pad, y = s->pad + sc(40) + sc(8), gap = sc(6), rh = sc(30);
    for (int i = 0; i < NPAGES; i++) {
        int tw = text_width(F->smb, PAGE_NAMES[i], S * 0.5f) + sc(20);
        if (x + tw > s->W - s->pad && x > s->pad) { x = s->pad; y += rh + gap; }
        s->tabR[i] = (Rect){x, y, tw, rh};
        x += tw + gap;
    }
    y += rh + sc(10);
    s->viewY = y;
    int fh = sc(46);
    s->btnApply = (Rect){s->pad, s->H - s->pad - fh, (s->W - s->pad * 2 - sc(10)) * 2 / 3, fh};
    s->btnClose = (Rect){s->pad + s->btnApply.w + sc(10), s->btnApply.y, s->W - s->pad * 2 - s->btnApply.w - sc(10), fh};
    s->viewH = s->btnApply.y - sc(10) - s->viewY;
    int cw = s->W - s->pad * 2 - sc(8), cy = 0;
    for (int i = 0; i < s->nrows; i++) {
        Row *r = &s->rows[i];
        r->y = cy;
        switch (r->kind) {
        case R_HEAD: r->h = sc(40); break;
        case R_TOGGLE: r->h = sc(50); break;
        case R_BUTTON: r->h = sc(52); break;
        case R_KEY: r->h = sc(36); break;
        case R_INFO: {
            char lines[12][200]; int nl;
            wrap_text(F->ui, r->label, cw, lines, 12, &nl);
            r->nchip = nl;
            r->h = nl * (F->ui->height + sc(2)) + sc(12);
            break;
        }
        default: {   /* CHOICE / CHSTR / SWATCH */
            int cx = 0, cyy = F->uib->height + sc(8), ch = sc(38), n = r->nopts;
            int rowsn = 1;
            for (int k = 0; k < n && k < 24; k++) {
                const char *lab = r->kind == R_SWATCH ? "" : r->opts[k];
                int w = r->kind == R_SWATCH ? sc(46) : imax(sc(48), text_width(F->smb, lab, S * 0.5f) + sc(26));
                if (cx + w > cw && cx > 0) { cx = 0; cyy += ch + sc(8); rowsn++; }
                r->chip[k] = (Rect){cx, cyy, w, ch};
                cx += w + sc(8);
            }
            r->nchip = imin(n, 24);
            r->h = F->uib->height + sc(8) + rowsn * (ch + sc(8)) + sc(8);
        }
        }
        cy += r->h;
    }
    s->content_h = cy;
    s->scroll = iclamp(s->scroll, 0, imax(0, s->content_h - s->viewH));
}

static void set_goto_page(Set *s, int p) {
    s->page = iclamp(p, 0, NPAGES - 1);
    s->scroll = 0; s->hover = -1;
    set_build(s);
    set_layout(s);
}

static void set_changed(App *a) {
    Set *s = a->u;
    theme_apply();
    float old = S;
    scale_apply(screen_w(), screen_h());
    if (fabsf(old - S) > 0.001f) { s->F = fonts_open(); shared_fonts_reset(); icon_cache_clear(); }
    if (s->page == P_ICONS) icon_cache_clear();
    s->modified = 1;
    set_layout(s);
    a->dirty = 1;
}

static void set_msg(Set *s, const char *m) { scopy(s->msg, sizeof s->msg, m); s->msg_ticks = 6; }

static int sh_run(const char *cmd) { char out[512]; return cmd_output(cmd, out, sizeof out); }

static void apply_resolution(Set *s, const char *mode, int rot, int custom) {
    char cmd[512], old[24];
    scopy(old, sizeof old, s->xr_cur);
    int oldrot = s->xr_currot;
    int ok;
    if (s->xr_out[0]) snprintf(cmd, sizeof cmd, "xrandr --output %s --mode %s --rotate %s 2>&1", s->xr_out, mode, ROT_NAMES[rot]);
    else snprintf(cmd, sizeof cmd, "xrandr -s %s 2>&1", mode);
    ok = sh_run(cmd);
    if (!ok && custom) { snprintf(cmd, sizeof cmd, "xrandr --fb %s 2>&1", mode); ok = sh_run(cmd); }
    if (!ok) { message_box("RESOLUCIÓN", "El servidor X rechazó ese modo."); return; }
    signal_instance("desktop", SIGUSR1);
    signal_instance("hud", SIGUSR1);
    usleep(500000);
    if (!form_dialog("CONSERVAR CAMBIOS", "¿Se ve bien? Si no confirmás, vuelve la resolución anterior.", NULL, 0, "MANTENER", 10)) {
        if (old[0] && s->xr_out[0]) {
            snprintf(cmd, sizeof cmd, "xrandr --output %s --mode %s --rotate %s 2>&1", s->xr_out, old, ROT_NAMES[oldrot]);
            sh_run(cmd);
        } else if (old[0]) { snprintf(cmd, sizeof cmd, "xrandr --fb %s 2>&1", old); sh_run(cmd); }
        signal_instance("desktop", SIGUSR1);
        signal_instance("hud", SIGUSR1);
    }
}

static void set_run_cmd(App *a, Row *r) {
    Set *s = a->u;
    switch (r->cmd) {
    case B_WALLIMG: {
        char path[500];
        scopy(path, sizeof path, cfg.wall_img);
        FField f[1] = {{"RUTA DEL ARCHIVO PNG", path, sizeof path, 0}};
        if (form_dialog("IMAGEN DE FONDO", "Escribí la ruta completa a un archivo .png (se escala para cubrir la pantalla).", f, 1, "USAR", 0)) {
            Img *t = path[0] ? png_load(path) : NULL;
            if (t) { img_free(t); scopy(cfg.wall_img, sizeof cfg.wall_img, path); cfg.wall = 4; set_build(s); set_changed(a); }
            else if (path[0]) message_box("IMAGEN", "No pude leer ese PNG (¿ruta correcta? ¿sin entrelazado?).");
        }
        break;
    }
    case B_NEWSHORT: shortcut_dialog(); signal_instance("desktop", SIGUSR1); break;
    case B_ORDER: { char p[1400]; cfg_path("iconpos", p, sizeof p); unlink(p); signal_instance("desktop", SIGUSR1); set_msg(s, "Iconos reordenados"); break; }
    case B_APPLYRES: if (s->nxr) apply_resolution(s, s->xr_modes[s->xr_sel], s->xr_rot, 0); set_goto_page(s, P_DISPLAY); break;
    case B_CUSTOMRES: {
        char m[24] = "";
        FField f[1] = {{"RESOLUCIÓN (ANCHOxALTO, ej. 1280x720)", m, sizeof m, 0}};
        int w, h;
        if (form_dialog("RESOLUCIÓN PERSONALIZADA", NULL, f, 1, "APLICAR", 0)) {
            if (sscanf(m, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240 && w <= 8192 && h <= 8192) apply_resolution(s, m, s->xr_rot, 1);
            else message_box("RESOLUCIÓN", "Formato inválido. Usá por ejemplo 1280x720.");
            set_goto_page(s, P_DISPLAY);
        }
        break;
    }
    case B_ADDKEY: {
        char combo[64] = "", cmd[500] = "";
        FField f[2] = {{"ATAJO (ej. Ctrl+Alt+B)", combo, sizeof combo, 0}, {"COMANDO", cmd, sizeof cmd, 0}};
        if (form_dialog("NUEVO ATAJO", NULL, f, 2, "AGREGAR", 0)) {
            char mask[8], key[32];
            if (!combo[0] || !cmd[0] || !parse_combo(combo, mask, sizeof mask, key, sizeof key) || !mask[0])
                message_box("ATAJO", "Necesito una combinación con al menos un modificador (Ctrl, Alt, Super o Shift) y un comando.");
            else {
                ckeys_load();
                ckeys = xrealloc(ckeys, sizeof *ckeys * (size_t)(nckeys + 1));
                scopy(ckeys[nckeys].combo, sizeof ckeys[nckeys].combo, combo);
                scopy(ckeys[nckeys].cmd, sizeof ckeys[nckeys].cmd, cmd);
                nckeys++;
                ckeys_save();
                s->modified = 1;
                set_build(s); set_layout(s);
            }
        }
        break;
    }
    case B_DELKEY: {
        char q[200];
        snprintf(q, sizeof q, "¿Eliminar el atajo %s?", r->label);
        if (form_dialog("ELIMINAR ATAJO", q, NULL, 0, "ELIMINAR", 0)) {
            ckeys_load();
            if (r->arg >= 0 && r->arg < nckeys) {
                memmove(&ckeys[r->arg], &ckeys[r->arg + 1], sizeof *ckeys * (size_t)(nckeys - r->arg - 1));
                nckeys--;
                ckeys_save();
                s->modified = 1;
                set_build(s); set_layout(s);
            }
        }
        break;
    }
    default: break;
    }
    a->dirty = 1;
}

/* resultado: fila*100 + chip+1 ; tabs 5000+i ; botones 6000/6001 */
static int set_hit(Set *s, int x, int y) {
    for (int i = 0; i < NPAGES; i++) if (in_rect(s->tabR[i], x, y)) return 5000 + i;
    if (in_rect(s->btnApply, x, y)) return 6000;
    if (in_rect(s->btnClose, x, y)) return 6001;
    if (y < s->viewY || y >= s->viewY + s->viewH) return -1;
    int cy = y - s->viewY + s->scroll, cx = x - s->pad;
    for (int i = 0; i < s->nrows; i++) {
        Row *r = &s->rows[i];
        if (cy < r->y || cy >= r->y + r->h) continue;
        if (r->kind == R_CHOICE || r->kind == R_CHSTR || r->kind == R_SWATCH) {
            for (int k = 0; k < r->nchip; k++) if (in_rect(r->chip[k], cx, cy - r->y)) return i * 100 + k + 1;
            return -1;
        }
        if (r->kind == R_TOGGLE || r->kind == R_BUTTON || (r->kind == R_KEY && r->cmd)) return i * 100;
        return -1;
    }
    return -1;
}

static void set_draw(App *a) {
    Set *s = a->u;
    Canvas *c = &a->w->cv;
    Fonts *F = s->F;
    clip_reset(c);
    fill_rect(c, 0, 0, s->W, s->H, C_BG, 1.f);
    int R = sc(17);
    draw_badge(c, (float)(s->pad + R), (float)(s->pad + R), (float)R, cfg.icon_shape, C_CYAN, 0.12f, fmaxf(1.f, S * 1.4f), 1.f, 0.9f * T.glow, (float)sc(6));
    draw_text_c(c, F->uib, s->pad + R, s->pad + R - F->uib->height / 2, "S", C_CYAN, 1.f, 0);
    draw_text(c, F->title, s->pad + R * 2 + sc(12), s->pad + sc(3), "AJUSTES", C_TEXT, 1.f, S * 2.f, 0);
    if (s->msg_ticks > 0) {
        draw_text(c, F->smb, s->W - s->pad - text_width(F->smb, s->msg, 0), s->pad + sc(10), s->msg, C_GREEN, 1.f, 0, 0);
    } else if (s->modified) {
        const char *m = "● SIN APLICAR";
        draw_text(c, F->smb, s->W - s->pad - text_width(F->smb, m, S), s->pad + sc(10), m, C_AMBER, 1.f, S, 0);
    }
    for (int i = 0; i < NPAGES; i++) panel_chip(c, F, s->tabR[i], PAGE_NAMES[i], i == s->page, s->hover == 5000 + i, C_CYAN);

    clip_set(c, 0, s->viewY, s->W, s->viewH);
    int cw = s->W - s->pad * 2 - sc(8);
    for (int i = 0; i < s->nrows; i++) {
        Row *r = &s->rows[i];
        int ry = s->viewY + r->y - s->scroll, rx = s->pad;
        if (ry + r->h < s->viewY || ry > s->viewY + s->viewH) continue;
        switch (r->kind) {
        case R_HEAD:
            draw_text(c, F->smb, rx, ry + sc(16), r->label, C_CYAN, 1.f, S * 1.5f, cw);
            fill_rect(c, rx, ry + sc(34), cw, imax(1, sc(1)), C_LINE, 0.9f);
            break;
        case R_INFO: {
            char lines[12][200]; int nl;
            wrap_text(F->ui, r->label, cw, lines, 12, &nl);
            for (int k = 0; k < nl; k++) draw_text(c, F->ui, rx, ry + sc(4) + k * (F->ui->height + sc(2)), lines[k], C_DIM, 1.f, 0, 0);
            break;
        }
        case R_TOGGLE: {
            int on = *r->val != 0;
            int hv = s->hover == i * 100;
            draw_text(c, F->uib, rx, ry + (r->h - F->uib->height) / 2, r->label, hv ? C_CYAN : C_TEXT, 1.f, 0, cw - sc(80));
            Rect sw = {rx + cw - sc(56), ry + (r->h - sc(28)) / 2, sc(56), sc(28)};
            Shape ts;
            memset(&ts, 0, sizeof ts);
            ts.cut = (float)sc(14); ts.sw = fmaxf(1.f, S * 1.2f);
            ts.top = ts.bot = on ? C_GREEN : C_BTN2; ts.fa = on ? 0.30f : 1.f;
            ts.stroke = on ? C_GREEN : C_LINE; ts.sa = 1.f;
            draw_chamfer(c, (float)sw.x, (float)sw.y, (float)sw.w, (float)sw.h, &ts);
            int kx = on ? sw.x + sw.w - sc(24) : sw.x + sc(4);
            draw_badge(c, (float)(kx + sc(10)), (float)(sw.y + sw.h / 2), (float)sc(10), 2, on ? C_GREEN : C_MUTED, 1.f, 0.f, 0.f, on ? 0.5f * T.glow : 0.f, (float)sc(5));
            fill_rect(c, rx, ry + r->h - 1, cw, 1, C_LINE, 0.35f);
            break;
        }
        case R_BUTTON: {
            Rect br = {rx, ry + sc(4), imin(cw, sc(460)), r->h - sc(8)};
            draw_button(c, F, br, r->label, BTN_NORMAL, s->hover == i * 100);
            break;
        }
        case R_KEY: {
            int w1 = imin(cw * 45 / 100, sc(230));
            int hv = r->cmd && s->hover == i * 100;
            if (hv) fill_rect(c, rx, ry, cw, r->h, C_RED, 0.12f);
            draw_text(c, F->smb, rx, ry + (r->h - F->smb->height) / 2, r->label, r->cmd ? C_MAGENTA : C_CYAN, 1.f, 0, w1 - sc(8));
            draw_text(c, F->ui, rx + w1, ry + (r->h - F->ui->height) / 2, r->text2, C_DIM, 1.f, 0, cw - w1);
            fill_rect(c, rx, ry + r->h - 1, cw, 1, C_LINE, 0.3f);
            break;
        }
        default: {
            draw_text(c, F->uib, rx, ry + sc(2), r->label, C_TEXT, 1.f, 0, cw);
            int sel = row_index(r);
            for (int k = 0; k < r->nchip; k++) {
                Rect cr = {rx + r->chip[k].x, ry + r->chip[k].y, r->chip[k].w, r->chip[k].h};
                int hv = s->hover == i * 100 + k + 1;
                if (r->kind == R_SWATCH) {
                    uint32_t col = ACCENT_A1[k];
                    Shape ss;
                    memset(&ss, 0, sizeof ss);
                    ss.cut = (float)sc(8); ss.top = ss.bot = col; ss.fa = 1.f;
                    ss.stroke = k == sel ? C_TEXT : C_LINE; ss.sa = 1.f; ss.sw = fmaxf(k == sel ? 3.f : 1.f, S * (k == sel ? 2.5f : 1.f));
                    if (k == sel) { ss.glow = col; ss.ga = 0.5f * T.glow; ss.gw = (float)sc(8); }
                    draw_chamfer(c, (float)cr.x, (float)cr.y, (float)cr.w, (float)cr.h, &ss);
                } else panel_chip(c, F, cr, r->opts[k], k == sel, hv, C_CYAN);
            }
            if (r->kind == R_SWATCH && sel >= 0 && sel < r->nopts) {
                int lw = text_width(F->smb, r->opts[sel], 0);
                draw_text(c, F->smb, rx + cw - lw, ry + sc(4), r->opts[sel], C_MUTED, 1.f, 0, 0);
            }
        }
        }
    }
    clip_reset(c);
    int maxs = imax(0, s->content_h - s->viewH);
    if (maxs > 0) {
        int tw = imax(3, sc(4)), tx = s->W - s->pad / 2 - tw / 2;
        fill_rect(c, tx, s->viewY, tw, s->viewH, C_LINE, 0.5f);
        int th = imax(sc(24), s->viewH * s->viewH / s->content_h);
        fill_rect(c, tx, s->viewY + (s->viewH - th) * s->scroll / maxs, tw, th, C_CYAN, 0.85f);
    }
    draw_button(c, F, s->btnApply, "APLICAR", BTN_PRIMARY, s->hover == 6000);
    draw_button(c, F, s->btnClose, "CERRAR", BTN_NORMAL, s->hover == 6001);
}

static void set_do_apply(App *a) {
    Set *s = a->u;
    apply_all(1);
    s->modified = 0;
    set_msg(s, "✓ Aplicado");
    a->dirty = 1;
}

static void set_activate(App *a, int h) {
    Set *s = a->u;
    if (h >= 5000 && h < 6000) { set_goto_page(s, h - 5000); a->dirty = 1; return; }
    if (h == 6000) { set_do_apply(a); return; }
    if (h == 6001) { a->quit = 1; return; }
    if (h < 0) return;
    Row *r = &s->rows[h / 100];
    int chip = h % 100;
    if (r->kind == R_TOGGLE) { *r->val = !*r->val; set_changed(a); }
    else if (r->kind == R_BUTTON || r->kind == R_KEY) set_run_cmd(a, r);
    else if (chip > 0) {
        row_choose(r, chip - 1);
        if (r->val == &cfg.theme || r->val == &cfg.accent || r->val == &cfg.scale || r->kind == R_CHSTR || r->val == &cfg.wall || r->val == &cfg.icon_shape)
            icon_cache_clear();
        if (r->val == &s->xr_sel || r->val == &s->xr_rot) a->dirty = 1; else set_changed(a);
    }
    a->dirty = 1;
}

static void set_button(App *a, int x, int y, int button, int press) {
    Set *s = a->u;
    int maxs = imax(0, s->content_h - s->viewH);
    if (press && (button == 4 || button == 5)) { s->scroll = iclamp(s->scroll + (button == 5 ? 1 : -1) * sc(80), 0, maxs); a->dirty = 1; return; }
    if (button != 1) return;
    if (press) { s->press = 1; s->px = x; s->py = y; s->scroll0 = s->scroll; s->moved = 0; return; }
    if (!s->press) return;
    s->press = 0;
    if (s->moved) return;
    set_activate(a, set_hit(s, x, y));
}

static void set_motion(App *a, int x, int y) {
    Set *s = a->u;
    if (s->press && s->py >= s->viewY && s->py < s->viewY + s->viewH) {
        if (s->moved || abs(y - s->py) > sc(8)) {
            s->moved = 1;
            s->scroll = iclamp(s->scroll0 - (y - s->py), 0, imax(0, s->content_h - s->viewH));
            a->dirty = 1;
        }
        return;
    }
    int h = set_hit(s, x, y);
    if (h != s->hover) { s->hover = h; a->dirty = 1; }
}

static void set_key(App *a, KeySym ks, int cp, unsigned st) {
    Set *s = a->u;
    (void)cp;
    int maxs = imax(0, s->content_h - s->viewH);
    a->dirty = 1;
    if (ks == XK_Escape) a->quit = 1;
    else if (ks == XK_Down) s->scroll = iclamp(s->scroll + sc(60), 0, maxs);
    else if (ks == XK_Up) s->scroll = iclamp(s->scroll - sc(60), 0, maxs);
    else if (ks == XK_Page_Down) s->scroll = iclamp(s->scroll + s->viewH, 0, maxs);
    else if (ks == XK_Page_Up) s->scroll = iclamp(s->scroll - s->viewH, 0, maxs);
    else if (ks == XK_Tab || ks == XK_ISO_Left_Tab) set_goto_page(s, (s->page + ((st & ShiftMask) || ks == XK_ISO_Left_Tab ? NPAGES - 1 : 1)) % NPAGES);
    else if ((ks == XK_Return || ks == XK_KP_Enter) && (st & ControlMask)) set_do_apply(a);
}

static void set_resize(App *a, int w, int h) {
    Set *s = a->u;
    s->W = w; s->H = h;
    set_layout(s);
}

static void set_tick(App *a) {
    Set *s = a->u;
    if (s->msg_ticks > 0) { s->msg_ticks--; a->dirty = 1; }
}

static int run_settings(int argc, char **argv) {
    if (!single_instance("settings")) return 0;
    Set *s = calloc(1, sizeof *s);
    s->F = fonts_open();
    s->hover = -1;
    s->nthemes = list_icon_themes(s->themes, 32);
    s->th_opts[0] = "Auto";
    for (int i = 0; i < s->nthemes; i++) s->th_opts[i + 1] = s->themes[i];
    int page = P_APPEAR;
    if (argc > 2) {
        static const char *keys[] = {"appearance", "icons", "bar", "display", "keys", "about"};
        for (int i = 0; i < NPAGES; i++) if (!strcmp(argv[2], keys[i])) page = i;
    }
    int sw = screen_w(), sh = screen_h();
    Win *w = win_create_managed(imin(sc(720), sw - sc(20)), imin(sc(640), sh - sc(60)), "Ajustes de SESAR", "sesar-settings");
    s->W = w->w; s->H = w->h;
    set_goto_page(s, page);
    XMapWindow(dpy, w->win);
    XSync(dpy, False);
    App a = {0};
    a.w = w; a.u = s; a.tick_ms = 500;
    a.draw = set_draw; a.key = set_key; a.button = set_button; a.motion = set_motion; a.tick = set_tick; a.resize = set_resize;
    run_app(&a);
    return 0;
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
    int sw = screen_w(), sh = screen_h();
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
    puts("sesar-shell: setup | session | menu | settings [pagina] | panel | power | desktop | hud | games |\n"
         "             fullscreen | goto N|next|prev | showdesktop | reload | autostart | xres | appmenu | files | uninstall");
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
    cfg_load();
    theme_apply();

    if (!strcmp(mode, "help") || !strcmp(mode, "--help") || !strcmp(mode, "-h")) { usage(); return 0; }
    if (!strcmp(mode, "appmenu")) return run_appmenu();
    if (!strcmp(mode, "files")) return run_files();
    if (!strcmp(mode, "uninstall")) return run_uninstall();
    if (!strcmp(mode, "reload")) { signal_instance("desktop", SIGUSR1); signal_instance("hud", SIGUSR1); return 0; }
    if (!strcmp(mode, "autostart")) return run_autostart();

    dpy = XOpenDisplay(NULL);
    int needs_x = strcmp(mode, "setup") != 0;
    if (!dpy) {
        if (needs_x) die("no puedo abrir el display (¿DISPLAY?)");
        S = cfg.scale > 0 ? cfg.scale / 100.f : 1.f;       /* setup sin X: escala de la config */
        return run_setup();
    }
    XSetErrorHandler(x_error);
    scr = DefaultScreen(dpy);
    x_init();
    scale_apply(screen_w(), screen_h());
    if (FT_Init_FreeType(&ftlib)) die("no se pudo iniciar FreeType");

    int rc = 0;
    if (!strcmp(mode, "menu")) rc = run_menu(argc, argv);
    else if (!strcmp(mode, "games")) rc = run_games();
    else if (!strcmp(mode, "power")) rc = run_power();
    else if (!strcmp(mode, "hud")) rc = run_hud();
    else if (!strcmp(mode, "desktop") || !strcmp(mode, "wallpaper")) rc = run_desktop();
    else if (!strcmp(mode, "panel")) rc = run_panel();
    else if (!strcmp(mode, "settings")) rc = run_settings(argc, argv);
    else if (!strcmp(mode, "setup")) rc = run_setup();
    else if (!strcmp(mode, "xres")) rc = run_xres();
    else if (!strcmp(mode, "session")) rc = run_session();
    else if (!strcmp(mode, "fullscreen")) toggle_fullscreen_active();
    else if (!strcmp(mode, "showdesktop")) toggle_show_desktop();
    else if (!strcmp(mode, "goto") && argc > 2) {
        if (!strcmp(argv[2], "next")) goto_desktop(current_desktop() + 1);
        else if (!strcmp(argv[2], "prev")) goto_desktop(current_desktop() - 1);
        else goto_desktop(atoi(argv[2]) - 1);
    } else { usage(); rc = 1; }
    if (dpy) XCloseDisplay(dpy);
    return rc;
}
