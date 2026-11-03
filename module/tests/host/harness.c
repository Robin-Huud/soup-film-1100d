/*
 * Host test for the soup module: builds soup.c itself against the shim in
 * include/ and drives it on a directory laid out like an 80D's SD card.
 *
 *   harness <card dir> <test.jpg> <test.cr2>
 *
 * The card dir must hold ML/FILM/ with the stocks; the test frames are
 * copied into DCIM folders as the test goes.
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "../../soup.c"
#include "screen.c"

/* ------------------------------------------------------------- the shim */

static const char *card;
static char dcim[64] = "B:/DCIM/100CANON";
int host_recording, host_busy;

static const char *map(const char *name, char *buf)
{
    if (name[1] == ':') name += 3;
    snprintf(buf, 512, "%s/%s", card, name);
    return buf;
}

FILE *FIO_OpenFile(const char *name, unsigned mode) { char b[512]; return fopen(map(name, b), "rb"); }
FILE *FIO_CreateFile(const char *name) { char b[512]; return fopen(map(name, b), "wb"); }
FILE *FIO_CreateFileOrAppend(const char *name) { char b[512]; return fopen(map(name, b), "ab"); }
int   FIO_ReadFile(FILE *f, void *buf, size_t n) { return (int) fread(buf, 1, n, f); }
int   FIO_WriteFile(FILE *f, const void *buf, size_t n) { return (int) fwrite(buf, 1, n, f); }
void  FIO_CloseFile(FILE *f) { fclose(f); }
int   FIO_RemoveFile(const char *name) { char b[512]; return remove(map(name, b)); }
int   FIO_RenameFile(const char *src, const char *dst) { char a[512], b[512]; return rename(map(src, a), map(dst, b)); }
char  camera_model[32] = "Canon EOS 1100D";
int   host_awake;   /* powersave_prohibit() calls not yet permitted */
void  powersave_prohibit(void) { host_awake++; }
void  powersave_permit(void) { host_awake--; }
int64_t FIO_SeekSkipFile(FILE *f, int64_t pos, int whence) { fseek(f, (long) pos, whence); return ftell(f); }

int FIO_GetFileSize(const char *name, uint32_t *size)
{
    char b[512]; struct stat st;
    if (stat(map(name, b), &st) != 0) return -1;
    *size = (uint32_t) st.st_size;
    return 0;
}

size_t read_file(const char *name, void *buf, size_t size)
{
    FILE *f = FIO_OpenFile(name, 0);
    if (!f) return 0;
    size_t n = fread(buf, 1, size, f);
    fclose(f);
    return n;
}

uint8_t *read_entire_file(const char *name, int *size)
{
    uint32_t sz;
    if (FIO_GetFileSize(name, &sz) != 0) return 0;
    uint8_t *buf = malloc(sz + 1);
    *size = (int) read_file(name, buf, sz);
    buf[*size] = 0;
    return buf;
}

struct fio_file { struct file_info fi; };
struct fio_dirent { DIR *d; char path[512]; };

struct fio_file *alloc_fio_file(void) { return calloc(1, sizeof(struct fio_file)); }
struct file_info convert_fio_file_info(struct fio_file *f) { return f->fi; }

int FIO_FindNextEx(struct fio_dirent *d, struct fio_file *f)
{
    struct dirent *e = readdir(d->d);
    if (!e) return -1;
    char p[1024]; struct stat st;
    snprintf(p, sizeof(p), "%s/%s", d->path, e->d_name);
    stat(p, &st);
    memset(&f->fi, 0, sizeof(f->fi));
    snprintf(f->fi.name, sizeof(f->fi.name), "%s", e->d_name);
    f->fi.size = (uint32_t) st.st_size;
    f->fi.mode = S_ISDIR(st.st_mode) ? ATTR_DIRECTORY : 0;
    return 0;
}

struct fio_dirent *FIO_FindFirstEx(const char *dir, struct fio_file *f)
{
    struct fio_dirent *d = calloc(1, sizeof(*d));
    map(dir, d->path);
    d->d = opendir(d->path);
    if (!d->d || FIO_FindNextEx(d, f) != 0) { if (d->d) closedir(d->d); free(d); return 0; }
    return d;
}

void FIO_FindClose(struct fio_dirent *d) { closedir(d->d); free(d); }

