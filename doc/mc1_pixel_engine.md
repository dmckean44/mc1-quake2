# MC1 pixel engine, version 1

A command-driven rasterizer that takes the per-pixel work of the Quake II
software renderer off the MRISC32-A1 CPU. The CPU still walks the BSP, sorts
edges into spans, lights surfaces, and transforms and clips models. The engine
fills world spans, builds lit surface-cache blocks, rasterizes opaque alias
model triangles and draws particles. Its arithmetic follows the software
renderer, so frames can be checked pixel for pixel against it.

Version 1 is "stage A". Stage B (world polygons instead of edge-sorted
spans) is outlined at the end.

A functional and timing model lives in the MRISC32 simulator
(`sim/pixel_engine.cpp`, enabled with `--pe`). The Quake II driver is in
`src/client/refresh/soft/sw_pe.c` and `header/pe.h`.

## Results in simulation

Quake II `demo1.dm2` timedemo at 320x240 with the low-poly models, 633
frames. CPU cycles are from `a1time` with the planned MiSTer memory path
(`--fill 8,8,24,4 --write 1,1,2,4 --wq 8`). Engine cycles are from the model
below. Frame rates assume a 96 MHz clock for both.

| | CPU, ideal | CPU, A1 model | CPU stalled on engine | frame | fps |
|---|---|---|---|---|---|
| software renderer | 5.22 M | 6.95 M | - | 6.95 M | 13.8 |
| pixel engine v1 | 1.96 M | 2.90 M | 0.01 M | 2.92 M | 32.9 |

The engine is busy 0.47 M cycles per frame (16%): spans 0.29 M, surface
blocks 0.11 M, triangles 0.06 M, particles 0.004 M. All rendered frames are
identical to the software renderer's; the only differences in the dumps are
console text and the clock.

The scripted gameplay benchmark (demo1 start, game logic running) drops from
4.02 M to 1.28 M ideal CPU cycles per frame.

With a 1024-word FIFO the CPU stalls 0.13 M cycles per frame, mostly while
`D_DrawSurfaces` issues a burst of span commands; 4096 words removes that.

## Registers

Base address `0xc0000200` (MMIO, uncached). All registers are 32 bits.

| offset | name | access | |
|---|---|---|---|
| 0x00 | ID | R | `0x50450001` ("PE", version 1) |
| 0x04 | CMD | W | command FIFO |
| 0x08 | SYNC | R | stalls the read until the engine is idle, returns the number of finished commands |
| 0x0c | WAIT | W | stalls the write until the number of finished commands is at least the value |
| 0x10 | DONE | R | number of finished commands (wraps at 2^32) |
| 0x18, 0x1c | BUSY lo/hi | R | engine busy cycles (performance counter) |
| 0x20, 0x24 | STALL lo/hi | R | cycles the CPU spent stalled on the engine |

The CPU stalls rather than polling: a full FIFO stalls writes to CMD, and
SYNC/WAIT stall the bus access until the condition holds.

## Commands

A command is a header word followed by its payload, all written to CMD:

    header = opcode << 24 | number_of_payload_words

Commands execute strictly in order. Each finished command increments DONE, so
the CPU can number its commands itself (the driver keeps `pe_submitted`) and
wait for any one of them, which also waits for everything before it.

The FIFO should hold at least 4096 words. A command larger than the FIFO
(only `SURFACE` can be) must be consumed while it is being written.

Payload data is copied by value, except for what the command points to: span
lists, textures, skins, the surface-cache block being written, and the tables
named in `TARGET`. The CPU must not change that memory until the command has
finished (see "Driver rules").

Pointers are byte addresses. Floats are IEEE single precision.

### 5: TARGET (5 words)

| word | |
|---|---|
| 0 | colour buffer base (8-bit pixels) |
| 1 | row stride in pixels; also the Z buffer stride |
| 2 | Z buffer base (signed 16-bit) |
| 3 | colormap (256 x 64 bytes, light level in bits 8..13 of the index) |
| 4 | alphamap (256 x 256 bytes) |

Sets render state used by all later commands. Sent once per frame.

### 6: SKIN (2 words)

| word | |
|---|---|
| 0 | skin base |
| 1 | skin width in texels |

