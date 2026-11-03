/*
 * Soup Engine for Magic Lantern (80D, 1100D).
 *
 * A film stock applied to every shot. After the camera has written a frame,
 * the full-size JPEG that DIGIC embeds in the CR2 (or the JPEG itself, when
 * shooting JPEG only) is decoded, graded and written next to it as
 * FLM_xxxx.JPG, with the same file number.
 *
 * The look is Soup Engine's (engine/, unchanged from upstream, which was
 * written for the 5D Classic). This file is the camera side: the menu, the
 * card watcher and the develop. Upstream patched Magic Lantern's 5D Classic
 * binary and called Canon's firmware at fixed addresses; here everything
 * goes through ML's own API, so nothing in it is specific to one body.
 *
 * Only the preview path is ported. Developing from the sensor data needs
 * an 80D colour matrix and a check of the 80D's raw slices in engine/cr2.c.
 */

#include <module.h>
#include <dryos.h>
#include <menu.h>
#include <config.h>
#include <bmp.h>
#include <lens.h>
#include <propvalues.h>
#include <powersave.h>

#include "engine/se.h"
#include "engine/cr2.h"
#include "engine/stock.h"
#include "engine/look.h"
#include "engine/jpeg_dec.h"
#include "engine/jpeg_enc.h"

#define SOUP_MAX_STOCKS 32
#define SOUP_STOCK_TEXT 8192

/*
 * The CR2 header. IFD0, which locates the preview, and the preview's own
 * markers up to the scan both sit near the front of the file: the scan
 * starts at 66 KB in a 60D frame. Low-memory bodies like the 1100D prefer
 * blocks of 512 KB or less, so every allocation here stays well under that.
 */
#define SOUP_HDR     (256 * 1024)
#define SOUP_WIN     (128 * 1024)   /* read window over the compressed preview */
#define SOUP_OUTBUF  (64 * 1024)    /* encoded bytes, flushed to the card when full */

/*
 * Decoder and encoder working memory, per output column: one MCU row per
 * component, an RGB row and the encoder's eight-row strip. Measured peaks
 * were 51-74 bytes a column (133 KB at 2592 wide, 444 KB at 6000), so 96
 * leaves room; the look has its own budget on top.
 */
#define SOUP_SCRATCH(width) ((size_t) (width) * 96 + 64 * 1024)

#define SOUP_QUALITY 88

static CONFIG_INT("soup.enabled", soup_enabled, 0);
static CONFIG_INT("soup.film", soup_film, 0);
static CONFIG_INT("soup.grain", soup_grain, -1);        /* 0..100, -1 = the stock's */
static CONFIG_INT("soup.glow", soup_glow, -1);          /* 0..200, -1 = the stock's */
static CONFIG_INT("soup.strength", soup_strength, 100); /* colour, 0..100 %         */
static CONFIG_INT("soup.stamp", soup_datestamp, 0);     /* 0 off, 1 date, 2 + time  */

static se_stock    *soup_stocks;
static int          soup_nstocks;
static const char  *soup_film_names[SOUP_MAX_STOCKS];

static volatile int soup_force;     /* "develop last shot" was selected */
static volatile int soup_busy;
static volatile int soup_pct;
static char         soup_status[64] = "Idle";

/* ------------------------------------------------------------------ log */
/*
 * The engine reports through se_log. Lines are held in RAM and written to
 * ML/LOGS/SOUP.LOG after each develop, so the card is never asked to append
 * a log line while the decoder is streaming the preview off it.
 */
static char soup_logbuf[4096];
static int  soup_loglen;

static void soup_log(void *ctx, const char *msg)
{
    int n = strlen(msg);
    if (soup_loglen + n + 2 > (int)sizeof(soup_logbuf)) return;
    memcpy(soup_logbuf + soup_loglen, msg, n);
    soup_loglen += n;
    soup_logbuf[soup_loglen++] = '\n';
}

static void soup_note(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    soup_log(0, line);
}

static void soup_log_flush(void)
{
    if (!soup_loglen) return;
    FILE *f = FIO_CreateFileOrAppend("ML/LOGS/SOUP.LOG");
    if (f)
    {
        FIO_WriteFile(f, soup_logbuf, soup_loglen);
        FIO_CloseFile(f);
    }
    soup_loglen = 0;
}

/* --------------------------------------------------------------- stocks */

static int soup_is_bw(const se_stock *s)
{
    return s->sat_set && s->sat <= 0.0f;
}

static void soup_load_stocks(void)
{
    soup_stocks = malloc(sizeof(se_stock) * SOUP_MAX_STOCKS);
    if (!soup_stocks) return;

    int size = 0;
    char *index = (char *) read_entire_file("ML/FILM/INDEX.TXT", &size);
    char *body = fio_malloc(SOUP_STOCK_TEXT);

    if (index && body)
    {
        char *line = index;
        for (int i = 0; i <= size && soup_nstocks < SOUP_MAX_STOCKS; i++)
        {
            if (i < size && index[i] != '\n' && index[i] != '\r') continue;
            index[i] = '\0';
            char *name = line;
            line = index + i + 1;
            if (!name[0]) continue;

            char path[FIO_MAX_PATH_LENGTH];
            snprintf(path, sizeof(path), "ML/FILM/%s", name);
            int n = read_file(path, body, SOUP_STOCK_TEXT);
            if (n <= 0) { soup_note("stock %s unreadable", name); continue; }

            se_stock *s = &soup_stocks[soup_nstocks];
            se_stock_init(s);
            if (se_stock_parse(body, n, s) != SE_OK)
            {
                soup_note("stock %s unparsable", name);
                continue;
            }
            if (!s->name[0])
                snprintf(s->name, sizeof(s->name), "%s", name);
            soup_nstocks++;
        }
    }

    if (index) free(index);
    if (body) fio_free(body);

    if (soup_nstocks == 0)
    {
        /* no index: offer a straight render so the menu is never empty */
        se_stock_init(&soup_stocks[0]);
        snprintf(soup_stocks[0].name, sizeof(soup_stocks[0].name), "Standard");
        soup_nstocks = 1;
    }

    /*
     * Black and white stocks (SAT 0) are listed as such, after the colour
     * ones. The stock files are shared with SoupLab, so their names are
     * left alone; only the list here changes.
     */
    se_stock *soup_order = malloc(sizeof(se_stock) * soup_nstocks);
    int ncolour = 0;
    for (int i = 0; soup_order && i < soup_nstocks; i++)
        if (!soup_is_bw(&soup_stocks[i])) ncolour++;
    for (int i = 0, c = 0, b = ncolour; soup_order && i < soup_nstocks; i++)
    {
        se_stock *s = &soup_stocks[i];
        if (soup_is_bw(s))
        {
            se_stock *d = &soup_order[b++];
            *d = *s;
            snprintf(d->name, sizeof(d->name), "BW %s", s->name);
        }
        else
            soup_order[c++] = *s;
    }
    if (soup_order)
    {
        memcpy(soup_stocks, soup_order, sizeof(se_stock) * soup_nstocks);
        free(soup_order);
    }

    for (int i = 0; i < soup_nstocks; i++)
        soup_film_names[i] = soup_stocks[i].name;
    if (soup_film >= soup_nstocks) soup_film = 0;
}

/* -------------------------------------------------------------- reading */
/*
 * The decoder asks for the compressed preview a piece at a time, at
 * increasing offsets. A window keeps that to one card read per 256 KB.
 */