const char *get_dcim_dir(void) { return dcim; }
int  job_state_ready_to_take_pic(void) { return !host_busy; }
void msleep(int ms) { usleep(ms * 1000 / 20); }   /* the watcher runs 20x fast */
int  get_ms_clock(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int) (t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
void menu_add(const char *where, struct menu_entry *e, int n) {}

static void *thread_main(void *fn) { ((void (*)(void *)) fn)(0); return 0; }
void task_create(const char *name, int prio, int stack, void (*fn)(void *), void *arg)
{
    pthread_t t;
    pthread_create(&t, 0, thread_main, (void *) fn);
    pthread_detach(t);
}

/* ---------------------------------------------------------------- tests */

static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok    "); else { printf("  FAIL  "); fails++; } \
                           printf(__VA_ARGS__); printf("\n"); } while (0)

static void put(const char *src, const char *dir, const char *name)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/%s' && cp '%s' '%s/%s/%s'", card, dir, src, card, dir, name);
    if (system(cmd) != 0) { printf("cannot copy %s\n", src); exit(2); }
}

static int exists(const char *dir, const char *name)
{
    char p[1024]; struct stat st;
    snprintf(p, sizeof(p), "%s/%s/%s", card, dir, name);
    return stat(p, &st) == 0;
}

/* the output must be a JPEG the engine itself can read back, at the size asked */
static int check_jpeg(const char *path, int w, int h)
{
    int size = 0;
    uint8_t *buf = read_entire_file(path, &size);
    se_jdec_info ji;
    int ok = buf && se_jpeg_dec_info(buf, size, &ji) == SE_OK && ji.width == w && ji.height == h;
    free(buf);
    return ok;
}

static void test_names(void)
{
    printf("frame names\n");
    CHECK(soup_frame_number("IMG_1234.CR2") == 1234, "IMG_1234.CR2");
    CHECK(soup_frame_number("_MG_0001.JPG") == 1, "_MG_0001.JPG");
    CHECK(soup_frame_number("FLM_1234.JPG") == -1, "FLM_ is our own output");
    CHECK(soup_frame_number("MVI_1234.MOV") == -1, "movies are skipped");
    CHECK(soup_frame_number("IMG_12a4.CR2") == -1, "non-digit number");
    CHECK(soup_frame_number("IMG_1234.CR2.xmp") == -1, "sidecar");
    CHECK(soup_stale_number("FLM_0042.TMP") == 42, "FLM_0042.TMP is a cut-off develop");
    CHECK(soup_stale_number("FLM_0042.JPG") == -1 && soup_stale_number("IMG_0042.TMP") == -1,
          "only FLM_ TMPs count");
}

/* A camera JPEG's APP1, built by hand in both byte orders. */
static int make_exif_jpeg(uint8_t *b, int be)
{
    uint8_t t[256] = { 0 };
    int n = 0;
    #define P16(v) do { if (be) { t[n] = (v) >> 8; t[n+1] = (v); } else { t[n] = (v); t[n+1] = (v) >> 8; } n += 2; } while (0)
    #define P32(v) do { uint32_t _v = (v); for (int _i = 0; _i < 4; _i++) t[n + (be ? 3 - _i : _i)] = _v >> (8 * _i); n += 4; } while (0)
    t[0] = t[1] = be ? 'M' : 'I'; n = 2; P16(42); P32(8);
    /* IFD0 at 8: Model, ExifIFD */
    P16(2);
    P16(0x0110); P16(2); P32(16); P32(100);
    P16(0x8769); P16(4); P32(1);  P32(40);
    P32(0);
    /* Exif IFD at 40: ExposureTime, FNumber, ISO, DateTimeOriginal */
    n = 40;
    P16(4);
    P16(0x829a); P16(5); P32(1); P32(120);
    P16(0x829d); P16(5); P32(1); P32(128);
    P16(0x8827); P16(3); P32(1); P16(800); P16(0);
    P16(0x9003); P16(2); P32(20); P32(140);
    P32(0);
    n = 100; memcpy(t + n, "Canon EOS 1100D", 16);
    n = 120; P32(1); P32(125);
    n = 128; P32(56); P32(10);
    memcpy(t + 140, "2026:11:03 17:20:05", 20);
    #undef P16
    #undef P32
    int len = 160;
    b[0] = 0xFF; b[1] = 0xD8;
    b[2] = 0xFF; b[3] = 0xE1; b[4] = (len + 8) >> 8; b[5] = len + 8;
    memcpy(b + 6, "Exif\0\0", 6);
    memcpy(b + 12, t, len);
    b[12 + len] = 0xFF; b[13 + len] = 0xDA;
    return 14 + len;
}

