# Contributors

This project stands on other people's work.

## Magic Lantern

The firmware add-on this runs in: the boot loader, the 1100D port, the menu,
the module system, the fonts in `sdcard/ML/fonts` and everything else in
`sdcard/` except the Soup module and stocks.

- Started by Trammell Hudson, and developed for years by the Magic Lantern
  community. The full history and contributor list are in the project's git
  log.
- This build uses **magiclantern_simplified**, maintained by the
  reticulatedpines team: https://github.com/reticulatedpines/magiclantern_simplified
  (commit `142472e95054d73640f50789ac0fd66325499d82`, unmodified).
- GPL v2.

## Soup Engine

The film engine in `module/engine/`, and the film stocks' format and looks.

- By **jiijii0**, written for the Canon 5D: https://github.com/jiijii0/soup-engine
  (commit `c6bede518eccd0b792f83718337ef3add2cf8ee5`, used unchanged).
- GPL v2 (`module/engine/LICENSE`).

## CHDK

The RBF bitmap font format that Magic Lantern and `module/tools/mkrbf.swift`
use comes from the CHDK project: https://chdk.fandom.com

## This port

- The Magic Lantern module, the 1100D work, the date stamp, the film screen
  and the font tool were written by the project owner together with
  **Claude** (Anthropic's AI model), and tested on a real EOS 1100D.