typedef struct {
    FILE    *f;
    uint32_t base;       /* where the preview starts in the file */
    uint8_t *win;
    uint32_t win_off;
    uint32_t win_len;
} soup_rd;

static int soup_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    soup_rd *r = ctx;
    uint8_t *d = dst;

    off += r->base;
    while (n)
    {
        if (off < r->win_off || off >= r->win_off + r->win_len)
        {
            uint32_t start = off & ~511u;
            FIO_SeekSkipFile(r->f, start, SEEK_SET);
            int got = FIO_ReadFile(r->f, r->win, SOUP_WIN);
            if (got <= 0) return -1;
            r->win_off = start;
            r->win_len = got;
            if (off >= start + got) return -1;
        }
        uint32_t at = off - r->win_off;
        uint32_t take = MIN(n, r->win_len - at);
        memcpy(d, r->win + at, take);
        d += take; off += take; n -= take;
    }
    return 0;
}

/* -------------------------------------------------------------- writing */

typedef struct {
    FILE    *f;
    uint8_t *buf;
    int      len;
    int      bytes;
    int      err;
} soup_out;

static int soup_flush(soup_out *o)
{
    if (!o->len) return 0;
    if (FIO_WriteFile(o->f, o->buf, o->len) != o->len)
    {
        o->err = 1;
        return -1;
    }
    o->bytes += o->len;
    o->len = 0;
    return 0;
}

static int soup_write(void *ctx, const uint8_t *data, size_t len)
{
    soup_out *o = ctx;
    while (len)
    {
        int take = MIN((int)len, SOUP_OUTBUF - o->len);
        memcpy(o->buf + o->len, data, take);
        o->len += take; data += take; len -= take;
        if (o->len == SOUP_OUTBUF && soup_flush(o)) return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------- exif */
/*
 * A JPEG straight from the camera carries its EXIF in APP1, as a small TIFF.
 * The engine only reads a CR2's, so the camera, date and exposure are taken
 * from here instead and copied onto the film version.
 */
typedef struct {
    const uint8_t *b;
    uint32_t       len;
    int            be;       /* Motorola byte order */
} soup_tiff;

static uint32_t soup_tu(const soup_tiff *t, uint32_t off, int n)
{
    if (off + n > t->len) return 0;
    const uint8_t *p = t->b + off;
    if (n == 2) return t->be ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]);
    return t->be ? ((uint32_t) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3])
                 : ((uint32_t) p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]);
}

/* an entry's value: inline when it fits in four bytes, else at the offset */
static uint32_t soup_tval(const soup_tiff *t, uint32_t e, int size)
{
    uint32_t count = soup_tu(t, e + 4, 4);
    return count * size <= 4 ? e + 8 : soup_tu(t, e + 8, 4);
}

static void soup_tstr(const soup_tiff *t, uint32_t e, char *dst, int cap)
{
    uint32_t at = soup_tval(t, e, 1), n = soup_tu(t, e + 4, 4);
    int i = 0;
    for (; i < cap - 1 && i < (int) n && at + i < t->len && t->b[at + i]; i++)
        dst[i] = t->b[at + i];
    dst[i] = 0;
}

static float soup_trational(const soup_tiff *t, uint32_t e)
{
    uint32_t at = soup_tval(t, e, 8);
    uint32_t den = soup_tu(t, at + 4, 4);
    return den ? (float) soup_tu(t, at, 4) / den : 0;
}

static void soup_tifd(const soup_tiff *t, uint32_t off, se_cr2_info *out, int depth)
{
    int n = soup_tu(t, off, 2);
    if (off + 2 + n * 12 > t->len || n > 256) return;
    for (int i = 0; i < n; i++)
    {
        uint32_t e = off + 2 + i * 12;
        switch (soup_tu(t, e, 2))
        {
        case 0x010f: soup_tstr(t, e, out->make, sizeof(out->make)); break;
        case 0x0110: soup_tstr(t, e, out->model, sizeof(out->model)); break;
        case 0x0132: if (!out->datetime[0]) soup_tstr(t, e, out->datetime, sizeof(out->datetime)); break;
        case 0x9003: soup_tstr(t, e, out->datetime, sizeof(out->datetime)); break;
        case 0x829a: out->exposure_time = soup_trational(t, e); break;
        case 0x829d: out->aperture = soup_trational(t, e); break;
        case 0x8827: out->iso = soup_tu(t, soup_tval(t, e, 2), 2); break;
        case 0x8769: if (!depth) soup_tifd(t, soup_tu(t, e + 8, 4), out, 1); break;
        default: break;
        }
    }
}

/* Fills what it finds; leaves the rest as it was. */
static void soup_jpeg_exif(const uint8_t *b, int len, se_cr2_info *out)
{
    int at = 2;                              /* past SOI */
    while (at + 4 <= len && b[at] == 0xFF)
    {
        int marker = b[at + 1];
        int seg = b[at + 2] << 8 | b[at + 3];
        if (marker == 0xDA || seg < 2) return;   /* the scan: no EXIF before it */
        if (marker == 0xE1 && seg >= 16 && at + 2 + seg <= len && !memcmp(b + at + 4, "Exif\0\0", 6))
        {
            soup_tiff t = { b + at + 10, seg - 8, b[at + 10] == 'M' };
            if (soup_tu(&t, 2, 2) == 42)
                soup_tifd(&t, soup_tu(&t, 4, 4), out, 0);
            return;
        }
        at += 2 + seg;
    }
}

/* ---------------------------------------------------------- the develop */

/* ------------------------------------------------------------ settings */
/*
 * The film screen's sliders, applied over the stock. Strength fades the
 * stock's colour (curves, saturation, per-hue moves, split tone) toward a
 * straight render; grain and glow replace the stock's own amounts, so a
 * slider means the same thing on every stock.
 */
static void soup_adjust(se_look *lk, int strength, int grain, int glow)
{
    int k = strength < 0 ? 0 : strength > 100 ? 100 : strength;
    if (k < 100)
    {
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < 256; i++)
                lk->map[c][i] = i + (lk->map[c][i] - i) * k / 100;
        lk->sat = 256 + (lk->sat - 256) * k / 100;
        for (int i = 0; i < 6; i++)
        {
            lk->hue_sat[i] = 256 + (lk->hue_sat[i] - 256) * k / 100;
            lk->hue_lum[i] = 256 + (lk->hue_lum[i] - 256) * k / 100;
            lk->hue_rot[i] = lk->hue_rot[i] * k / 100;
        }
        for (int c = 0; c < 3; c++)
        {
            lk->sh[c] = lk->sh[c] * k / 100;
            lk->hi[c] = lk->hi[c] * k / 100;
        }
    }
    if (grain >= 0) lk->grain = grain > 100 ? 100 : grain;
    if (glow >= 0)
    {
        lk->hal_amount = glow > 200 ? 200 : glow;
        /* a stock without halation has no colour or reach for it */
        if (glow > 0 && !lk->hal_tint[0] && !lk->hal_tint[1] && !lk->hal_tint[2])
        {
            lk->hal_tint[0] = 255; lk->hal_tint[1] = 64; lk->hal_tint[2] = 24;
            lk->hal_thresh = 200;
            if (lk->hal_radius < 2) lk->hal_radius = 2;
        }
    }
}

