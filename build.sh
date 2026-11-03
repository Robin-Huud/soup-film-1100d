#!/bin/sh
# Builds Magic Lantern for the Canon EOS 1100D (firmware 1.0.5) with the
# Soup module, and stages the result as an SD card in sdcard/.
#
#   ./build.sh
#
# Needs git and Docker. Magic Lantern itself is not changed: the module is
# copied into a checkout of the commit below and built there.
set -eu
ROOT="$(cd "$(dirname "$0")" && pwd)"
ML_REPO="${ML_REPO:-https://github.com/reticulatedpines/magiclantern_simplified}"
ML_COMMIT=142472e95054d73640f50789ac0fd66325499d82
PLATFORM=1100D.105

[ -d "$ROOT/ml/.git" ] || git clone "$ML_REPO" "$ROOT/ml"
git -C "$ROOT/ml" checkout -q "$ML_COMMIT"
rm -rf "$ROOT/ml/modules/soup"
cp -R "$ROOT/module" "$ROOT/ml/modules/soup"

docker build -q -t soup-film-build "$ROOT" >/dev/null
docker run --rm -v "$ROOT":/work -w /work soup-film-build sh -ec "
  make -C ml/platform/$PLATFORM -j\$(nproc)
  make -C ml/modules/soup
  rm -rf sdcard.new
  cp -r ml/platform/$PLATFORM/build/zip sdcard.new
  cp ml/modules/soup/build/soup.mo sdcard.new/ML/modules/
  mkdir -p sdcard.new/ML/FILM
  cp module/film/* sdcard.new/ML/FILM/"

rm -rf "$ROOT/sdcard"
mv "$ROOT/sdcard.new" "$ROOT/sdcard"
# the film screen's fonts, if made (fonts/make-fonts.sh)
for f in "$ROOT"/fonts/SOUP*.RBF; do [ -f "$f" ] && cp "$f" "$ROOT/sdcard/ML/fonts/"; done
echo "SD card files are in sdcard/"
