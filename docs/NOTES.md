# Reverse-engineering notes

What was recovered from PS3 system software 4.93. Addresses refer to
`dev_flash/vsh/module/qgl_canyon_app.sprx` after decryption
(`rpcs3 --decrypt`) and relocation with `tools/re/prxdis.py`.

## Where the visualizer lives

| File | Contents |
| --- | --- |
| `vsh/module/soundvisualizer_plugin.sprx` | XMB page that hosts a visualizer |
| `vsh/module/qglbase.sprx` | "QGL" engine: shader/texture/FBO wrappers, menus, math |
| `vsh/module/qgl_canyon_app.sprx` | the canyon visualizer (this project) |
| `vsh/module/qgl_gaia_app.sprx` | the Earth visualizer |
| `vsh/resource/qgl/canyon.qrc` | canyon shaders, presets, normal map |
| `vsh/resource/qgl/earth.qrc` | Earth textures, atmosphere tables, camera paths, SPU jobs |
| `vsh/resource/qgl/lines.qrc` | the XMB background wave ("moyou"), same QGL format |

## QRC containers

- `"QRCC"`, u32 BE uncompressed size, then a zlib stream containing a `"QRCF"` file
  (some files are stored as a bare QRCF).
- QRCF header (all fields BE): `+0x08` tree offset, `+0x10` id-string table,
  `+0x18` tag/attribute-name table, `+0x30` data block (each an offset/size pair).
- The tree is XML-like. Each element is `tag, attrCount, parent, prev, next, firstChild,
  lastChild`, then `attrCount × {nameOff, type, v1, v2}`. File elements have a `src`
  attribute (type 6: data offset, size) and an `id` attribute (type 7: offset into the
  id table; the string starts 4 bytes after it).

`*.mnu` files are text: `#MNU_1.0`, then `NAME:float:value` lines.
`canyon/CANYON_*.mnu` hold the defaults; `override/<preset>/canyon/*.mnu` override them.

## CgBinaryProgram (.vpo / .fpo)

Standard PS3 Cg binary: profile (`0x1b5b` VP, `0x1b5c` FP), parameter table (48-byte
entries: type, resource, variability, resIndex, name, default, embedded-constant list,
semantic, direction, ...) and microcode.

Vertex microcode is 4 BE words per instruction (NV40 layout, as in RPCS3's
`RSXVertexProgram.h`). **Gotcha:** vector `ADD` reads operands 0 and **2**; `MAD` reads
0, 1, 2; scalar ops read operand 2. Uniforms are constant registers (`c[resIndex]`).

Fragment microcode has each 32-bit word's 16-bit halves swapped. Uniforms are not
registers: each one is an inline 16-byte literal that follows the instruction using it,
and the parameter's embedded-constant list gives the ucode offsets to patch. The
translator turns those slots into named GLSL uniforms. Samplers come from parameters with
resource `TEXUNIT0+n`; type 1067 is a 3D sampler (`_Tex` in `canyon_layer_blur`).

## Canyon terrain

128 × 128 quads (4 verts each, 24-byte vertices of 12 halves). Index in vertex:

| halves | attribute (RSX input) | contents |
| --- | --- | --- |
| 0–3 | position (0) | x = column − 64 + slide, y = smoothed height, z, w = target height |
| 4–7 | normal (2) | normal xyz, w = rand·π (static) |
| 8–11 | texcoord0 (8) | i/127, j/127, z − 50·(1 − i/64)² ("curved z"), rand·500 per quad |

The quads form a ring of 128 strips; `ring` (object `+0x21c`) is the slot written next.

**Spectrum → columns** (`0xd1f8`): the audio callback fills `{u32; float L[256]; float R[256]}`.
Columns 0–63 take `L[floor(i/64·255)]`, columns 64–127 take `R[floor(255 − i/64·255)]`.

**Per update** (`0x943c`, `dt`, columns):
1. `spec[i] *= expf(TERRAIN_WIDTH · (1 − 2·|0.5 − i/128|))`, then divide all by `max(1, max)`.
2. `scroll += dt · POS_SPEED`. If `rowBase < scroll`: `rowBase += 1` and generate a row.
   If `scroll > 99`, rebase everything by `−scroll` (keeps half floats precise).

