# CRT filter: implementation and cost

How the CRT filter (Settings → Display → CRT filter, `crt=` in `settings.ini`) is
drawn on the dashboard and in the games, and what it costs. User-facing description:
README, "CRT filter".

## Implementation

- **Dashboard** (`src/crt.c`): the frame is drawn into an offscreen target the size of
  the output and put on screen by one more full-screen pass (one triangle, 1 bilinear
  fetch per pixel, the rest mediump ALU with no exp or division; the level's constants
  reach the pixel as flat varyings, so it loads no uniform). `crt_pass=mesh` in
  `settings.ini` draws the pass as a curved mesh instead (about 1800 triangles): the
  curvature, the vignette and the rounded edge are then worked out per vertex and the
  pixel fetches at an interpolated coordinate. Same picture (50 dB apart); on psc-sim's
  software GPU the triangles cost more than they save, on the console's tile-based GPU
  it should be the cheaper of the two: try it there with the `bench` file. MSAA is kept: on the console the target is
  multisampled on chip (`GL_EXT_multisampled_render_to_texture`, resolved for free);
  where that is missing a multisampled renderbuffer is resolved with one blit. The
  target's depth and the window's old contents are invalidated, so neither costs memory
  bandwidth. The splash, the launch animation and the in-game dialog go through it too.
  Cost (`LUMEN_COST=1`, psc-sim): +0.92 Mpx/frame at 720p, +2.07 Mpx/frame at 1080p, one
  draw call; check it on the console with the `bench` file.
- **Games**: `lumen/shaders/crt-lumen-{soft,strong}.glslp` (one-pass GLSL,
  `dist/shaders/crt-lumen.glsl`, same maths and values). The pass draws straight into
  the screen (no `scale_type`, so RetroArch adds no FBO and no copy pass), with 1 bilinear
  fetch per pixel, its constants worked out per vertex (flat varyings: the pixel loads no
  uniform), a gaussian beam without exp or division and its colour maths in mediump.
  `crt_shader=quality` in `settings.ini` switches the games to the older, heavier maths
  (`crt-lumen-{soft,strong}-quality.glslp`: 5 fetches, all highp). When the setting is on, the
  launch request carries `crt=soft|strong`; `run.sh` puts `video_shader_enable = "true"`
  first in the extra config and hands the preset to RetroArch with `--set-shader`
  (`LUMEN_RA_SHADER`; RetroArch 1.22 has no config key for the shader path). It works the
  same for software cores and for the GLES 3 cores (Mupen64Plus-Next, PPSSPP), whose
  image RetroArch passes through the shader too. Scanlines follow the core's lines:
  224/240p are drawn line by line; above 300 lines (480i, enhanced resolutions) the
  density is halved until it fits, so upscaled 3D keeps its detail. A custom
  `run_retroarch()` in `lumen.conf` must pass `--set-shader "$LUMEN_RA_SHADER"` when it
  is set (the shipped `lumen.conf` does). Without the presets the game runs unfiltered
  (logged in `session.log`).

## Cost

`tools/crt-bench/crt-bench.sh` (needs psc-sim's Docker image) draws the shaders the way
RetroArch's gl driver does, and the dashboard's pass through the real `src/crt.c`, times
them on llvmpipe with one thread and compares the pictures. Synthetic frames, 1280x720
screen, ms per whole frame (Soft / Strong):

| | no filter | 0.20.0 | now |
|---|---|---|---|
| Game 320x240 (4:3 viewport) | 6.3 / 6.4 | 22.2 / 22.4 | 8.5 / 8.6 |
| Game 640x480 (enhanced) | 6.4 / 6.3 | 22.8 / 23.2 | 8.7 / 8.8 |
| Dashboard 1280x720 | 8.5 / 8.5 | 19.9 / 20.0 | 10.8 / 11.2 |

So the filter itself costs about 2.2 ms instead of 15.9 ms on a game frame (about 7x
less) and 2.5 ms instead of 11.4 ms on the dashboard (about 4.5x less). The look stays
within 38-44 dB PSNR of the quality maths (`docs/images/crt-fast-vs-quality.png`); the
visible difference is a little less glow around thin bright strokes.

How it got there: every formulation of the pixel's maths was measured against its
alternatives (`CRT_BENCH_PASS=1` times the pass alone, without the upload and the clear,
which are 5.3 ms of a game frame here) and the fastest that kept the picture was kept:
no FBO, 1 fetch, constants per vertex as flat varyings, the beam as (1 - x/8)^8 with its
width a quadratic of r + g + b, the mask as `min(abs(...))`, one scalar for beam, vignette
and edge. Each alternative left is 0.05-0.9 ms slower. llvmpipe is not the GE8300: on the
console the removed FBO round trip (about 5.5 MB of memory traffic per frame) and the move
from ~15 transcendental ops per pixel in highp to one sqrt in mostly FP16 should weigh at
least as much; check it there with the `bench` file and RetroArch's frame-time counter.

