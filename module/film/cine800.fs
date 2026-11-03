# CineStill 800T: tungsten cinema film with the anti-halation layer removed.
#
# Balanced for tungsten, so daylight goes cool and teal while lamps stay
# neutral-warm. The famous part is the red glow round every light source:
# with no remjet layer, halation is strong and red. Lifted, teal shadows.
VERSION   3
NAME      CINE 800T
DESC      TEAL SHADOWS, RED GLOW AROUND LIGHTS, NIGHT FILM
WB        AS_SHOT
CURVE     0,14 48,52 128,130 200,204 255,246
CURVE_R   0,4 64,64 128,128 192,196 255,252
CURVE_G   0,10 128,130 255,248
CURVE_B   0,22 64,86 128,146 192,204 255,252
SAT       104
HUE_SAT   112,88,82,118,124,92
HUE_ROT   6,-10,-14,4,-10,-20
HUE_LUM   102,98,88,104,108,100
SPLIT_SH  -20,6,22
SPLIT_HI  10,4,-6
HALATION  200 220 36
HAL_TINT  255,40,20
GRAIN     34
GRAIN_SIZE 36
