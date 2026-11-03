/*
 * Re-encode a JPEG with the engine's own encoder: baseline, no restart
 * markers, like Canon's CR2 previews. Desktop tools often add DRI, which
 * the streaming decoder refuses.
 *
 *   reencode in.jpg out.jpg
 */
#include <stdio.h>
#include <stdlib.h>
#include "../../engine/jpeg_dec.h"
#include "../../engine/jpeg_enc.h"

static int sink(void *ctx, const uint8_t *data, size_t len)
{
    return fwrite(data, 1, len, ctx) == len ? 0 : -1;
}

static int rows(void *ctx, const uint8_t *rgb, int nrows, int width)
{
    return se_jpeg_write_rows(ctx, rgb, nrows) == SE_OK ? 0 : -1;
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len);
    fread(buf, 1, len, f);
    fclose(f);

    se_jdec_info ji;
    if (se_jpeg_dec_info(buf, len, &ji) != SE_OK) return 1;

    size_t asz = 64 << 20;
    se_arena a;
    se_arena_init(&a, malloc(asz), asz);
    se_jpeg *enc = SE_NEW(&a, se_jpeg, 1);
    uint8_t *strip = SE_NEW(&a, uint8_t, (size_t) 8 * ji.width * 3);
    se_jpeg_exif ex = { "Canon", "Canon EOS 80D" };

    FILE *o = fopen(argv[2], "wb");
    if (se_jpeg_begin(enc, ji.width, ji.height, 92, strip, &ex, sink, o) != SE_OK) return 1;
    se_err rc = se_jpeg_decode(buf, len, &a, rows, enc, &ji);
    if (rc == SE_OK) rc = se_jpeg_end(enc);
    fclose(o);
    printf("%s: %dx%d, %s\n", argv[2], ji.width, ji.height, se_strerror(rc));
    return rc != SE_OK;
}