Texture for later `TRIANGLE` commands.

### 1: SPANS (21 words)

Draws one surface's list of spans, as `D_DrawSpansPow2` plus `D_DrawZSpans`
(and `TurbulentPow2`, `D_FlatFillSurface`).

| word | |
|---|---|
| 0 | flags: 1 TEX, 2 Z, 4 TURB, 8 FLAT, bits 8..15 flat colour |
| 1 | first span: `struct { int u, v, count; span *next; }`, list ends at `next == 0` |
| 2 | texture base (surface-cache block, or the raw texture for sky and water) |
| 3 | texture row width |
| 4, 5 | sadjust, tadjust (16.16) |
| 6, 7 | bbextents, bbextentt (16.16 clamp limits) |
| 8..10 | s/z: origin, step per u, step per v (float) |
| 11..13 | t/z: origin, step per u, step per v (float) |
| 14..16 | 1/z: origin, step per u, step per v (float) |
| 17..19 | Z buffer gradient: origin, step per u, step per v (float) |
| 20 | turbulence table (CYCLE=128 ints), for TURB |

For each span, with `pos = v * stride + u`:

* **Z**: `izi = trunc(zgrad(u, v) * 2^31)`, saturated; pixel `i` gets
  `(izi + i * trunc(zstepu * 2^31)) >> 16`. Z is written for every pixel; the
  world is drawn first and has no overdraw.
* **FLAT**: fill `count` pixels with the flat colour.
* **TEX**: perspective-correct texturing in segments. The segment length is
  `2^shift`, chosen once per command: start at 4, and if
  `trunc(zistepu * 65536 * 16)` and the same for zistepv are both zero, grow
  it until either becomes non-zero (then use the previous value) or it
  reaches the stride. At each segment end compute `z = 65536 / (1/z)` and
  `s = trunc(s/z * z) + sadjust`, clamped to `[0, bbextents]` at the span
  start and `[2^shift, bbextents]` at segment ends (same for t). Steps are
  `(next - cur) >> shift` for full segments and `(next - cur) / (n - 1)` for
  the last one, which ends on its last pixel. The texel is
  `tex[(s >> 16) + (t >> 16) * width]`.
* **TURB**: as TEX, but s and t are reduced modulo `128 << 16` at each segment
  start and the texel is `tex[(tt << 6) + ss]` with
  `ss = ((s + turb[(t >> 16) & 127]) >> 16) & 63` and
  `tt = ((t + turb[(s >> 16) & 127]) >> 16) & 63`. Water uses a sine table
  offset by time, flowing non-warped surfaces an all-zero table.

### 2: SURFACE (10 + (numh+1)*(numv+1) words)

Builds a lit surface-cache block, as `R_DrawSurface` with the per-mip
`R_DrawSurfaceBlock8` routines.

| word | |
|---|---|
| 0 | texture base for this mip level |
| 1 | texture width |
| 2 | texture height (rows before wrapping back) |
| 3 | starting texture column |
| 4 | starting texture row address |
| 5, 6 | numh, numv: blocks across and down |
| 7 | level: blocks are `2^level` texels square (4 at mip 0 ... 1 at mip 3) |
| 8 | destination (surface-cache block data) |
| 9 | destination row stride |
| 10... | light values at block corners, row by row, `numh + 1` per row |

Blocks are drawn column by column. In a block with corner lights `lt, rt` (top)
and `lb, rb` (bottom), row `i` interpolates `left = lt + i * ((lb - lt) >> level)`
and `right` likewise, and texel `b` of that row gets

    light = left + b * ((right - left) >> level)
    out = colormap[(light & ~0xff) | texel]

The source wraps vertically after `height` rows (checked after each block)
and horizontally after `width` columns (checked after each column).

### 3: TRIANGLE (18 words)

An opaque, Gouraud-lit, affine-textured alias model triangle, as
`R_DrawTriangle` with `R_PolysetDrawSpans8_Opaque`.

Three vertices of six words each: `u, v` (screen pixels), `s, t` (16.16 skin
coordinates), `l` (light, colormap row in bits 8..13), `zi` (1/z, 16.16; the
Z buffer holds `zi >> 16`).