**Row generation** (`0x61d4`), with three profiles A (oldest), B, C of `{pos, normal}`:
- `x = i/64 − 1`, `c = clamp((|x − VALLEY_CENTRE| − VALLEY_WIDTH)·VALLEY_SHARP, 0, 1)`
- `target = smoothstep(c)·VALLEY_HEIGHT + spec[i] + (rand/2 + 0.5)·0.02`
- `k = 0.7 + 0.295·|x|`, `vel += (target − B.y)(1 − k)`, `C = (i + slide, B.y + vel, 1, target)`, `vel *= k/2`
- 5 relaxation passes: `y += (y[i−1] + y[i+1] − 2y)/4` (clamped neighbours)
- `B.normal = normalize(cross(B[i−1] − B[i+1], C[i] − A[i]))`
- write A into ring slot `ring` with z ∈ [rowBase + 127, rowBase + 128], then A←B, B←C

Drawing skips the slot about to be overwritten and the newest one.

## Uniforms (terrain pass)

| uniform | value |
| --- | --- |
| `_Line` | (LINE WIDTH, LINE HEIGHT, LINE LENGTH, LINE BIAS) |
| `_Time` | (LINE AHEAD + s, TIME AHEAD + s, 126.9 + s, s), s = scroll |
| `_Scale` | (SCALE X, 0.5·0.3·TERRAIN HEIGHT·(1 + cos(pulse)), 1, 0); pulse is always 0 |
| `_TexScale`, `_BumpScale` | TEXTURE SCALE, BUMP SCALE |
| `_Fog_min`, `_Fog_max_min` | FOG MIN, FOG MAX − FOG MIN |
| `_FogColour`, `_LineColour` | FOG RGB, LINE RGB, times FADE COLOUR² |
| `_Eye`, `_MVM` | camera position, projection·view (rows) |
| `_FresLUT` | 512×1 ramp: `pow(1 − k/511, COLOUR RGB)·SCALE + BIAS`, times FADE COLOUR² |

In the vertex shader, `Time.z` clips everything beyond the newest row (the reveal edge).
`|z − Time.x| / Line.x` gives a glowing band ("the line") whose height is modulated by
`Line.y·cos(dist·Line.z)`. Where curved z exceeds `Time.y`, quads fly apart by
`rand·500·d²`; this makes the floating-tile presets.

Camera: eye = (POS X, POS Y, POS Z + s), target = (TARGET X/Y/Z, + s on z),
up = +y, vertical FOV = ZOOM degrees, near 0.1, far 10000.

## Post-processing

1. Blur amount (`canyon_blur_ammount`): terrain again, writing `|dist − FOCAL DEPTH| / FOCAL VARIANCE`.
   The sky clears to SKY BLUR.
2. Five passes of `canyon_layer_blur_blur` (4-tap box) fill the 5 slices of a 3D texture.
3. `canyon_layer_blur`, blended (SRC_ALPHA, 1 − SRC_ALPHA) over the sharp frame:
   - Picks a blur slice from `sat(amount·0.75 + LOD)` and jitters it with noise.
   - Mixes in the previous output (`_Front`, scaled by FEEDBACK X/Y, rotated by
     FEEDBACK ROT) with weight `_Feedback`.
4. `_Feedback = FEEDBACK · fade`. The fade (`T768`) is 1 from construction and while running
   (states 0/2, set by `0xaa5c`); `0xaa20` switches to state 1, which ramps it from 1 to 0 over
   100 frames (state 3 then holds 0), i.e. the shut-down fade. So the trails are a steady-state
   effect: every frame mixes in the previous output, scaled by FEEDBACK X/Y and rotated by
   FEEDBACK ROT (radians; canyon04/12 use −π, a mirrored copy).

The menu "apply" functions show which preset value lands in which object field:
`0x12d10` (LINE), `0x13460` (TERRAIN), `0x13b38` (BLUR), `0x1425c` (CAMERA),
`0x14994` (COLOUR) and `0x14ecc` (FOG).

## Imports

PRX imports are by NID: `sha1(name + 6759659904250490566427499489741A)[0:4]` read
little-endian. That identifies libc calls (`0xfcf08193` = `expf`, `0xb5e28191` = the
internal `_Sinf(x, quadrant)` used as sin and cos). `qglbase` exports are C++
names that can't be recovered this way.
