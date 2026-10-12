# MC1 pixel engine, version 7

A command-driven rasterizer that takes the per-pixel work of the Quake II
software renderer off the MRISC32-A1 CPU. Its arithmetic follows the software
renderer, so frames can be checked pixel for pixel against it.

* Version 1 ("stage A"): the CPU still walks the BSP, sorts edges into spans,
  lights surfaces, and transforms and clips models. The engine fills world
  spans, builds lit surface-cache blocks, rasterizes opaque alias model
  triangles and draws particles.
* Version 2 ("stage B") adds world polygons: the CPU no longer sorts edges or
  generates spans. It clips and projects each visible face's edges and sends
  the faces front to back; the engine fills each one where no earlier face has
  drawn, using a coverage bitmap.
* Version 3 lights surface-cache blocks in the engine, from the lightmap
  samples and the dynamic lights, and can hold a block's build back until the
  face that uses it draws a pixel, so covered faces cost neither CPU nor
  engine time.
* Version 4 gives each dynamic light in LITSURF a brightness, for games whose
  lights are coloured (Heretic II lights with the brightest channel).
* Version 5 reads alias model triangles from memory (MESH), so the CPU only
  transforms the vertices and handles the triangles that need clipping.
* Version 6 draws sprite polygons (SPRITE): perspective-textured spans with
  opaque, 33%, 66% or additive blending, as Heretic II's sprites need.
* Version 7 adds turbulent SPRITE polygons, so translucent world surfaces
  (water, glass) are drawn by the engine too.

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
| stage A (`sw_pe 1`) | 1.81 M | 2.72 M | 0.01 M | 2.73 M | 35.2 |
| stage B (`sw_pe 2`) | 1.44 M | 2.25 M | 0.004 M | 2.25 M | 42.7 |

Both rows use a version 3 engine and include the CPU-side changes (projected
vertex cache, vectorized alias vertex transform). Stage B on a version 2
engine, before those changes, ran at 2.52 M cycles per frame (38.2 fps).

Engine busy per frame in stage B: 0.49 M cycles (22%): polygons with the
surface blocks they build 0.42 M, triangles 0.06 M, particles 0.004 M. Of
the 93 surface-block builds requested per frame, 23 are drawn; the faces of
the other 70 turn out to be covered.

Stage A's frames are identical to the software renderer's. Stage B's are too,
except for a few pixels (8 at most in a frame, in 4 of 18 checked frames)
where two faces of the same brush model share a BSP leaf and overlap; the edge
sorter orders those by 1/z at each span start, stage B by their nearest
vertex. The other differences in the dumps are console text and the clock.
The same holds for the scripted gameplay benchmark (demo1 start, game logic
running), which drops from 4.02 M ideal CPU cycles per frame to 0.91 M in
stage B.

The command FIFO matters once the CPU is this fast: stage B sends a whole
frame's world (about 9 K words) in a burst. With 4096 words the CPU stalls
0.13 M cycles per frame, with 8192 0.03 M, with 16384 none.

## Registers

Base address `0xc0000200` (MMIO, uncached). All registers are 32 bits.

| offset | name | access | |
|---|---|---|---|
| 0x00 | ID | R | `0x5045` ("PE") << 16 \| version; the model reports version 7 |
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

The FIFO should hold at least 16384 words (64 KB). It need not be block RAM: a
ring buffer in DDR3 that the engine streams from is fine, since commands are
only about 3 MB/s. A command larger than the FIFO (only `SURFACE` can be)
must be consumed while it is being written.

Payload data is copied by value, except for what the command points to: span
lists, textures, skins, the surface-cache block being written, and the tables
named in `TARGET`. The CPU must not change that memory until the command has
finished (see "Driver rules").

Pointers are byte addresses. Floats are IEEE single precision.

### 7: COVER (5 words), version 2

| word | |
|---|---|
| 0, 1 | x0, y0: view rectangle top left |
| 2, 3 | x1, y1: view rectangle bottom right (exclusive) |
| 4 | fixed-point shift of POLY edge positions (20, or 18 above 2048 pixels) |

Clears the coverage bitmap of the view rectangle: one bit per pixel, held in
the engine (9.4 KB at 320x240). Sent once per frame before the world.

### 8: POLY (21 + 3n words), version 2

A world face, drawn into the pixels inside it that are not yet covered, which
it then covers. Faces arrive front to back, so this resolves visibility
exactly like the edge sorter for the static world.