/* ----------------------------------------------------------- date stamp */
/*
 * The orange seven-segment date a film camera's back exposed into the
 * corner, as SoupLab draws it (DateStamp.swift): a pointed bar per segment,
 * a slight forward lean, a soft orange glow and a hot core. It goes on
 * before the look, as light on the film would, so the stock grades and
 * grains it with the rest of the frame.
 *
 * The glyphs are filled once into a coverage mask over the corner they
 * occupy, in integers at 4x4 samples a pixel, and blurred for the glow;
 * after that each row through the corner is a blend.
 */
typedef struct {
    int      x0, y0, w, h;   /* the stamped corner, in output pixels */
    uint8_t *core;           /* glyph coverage, 0..255               */
    uint8_t *glow;           /* the same, blurred                    */
    uint8_t *row;            /* one output row, while it is stamped  */
} soup_stamp;

#define SOUP_SUB 4           /* samples per pixel, each way */

/* A convex polygon in 1/SOUP_SUB pixels, filled into the mask */
static void soup_fill(soup_stamp *st, const int *px, const int *py, int n)
{
    int minx = px[0], maxx = px[0], miny = py[0], maxy = py[0];
    for (int i = 1; i < n; i++)
    {
        if (px[i] < minx) minx = px[i];
        if (px[i] > maxx) maxx = px[i];
        if (py[i] < miny) miny = py[i];
        if (py[i] > maxy) maxy = py[i];
    }
    int x0 = minx / SOUP_SUB, x1 = maxx / SOUP_SUB, y0 = miny / SOUP_SUB, y1 = maxy / SOUP_SUB;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= st->w) x1 = st->w - 1;
    if (y1 >= st->h) y1 = st->h - 1;

    /* corners relative to the box keep the cross products inside 32 bits */
    int qx[8], qy[8];
    for (int i = 0; i < n; i++) { qx[i] = px[i] - minx; qy[i] = py[i] - miny; }

    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
        {
            int hits = 0;
            for (int sy = 0; sy < SOUP_SUB; sy++)
                for (int sx = 0; sx < SOUP_SUB; sx++)
                {
                    /* sample centres, in doubled sub-pixel units */
                    int X = 2 * (x * SOUP_SUB + sx - minx) + 1;
                    int Y = 2 * (y * SOUP_SUB + sy - miny) + 1;
                    int pos = 0, neg = 0;
                    for (int i = 0; i < n; i++)
                    {
                        int j = i + 1 == n ? 0 : i + 1;
                        int c = (2 * qx[j] - 2 * qx[i]) * (Y - 2 * qy[i])
                              - (2 * qy[j] - 2 * qy[i]) * (X - 2 * qx[i]);
                        if (c > 0) pos = 1;
                        if (c < 0) neg = 1;
                    }
                    if (!(pos && neg)) hits++;
                }
            if (!hits) continue;
            int v = st->core[y * st->w + x] + hits * 255 / (SOUP_SUB * SOUP_SUB);
            st->core[y * st->w + x] = v > 255 ? 255 : v;
        }
}

/* Glyph space: y up from the baseline, in 1/64 of the digit height */
typedef struct {
    soup_stamp *st;
    int h;                   /* digit height, in sub-pixels            */
    int ox, oy;              /* glyph origin, in sub-pixels, y down    */
    int pen;                 /* x of the current glyph, in 1/64 h      */
} soup_pen;

static void soup_poly(soup_pen *p, const int *gx, const int *gy, int n)
{
    int px[8], py[8];
    for (int i = 0; i < n; i++)
    {
        /* the lean: x moves right by a tenth of the height above the line */
        px[i] = p->ox + ((p->pen + gx[i]) * p->h + gy[i] * p->h / 10) / 64;
        py[i] = p->oy - gy[i] * p->h / 64;
    }
    soup_fill(p->st, px, py, n);
}

/* A pointed bar from a to b, short of each end so segments don't touch */
static void soup_bar(soup_pen *p, int ax, int ay, int bx, int by)
{
    const int t = 10;                    /* 0.15 h               */
    int gap = 2, k = t / 2;              /* 0.18 t, half the bar */
    if (ay == by)
    {
        int x0 = ax + gap, x1 = bx - gap, y = ay;
        int gx[6] = { x0, x0 + k, x1 - k, x1, x1 - k, x0 + k };
        int gy[6] = { y, y + k, y + k, y, y - k, y - k };
        soup_poly(p, gx, gy, 6);
    }
    else
    {
        int y0 = ay + gap, y1 = by - gap, x = ax;
        int gx[6] = { x, x + k, x + k, x, x - k, x - k };
        int gy[6] = { y0, y0 + k, y1 - k, y1, y1 - k, y0 + k };
        soup_poly(p, gx, gy, 6);
    }
}

static const char *const soup_segs[10] = {
    "abcdef", "bc", "abged", "abgcd", "fgbc", "afgcd", "afgedc", "abc", "abcdefg", "abcdfg",
};

/* advance after a character, in 1/64 h (digit 0.5 h + 0.2 h) */
static int soup_advance(char c)
{
    return c == ' ' ? 23 : c == '\'' ? 17 : c == ':' ? 19 : 45;
}

static void soup_glyph(soup_pen *p, char c)
{
    const int t = 10, w = 32, h = 64;
    const int l = t / 2, r = w - t / 2, top = h - t / 2, mid = h / 2, bot = t / 2;
    if (c >= '0' && c <= '9')
    {
        for (const char *s = soup_segs[c - '0']; *s; s++)
            switch (*s)
            {
                case 'a': soup_bar(p, l, top, r, top); break;
                case 'b': soup_bar(p, r, mid, r, top); break;
                case 'c': soup_bar(p, r, bot, r, mid); break;
                case 'd': soup_bar(p, l, bot, r, bot); break;
                case 'e': soup_bar(p, l, bot, l, mid); break;
                case 'f': soup_bar(p, l, mid, l, top); break;
                default:  soup_bar(p, l, mid, r, mid); break;
            }
    }
    else if (c == '\'')
        soup_bar(p, t, 37, t, h);
    else if (c == ':')
    {
        for (int i = 0; i < 2; i++)
        {
            int y = i ? 41 : 15, x = 4, s = 11;
            int gx[4] = { x, x + s, x + s, x }, gy[4] = { y, y, y + s, y + s };
            soup_poly(p, gx, gy, 4);
        }
    }
    p->pen += soup_advance(c);
}

/* A box blur of radius r along rows (dx = 1) or columns, in place */
static void soup_box(uint8_t *m, int w, int h, int r, int horiz, uint8_t *line)
{
    int n = horiz ? w : h, lines = horiz ? h : w;
    int step = horiz ? 1 : w, next = horiz ? w : 1;
    int win = 2 * r + 1;
    for (int L = 0; L < lines; L++)
    {
        uint8_t *p = m + L * next;
        for (int i = 0; i < n; i++) line[i] = p[i * step];
        int sum = 0;
        for (int i = -r; i <= r; i++) sum += (i >= 0 && i < n) ? line[i] : 0;
        for (int i = 0; i < n; i++)
        {
            p[i * step] = sum / win;
            int out = i - r, in = i + r + 1;
            if (out >= 0) sum -= line[out];
            if (in < n) sum += line[in];
        }
    }
}

