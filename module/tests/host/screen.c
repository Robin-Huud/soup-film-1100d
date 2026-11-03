/*
 * The film screen, drawn on a desktop: ML's drawing calls into a 720x480
 * palette buffer, with ML's own RBF fonts in the 1100D's choice (arghlf22
 * for FONT_MED on a low-resolution screen), written out as a PPM.
 *
 * The palette is an approximation of Canon's; the layout and the fonts
 * are what the camera draws.
 */
static uint8_t scr[480][720];
int host_menu_shown;

typedef struct {
    int height, width, first, last, csize;
    uint8_t wtab[256];
    uint8_t *cdata;
} host_font;
static host_font fonts[8];
static int host_nfonts = 5;
static const char *host_fontdir;

static void load_font(int id, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no font %s\n", path); exit(2); }
    int32_t h[23];
    fread(h, 4, 2, f); fseek(f, 8 + 64, SEEK_SET); fread(h, 4, 11, f);
    host_font *F = &fonts[id];
    F->csize = h[0]; F->height = h[2]; F->first = h[4]; F->last = h[5];
    F->width = F->csize * 8 / F->height;
    fseek(f, h[7], SEEK_SET); fread(F->wtab + F->first, 1, F->last - F->first + 1, f);
    F->cdata = calloc(F->last - F->first + 1, F->csize);
    fseek(f, h[8], SEEK_SET); fread(F->cdata, F->csize, F->last - F->first + 1, f);
    fclose(f);
}

void bmp_fill(uint8_t color, int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < 720 && j >= 0 && j < 480) scr[j][i] = color;
}

void fill_circle(int x, int y, int r, int color)
{
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r) bmp_fill(color, x + i, y + j, 1, 1);
}

void clrscr(void) { memset(scr, 0, sizeof(scr)); }
int  gui_menu_shown(void) { return host_menu_shown; }
void gui_stop_menu(void) { host_menu_shown = 0; }
void menu_redraw(void) {}

uint32_t font_by_name(char *file, uint32_t fg, uint32_t bg)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.RBF", host_fontdir, file);
    load_font(host_nfonts, p);
    return FONT_DYN(host_nfonts++, fg, bg);
}

static host_font *font_of(int spec) { return &fonts[(spec >> 16) & 7]; }

int bmp_string_width(int spec, const char *s)
{
    host_font *F = font_of(spec);
    int w = 0;
    for (; *s; s++) w += F->wtab[(uint8_t) *s];
    return w;
}

int bmp_printf(uint32_t spec, int x, int y, const char *fmt, ...)
{
    char s[256];
    va_list ap; va_start(ap, fmt); vsnprintf(s, sizeof(s), fmt, ap); va_end(ap);
    host_font *F = font_of(spec);
    int fg = spec & 0xFF, bg = (spec >> 8) & 0xFF;
    int len = ((spec >> 22) & 0x3F0), w = bmp_string_width(spec, s);
    if (!len) len = w;
    if ((spec & FONT_ALIGN_MASK) == FONT_ALIGN_CENTER) x -= len / 2, x += (len - w) / 2;
    if ((spec & FONT_ALIGN_MASK) == FONT_ALIGN_RIGHT)  x -= len, x += len - w;
    int x0 = x;
    for (char *c = s; *c; c++)
    {
        int ch = (uint8_t) *c, cw = F->wtab[ch];
        if (x + cw > x0 + len) break;
        if (ch < F->first || ch > F->last) continue;
        const uint8_t *d = F->cdata + (ch - F->first) * F->csize;
        bmp_fill(bg, x, y, cw, F->height);
        for (int yy = 0; yy < F->height; yy++)
            for (int xx = 0; xx < cw; xx++)
                if (d[yy * F->width / 8 + xx / 8] & (1 << (xx % 8))) bmp_fill(fg, x + xx, y + yy, 1, 1);
        x += cw;
    }
    return x - x0;
}

static void rgb_of(int c, uint8_t *p)
{
    if (c == COLOR_WHITE)  { p[0] = p[1] = p[2] = 255; return; }
    if (c == COLOR_ORANGE) { p[0] = 255; p[1] = 140; p[2] = 0; return; }
    if (c >= 38 && c <= 79) { p[0] = p[1] = p[2] = (c - 38) * 255 / 41; return; }
    p[0] = p[1] = p[2] = 0;
}

