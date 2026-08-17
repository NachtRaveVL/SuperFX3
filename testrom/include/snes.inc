; SNES CPU and PPU registers used by the diagnostic supervisor.

INIDISP  = $2100
BGMODE   = $2105
BG1SC    = $2107
BG2SC    = $2108
BG12NBA  = $210B
BG1HOFS  = $210D
BG1VOFS  = $210E
BG2HOFS  = $210F
BG2VOFS  = $2110
VMAIN    = $2115
VMADDL   = $2116
VMDATAL  = $2118
CGADD    = $2121
CGDATA   = $2122
TM       = $212C
TS       = $212D
CGWSEL   = $2130
CGADSUB  = $2131
COLDATA  = $2132
NMITIMEN = $4200
MDMAEN   = $420B
HDMAEN   = $420C
RDNMI    = $4210
HVBJOY   = $4212
JOY1L    = $4218

DMAP0    = $4300
BBAD0    = $4301
A1T0L    = $4302
A1B0     = $4304
DAS0L    = $4305

DMAP5    = $4350
BBAD5    = $4351
A1T5L    = $4352
A1B5     = $4354

DMAP6    = $4360
BBAD6    = $4361
A1T6L    = $4362
A1B6     = $4364

DMAP7    = $4370
BBAD7    = $4371
A1T7L    = $4372
A1B7     = $4374

JOY_B      = $8000
JOY_Y      = $4000
JOY_SELECT = $2000
JOY_START  = $1000
JOY_UP     = $0800
JOY_DOWN   = $0400
JOY_LEFT   = $0200
JOY_RIGHT  = $0100
JOY_A      = $0080
JOY_X      = $0040
JOY_L      = $0020
JOY_R      = $0010

TEXT_MAP_WRAM = $7E2000
BG1_MAP_WRAM  = $7E2800
TEXT_MAP_BYTES = $0800
BG1_MAP_BYTES  = $0800

BG1_MAP_VRAM = $0000
BG2_MAP_VRAM = $0800
BG1_TILE_VRAM = $1000
BG2_FONT_VRAM = $4000

; BG1 visual output uses tile 0. Every other map cell points at a permanently
; blank tile 1 so a test tile cannot accidentally carpet the entire screen.
BG1_BLANK_TILE = $0001
BG1_VISUAL_COL = 2
BG1_VISUAL_ROW = 15
BG1_VISUAL_MAP_OFFSET = ((BG1_VISUAL_ROW * 32 + BG1_VISUAL_COL) * 2)