/* "YYYY:MM:DD HH:MM:SS" into the stamp's text; 0 when there is no date */
static int soup_stamp_text(const char *dt, int with_time, char *out, int cap)
{
    int Y, M, D, hh, mm;
    if (!dt || strlen(dt) < 16) return 0;
    for (int i = 0; i < 16; i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && (dt[i] < '0' || dt[i] > '9')) return 0;
    Y  = (dt[2] - '0') * 10 + dt[3] - '0';
    M  = (dt[5] - '0') * 10 + dt[6] - '0';
    D  = (dt[8] - '0') * 10 + dt[9] - '0';
    hh = (dt[11] - '0') * 10 + dt[12] - '0';
    mm = (dt[14] - '0') * 10 + dt[15] - '0';
    if (M < 1 || M > 12 || D < 1 || D > 31) return 0;
    if (with_time)
        snprintf(out, cap, "'%02d %2d %2d  %2d:%02d", Y, M, D, hh, mm);
    else
        snprintf(out, cap, "'%02d %2d %2d", Y, M, D);
    return 1;
}

static void soup_stamp_free(soup_stamp *st)
{
    if (st->core) fio_free(st->core);
    if (st->glow) fio_free(st->glow);
    if (st->row) fio_free(st->row);
    memset(st, 0, sizeof(*st));
}

/* Lays the stamp out for a W x H frame; leaves it empty if it can't */
static int soup_stamp_begin(soup_stamp *st, const char *text, int W, int H)
{
    memset(st, 0, sizeof(*st));
    int sh = W < H ? W : H;
    int h = sh * 32 / 1000;                 /* digit height, 0.032 of the short side */
    if (h < 8) return 0;
    int blur = h * 45 / 100;                /* the glow's reach */
    int r = blur / 3 > 1 ? blur / 3 : 1;    /* three box passes approach a gaussian */
    int pad = 3 * r + 2;

    int adv = 0;
    for (const char *c = text; *c; c++) adv += soup_advance(*c);
    int tw = (adv - 13) * h / 64 + h / 10;  /* less the last gap, plus the lean */

    int gx = W - sh * 6 / 100 - tw;         /* the text's left edge */
    int gy = H - sh * 5 / 100;              /* its baseline         */
    st->x0 = gx - pad;
    st->y0 = gy - h - pad;
    st->w  = tw + 2 * pad;
    st->h  = h + 2 * pad;
    if (st->x0 < 0 || st->y0 < 0 || st->x0 + st->w > W || st->y0 + st->h > H) return 0;

    st->core = fio_malloc(st->w * st->h);
    st->glow = fio_malloc(st->w * st->h);
    st->row  = fio_malloc(W * 3 + st->h);   /* also the blur's line */
    if (!st->core || !st->glow || !st->row) { soup_stamp_free(st); return 0; }
    memset(st->core, 0, st->w * st->h);

    soup_pen p = { st, h * SOUP_SUB, pad * SOUP_SUB, (h + pad) * SOUP_SUB, 0 };
    for (const char *c = text; *c; c++) soup_glyph(&p, *c);

    memcpy(st->glow, st->core, st->w * st->h);
    for (int i = 0; i < 3; i++)
    {
        soup_box(st->glow, st->w, st->h, r, 1, st->row);
        soup_box(st->glow, st->w, st->h, r, 0, st->row);
    }
    return 1;
}

/* Exposes the stamp into one output row (y), in place */
static void soup_stamp_row(const soup_stamp *st, uint8_t *row, int y)
{
    /* hotter than SoupLab's, because the stock grades it afterwards */
    static const int glow[3] = { 255, 80, 8 }, fill[3] = { 255, 112, 20 }, hot[3] = { 255, 170, 90 };
    const uint8_t *cm = st->core + (y - st->y0) * st->w;
    const uint8_t *gm = st->glow + (y - st->y0) * st->w;
    uint8_t *px = row + st->x0 * 3;
    for (int x = 0; x < st->w; x++, px += 3)
    {
        int g = gm[x] * 2 > 255 ? 255 : gm[x] * 2, a = cm[x] * 240 >> 8, s = cm[x] * 160 >> 8;
        if (!g && !a) continue;
        for (int c = 0; c < 3; c++)
        {
            int v = px[c];
            v += (glow[c] - v) * g / 255;
            v += (fill[c] - v) * a / 255;
            v += hot[c] * (255 - v) / 255 * s / 255;   /* screen */
            px[c] = v > 255 ? 255 : v;
        }
    }
}

typedef struct {
    se_jpeg     *enc;
    se_look_run *run;
    int          ow, oh;
    int          rows;
    int          in_rows;        /* rows received from the decoder */
    soup_stamp   stamp;          /* empty when there is none       */
    int          err;
} soup_look;

static int soup_emit(void *ctx, const uint8_t *row, int width)
{
    soup_look *L = ctx;
    if (se_jpeg_write_rows(L->enc, row, 1) != SE_OK) { L->err = 1; return -1; }
    L->rows++;
    soup_pct = L->rows * 100 / L->oh;
    return 0;
}

static int soup_rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    soup_look *L = ctx;
    /* the look and encoder were opened for ow columns */
    if (width != L->ow) { L->err = 1; return -1; }

    /* rows through the stamp go one at a time, through a copy: the
     * decoder's rows are its own */
    const soup_stamp *st = &L->stamp;
    int y = L->in_rows;
    L->in_rows += nrows;
    if (!st->core || y + nrows <= st->y0 || y >= st->y0 + st->h)
    {
        if (se_look_rows(L->run, rgb, nrows, soup_emit, L) != SE_OK) { L->err = 1; return -1; }
        return 0;
    }
    for (int i = 0; i < nrows; i++, y++)
    {
        const uint8_t *src = rgb + (size_t) i * width * 3;
        if (y >= st->y0 && y < st->y0 + st->h)
        {
            memcpy(st->row, src, (size_t) width * 3);
            soup_stamp_row(st, st->row, y);
            src = st->row;
        }
        if (se_look_rows(L->run, src, 1, soup_emit, L) != SE_OK) { L->err = 1; return -1; }
    }
    return 0;
}