static void test_exif(void)
{
    printf("jpeg exif\n");
    for (int be = 0; be <= 1; be++)
    {
        uint8_t buf[512];
        int len = make_exif_jpeg(buf, be);
        se_cr2_info info;
        memset(&info, 0, sizeof(info));
        soup_jpeg_exif(buf, len, &info);
        CHECK(!strcmp(info.model, "Canon EOS 1100D") && !strcmp(info.datetime, "2026:11:03 17:20:05")
              && info.iso == 800 && info.aperture > 5.59f && info.aperture < 5.61f
              && info.exposure_time > 0.0079f && info.exposure_time < 0.0081f,
              "%s order: %s, %s, ISO %d, f/%.1f, 1/%.0f", be ? "Motorola" : "Intel",
              info.model, info.datetime, info.iso, info.aperture,
              info.exposure_time ? 1 / info.exposure_time : 0);
        memset(&info, 0, sizeof(info));
        soup_jpeg_exif(buf, len - 60, &info);   /* cut short: must not read past the end */
        CHECK(1, "%s order, truncated: no crash", be ? "Motorola" : "Intel");
    }
    uint8_t plain[] = { 0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x04, 0, 0, 0xFF, 0xDA };
    se_cr2_info info;
    memset(&info, 0, sizeof(info));
    soup_jpeg_exif(plain, sizeof(plain), &info);
    CHECK(!info.model[0] && !info.iso, "no APP1: nothing filled");
}

static void test_reader(const char *cr2)
{
    printf("read window\n");
    FILE *f = fopen(cr2, "rb");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    uint8_t *all = malloc(len);
    fseek(f, 0, SEEK_SET);
    fread(all, 1, len, f);
    fseek(f, 0, SEEK_SET);

    soup_rd rd = { f, 1000, malloc(SOUP_WIN), 0, 0 };
    uint8_t tmp[70000];
    int bad = 0;
    srand(1);
    /* forward runs that straddle window edges, then a few backward jumps */
    uint32_t off = 0;
    for (int i = 0; i < 400; i++)
    {
        uint32_t n = 1 + rand() % sizeof(tmp);
        if (i % 50 == 49) off = rand() % (len / 2);
        if (rd.base + off + n > (uint32_t) len) { off = 0; continue; }
        if (soup_read(&rd, off, tmp, n) != 0 || memcmp(tmp, all + rd.base + off, n)) bad++;
        off += n;
    }
    CHECK(bad == 0, "400 reads match the file (%d mismatched)", bad);
    CHECK(soup_read(&rd, len, tmp, 10) != 0, "read past the end fails");
    free(rd.win); free(all); fclose(f);
}

static void test_scan(const char *jpg, const char *cr2)
{
    printf("folder scan\n");
    put(cr2, "DCIM/100CANON", "IMG_0001.CR2");
    put(jpg, "DCIM/100CANON", "IMG_0002.JPG");
    put(cr2, "DCIM/100CANON", "IMG_0002.CR2");
    put(jpg, "DCIM/100CANON", "FLM_0009.JPG");
    soup_scan_result r;
    CHECK(soup_scan("B:/DCIM/100CANON", -1, &r) == 0 && r.newest == 2, "newest is 2, FLM_ ignored");
    CHECK(soup_scan("B:/DCIM/100CANON", 0, &r) == 0 && r.next == 1, "next after 0 is 1");
    CHECK(soup_scan("B:/DCIM/100CANON", 1, &r) == 0 && !strcmp(r.next_name, "IMG_0002.CR2"),
          "CR2 preferred over JPG (%s)", r.next_name);
    CHECK(soup_scan("B:/DCIM/100CANON", 2, &r) == 0 && r.next == -1, "nothing after 2");
    CHECK(r.stale == -1, "no TMP, nothing stale");
    CHECK(soup_scan("B:/DCIM/999CANON", -1, &r) != 0, "missing folder fails");
}

