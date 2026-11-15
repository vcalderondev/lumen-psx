# 3D models

The cartridges, cases and consoles of the launch animation (`insert_cartridge`,
`open_case_disc`) are built here in Blender, by script, and exported to
`assets/models/<key>.lmdl`, which Lumen loads at run time (`src/meshes.inc`; a missing
model falls back to the procedural one in `src/consoles.inc`).

- `ref/*.md` — the measurements each model is built from (published sizes, design patent
  drawings, ECMA standards, photos measured to scale), with sources and confidence.
- `lmdl.py` — the toolkit: rounded outlines, bevels, booleans, parts and how Lumen draws
  them (plastic / label / screen / light / glass / dark, animated flaps, doors, switches),
  the `.lmdl` exporter and preview renders.
- One script per model family: `media.py` (jewel case, DVD and UMD keep cases, the UMD),
  `n64.py`, `megadrive.py`, `snes.py`, `gba.py`, `pce.py`, `carts.py` (Game Boy, NES,
  Super Famicom, GBA cartridges, HuCard), `from_glb.py` (the Game Boy and NES consoles,
  adapted from CC BY models listed in `CREDITS.md`).
- `build.sh` rebuilds everything; previews land in `build/`.

Frames (mm, y up, front towards the viewer): top loaders have the slot centre at the
origin; front loaders the top at 0; handhelds the body centre; cartridges and cases their
centre. No logos or brand lettering are modelled.