static void screen_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n720 480\n255\n");
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 720; x++) { uint8_t p[3]; rgb_of(scr[y][x], p); fwrite(p, 1, 3, f); }
    fclose(f);
}

/* Draws the screen in a few states, as stills, from the keys a user would press */
static int render_screens(const char *fontdir, const char *outdir)
{
    static const char *names[5] = { "term12", "term20", "arghlf22", "argnor28", "argnor32" };
    char p[512];
    for (int i = 0; i < 5; i++) { snprintf(p, sizeof(p), "%s/%s.rbf", fontdir, names[i]); load_font(i, p); }
    /* the screen's own fonts, when the directory has them */
    host_fontdir = fontdir;
    snprintf(p, sizeof(p), "%s/SOUP24.RBF", fontdir);
    FILE *own = fopen(p, "rb");
    if (own)
    {
        fclose(own);
        ui_body = font_by_name("SOUP24", COLOR_WHITE, COLOR_BLACK);
        ui_title = font_by_name("SOUP40", COLOR_WHITE, COLOR_BLACK);
        ui_title_y = 92;
        ui_head_y = 16;
    }

    ui_shown = 1;
    soup_enabled = 1;

    /* the screen covers Canon's menu: drawn over a marker, none of it is left */
    memset(scr, 0xEE, sizeof(scr));
    soup_ui_draw();
    int left = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 720; x++)
            left += scr[y][x] == 0xEE;
    printf("%s  screen covers Canon's menu (%d pixels left)\n", left ? "FAIL" : "ok  ", left);
    if (left) return 1;

    for (int i = 0; i < soup_nstocks; i++) if (strstr(soup_stocks[i].name, "PORTRA")) soup_film = i;
    snprintf(soup_status, sizeof(soup_status), "FLM_0004.JPG, PORTRA 400, 48.0s");
    clrscr(); soup_ui_draw();
    snprintf(p, sizeof(p), "%s/1_film.ppm", outdir); screen_save(p);

    /* down to grain, three steps up from the film's own */
    soup_keypress(MODULE_KEY_PRESS_DOWN);
    for (int i = 0; i < 3; i++) soup_keypress(MODULE_KEY_PRESS_RIGHT);
    soup_keypress(MODULE_KEY_PRESS_DOWN); soup_keypress(MODULE_KEY_PRESS_DOWN);
    soup_keypress(MODULE_KEY_PRESS_DOWN); soup_keypress(MODULE_KEY_PRESS_RIGHT);
    soup_busy = 1; soup_pct = 45;
    clrscr(); soup_ui_draw();
    snprintf(p, sizeof(p), "%s/2_settings.ppm", outdir); screen_save(p);

    /* down to Reset: SET puts the sliders back and leaves developing alone */
    soup_busy = 0;
    soup_glow = 150; soup_strength = 60;
    soup_keypress(MODULE_KEY_PRESS_DOWN);
    clrscr(); soup_ui_draw();
    snprintf(p, sizeof(p), "%s/4_reset.ppm", outdir); screen_save(p);
    int was = soup_enabled;
    soup_keypress(MODULE_KEY_PRESS_SET);
    int ok = soup_grain == -1 && soup_glow == -1 && soup_strength == 100 && soup_enabled == was && soup_is_film();
    printf("%s  reset puts Grain, Strength and Glow back to the film's\n", ok ? "ok  " : "FAIL");
    if (!ok) return 1;
    clrscr(); soup_ui_draw();
    snprintf(p, sizeof(p), "%s/5_reset_done.ppm", outdir); screen_save(p);
    soup_keypress(MODULE_KEY_PRESS_UP);
    soup_grain = 35;

    soup_busy = 0;
    soup_keypress(MODULE_KEY_PRESS_SET);
    soup_keypress(MODULE_KEY_PRESS_UP); soup_keypress(MODULE_KEY_PRESS_UP);
    clrscr(); soup_ui_draw();
    snprintf(p, sizeof(p), "%s/3_off.ppm", outdir); screen_save(p);
    printf("grain %d, glow %d, stamp %d, enabled %d\n", soup_grain, soup_glow, soup_datestamp, soup_enabled);
    return 0;
}