static void test_develop(void)
{
    printf("develop\n");
    se_err rc;
    int t;

    t = get_ms_clock();
    rc = soup_develop("B:/DCIM/100CANON/IMG_0001.CR2", "B:/DCIM/100CANON/FLM_0001.JPG", &soup_stocks[0], 0);
    CHECK(rc == SE_OK, "CR2 preview, half: %s, %d ms", se_strerror(rc), get_ms_clock() - t);
    CHECK(check_jpeg("B:/DCIM/100CANON/FLM_0001.JPG", 3000, 2000), "output is 3000x2000");

    t = get_ms_clock();
    rc = soup_develop("B:/DCIM/100CANON/IMG_0002.JPG", "B:/DCIM/100CANON/FLM_0002.JPG", &soup_stocks[0], 1);
    CHECK(rc == SE_OK, "JPEG source, full: %s, %d ms", se_strerror(rc), get_ms_clock() - t);
    CHECK(check_jpeg("B:/DCIM/100CANON/FLM_0002.JPG", 6000, 4000), "output is 6000x4000");

    rc = soup_develop("B:/DCIM/100CANON/NOPE.CR2", "B:/DCIM/100CANON/FLM_0003.JPG", &soup_stocks[0], 0);
    CHECK(rc != SE_OK && !exists("DCIM/100CANON", "FLM_0003.JPG"), "missing source fails, no output left");

    int bad = 0;
    for (int i = 0; i < soup_nstocks; i++)
    {
        char dst[64];
        snprintf(dst, sizeof(dst), "B:/DCIM/STOCKS/%s.JPG", soup_stocks[i].name);
        if (soup_develop("B:/DCIM/100CANON/IMG_0001.CR2", dst, &soup_stocks[i], 0) != SE_OK
            || !check_jpeg(dst, 3000, 2000))
        {
            printf("        %s failed\n", soup_stocks[i].name);
            bad++;
        }
    }
    CHECK(bad == 0, "all %d stocks develop", soup_nstocks);
}

static int wait_for(const char *dir, const char *name, int ms)
{
    /* the file appears when the develop starts; done means the task is idle again */
    for (int t = 0; t < ms; t += 50) { if (exists(dir, name) && !soup_busy) return 1; usleep(50000); }
    return 0;
}

static void test_watcher(const char *cr2)
{
    printf("watcher\n");
    put(cr2, "DCIM/101CANON", "IMG_0100.CR2");     /* on the card before the task looks */
    snprintf(dcim, sizeof(dcim), "B:/DCIM/101CANON");
    soup_enabled = 1;
    soup_film = 3;
    task_create("soup_task", 0, 0, soup_task, 0);
    usleep(300000);

    host_busy = 1;
    put(cr2, "DCIM/101CANON", "IMG_0101.CR2");
    CHECK(!wait_for("DCIM/101CANON", "FLM_0101.JPG", 1500), "waits while the camera is busy");
    host_busy = 0;
    CHECK(wait_for("DCIM/101CANON", "FLM_0101.JPG", 20000), "new shot developed: %s", soup_status);
    CHECK(!exists("DCIM/101CANON", "FLM_0100.JPG"), "frame already on the card left alone");

    put(cr2, "DCIM/101CANON", "IMG_0102.CR2");
    put(cr2, "DCIM/101CANON", "IMG_0103.CR2");
    CHECK(wait_for("DCIM/101CANON", "FLM_0103.JPG", 30000) && exists("DCIM/101CANON", "FLM_0102.JPG"),
          "two quick shots both developed");

    soup_enabled = 0;
    put(cr2, "DCIM/101CANON", "IMG_0104.CR2");
    CHECK(!wait_for("DCIM/101CANON", "FLM_0104.JPG", 1500), "nothing while disabled");

    soup_force = 1;
    CHECK(wait_for("DCIM/101CANON", "FLM_0104.JPG", 20000), "develop last shot works while disabled");
    CHECK(!exists("DCIM/101CANON", "FLM_0104.TMP"), "no TMP left after a develop");
    CHECK(host_awake == 0, "auto power off allowed again after developing");
    CHECK(check_jpeg("B:/DCIM/101CANON/FLM_0104.JPG", 3000, 2000), "renamed output is a whole JPEG");
}

/*
 * The camera powered off mid-develop: FLM_0201.TMP is half a picture, and
 * 0202 was shot while 0201 was developing. A fresh start must redo 0201,
 * then 0202, and leave 0200 (developed before) alone.
 */
