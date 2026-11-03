Soup Engine
===========

Film simulation on every shot. After each photo, the embedded full-size
JPEG preview in the CR2 (or the JPEG itself) is graded with a film stock
and saved next to it as FLM_xxxx.JPG.

Port of Soup Engine (5D Classic) to an ordinary ML module. The engine in
engine/ is upstream's, unchanged; soup.c is the camera glue.

Stocks are loaded from ML/FILM/INDEX.TXT. Make new ones with Soup Stocks.

Not for upstream ML: written with an LLM, see LLM_POLICY.md.

:Author: jiijii0 (engine); camera port (80D, 1100D) written with Claude
:License: GPL
:Summary: Film simulation JPEG for every shot
:Website: https://github.com/jiijii0/soup-engine
