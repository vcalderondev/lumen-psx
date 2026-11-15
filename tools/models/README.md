# 3D models

The cartridges, cases and consoles of the launch animation (`insert_cartridge`,
`open_case_disc`) are built here in Blender, by script, and exported to
`assets/models/<key>.lmdl`, which Lumen loads at run time (`src/meshes.inc`; a missing
model falls back to the procedural one in `src/consoles.inc`). Blender writes the
full-detail model (`.lmdl` v1, a triangle soup per object, in `build/v1/`);
`optimize.py` makes the one that ships (`.lmdl` v2).

- `ref/*.md` — the measurements each model is built from (published sizes, design patent
  drawings, ECMA standards, photos measured to scale), with sources and confidence.
- `lmdl.py` — the toolkit: rounded outlines, bevels, booleans, parts and how Lumen draws
  them (plastic / label / screen / light / glass / dark, animated flaps, doors, switches),
  the `.lmdl` exporter and preview renders.
- One script per model family: `media.py` (jewel case, DVD and UMD keep cases, the UMD),
  `n64.py`, `megadrive.py`, `snes.py`, `gba.py`, `pce.py`, `carts.py` (Game Boy, NES,
  Super Famicom, GBA cartridges, HuCard), `from_glb.py` (the Game Boy and NES consoles,
  adapted from CC BY models listed in `CREDITS.md`).
- `optimize.py` — plain Python, no Blender: merges the objects that draw the same way
  into one part (one draw call each), decimates within a tolerance (0.05 mm cartridges
  and cases, 0.10 mm consoles: its header says why not more), and writes one vertex
  buffer with 16-bit indices. `optimize.py --info <dir>` lists parts, triangles and sizes.
- `build.sh` rebuilds everything; previews land in `build/`.

The PlayStation Classic's GPU is limited by the pixels it shades, not by triangles: what
keeps the launch animation fast is how the models are drawn (`mdl_draw` in
`src/meshes.inc`: solid parts with blending off, their own small shader), so a new model
costs little as long as its solid parts are opaque (`rgba` alpha 1) and only real glass
is `glass`.

Frames (mm, y up, front towards the viewer): top loaders have the slot centre at the
origin; front loaders the top at 0; handhelds the body centre; cartridges and cases their
centre. No logos or brand lettering are modelled.
