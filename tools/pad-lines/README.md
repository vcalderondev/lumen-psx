# The controllers of the "pair your controller" card

`assets/ui/pad.png`, `pad2.svg` ... are shown one after another. They share one look: white
outlines of one weight on a dark body, the face symbols or letters in their colours.

## From a vector drawing (pad2, pad3, pad4)

```sh
python3 recolour.py <source.svg> out.svg [boxes]     # colours only: outlines white, surfaces dark
python3 frame.py out.svg src/padN.svg '{"uniform": 0.68, "line": {...}, "fill": {...}, "drop": [...], "credit": "a comment for the SVG"}'
../make-illustrations.sh                              # the PNGs the console draws
```

`uniform` gives every outline the same weight in pixels of the rendered picture (0.68: the
weight of the DualShock 4 drawing, the reference). `line` / `fill` colour shapes by their
index in document order (the symbols), `drop` removes some.

## From a 3D model (pad: the DualSense)

No free vector of the DualSense has the detail of the others, so its lines are drawn from a
3D model (glTF, not in the repository; its source is recorded in lumen-design,
`guidelines/credits.md`):

```sh
npm i three@0.170                                     # in this folder, once
MODELS=/path/to/models sh render.sh playstation_5_dualsense "passes=1&w=2880&h=2100&fov=8&elev=0&yaw=180&opaque=1&fit=1.04"
uv run --with pillow --with numpy python lines.py out/playstation_5_dualsense_passes ds 3.6 0.14 "2120,710,2276,860,2120,1010,1964,860" 31 0.93 0.76
cp ds.png ../../assets/ui/pad.png; cp ds-1080.png ../../assets/ui/pad-1080.png
```

`render.sh` starts a local server and a headless Chrome that renders three flat pictures of
the model's front (normals, base colours, depth) with three.js; `lines.py` draws a line
wherever the surface turns sharply, steps in depth, changes colour or ends, and adds the
four symbols at the given centres.

The last two numbers are the strength of the lines at 720p and at 1080p. They are set so the
lines are as bright as the vector drawings' outlines (measured on the PNGs: median 0.40 and
90th percentile 0.64 of full white for the DualShock 4 at 1080p; the DualSense gives 0.40
and 0.66 with 0.76).