Triangles with `(u0-u2)(v0-v1) - (u0-u1)(v0-v2) >= 0` are back-facing and
skipped (the CPU already culls them). The rasterizer uses the edge tables,
`FloorDivMod` edge stepping and gradient setup of the software renderer
(`R_PolysetCalcGradients`: one reciprocal and a handful of multiplies per
triangle, light steps rounded up). A pixel is written where
`(zi >> 16) >= zbuffer`, and its Z is written too.

### 4: PARTICLE (6 words)

| word | |
|---|---|
| 0, 1 | u, v: top-left corner |
| 2 | Z value (16-bit) |
| 3 | size in pixels |
| 4 | colour |
| 5 | blend: 0 opaque, 1 33% (`alphamap[colour + dst * 256]`), 2 66% (`alphamap[colour * 256 + dst]`) |

Nothing is drawn if the Z buffer at `stride * size / 2 + size / 2` from the
corner is greater than the particle's Z. Otherwise each pixel with
`zbuffer <= Z` is drawn and gets the particle's Z.

## Arithmetic

The model reproduces the software renderer bit for bit, including float
rounding of the gradient and 1/z work and C's truncating conversions. A
hardware version only needs to be close: a 1-ulp difference moves a texel
coordinate by a fraction of a texel. Integer stepping, clamps and table
lookups should be exact, since they decide which memory is read.

Per segment the span unit needs one 1/z reciprocal and two multiplies; per
triangle one reciprocal and about twelve multiply-adds. A single pipelined
float unit shared between them is enough at the rates below.

## Timing model

Engine time in the simulator is counted in engine clocks, at the CPU clock.
The simulator counts ideal CPU cycles, so CPU time is taken as
`cycles * cpi` with `cpi` from `a1time` (1.48 for this build). The CPU is
stalled when the FIFO is full and on WAIT/SYNC.

| item | cycles |
|---|---|
| command | 4 (TARGET, SKIN: 1) |
| span | 6 |
| span segment | 2 |
| textured or flat pixel | 1 |
| Z-only pixel | 0.5 |
| surface block | 1 per row + 0.5 per texel |
| triangle | 30 + 3 per scan line + 1 per pixel |
| particle | 8 + 1 per pixel |
| texture cache miss | 24 (8 KB direct-mapped, 32-byte lines) |

Assumed structure: colour and Z writes go to VRAM through write-combining
buffers; texels come through a small cache from SDRAM; the colormap and
alphamap are copied into block RAM so a lookup costs no memory access.
Simulator options: `--pe-cpi`, `--pe-miss`, `--pe-fifo`, plus `--pe-defer`
(run commands only when the CPU waits, to expose missing synchronization) and
`--pe-nostall` (keep simulated cycles equal to trace records for `a1time`).

## Driver rules

The engine reads memory after the CPU has moved on, so the driver waits before
it reuses or touches that memory. In the Quake II driver:

* **Frame end**: wait for everything before the page flip, frame dumps,
  screenshots and any 2D drawing (`RE_Draw_*`).
* **CPU drawing**: wait before drawing on the CPU what the engine does not
  draw: translucent surfaces, sprites and beams (`R_DrawPoly`), translucent,
  shell or IR-goggle models, custom particles and the underwater warp.
* **Span buffer**: wait for the last `SPANS` command before refilling the
  edge span buffer (frame start, and on overflow in `R_ScanEdges`). With the
  engine present the buffer is 64 spans per screen column, which holds a
  whole frame.
* **Surface cache**: every cache block records the last command that reads or
  writes it (`pe_fence`). `D_SCAlloc` waits for it before reusing a block's
  memory; flushing the cache and freeing buffers wait for everything.
* **Registration and mode changes**: wait for everything.

## Stage B

With stage A the CPU spends about 1.6 M of its 2.9 M cycles per frame on the
world's edge list: clipping edges (`R_ClipEdge`), sorting them and generating
spans (`R_GenerateSpans`, `R_ScanEdges`), and BSP traversal. Stage B replaces
that with Z-buffered world polygons: the CPU walks the BSP front to back,
culls and clips faces and sends their vertices; the engine sets up gradients,
rasterizes with an early Z test, and keeps building surface-cache blocks as in
stage A.