Words 0..20 are as in SPANS, except that word 1 is the number of edges `n`.
Each edge is three words:

| word | |
|---|---|
| 0 | first scan line (bits 0..14), 1 if a left edge (bit 15), last scan line (bits 16..31) |
| 1 | u at the first scan line, fixed point, rounded up (`edge_t.u`) |
| 2 | u step per scan line (`edge_t.u_step`) |

These are the screen edges `R_EmitEdge` computes after clipping and
projection; horizontal edges are left out. On each scan line the edges that
cover it are sorted by u (left edges first at equal u); a span opens where
the count of open left edges reaches 1 and closes when it returns to 0, at
`u >> shift`. A span left open at the end of the line runs to x1: faces
clipped by the right side of the view have no right edge. Inside each span,
every run of uncovered pixels is drawn as a SPANS span starting at that run.

### 9: FILL (21 words), version 2

Words as in SPANS (word 1 unused). Draws every uncovered pixel of the view
rectangle, in runs as for POLY. Used last, for the background colour and Z.

### 10: LITSURF (17 + 4k or 17 + 5k words), version 3

Builds a surface-cache block like SURFACE, but computes the corner lights
itself, as `R_BuildLightMap` with `r_colorlight 0`.

| word | |
|---|---|
| 0..9 | as SURFACE |
| 10 | lightmap samples (RGB bytes, one map after another, `(numh+1)*(numv+1)` samples per map) |
| 11 | number of light styles (0..4) |
| 12..15 | scale per style (8.8 fixed point) |
| 16 | number of dynamic lights `k` (bits 0..15); bit 16: lights carry a scale (version 4) |
| 17... | per dynamic light: radius, cutoff, s, t (floats; position in lightmap space), then the scale (float) if bit 16 is set |

For each sample: `sum = Σ max(r, g, b) * scale` over the styles (unsigned
integer). Then each dynamic light adds to the samples within its cutoff, as
`R_AddDynamicLights` does with integer distances on the 16-texel lightmap
grid and a float add: `sum = (unsigned)((float)sum + (radius - dist) * scale)`,
where `scale` is 256 when the lights carry none.
Finally `light = max(64, (255 * 256 - max(sum, 0)) >> 2)`. The engine needs
scratch for at least 1024 samples; lit surfaces are at most 17 x 17.

The CPU keeps negative lights, coloured light, `r_fullbright` and maps without
light data.

### 11: DEFER (1 word), version 3

| word | |
|---|---|
| 0 | address of a flag word |

The next SURFACE or LITSURF is not run. It is held until the next POLY: if
that POLY draws at least one pixel, the engine runs the held build first and
writes 1 to the flag; otherwise the build is dropped and the flag is left
alone. The driver clears the flag when it sends the build and treats a block
whose flag is still 0 as not built.

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

### 12: MESH (6 words), version 5

The triangles of one alias model mesh, read from memory and drawn as
TRIANGLE commands would draw them.

| word | |
|---|---|
| 0 | vertex records |
| 1 | vertex record size in bytes |
| 2 | triangles: 12 bytes each, three 16-bit vertex indices then three 16-bit texture coordinate indices |
| 3 | triangle list: 16-bit indices into the triangles |
| 4 | number of entries in the triangle list |
| 5 | texture coordinates: two signed 16-bit values (s, t in skin texels) each |

A vertex record holds `u` at byte 0, `v` at 4, the light at 16, `zi` at 28
and clip flags at 32 (the Quake II/Heretic II `finalvert_t`). Triangles with
any clip flag set on a vertex are skipped; the CPU draws those. The others
get `s << 16`, `t << 16` from the texture coordinates and go through the
TRIANGLE rasterizer, back-face test included.

### 13: SPRITE (19 or 83 words), version 6

A polygon's span list drawn as `R_PolygonDrawSpans` with 16-pixel affine
spanlets (not the turbulent path), and each pixel as Heretic II's
`R_DrawSpanletBlend`.

| word | |
|---|---|
| 0 | span list (`u, v, count, next` words per span; ends with count `INT_MIN`) |
| 1 | texture base |
| 2 | texture width in texels |
| 3..6 | sadjust, tadjust, bbextents, bbextentt (16.16) |
| 7..15 | s/z, t/z and 1/z: origin, step u, step v each (floats) |
| 16 | blend in bits 0..1 (0 opaque, 1 33%, 2 66%, 3 additive); bit 2: no depth test; bit 3: turbulent (version 7) |
| 17 | 1 if a colour remap follows in words 19..82 |
| 18 | additive table (256 x 256 bytes, `table[colour * 256 + dst]`), or for turbulent polygons the turbulence table (128 ints) |
| 19..82 | colour remap, 256 bytes |