static se_err soup_develop(const char *src, const char *dst, const se_stock *stock, int full)
{
    se_err rc = SE_OK;
    uint8_t *hdr = 0, *win = 0, *outbuf = 0, *scratch = 0, *lookmem = 0;
    soup_out out = { 0 };
    soup_rd rd = { 0 };
    soup_look L = { 0 };
    se_cr2_info info;
    se_jdec_info ji;
    se_jpeg_exif ex;
    se_arena arena, lookar;
    se_look lk;
    uint32_t poff = 0, plen = 0, fsize = 0;
    int pw, ph, ow, oh;
    int scale = full ? 1 : 2;

    memset(&info, 0, sizeof(info));
    if (FIO_GetFileSize(src, &fsize) != 0) return SE_E_IO;

    hdr = fio_malloc(SOUP_HDR);
    win = fio_malloc(SOUP_WIN);
    outbuf = malloc(SOUP_OUTBUF);
    if (!hdr || !win || !outbuf) { rc = SE_E_NOMEM; goto done; }

    int got = read_file(src, hdr, SOUP_HDR);
    if (got <= 0) { rc = SE_E_IO; goto done; }

    /* a CR2 carries the preview inside it; a JPEG is the preview */
    if (got > 4 && hdr[0] == 0xFF && hdr[1] == 0xD8)
    {
        poff = 0;
        plen = fsize;
        soup_jpeg_exif(hdr, got, &info);
    }
    else
    {
        rc = se_cr2_find_preview(hdr, got, &poff, &plen, &pw, &ph);
        if (rc != SE_OK) { soup_note("no preview in %s (rc=%d)", src, rc); goto done; }
        if (se_cr2_parse(hdr, got, &info) != SE_OK)
            memset(&info, 0, sizeof(info));
        if (poff >= (uint32_t) got) { rc = SE_E_UNSUPPORTED; goto done; }
    }

    uint8_t *pv = hdr + poff;
    rc = se_jpeg_dec_info(pv, got - poff, &ji);
    if (rc != SE_OK) goto done;
    se_jpeg_scaled_size(ji.width, ji.height, scale, &ow, &oh);

    /* grain and halation are in output pixels, so they follow the scale */
    se_look_from_stock(stock, scale, &lk);
    soup_adjust(&lk, soup_strength, soup_grain, soup_glow);
    size_t lookneed = se_look_fit(&lk, ow, SE_LOOK_BUDGET);
    scratch = malloc(SOUP_SCRATCH(ow));
    lookmem = malloc(lookneed + 64);
    if (!scratch || !lookmem) { rc = SE_E_NOMEM; goto done; }
    se_arena_init(&arena, scratch, SOUP_SCRATCH(ow));
    se_arena_init(&lookar, lookmem, lookneed + 64);

    rc = se_look_begin(&L.run, &lk, ow, &lookar);
    if (rc != SE_OK) goto done;
    L.ow = ow;
    L.oh = oh;
    L.enc = SE_NEW(&arena, se_jpeg, 1);
    uint8_t *strip = SE_NEW(&arena, uint8_t, (size_t) 8 * ow * 3);
    if (!L.enc || !strip) { rc = SE_E_NOMEM; goto done; }

    rd.f = FIO_OpenFile(src, O_RDONLY | O_SYNC);
    if (!rd.f) { rc = SE_E_IO; goto done; }
    rd.base = poff;
    rd.win = win;

    FIO_RemoveFile(dst);
    out.f = FIO_CreateFile(dst);
    if (!out.f) { rc = SE_E_IO; goto done; }
    out.buf = outbuf;

    memset(&ex, 0, sizeof(ex));
    ex.make          = info.make[0] ? info.make : "Canon";
    ex.model         = info.model[0] ? info.model : camera_model;
    ex.software      = stock->name;
    ex.datetime      = info.datetime[0] ? info.datetime : 0;
    ex.exposure_time = info.exposure_time;
    ex.aperture      = info.aperture;
    ex.iso           = info.iso;

    char stamp[24];
    if (soup_datestamp && soup_stamp_text(info.datetime, soup_datestamp == 2, stamp, sizeof(stamp))
        && !soup_stamp_begin(&L.stamp, stamp, ow, oh))
        soup_note("no room for the date stamp");

    rc = se_jpeg_begin(L.enc, ow, oh, SOUP_QUALITY, strip, &ex, soup_write, &out);
    if (rc != SE_OK) goto done;

    rc = se_jpeg_decode_stream_scaled(pv, got - poff, plen, soup_read, &rd,
                                      scale, &arena, soup_rows, &L, &ji);
    /* the halation delay line still holds `radius` rows */
    if (rc == SE_OK && !L.err) rc = se_look_end(L.run, soup_emit, &L);
    if (rc == SE_OK && !L.err) rc = se_jpeg_end(L.enc);
    if (rc == SE_OK && soup_flush(&out)) rc = SE_E_IO;
    if (rc == SE_OK && (L.err || out.err)) rc = SE_E_IO;
    if (rc == SE_OK)
        soup_note("%s: %dx%d, %d KB, scratch peak %d KB, look %d KB", dst, ow, oh, out.bytes >> 10,
                  (int) (arena.peak >> 10), (int) (lookneed >> 10));

done:
    if (rd.f) FIO_CloseFile(rd.f);
    if (out.f) FIO_CloseFile(out.f);
    if (rc != SE_OK && out.f) FIO_RemoveFile(dst);
    if (hdr) fio_free(hdr);
    if (win) fio_free(win);
    if (outbuf) free(outbuf);
    if (scratch) free(scratch);
    if (lookmem) free(lookmem);
    soup_stamp_free(&L.stamp);
    return rc;
}

/* -------------------------------------------------------------- watcher */
/*
 * Canon names frames XXX_nnnn.CR2 / .JPG (IMG_, or _MG_ in Adobe RGB).
 * The film frame takes the same number, so it sorts beside its original
 * and stays a valid 8.3 DCF name the camera can play back.
 */
static int soup_frame_number(const char *name)
{
    if (strlen(name) != 12 || name[8] != '.') return -1;
    if (strncmp(name, "FLM_", 4) == 0) return -1;
    if (strcmp(name + 9, "CR2") && strcmp(name + 9, "JPG")) return -1;
    int n = 0;
    for (int i = 4; i < 8; i++)
    {
        if (name[i] < '0' || name[i] > '9') return -1;
        n = n * 10 + name[i] - '0';
    }
    return n;
}

/* FLM_nnnn.TMP: a develop that was cut off, by power-off or a card pulled */
static int soup_stale_number(const char *name)
{
    if (strlen(name) != 12 || strncmp(name, "FLM_", 4) || strcmp(name + 8, ".TMP")) return -1;
    int n = 0;
    for (int i = 4; i < 8; i++)
    {
        if (name[i] < '0' || name[i] > '9') return -1;
        n = n * 10 + name[i] - '0';
    }
    return n;
}

typedef struct {
    int      newest;         /* highest frame number in the folder */
    int      stale;          /* lowest FLM_nnnn.TMP left behind, or -1 */
    int      next;           /* lowest frame number above `after`  */
    char     next_name[16];
    uint32_t next_size;
} soup_scan_result;

/* One pass over the folder. A CR2 is preferred over a JPEG of the same frame. */
static int soup_scan(const char *dir, int after, soup_scan_result *res)
{
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path, sizeof(path), "%s/", dir);

    res->newest = -1;
    res->next = -1;
    res->stale = -1;

    struct fio_file *file = alloc_fio_file();
    if (!file) return -1;
    struct fio_dirent *dirent = FIO_FindFirstEx(path, file);
    if (IS_ERROR(dirent)) { free(file); return -1; }

    do
    {
        struct file_info fi = convert_fio_file_info(file);
        if (fi.mode & ATTR_DIRECTORY) continue;
        int t = soup_stale_number(fi.name);
        if (t >= 0 && (res->stale < 0 || t < res->stale)) res->stale = t;
        int n = soup_frame_number(fi.name);
        if (n < 0) continue;
        if (n > res->newest) res->newest = n;
        if (n <= after) continue;
        int is_cr2 = strcmp(fi.name + 9, "CR2") == 0;
        if (res->next < 0 || n < res->next || (n == res->next && is_cr2))
        {
            res->next = n;
            snprintf(res->next_name, sizeof(res->next_name), "%s", fi.name);
            res->next_size = fi.size;
        }
    }
    while (FIO_FindNextEx(dirent, file) == 0);

    FIO_FindClose(dirent);
    free(file);
    return 0;
}

