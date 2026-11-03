/*
 * Just enough of Magic Lantern's module API for soup.c to build and run on
 * a desktop. Card paths map onto a directory: "B:/DCIM/..." and "ML/..."
 * both resolve under SOUP_CARD.
 */
#ifndef ML_HOST_H
#define ML_HOST_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define COUNT(x) ((int)(sizeof(x) / sizeof((x)[0])))

/* config, module info: plain variables, nothing registered */
#define CONFIG_INT(name, var, def) int var = (def)
#define MODULE_INFO_START()
#define MODULE_INIT(f)
#define MODULE_DEINIT(f)
#define MODULE_INFO_END()
#define MODULE_CONFIGS_START()
#define MODULE_CONFIG(v)
#define MODULE_CONFIGS_END()

/* memory */
#define fio_malloc malloc
#define fio_free free

/* files */
#define FIO_MAX_PATH_LENGTH 0x80
#define ATTR_DIRECTORY 0x10
#define O_RDONLY 0
#define O_SYNC 0
#define IS_ERROR(p) ((p) == NULL)

struct fio_dirent;
struct fio_file;
struct file_info {
    uint16_t mode;
    uint32_t size;
    uint64_t timestamp;
    char name[FIO_MAX_PATH_LENGTH];
};

FILE *FIO_OpenFile(const char *name, unsigned mode);
FILE *FIO_CreateFile(const char *name);
FILE *FIO_CreateFileOrAppend(const char *name);
int   FIO_ReadFile(FILE *f, void *buf, size_t n);
int   FIO_WriteFile(FILE *f, const void *buf, size_t n);
void  FIO_CloseFile(FILE *f);
int   FIO_RemoveFile(const char *name);
int   FIO_GetFileSize(const char *name, uint32_t *size);
int   FIO_RenameFile(const char *src, const char *dst);
extern char camera_model[32];
void  powersave_prohibit(void);
void  powersave_permit(void);
int64_t FIO_SeekSkipFile(FILE *f, int64_t pos, int whence);
struct fio_file *alloc_fio_file(void);
struct file_info convert_fio_file_info(struct fio_file *f);
struct fio_dirent *FIO_FindFirstEx(const char *dir, struct fio_file *f);
int   FIO_FindNextEx(struct fio_dirent *d, struct fio_file *f);
void  FIO_FindClose(struct fio_dirent *d);
size_t read_file(const char *name, void *buf, size_t size);
uint8_t *read_entire_file(const char *name, int *size);
const char *get_dcim_dir(void);

/* camera state */
extern int host_recording, host_busy;
#define RECORDING host_recording
int  job_state_ready_to_take_pic(void);
void msleep(int ms);
int  get_ms_clock(void);
void task_create(const char *name, int prio, int stack, void (*fn)(void *), void *arg);

/* menu */
struct menu_display_info { char value[64], rinfo[64], help[128]; };
struct menu_entry {
    const char *name;
    int *priv;
    int max;
    const char **choices;
    const char *help;
    void (*select)(void *priv, int delta);
    void (*update)(struct menu_entry *entry, struct menu_display_info *info);
    struct menu_entry *children;
};
#define MENU_SELECT_FUNC(f) void f(void *priv, int delta)
#define MENU_UPDATE_FUNC(f) void f(struct menu_entry *entry, struct menu_display_info *info)
#define MENU_SET_VALUE(...) snprintf(info->value, sizeof(info->value), __VA_ARGS__)
#define MENU_SET_RINFO(...) snprintf(info->rinfo, sizeof(info->rinfo), __VA_ARGS__)
#define MENU_SET_HELP(...)  snprintf(info->help, sizeof(info->help), __VA_ARGS__)
#define CHOICES(...) ((const char *[]) { __VA_ARGS__ })
#define MENU_EOL { 0 }
void menu_add(const char *where, struct menu_entry *e, int n);

/* module callbacks and keys */
#define COERCE(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define MODULE_CBRS_START()
#define MODULE_CBR(cbr, fn, ctx)
#define MODULE_CBRS_END()
#define MODULE_KEY_PRESS_HALFSHUTTER       ( 1)
#define MODULE_KEY_UNPRESS_HALFSHUTTER     ( 2)
#define MODULE_KEY_PRESS_FULLSHUTTER       ( 3)
#define MODULE_KEY_UNPRESS_FULLSHUTTER     ( 4)
#define MODULE_KEY_WHEEL_UP                ( 5)
#define MODULE_KEY_WHEEL_DOWN              ( 6)
#define MODULE_KEY_WHEEL_LEFT              ( 7)
#define MODULE_KEY_WHEEL_RIGHT             ( 8)
#define MODULE_KEY_PRESS_SET               ( 9)
#define MODULE_KEY_PRESS_UP                (12)
#define MODULE_KEY_PRESS_RIGHT             (15)
#define MODULE_KEY_PRESS_LEFT              (16)
#define MODULE_KEY_PRESS_DOWN              (19)
#define MODULE_KEY_UNPRESS_UDLR            (20)
#define MODULE_KEY_MENU                    (22)
#define MODULE_KEY_INFO                    (23)
#define MODULE_KEY_PLAY                    (24)
#define MODULE_KEY_TRASH                   (25)
#define MODULE_KEY_LV                      (28)
#define MODULE_KEY_Q                       (29)

/* the screen: drawn into a 720x480 buffer, see screen.c */
#define COLOR_WHITE  0x01
#define COLOR_BLACK  0x02
#define COLOR_ORANGE 0x13
#define COLOR_GRAY(percent) (38 + (percent) * 41 / 100)
#define FONT(font, fg, bg) (((font) & 0xFFFF0000) | ((bg) & 0xFF) << 8 | ((fg) & 0xFF))
#define FONT_DYN(id, fg, bg) FONT((id) << 16, fg, bg)
#define FONT_SMALL     FONT_DYN(0, COLOR_WHITE, COLOR_BLACK)
#define FONT_MED       FONT_DYN(2, COLOR_WHITE, COLOR_BLACK)
#define FONT_MED_LARGE FONT_DYN(3, COLOR_WHITE, COLOR_BLACK)
#define FONT_LARGE     FONT_DYN(4, COLOR_WHITE, COLOR_BLACK)
#define FONT_ALIGN_MASK   0x03000000
#define FONT_ALIGN_LEFT   0x00000000
#define FONT_ALIGN_CENTER 0x01000000
#define FONT_ALIGN_RIGHT  0x02000000
#define FONT_TEXT_WIDTH(width) ((((unsigned) (width) + 8) >> 4 << 26) & 0xFC000000u)
int  bmp_printf(uint32_t fontspec, int x, int y, const char *fmt, ...);
int  bmp_string_width(int fontspec, const char *str);
void bmp_fill(uint8_t color, int x, int y, int w, int h);
void fill_circle(int x, int y, int r, int color);
void clrscr(void);
int  gui_menu_shown(void);
void gui_stop_menu(void);
void menu_redraw(void);
uint32_t font_by_name(char *file, uint32_t fg, uint32_t bg);

#endif
