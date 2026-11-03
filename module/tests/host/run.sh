#!/bin/sh
# Build and run the host test. Given a 6000x4000 JPEG, runs the test suite
# on it (a CR2 wrapper is made here). Given a camera CR2, develops it
# through every stock instead.
#
#   tests/host/run.sh test.jpg
#   tests/host/run.sh IMG_1234.CR2
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
MOD="$HERE/../.."
OUT="$HERE/out"

rm -rf "$OUT" && mkdir -p "$OUT/card/ML/FILM" "$OUT/card/ML/LOGS" "$OUT/card/DCIM/STOCKS"
cp "$MOD"/film/* "$OUT/card/ML/FILM/"

ENGINE="se wb cr2 color lut stock grade look jpeg_dec jpeg_enc"
SRC=""; for f in $ENGINE; do SRC="$SRC $MOD/engine/$f.c"; done

cc -std=gnu99 -O2 -g -Wall -Wno-unused-parameter -Wno-unused-function -DSE_HOSTED \
   -I"$HERE/include" -fsanitize=address,undefined \
   -o "$OUT/harness" "$HERE/harness.c" $SRC -lm -lpthread

# A camera file is developed through every stock; outputs in out/card/DCIM/STOCKS
case "$1" in
  *.CR2|*.cr2)
    exec "$OUT/harness" "$OUT/card" "$1" ;;
esac

# Canon's previews have no restart markers; desktop encoders often add them
cc -std=gnu99 -O2 -DSE_HOSTED -o "$OUT/reencode" "$HERE/reencode.c" $SRC -lm
"$OUT/reencode" "$1" "$OUT/test.jpg"
python3 "$HERE/make_cr2.py" "$OUT/test.jpg" "$OUT/test.CR2"

"$OUT/harness" "$OUT/card" "$OUT/test.jpg" "$OUT/test.CR2"
