#!/bin/sh
# Makes the film screen's fonts on a Mac, from a font installed there, and
# puts them in sdcard/ML/fonts. They are not in the repository because they
# are renders of a commercial typeface; without them the screen uses Magic
# Lantern's own fonts.
#
#   fonts/make-fonts.sh                       # Avenir Next Demi Bold
#   fonts/make-fonts.sh "Futura-Medium"       # or any installed font
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
FONT="${1:-AvenirNext-DemiBold}"
swiftc -O "$HERE/../module/tools/mkrbf.swift" -o "$HERE/mkrbf"
"$HERE/mkrbf" "$FONT" 24 "$HERE/SOUP24.RBF"
"$HERE/mkrbf" "$FONT" 40 "$HERE/SOUP40.RBF"
mkdir -p "$HERE/../sdcard/ML/fonts"
cp "$HERE/SOUP24.RBF" "$HERE/SOUP40.RBF" "$HERE/../sdcard/ML/fonts/"
echo "Copy sdcard/ML/fonts/SOUP24.RBF and SOUP40.RBF to ML/fonts on the card."