A turbulent polygon is drawn as `R_PolygonDrawSpans` with `iswater` set: s and t
are not clamped, are masked to 128 texels at the start of each spanlet, and the
texel is `tex[((s + turb[(t >> 16) & 127]) >> 16 & 63) + (((t + turb[(s >> 16) & 127]) >> 16 & 63) << 6)]`
with no transparent colour, as `R_DrawSpanletTurbulentBlended33/66`.

Texel 255 is transparent. Unless bit 2 is set, a pixel is drawn only where
`zbuffer <= izi >> 16`. Opaque pixels write Z; blended ones do not. The
remap is copied by value so the driver can reuse its tables at once.

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

## Memory coherency

The CPU reads memory the engine writes: the frame and Z buffers (2D drawing,
translucent surfaces and sprites, screenshots), surface-cache blocks
(translucent surfaces) and DEFER flags. The A1's data cache is write-through,
so its lines can be stale but never dirty. The engine must therefore
invalidate any CPU data cache line it writes to, for example by sending its
write addresses to the cache's tag port. The driver waits for the engine
before such reads but does not flush the cache.

Likewise the engine's texture cache must not keep lines of a surface-cache
block it is rebuilding.

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
`cycles * cpi` with `cpi` from `a1time` (1.56 for stage B in this build).
The CPU is stalled when the FIFO is full and on WAIT/SYNC.

| item | cycles |
|---|---|
| command | 4 (TARGET, SKIN, COVER, DEFER: 1) |
| span | 6 |
| span segment | 2 |
| textured or flat pixel | 1 |
| Z-only pixel | 0.5 |
| surface block | 1 per row + 0.5 per texel |
| triangle | 30 + 3 per scan line + 1 per pixel |
| MESH triangle fetch | 12 |
| sprite | 6 per span + 2 per spanlet + 1 per pixel |
| particle | 8 + 1 per pixel |
| polygon | 2 per edge + 2 per covered scan line + 1/16 per pixel tested, plus its spans |
| lighting | 1 per sample per style or dynamic light |
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
* **Sprite spans** (SPRITE): give each sprite its own part of the span pool,
  and wait for the last SPRITE command before reusing the pool from the
  start.
* **Model vertices** (MESH): give each model its own part of the vertex
  buffer, and wait for the last MESH command before reusing the buffer from
  the start.
* **Span buffer**: wait for the last `SPANS` command before refilling the
  edge span buffer (frame start, and on overflow in `R_ScanEdges`). With the
  engine present the buffer is 64 spans per screen column, which holds a
  whole frame.
* **Surface cache**: every cache block records the last command that reads or
  writes it (`pe_fence`). `D_SCAlloc` waits for it before reusing a block's
  memory; flushing the cache and freeing buffers wait for everything. With a
  version 3 engine, blocks for world faces in stage B are sent with DEFER and
  carry the flag (`pe_valid`); `D_CacheSurface` rebuilds a block whose flag
  is 0. Blocks for translucent surfaces are always built at once.
* **Registration and mode changes**: wait for everything.

For stage B (`sw_pe 2`, version 2 engines; `sw_draworder` and `sw_drawflat`
fall back to stage A):

* `R_EmitEdge` and `R_EmitCachedEdge` record each face's screen edges
  instead of inserting them into the edge tables, and `R_ScanEdges` is
  replaced by `PE_DrawWorld`.
* Order: world faces are already in BSP key order. Brush model faces are
  sorted by key (the key of their leaf) and merged in. The sky box faces come
  after all others (their key is the largest), then FILL with the background.
* All faces that survive clipping are sent, including ones the engine will
  find fully covered. With version 3 their surface-cache blocks are only
  built if they draw.

## Where the CPU time goes now

Stage B, A1 model, per frame: clipping and projecting edges 0.56 M
(`R_ClipEdge`, `R_RenderFace`, `R_ProjectVertex`, `TransformVector`), BSP
traversal and culling 0.32 M, alias models 0.31 M (vertex transform and the
triangle loop), per-face setup and submission 0.16 M, particles 0.14 M,
alias triangle submission 0.09 M, surface-cache bookkeeping 0.06 M.
