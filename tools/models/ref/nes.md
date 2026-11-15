# NES (NES-001 front loader) + NES Game Pak: geometric reference

Research only. No 3D model files were downloaded or copied. The numbers come from published specs, the Nintendo design
patents (orthographic drawings measured with pixel scans and scaled to the published width), and a straight-on
Wikimedia Commons photo of a Game Pak measured the same way.

**Confidence key:** **H** = published spec, or several sources agree. **M** = measured from an orthographic patent
drawing or a straight-on photo scaled to a known dimension (about ±2 mm). **L** = estimated from perspective photos
(about ±5 mm), or inferred.

**Console coordinates:** the origin is the centre of the 256 × 203 plan box, on the table plane. +x points right (seen
from the front), +z points to the front, and +y points up (y = 0 is the table). The outer sides are at x = ±128, the
front at z = +101.5 and the back at z = −101.5.

**Sources:**
- **P-con:** US Des. 299,726, "Video game control unit" (the NES-001 design), filed 4 Nov 1985, granted 7 Feb 1989.
  Sheet 1 has FIG 1 (3/4 view) and FIG 2 (door open). Sheet 2 has FIG 3 (front), FIG 4 (rear) and FIG 5 (top plan).
  https://patentimages.storage.googleapis.com/pages/USD299726-1.png, -2.png, -3.png. The patent number was
  identified from the nesdev "Patents" page (https://www.nesdev.org/wiki/Patents).
  All P-con mm values are pixel scans of FIG 3/4/5 scaled so the front width equals 256 mm (4.42 px/mm).
- **P-cart:** US Des. 294,020, "Cartridge for game machine" (the NES cartridge design), granted 2 Feb 1988.
  Sheets: https://patentimages.storage.googleapis.com/pages/USD294020-1.png … -5.png. Sheet 2 has FIG 3 (top end),
  FIG 4 (face), FIG 5 (bottom end) and FIG 6 (side). There is also an alternate design, US Des. 292,399
  (Commons: File:NES_cartridge_patent.png). It shows the ribbed side edges at the top corners.
- **D.com:** dimensions.com "Nintendo Entertainment System": H 3.5 in (88.9 mm), W 10.1 in (256 mm), D 8 in
  (203.2 mm), 2.27 kg.
- **CHM:** Computer History Museum catalog 102673389 (NES-001): 3¾ × 10⅛ × 8 in (95 × 257 × 203 mm).
- **RTC:** retrotechwiki.com (formerly wiki.retrotechcollection.com): 25.4 × 20.3 × 7.6 cm.
- **WP-pak:** Wikipedia "Nintendo Entertainment System Game Pak": 13.3 cm high × 12 cm wide.
- **iFixit-pak:** iFixit "NES Cartridge": 13.3 × 12 × 2 cm; 5 screws, later 3 screws plus 2 clips; 3.8 mm security
  screws.
- **CM:** consolemods.org "Dimensions for Game Cartridges" (via search excerpt; the page itself is Cloudflare-blocked):
  NES (US/PAL) 134 high × 120 wide × 17 thick.
- **Nesdev-pak:** nesdev.org "NES cartridge dimensions": label about 55 × 97 mm, of which 7 mm folds over the top.
  PCB is 100 mm wide. The shell outline is still marked "TODO".
- **Photo-pak:** Commons File:NES-Cartridge.jpg (1900 × 2100 px, straight-on face, no label). Pixel scan: 1787 px =
  120 mm, so 14.89 px/mm. The measured height of 134.1 mm independently confirms CM and WP.

---

## 1. Console overall

| Quantity | Value | Conf. | Source / notes |
|---|---|---|---|
| Width (x) | **256** | H | D.com 256, CHM 257, RTC 254. |
| Depth (z) | **203** | H | D.com 203.2, CHM 203, RTC 203. The P-con plan view gives a W:D ratio of 1.289, which would make D ≈ 199 at W 256. Use 203. |
| Height (y), with feet | **≈ 94** | M | P-con front view: 94.6 (body 91.0 + feet 3.6). CHM gives 95. **Conflicts:** D.com 88.9 (perhaps without feet, or rounded), RTC 76 (probably wrong). |
| Upper / lower shell split (parting line) | y = **50.2** | M | P-con FIG 3 and FIG 4 agree (50.2 / 50.6). |
| Weight | 2.27 kg | M | D.com. Not needed for modelling. |

### Buildable shape

1. **Upper shell** (light grey): a block x ±128, z ±101.5, y 50.2 → 94.6.
   - Fillet the top edges with R ≈ 3 mm. The patent corners are tight; photos show a small soft radius (L).
   - Vertical corner radius in plan is about 3 mm (M, P-con FIG 5 corners).
   - The upper shell is very slightly wider at the parting line (≈ 257 mm, P-con). Ignore this, or add a 0.5 mm lip.
2. **Lower shell** (dark grey): y 3.6 → 50.2.
   - **The side walls taper inward toward the bottom.** Half-width is about 128.5 at y = 45, 125.6 at y = 31, 122.6 at
     y = 24, 115.3 at y = 8 and about 114 at the bottom (M, P-con FIG 3; FIG 4 rear shows the same).
   - Build this as a block with its left and right faces inclined about 20° from vertical, or use a loft from 257 wide
     at the top to 228 wide at the bottom.
   - The front and rear faces of the lower shell are vertical. Photos show the front face of the lower shell sitting
     about 2–3 mm behind the upper shell and door, which casts a shadow ledge (L).
3. **Feet:** four low pads. In the front view they are at x ≈ −109…−91 and +93…+108.5, so the centres are at about
   **x = ±100** (M). Each is about 18 mm wide and 3.6 mm tall (M). Their z position is not on the drawings. Estimate
   **z = ±80** (L).
4. **Black vertical band** wrapping top, front and back: **x = +54.3 … +95.6** (41 mm wide; M, P-con FIG 3, 4 and 5
   all agree within 1 mm). On the top it is split along z into three parts:
   - Rear black block: z −101.5 … −62.6 (39 mm).
   - Ribbed light-grey vent section: z −62.6 … +62.8 (125 mm). It has **43 grooves at 2.97 mm pitch** (M, FIG 5 scan).
     Model the grooves about 1.2 mm wide and about 1 mm deep, running along x across the full band width (L).
   - Front black block: z +62.8 … +101.5 (39 mm).
   - The black block continues down the front and rear faces over the full height, to the bottom of the lower shell.
   - On the front, the band has grooves at y = 85.5 and y = 60 (black-on-black split lines; M) as well as the parting
     line at y = 50.
5. **Right margin:** light grey (upper) and dark grey (lower), x +95.6 … +128 (32 mm).

## 2. Cartridge door (front loader)

| Feature | Value | Conf. |
|---|---|---|
| Door face outline | x **−94.6 … +54.3** (149 mm wide), y **59.5 … ≈92** (≈33 mm tall; the top edge runs into the shell's front top radius) | M (P-con FIG 3) |
| Light-grey strip below the door | y 50.2 … 59.5 (part of the upper shell) | M |
| Finger tab at the bottom edge of the door | x ≈ −33 … −2 (31 mm wide), about 3 mm tall, lighter grey. A small lip protrudes about 1–2 mm. | M (position), L (depth) |
| Top "lid" panel outline (split groove on top) | x −93.7 … +55, z +62.6 … +101.5. It lines up exactly with the door's width and the front edge of the ribbed band. | M (P-con FIG 5) |
| Hinge | The door hinges at its **top edge** and is pushed **inward and upward** into the cartridge bay (about 90°). In P-con FIG 2 the door is drawn swung up, with the top-lid area open above the bay. | M (patent drawing) / H (common knowledge) |
| Bay opening behind the door | About 145 × 30 mm. The cart is 120 × 17. | L |
| Insertion | The cart goes in label-up, connector end first, sliding horizontally **about 120–125 mm** into the tray (the cart is 134 long; the grip end ends up just behind the door). The tray and cart are then **pressed down about 10–13 mm** and latch. A spring pops it back up when pressed again. | L. Nesdev forum describes the mechanism ("pressed down and locked into place") but gives no published travel figure. |
| Inserted cart | Fully hidden. The door closes over it. Nothing protrudes. | H |

Model the door as a separate 149 × 33 × 3 mm plate with its pivot at (y ≈ 92, z ≈ +99), so you can animate it.

## 3. Visible features (console coordinates, mm)

| Feature | Position / size | Conf. | Notes |
|---|---|---|---|
| Button panel recess (lower front) | x −101.1 … −25.2 (76 W), y ≈16 … 50 (34 H), about 3 mm deep | M (x, y), L (depth) | Rectangular recess in the dark lower shell. |
| Power LED | x −95.5 … −89.4, y 22.9 … 29.2 (6 × 6 mm square) | M | Red when on, dark red/black when off. |
| POWER button | x −87.8 … −60.4 (27.4 W), y 18.8 … 33.7 (15 H). The face is inset 2.5 mm from the edge (bevelled border). It protrudes about 5 mm from the panel. | M (size), L (protrusion) | Push button. Red "POWER" lettering (not drawn). |
| RESET button | x −57.5 … −30.3 (27.2 W), same y and depth as POWER | M | |
| Controller port tiles (front of black band) | Tile area y 16.0 … 45.7. Port 1 at x +58.1 … +74.8, port 2 at x +76.4 … +93.0 (each about 16.5 W × 30 H). Centres x ≈ **+66.5 / +84.7** (18 mm pitch), y ≈ 31. | M | Each tile holds a 7-pin socket. The visible socket outline is about 12 × 22 mm, rounded rectangle, black, with one chamfered end. P-con FIG 3 shows 7 contact holes: a 2 × 3 grid plus one extra hole at the top. Model each as a black recess about 3 mm deep. Red "1"/"2" numerals sit above them (not drawn). |
| Rear I/O panel recess | x +47.2 … +109.8, y 12.9 … 37.6 (behind the black band, at the right rear) | M (P-con FIG 4, mirrored) | From the console's right: DC power jack (box x ≈ +95…+105), RF channel 3/4 switch (x ≈ +74…+84.5), RF out round jack (x ≈ +58.5…+69.7, about 11 mm). There is a long slot about 58 mm wide along the bottom of the panel at y ≈ 14–20 (L). |
| Rear centre tab | x ≈ −29 … −12 (17.5 W), y 3.6 … 17.4 | M | Small rectangular boss at the bottom of the rear face (lower shell). |
| A/V RCA jacks (video yellow, audio red) | Right side face (x = +128), lower shell, toward the rear | L | Visible in Commons NES-Console-FR/FL photos. |
| Expansion port | Underside (cover plate) | H (Wikipedia) | Not visible in the animation. |
| Logo area "Nintendo / ENTERTAINMENT SYSTEM" | Printed on the door face, left part: x ≈ −90 … −10, y ≈ 68 … 85 | L | Position only. Red text. |
| Ventilation | The 43 top grooves in the band are the main visible vents | M | |

## 4. Colours and finish

Photo samples come from Commons Evan-Amos studio photos (bright, slightly over-exposed). The base-colour suggestions
are darkened for neutral lighting.

| Part | Sampled sRGB | Suggested base sRGB | Finish | Conf. |
|---|---|---|---|---|
| Upper shell, door, top lid, ribbed vent strip | #D5D3D1 (top, lit) / #B6B5AF (door face) / #AFABAA (ribs) | **#C9C7C2** (warm light grey) | matte, fine texture | M |
| Lower shell, button panel | #837D7F (front, shaded) / #9A9797 (side) | **#8C898A** | matte | M |
| POWER/RESET buttons | #7C7978 | **#807D7C** (same family as the lower shell, a touch darker) | satin | M |
| Black band (top blocks + front/rear) | #343434 / #413E3F | **#2A2A2A** | satin / slight gloss | M |
| Controller socket interior | #010101 | **#0A0A0A** | matte | M |
| Red lettering (logo, POWER/RESET, port numbers) | n/a (thin text) | **#C8102E** (approx. NES red) | printed | L |
| LED (on) | n/a | **#FF2A1A** (emissive) | glossy lens | L |
| Dark seams/grooves | n/a | #5A5858 | n/a | L |

## 5. NES Game Pak (NTSC-U / PAL, 72-pin)

Cart coordinates: lay the cart face up, connector end down. u = mm from the **left edge** and v = mm from the **top
(grip) edge**, as seen on the front face (the face with the label and the vertical grip strip).

| Quantity | Value | Conf. | Source |
|---|---|---|---|
| Width | **120.0** | H | CM, WP, iFixit-pak, Photo-pak (scale reference) |
| Height | **134** (photo scan 134.1) | H | CM 134, WP/iFixit 133, Photo-pak 134.1 |
| Thickness | **17** | M/H | CM 17; P-cart FIG 6/FIG 3 scale to 16.5–17.4. **Conflict:** iFixit gives 2 cm / 0.75 in (rounded). Use 17. |
| Narrow connector end | The bottom 24.5 mm (v 109.6 … 134.1) is **106.2 wide**. It steps in **6.9 mm** on each side with square shoulders. | M | Photo-pak scan |
| Label recess (front) | u **48.1 … 104.5** (56.4 W), v **0 … 91.7**. It is open at the top edge, because the label folds over onto the top end. Recess about 0.5 mm deep. | M | Photo-pak; matches Nesdev-pak label 55 × 97 (90 on the face plus 7 folded over the top) |
| Vertical grip strip | u **18.8 … 45.1** (26.3 W). **47 horizontal grooves at 2.5 mm pitch** from v 3.7 to v 120.7. Smooth pad at the bottom (v ≈ 121 … 134). The top section (v ≈ 1.5 … 16) sits in a slightly darker recessed window. | M | Photo-pak scan |
| Arrow (moulded, pointing down toward the connector) | Centre u ≈ 60, v ≈ 104, a triangle about 12 mm wide and 10 mm tall, embossed or debossed about 0.5 mm | L/M | Photo-pak |
| Small round mark (below the label) | u ≈ 60, v ≈ 78, Ø ≈ 3 | L | Photo-pak |
| Top end details | Small notches about 4 × 2 mm at both top corners, plus shallow ribbed grip on the side edges near the top corners | L | Photo-pak, P-cart D292,399 FIG 1 |
| Back face | Flat, with 5 screws in early carts (later 3 screws plus 2 clips) and a recessed "seal/instructions" panel | M | iFixit-pak; P-cart FIG 4 |
| Colour | Sampled #777777 … #7D7D7D (studio light). Suggested base **#808080** (mid grey) | — | matte | M |
| Label | Printed art (not modelled). Use a flat colour or neutral grey | — | glossy paper | — |
| Inside the console | Fully inside the front loader once inserted and pressed down. Nothing visible with the door closed. | H | — |

### Buildable cart

1. Box 120 × 134 × 17.
2. Boolean-subtract two corner notches 6.9 × 24.5 at the bottom.
3. Subtract the label recess 56.4 × 91.7 × 0.5 at u 48.1.
4. Subtract 47 grooves (each 26.3 × 1.0 × 0.8 mm, at 2.5 mm pitch) for the grip strip.
5. Fillet all outer edges with R ≈ 1 mm (L).

## 6. Reference photos and drawings

- Front-left 3/4: https://commons.wikimedia.org/wiki/File:Nintendo-Entertainment-System-NES-Console-FL.jpg
- Front-right 3/4: https://commons.wikimedia.org/wiki/File:Nintendo-Entertainment-System-NES-Console-FR.jpg
- Back-left 3/4: https://commons.wikimedia.org/wiki/File:Nintendo-Entertainment-System-NES-Console-BL.jpg
- Back-right 3/4: https://commons.wikimedia.org/wiki/File:Nintendo-Entertainment-System-NES-Console-BR.jpg
- With controller: https://commons.wikimedia.org/wiki/File:NES-Console-Set.jpg
- PAL unit: https://commons.wikimedia.org/wiki/File:Wikipedia_NES_PAL.jpg
- Game Pak face (straight-on): https://commons.wikimedia.org/wiki/File:NES-Cartridge.jpg
- Cart design patent (3/4 views): https://commons.wikimedia.org/wiki/File:NES_cartridge_patent.png
- **Orthographic console views (best for top, front, rear):** https://patentimages.storage.googleapis.com/pages/USD299726-2.png.
  The door-open view is on -1.png.
- Orthographic cart views: https://patentimages.storage.googleapis.com/pages/USD294020-2.png

## 7. Gaps and conflicts

- **Height:** 94–95 (patent scan, CHM) vs 88.9 (D.com) vs 76 (RTC). We chose 94 including feet.
- **Depth:** the patent plan ratio suggests about 199. All published sources say 203, so we used 203.
- **Cart thickness:** 17 (CM, patent) vs 20 (iFixit, rounded).
- **Missing data:**
  - Push-down travel of the tray (estimated 10–13 mm). No published figure was found.
  - Feet z positions.
  - Side-view profile (P-con sheet 3 was not scanned in detail).
  - Exact edge radii.
  - Socket inner outline.
- **Not used:** the Printables "NES Frontloader replica Shell" page (claims to be dimensionally accurate) and the
  consolemods page were both behind a Cloudflare challenge.