/*
 * The frame is developed into FLM_nnnn.TMP and renamed when it is whole, so
 * a develop cut off by power-off never leaves a half picture behind as a
 * JPG; the watcher finds the TMP at the next start and does it again.
 * Auto power off would otherwise end a develop: on DIGIC 4 a frame takes
 * longer than the 1100D's default 30 s. Frames are always developed at half
 * size: full size does not work on the 1100D yet.
 */
static void soup_run(const char *dir, const char *name, int number)
{
    char src[FIO_MAX_PATH_LENGTH], tmp[FIO_MAX_PATH_LENGTH], dst[FIO_MAX_PATH_LENGTH];
    snprintf(src, sizeof(src), "%s/%s", dir, name);
    snprintf(tmp, sizeof(tmp), "%s/FLM_%04d.TMP", dir, number);
    snprintf(dst, sizeof(dst), "%s/FLM_%04d.JPG", dir, number);

    const se_stock *stock = &soup_stocks[soup_film];
    snprintf(soup_status, sizeof(soup_status), "Developing %s", name);
    soup_busy = 1;
    soup_pct = 0;
    powersave_prohibit();

    int t0 = get_ms_clock();
    se_err rc = soup_develop(src, tmp, stock, 0);
    if (rc == SE_OK)
    {
        FIO_RemoveFile(dst);
        if (FIO_RenameFile(tmp, dst) != 0) { FIO_RemoveFile(tmp); rc = SE_E_IO; }
    }
    else
        FIO_RemoveFile(tmp);
    int dt = get_ms_clock() - t0;

    powersave_permit();
    soup_busy = 0;
    if (rc == SE_OK)
        snprintf(soup_status, sizeof(soup_status), "FLM_%04d.JPG, %s, %d.%ds",
                 number, stock->name, dt / 1000, dt % 1000 / 100);
    else
        snprintf(soup_status, sizeof(soup_status), "%s failed: %s", name, se_strerror(rc));
    soup_note("%s -> %s: %s, %d ms", src, stock->name, rc == SE_OK ? "ok" : se_strerror(rc), dt);
    soup_log_flush();
}

static void soup_task(void *unused)
{
    char dir[FIO_MAX_PATH_LENGTH] = "";
    soup_scan_result res;
    int done_upto = -1;
    char wait_name[16] = "";
    uint32_t wait_size = 0;

    while (1)
    {
        msleep(1000);

        /* never compete with the camera while it is busy with a frame */
        if (RECORDING || !job_state_ready_to_take_pic())
            continue;

        /*
         * The first look at a folder only records where it stands: frames
         * already on the card are left alone, and only new shots are
         * developed. Changing folder starts over the same way.
         */
        const char *now = get_dcim_dir();
        if (strcmp(now, dir) != 0)
        {
            snprintf(dir, sizeof(dir), "%s", now);
            done_upto = soup_scan(dir, 0x7fffffff, &res) == 0 ? res.newest : -1;
            wait_name[0] = 0;
            /* resume from a develop that was cut off, and the shots after it */
            if (done_upto >= 0 && res.stale >= 0)
            {
                soup_note("resuming from frame %d", res.stale);
                done_upto = res.stale - 1;
            }
        }

        if (soup_force)
        {
            soup_force = 0;
            /* the first pass finds the newest number, the second its best source */
            if (soup_scan(dir, 0x7fffffff, &res) == 0 && res.newest >= 0
                && soup_scan(dir, res.newest - 1, &res) == 0 && res.next >= 0)
            {
                soup_run(dir, res.next_name, res.next);
                if (res.next > done_upto) done_upto = res.next;
            }
            else
            {
                snprintf(soup_status, sizeof(soup_status), "No shot in %s", dir);
            }
            continue;
        }

        if (!soup_enabled)
            continue;
        if (soup_scan(dir, done_upto, &res) != 0 || res.next < 0)
            continue;

        char flm[FIO_MAX_PATH_LENGTH];
        uint32_t flm_size;
        snprintf(flm, sizeof(flm), "%s/FLM_%04d.JPG", dir, res.next);
        if (FIO_GetFileSize(flm, &flm_size) == 0)
        {
            done_upto = res.next;
            continue;
        }

        /* Canon may still be writing: wait until the size holds for a poll */
        if (strcmp(res.next_name, wait_name) != 0 || res.next_size != wait_size)
        {
            snprintf(wait_name, sizeof(wait_name), "%s", res.next_name);
            wait_size = res.next_size;
            continue;
        }

        soup_run(dir, res.next_name, res.next);
        done_upto = res.next;
        wait_name[0] = 0;
    }
}

/* ---------------------------------------------------------- film screen */
/*
 * Trash opens this instead of Magic Lantern's menu: the stock, the four
 * sliders and the size, and nothing else. ML's menu is still there, behind
 * INFO, for the rest of ML (and for unloading this module). The screen is
 * drawn over ML's menu, which is told not to draw while it is up -- the
 * same arrangement as ML's games -- and the keys are taken before ML's
 * menu sees them.
 */
extern int menu_redraw_blocked;

/*
 * The screen's own fonts, ML/FONTS/SOUP24.RBF and SOUP40.RBF (made with
 * tools/mkrbf.swift), in ML's two spare font slots. Without them it falls
 * back to ML's fonts.
 */
static uint32_t ui_body = FONT_MED, ui_title = FONT_LARGE;
static int ui_title_y = 100, ui_head_y = 22;

static void soup_ui_fonts(void)
{
    uint32_t size;
    if (FIO_GetFileSize("ML/FONTS/SOUP24.RBF", &size) == 0 && size)
        ui_body = font_by_name("SOUP24", COLOR_WHITE, COLOR_BLACK);
    if (FIO_GetFileSize("ML/FONTS/SOUP40.RBF", &size) == 0 && size)
    {
        ui_title = font_by_name("SOUP40", COLOR_WHITE, COLOR_BLACK);
        ui_title_y = 92;
        ui_head_y = 16;
    }
}

enum { ROW_FILM, ROW_GRAIN, ROW_STRENGTH, ROW_GLOW, ROW_DATE, ROW_RESET, ROWS };

static volatile int ui_shown;       /* our screen is up                    */
static volatile int ui_ml_menu;     /* INFO: ML's own menu, until it closes */
static volatile int ui_close;
static volatile int ui_dirty;
static volatile int ui_row;

#define UI_BG     COLOR_BLACK
#define UI_PANEL  COLOR_GRAY(14)
#define UI_TRACK  COLOR_GRAY(28)
#define UI_DIM    COLOR_GRAY(55)
#define UI_TEXT   COLOR_WHITE
#define UI_ACCENT COLOR_ORANGE

/* the stock's own amounts, where the sliders' "Film" detent sits */
static int soup_film_grain(void)
{
    return (int) (soup_stocks[soup_film].grain + 0.5f);
}

static int soup_film_glow(void)
{
    int g = (int) (soup_stocks[soup_film].hal_amount + 0.5f);
    return g > 200 ? 200 : g;
}

/* Grain, Strength and Glow as the stock has them */
static int soup_is_film(void)
{
    return soup_grain < 0 && soup_glow < 0 && soup_strength == 100;
}

static void soup_reset(void)
{
    soup_grain = -1;
    soup_glow = -1;
    soup_strength = 100;
}

/*
 * One step of a slider that has the stock's own value as a detent: -1
 * means "the stock's", and stepping across the stock's value stops on it.
 */