static void test_resume(const char *cr2)
{
    printf("resume after power-off\n");
    put(cr2, "DCIM/102CANON", "IMG_0200.CR2");
    put(cr2, "DCIM/102CANON", "FLM_0200.JPG");
    put(cr2, "DCIM/102CANON", "IMG_0201.CR2");
    put(cr2, "DCIM/102CANON", "IMG_0202.CR2");
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "head -c 524288 '%s' > '%s/DCIM/102CANON/FLM_0201.TMP'", cr2, card);
    system(cmd);
    struct stat st0; char p[1024];
    snprintf(p, sizeof(p), "%s/DCIM/102CANON/FLM_0200.JPG", card);
    stat(p, &st0);

    soup_enabled = 1;
    snprintf(dcim, sizeof(dcim), "B:/DCIM/102CANON");   /* the running task sees a new folder */
    CHECK(wait_for("DCIM/102CANON", "FLM_0202.JPG", 40000), "cut-off frame and the one after redone");
    CHECK(check_jpeg("B:/DCIM/102CANON/FLM_0201.JPG", 3000, 2000), "FLM_0201.JPG is whole");
    CHECK(!exists("DCIM/102CANON", "FLM_0201.TMP"), "stale TMP gone");
    struct stat st1; stat(p, &st1);
    CHECK(st1.st_mtime == st0.st_mtime && st1.st_size == st0.st_size, "already developed frame untouched");
    soup_enabled = 0;
}

/* The film screen's controls: strength, grain, glow and the date stamp */
static void test_controls(void)
{
    printf("controls\n");
    se_look lk, base;
    se_look_from_stock(&soup_stocks[0], 2, &base);

    lk = base;
    soup_adjust(&lk, 100, -1, -1);
    CHECK(memcmp(&lk, &base, sizeof(lk)) == 0, "full strength, stock grain and glow change nothing");

    lk = base;
    soup_adjust(&lk, 0, -1, -1);
    int flat = lk.sat == 256 && !lk.sh[0] && !lk.hi[2] && lk.grain == base.grain;
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) flat &= lk.map[c][i] == i;
    for (int i = 0; i < 6; i++) flat &= lk.hue_sat[i] == 256 && lk.hue_lum[i] == 256 && !lk.hue_rot[i];
    CHECK(flat, "zero strength is a straight render, grain kept");

    lk = base;
    soup_adjust(&lk, 50, 60, 0);
    CHECK(lk.grain == 60 && lk.hal_amount == 0, "grain and glow override the stock");
    soup_adjust(&lk, 100, 500, 900);
    CHECK(lk.grain == 100 && lk.hal_amount == 200, "and are clamped");

    CHECK(soup_step(-1, 1, 5, 100, 12) == 15 && soup_step(15, -1, 5, 100, 12) == -1
          && soup_step(10, 1, 5, 100, 12) == -1 && soup_step(-1, -1, 5, 100, 12) == 10,
          "the stock's grain is a detent on the slider");
    CHECK(soup_step(0, -1, 5, 100, 12) == 0 && soup_step(100, 1, 5, 100, 12) == 100
          && soup_step(-1, 1, 10, 200, 200) == 200, "slider ends hold");

    char t[24];
    CHECK(soup_stamp_text("2026:11:03 18:42:07", 0, t, sizeof(t)) && !strcmp(t, "'26 11  3"), "date: [%s]", t);
    CHECK(soup_stamp_text("2026:01:23 08:05:00", 1, t, sizeof(t)) && !strcmp(t, "'26  1 23   8:05"), "date and time: [%s]", t);
    CHECK(!soup_stamp_text("", 1, t, sizeof(t)) && !soup_stamp_text("0000:00:00 00:00:00", 0, t, sizeof(t)),
          "no date, no stamp");

    soup_stamp st;
    int ok = soup_stamp_begin(&st, "'26 11  3  18:42", 2136, 1424);
    int lit = 0, full = 0, glow = 0;
    for (int i = 0; ok && i < st.w * st.h; i++) { lit += st.core[i] > 0; full += st.core[i] == 255; glow += st.glow[i] > 0; }
    CHECK(ok && st.x0 + st.w <= 2136 && st.y0 + st.h <= 1424 && st.x0 > 2136 / 2 && st.y0 > 1424 * 3 / 4,
          "stamp sits in the bottom right: %dx%d at %d,%d", st.w, st.h, st.x0, st.y0);
    CHECK(full > 500 && lit > full && glow > lit, "glyphs filled (%d px), edges smoothed (%d), glow wider (%d)", full, lit, glow);
    soup_stamp_free(&st);
    CHECK(!soup_stamp_begin(&st, "'26 11  3  18:42", 120, 80), "no stamp on a frame too small for it");
}