static int soup_step(int cur, int dir, int step, int max, int film)
{
    int pos = cur < 0 ? film : cur;
    int next = dir > 0 ? (pos / step + 1) * step : ((pos + step - 1) / step - 1) * step;
    if (next < 0) next = 0;
    if (next > max) next = max;
    if (cur >= 0 && (dir > 0 ? film > pos && film <= next : film < pos && film >= next))
        return -1;
    return next;
}

static void soup_change(int row, int dir)
{
    switch (row)
    {
        case ROW_FILM:     soup_film = (soup_film + dir + soup_nstocks) % soup_nstocks; break;
        case ROW_GRAIN:    soup_grain = soup_step(soup_grain, dir, 5, 100, soup_film_grain()); break;
        case ROW_STRENGTH: soup_strength = COERCE(soup_strength + dir * 10, 0, 100); break;
        case ROW_GLOW:     soup_glow = soup_step(soup_glow, dir, 10, 200, soup_film_glow()); break;
        case ROW_DATE:     soup_datestamp = COERCE(soup_datestamp + dir, 0, 2); break;
    }
}

static unsigned int soup_keypress(unsigned int key)
{
    if (!ui_shown) return 1;

    switch (key)
    {
        /* the shutter closes the menu as usual; playback and LV still work */
        case MODULE_KEY_PRESS_HALFSHUTTER:
        case MODULE_KEY_UNPRESS_HALFSHUTTER:
        case MODULE_KEY_PRESS_FULLSHUTTER:
        case MODULE_KEY_UNPRESS_FULLSHUTTER:
        case MODULE_KEY_PLAY:
        case MODULE_KEY_LV:
            return 1;

        case MODULE_KEY_TRASH:
        case MODULE_KEY_MENU:
            ui_close = 1;
            break;
        case MODULE_KEY_INFO:
            ui_ml_menu = 1;
            break;
        case MODULE_KEY_PRESS_UP:
            ui_row = (ui_row + ROWS - 1) % ROWS;
            break;
        case MODULE_KEY_PRESS_DOWN:
            ui_row = (ui_row + 1) % ROWS;
            break;
        case MODULE_KEY_PRESS_LEFT:
        case MODULE_KEY_WHEEL_LEFT:
        case MODULE_KEY_WHEEL_UP:
            soup_change(ui_row, -1);
            break;
        case MODULE_KEY_PRESS_RIGHT:
        case MODULE_KEY_WHEEL_RIGHT:
        case MODULE_KEY_WHEEL_DOWN:
            soup_change(ui_row, 1);
            break;
        case MODULE_KEY_PRESS_SET:
            if (ui_row == ROW_RESET) soup_reset();
            else soup_enabled = !soup_enabled;
            break;
        case MODULE_KEY_Q:
            soup_force = 1;
            snprintf(soup_status, sizeof(soup_status), "Queued last shot");
            break;
    }
    /* everything else would act on ML's menu, unseen */
    ui_dirty = 1;
    return 0;
}

/* a solid triangle pointing left (dir < 0) or right, centred on x, y */
static void soup_arrow(int x, int y, int dir, int size, int color)
{
    for (int i = 0; i < size; i++)
    {
        int len = 2 * (size - i);
        int col = dir < 0 ? x + size / 2 - i - 1 : x - size / 2 + i;
        bmp_fill(color, col, y - len / 2, 1, len);
    }
}

/* a pill, rounded at both ends */
static void soup_pill(int color, int x, int y, int w, int h)
{
    int r = h / 2;
    bmp_fill(color, x + r, y, w - 2 * r, h);
    fill_circle(x + r, y + r, r, color);
    fill_circle(x + w - r - 1, y + r, r, color);
}

static void soup_slider(int y, int value, int max, int film, int selected, const char *text)
{
    const int x0 = 250, w = 300, cy = y + 17;
    int v = value < 0 ? film : value;
    int at = x0 + v * w / max;
    bmp_fill(UI_TRACK, x0, cy - 2, w, 5);
    bmp_fill(selected ? UI_ACCENT : UI_DIM, x0, cy - 2, at - x0, 5);
    if (film >= 0)
        bmp_fill(UI_TEXT, x0 + film * w / max - 1, cy - 9, 3, 19);
    fill_circle(at, cy, 10, selected ? UI_ACCENT : UI_TEXT);
    if (value < 0)
        fill_circle(at, cy, 5, UI_BG);   /* hollow: the stock's own */
    bmp_printf(FONT(ui_body, selected ? UI_TEXT : UI_DIM, UI_BG) | FONT_ALIGN_RIGHT, 680, y + 5, "%s", text);
}

static void soup_choices(int y, const char *const *names, int n, int value, int selected)
{
    int x = 250;
    for (int i = 0; i < n; i++)
    {
        int fg = i == value ? (selected ? UI_ACCENT : UI_TEXT) : UI_TRACK;
        x += bmp_printf(FONT(ui_body, fg, UI_BG), x, y + 5, "%s", names[i]) + 28;
    }
}