/* Develop one real frame through every stock, as the camera would.
 * SOUP_GRAIN, SOUP_GLOW, SOUP_STRENGTH and SOUP_STAMP set the film screen's
 * controls; SOUP_ONLY limits it to stocks whose name contains it. */
static int develop_all(const char *photo)
{
    const char *e, *only = getenv("SOUP_ONLY");
    if ((e = getenv("SOUP_GRAIN")))    soup_grain = atoi(e);
    if ((e = getenv("SOUP_GLOW")))     soup_glow = atoi(e);
    if ((e = getenv("SOUP_STRENGTH"))) soup_strength = atoi(e);
    if ((e = getenv("SOUP_STAMP")))    soup_datestamp = atoi(e);

    const char *ext = strrchr(photo, '.');
    char name[16], src[64];
    snprintf(name, sizeof(name), "IMG_0001%s", ext ? ext : ".CR2");
    put(photo, "DCIM/100CANON", name);
    snprintf(src, sizeof(src), "B:/DCIM/100CANON/%s", name);

    int bad = 0;
    for (int full = 0; full <= 1; full++)
    {
        for (int i = 0; i < soup_nstocks; i++)
        {
            if (only && !strstr(soup_stocks[i].name, only)) continue;
            if (full && i && !only) break;   /* full size once is enough */
            char dst[64];
            snprintf(dst, sizeof(dst), "B:/DCIM/STOCKS/%s%s.JPG", soup_stocks[i].name, full ? " FULL" : "");
            int t = get_ms_clock();
            se_err rc = soup_develop(src, dst, &soup_stocks[i], full);
            printf("  %-6s %-16s %s, %d ms\n", rc == SE_OK ? "ok" : "FAIL",
                   soup_stocks[i].name, se_strerror(rc), get_ms_clock() - t);
            bad += rc != SE_OK;
        }
    }
    soup_log_flush();
    return bad != 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4)
    {
        fprintf(stderr, "usage: harness <card> <test.jpg> <test.cr2>\n"
                        "       harness <card> <photo.CR2|.JPG>\n");
        return 2;
    }
    card = argv[1];

    se_set_log(soup_log, 0);
    soup_load_stocks();
    if (argc == 4 && !strcmp(argv[2], "--screen")) return render_screens(getenv("SOUP_FONTS"), argv[3]);
    if (argc == 3) return develop_all(argv[2]);
    printf("stocks\n");
    CHECK(soup_nstocks == 21, "%d stocks from ML/FILM/INDEX.TXT", soup_nstocks);
    {
        int first_bw = -1, misplaced = 0;
        for (int i = 0; i < soup_nstocks; i++)
        {
            int bw = !strncmp(soup_stocks[i].name, "BW ", 3);
            if (bw != soup_is_bw(&soup_stocks[i])) misplaced++;
            if (bw && first_bw < 0) first_bw = i;
            if (!bw && first_bw >= 0) misplaced++;
        }
        CHECK(first_bw == soup_nstocks - 3 && !misplaced, "black and white listed last as BW: %s, %s, %s",
              soup_stocks[18].name, soup_stocks[19].name, soup_stocks[20].name);
    }

    {
        /* DryOS on DIGIC 4 reads 8.3 names only: bleachbypass.fs was unreadable on an 1100D */
        int size = 0, bad = 0;
        char *idx = (char *) read_entire_file("ML/FILM/INDEX.TXT", &size);
        for (char *l = idx; idx && l < idx + size; )
        {
            char *e = l; while (e < idx + size && *e != '\n' && *e != '\r') e++;
            char *dot = memchr(l, '.', e - l);
            if (e > l && (!dot || dot - l > 8 || e - dot - 1 > 3)) bad++;
            l = e + 1;
        }
        free(idx);
        CHECK(bad == 0, "every stock file has an 8.3 name (%d too long)", bad);
    }
    test_names();
    test_exif();
    test_reader(argv[3]);
    test_scan(argv[2], argv[3]);
    test_develop();
    test_controls();
    test_watcher(argv[3]);
    test_resume(argv[3]);

    soup_log_flush();
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