static void soup_ui_draw(void)
{
    static const char *const date_names[] = { "Off", "Date", "Date + time" };
    static const char *const labels[] = { 0, "Grain", "Strength", "Glow", "Date stamp", "Reset to film" };
    int row = ui_row;
    char text[16];

    /* header: the name, and whether developing is on */
    bmp_fill(UI_BG, 0, 0, 720, 66);
    bmp_printf(FONT(ui_title, UI_ACCENT, UI_BG), 40, ui_head_y, "SOUP");
    bmp_printf(FONT(ui_title, UI_DIM, UI_BG), 40 + bmp_string_width(ui_title, "SOUP "), ui_head_y, "film");
    soup_pill(soup_enabled ? UI_ACCENT : UI_PANEL, 572, 20, 108, 36);
    bmp_printf(FONT(ui_body, soup_enabled ? UI_BG : UI_DIM, soup_enabled ? UI_ACCENT : UI_PANEL) | FONT_ALIGN_CENTER,
               626, 26, soup_enabled ? "ON" : "OFF");

    /* the stock, on a strip of film */
    bmp_fill(UI_BG, 0, 66, 720, 138);
    bmp_fill(UI_PANEL, 30, 72, 660, 124);
    for (int x = 42; x + 20 < 690; x += 36)
    {
        bmp_fill(UI_BG, x, 80, 20, 13);
        bmp_fill(UI_BG, x, 175, 20, 13);
    }
    int sel = row == ROW_FILM;
    bmp_printf(FONT(ui_title, UI_TEXT, UI_PANEL) | FONT_ALIGN_CENTER, 360, ui_title_y, "%s", soup_stocks[soup_film].name);
    bmp_printf(FONT(ui_body, sel ? UI_ACCENT : UI_DIM, UI_PANEL) | FONT_ALIGN_CENTER, 360, 140,
               "%d of %d", soup_film + 1, soup_nstocks);
    soup_arrow(66, 134, -1, 14, sel ? UI_ACCENT : UI_TRACK);
    soup_arrow(654, 134, 1, 14, sel ? UI_ACCENT : UI_TRACK);

    /* the settings */
    for (int r = ROW_GRAIN; r < ROWS; r++)
    {
        int y = 204 + (r - ROW_GRAIN) * 38;
        sel = r == row;
        bmp_fill(UI_BG, 0, y, 720, 38);
        if (sel) bmp_fill(UI_ACCENT, 40, y + 6, 5, 24);
        bmp_printf(FONT(ui_body, sel ? UI_TEXT : UI_DIM, UI_BG), 62, y + 5, "%s", labels[r]);
        switch (r)
        {
            case ROW_GRAIN:
                if (soup_grain < 0) snprintf(text, sizeof(text), "Film");
                else snprintf(text, sizeof(text), "%d", soup_grain);
                soup_slider(y, soup_grain, 100, soup_film_grain(), sel, text);
                break;
            case ROW_STRENGTH:
                snprintf(text, sizeof(text), "%d%%", soup_strength);
                soup_slider(y, soup_strength, 100, -1, sel, text);
                break;
            case ROW_GLOW:
                if (soup_glow < 0) snprintf(text, sizeof(text), "Film");
                else snprintf(text, sizeof(text), "%d", soup_glow);
                soup_slider(y, soup_glow, 200, soup_film_glow(), sel, text);
                break;
            case ROW_DATE:
                soup_choices(y, date_names, 3, soup_datestamp, sel);
                break;
            case ROW_RESET:
                if (soup_is_film())
                    bmp_printf(FONT(ui_body, UI_TRACK, UI_BG), 250, y + 5, "As the film has it");
                else
                    bmp_printf(FONT(ui_body, sel ? UI_ACCENT : UI_DIM, UI_BG), 250, y + 5, "Grain, Strength, Glow");
                break;
        }
    }

    /* below the last row, so Canon's menu underneath never shows */
    int end = 204 + (ROWS - ROW_GRAIN) * 38;
    if (end < 394) bmp_fill(UI_BG, 0, end, 720, 394 - end);

    /* what it is doing, and the keys */
    bmp_fill(UI_BG, 0, 394, 720, 86);
    bmp_fill(UI_PANEL, 30, 398, 660, 2);
    if (soup_busy)
    {
        bmp_printf(FONT(ui_body, UI_ACCENT, UI_BG), 40, 408, "Developing  %d%%", soup_pct);
        bmp_fill(UI_TRACK, 40, 436, 640, 4);
        bmp_fill(UI_ACCENT, 40, 436, 640 * soup_pct / 100, 4);
    }
    else
        bmp_printf(FONT(ui_body, UI_DIM, UI_BG) | FONT_TEXT_WIDTH(640), 40, 408, "%s", soup_status);
    int x = 40;
    static const char *const keys[][2] = { { "SET", "on/off" }, { "Q", "develop last" }, { "INFO", "all of ML" } };
    static const char *const reset_key[2] = { "SET", "reset" };
    for (int i = 0; i < 3; i++)
    {
        const char *const *k = i == 0 && row == ROW_RESET ? reset_key : keys[i];
        x += bmp_printf(FONT(ui_body, UI_TEXT, UI_BG), x, 446, "%s ", k[0]);
        x += bmp_printf(FONT(ui_body, UI_TRACK, UI_BG), x, 446, "%s", k[1]) + 30;
    }
}

static void soup_ui_close(void)
{
    ui_shown = 0;
    clrscr();
    menu_redraw_blocked = 0;
}

static void soup_ui_task(void *unused)
{
    int last_pct = -1, last_busy = 0, ticks = 0;
    char last_status[sizeof(soup_status)] = "";

    while (1)
    {
        msleep(50);

        if (!gui_menu_shown())
        {
            if (ui_shown) soup_ui_close();
            ui_ml_menu = 0;
            continue;
        }

        /* INFO: hand the screen back to ML's menu until it closes */
        if (ui_ml_menu)
        {
            if (ui_shown)
            {
                soup_ui_close();
                menu_redraw();
            }
            continue;
        }

        if (!ui_shown)
        {
            menu_redraw_blocked = 1;
            ui_shown = 1;
            ui_close = 0;
            clrscr();
            ui_dirty = 1;
        }

        if (ui_close)
        {
            ui_close = 0;
            gui_stop_menu();
            continue;
        }

        /* the develop's progress, as it goes */
        if (soup_busy != last_busy || soup_pct != last_pct || strcmp(soup_status, last_status))
        {
            last_busy = soup_busy;
            last_pct = soup_pct;
            snprintf(last_status, sizeof(last_status), "%s", soup_status);
            ui_dirty = 1;
        }

        /* Canon redraws its menu underneath now and then (the clock, for one)
         * and paints over this screen, so draw it all again every second */
        if (++ticks >= 20) ui_dirty = 1;

        if (ui_dirty)
        {
            ui_dirty = 0;
            ticks = 0;
            soup_ui_draw();
        }
    }
}

/* ----------------------------------------------------------------- menu */

static MENU_SELECT_FUNC(soup_develop_last)
{
    soup_force = 1;
    snprintf(soup_status, sizeof(soup_status), "Queued last shot");
}

static MENU_UPDATE_FUNC(soup_main_update)
{
    if (soup_enabled)
        MENU_SET_RINFO("%s", soup_film_names[soup_film]);
    if (soup_busy)
        MENU_SET_HELP("Developing: %d%%", soup_pct);
}

static MENU_UPDATE_FUNC(soup_status_update)
{
    if (soup_busy)
        MENU_SET_VALUE("%d%%", soup_pct);
    else
        MENU_SET_VALUE("%s", soup_status);
}

static struct menu_entry soup_children[] =
{
    {
        .name    = "Film",
        .priv    = &soup_film,
        .max     = 0,                   /* set once the stocks are loaded */
        .choices = soup_film_names,
        .help    = "Film stock. Add your own with Soup Stocks, listed in ML/FILM/INDEX.TXT.",
    },
    {
        .name    = "Develop last shot",
        .select  = soup_develop_last,
        .help    = "Develop the newest frame now, even if it already has a film version.",
    },
    {
        .name    = "Status",
        .update  = soup_status_update,
        .help    = "Last develop. Details in ML/LOGS/SOUP.LOG.",
    },
    MENU_EOL,
};

static struct menu_entry soup_menu[] =
{
    {
        .name     = "Soup Engine",
        .priv     = &soup_enabled,
        .max      = 1,
        .update   = soup_main_update,
        .help     = "Film simulation: saves FLM_xxxx.JPG next to every new shot.",
        .children = soup_children,
    },
};

static unsigned int soup_init()
{
    se_set_log(soup_log, 0);
    soup_load_stocks();
    if (!soup_stocks) return 1;

    soup_children[0].max = soup_nstocks - 1;
    soup_note("soup: %d stocks loaded", soup_nstocks);
    soup_log_flush();

    menu_add("Shoot", soup_menu, COUNT(soup_menu));
    task_create("soup_task", 0x1f, 0x10000, soup_task, 0);
    soup_ui_fonts();
    task_create("soup_ui", 0x1e, 0x2000, soup_ui_task, 0);
    return 0;
}

static unsigned int soup_deinit()
{
    return 0;
}

MODULE_INFO_START()
    MODULE_INIT(soup_init)
    MODULE_DEINIT(soup_deinit)
MODULE_INFO_END()

MODULE_CBRS_START()
    MODULE_CBR(CBR_KEYPRESS, soup_keypress, 0)
MODULE_CBRS_END()

MODULE_CONFIGS_START()
    MODULE_CONFIG(soup_enabled)
    MODULE_CONFIG(soup_film)
    MODULE_CONFIG(soup_grain)
    MODULE_CONFIG(soup_glow)
    MODULE_CONFIG(soup_strength)
    MODULE_CONFIG(soup_datestamp)
MODULE_CONFIGS_END()
