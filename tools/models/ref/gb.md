# Game Boy (DMG-01, grey) + Game Boy Game Pak (DMG): geometric reference

Research only. No 3D model files were downloaded or copied. Overall sizes come from published specs. Layout
positions were **measured by pixel scans of near-orthographic Wikimedia Commons photos**, scaled to the published
overall size (method: bounding box of the alpha mask = 90 × 148 mm, then feature bounding boxes / colour masks).

**Confidence key:** **H** = published spec or several sources agree. **M** = measured from a near-orthographic
photo scaled to a known dimension (about ±1 mm for large features, ±0.5 mm for small ones). **L** = estimated from
perspective photos or inferred (±2–3 mm or worse).

**Coordinate system (console):** origin at the centre of the 90 × 148 front outline, at mid-depth.
+x right (seen from the front), +y up, +z towards the viewer. So the front face is z = +16, the back z = −16,
the top edge y = +74, the bottom edge y = −74, the sides x = ±45.

Primary sources:
- **WP:** Wikipedia "Game Boy" (spec table) and "Game Boy Game Pak".
- **NIN:** nintendo.co.uk Game Boy product page ("90mm x 148mm x 32mm").
- **SMG:** Science Museum Group catalogue co8184133 (gives 145 × 90 × 32).
- **iFx:** iFixit Device pages "Game Boy" and "Nintendo Game Cartridge".
- **SUDO:** sudomod.com forum, Game Boy Zero case CAD thread (A/B hole diameter measured at 10.75 mm on two original
  DMG shells) and the "Gameboy Button dimensions?" thread (10.05–10.8 mm reported).
- **RM:** retromodding.com DMG glass lens pages (lens "OEM (64 mm) view area" — i.e. diagonal-ish naming, not a size).
- **HHL:** handheldlegend.com DMG cover screen (lens thickness 1.0 mm).
- **PR:** Printables model 798350 "Game Boy cartridge" description (57 × 65 × 7.75 mm, "accurate to the original").
- **PH-F:** `File:Геймбой.png` (786 × 1280, transparent background, straight-on front). Scale 0.1186 mm/px,
  centre at px (392.5, 639.5). **Main layout source.**
- **PH-L / PH-R / PH-T / PH-B:** `File:Геймбой (левая боковина).png`, `(правая боковина).png`,
  `(верхний торец, выключено).png`, `File:Геймбой со вставленным картриджем.png` (back view with cart).
- **PH-C:** `File:Game Boy Game Paks.jpg` (grey DMG cart, near-orthographic, blank label). Scale 0.0874 mm/px.

---

## 1. Console overall shape

### 1.1 Overall size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Width (x) | **90 mm** | H | WP, NIN, iFx, SMG |
| Height (y) | **148 mm** | H | WP, NIN, iFx |
| Depth (z) | **32 mm** | H | WP, NIN, SMG |
| Mass | 220 g without batteries (WP table); ~300 g with 4 × AA | H | WP |

**Conflicts:** SMG gives 145 mm height (probably measured without the top lip or rounded). PH-F aspect ratio is
1243 : 757 px = 1.642, matching 148 / 90 = 1.644, so 148 is consistent with the photo.

### 1.2 Front outline (plan of the front face)

A 90 × 148 rectangle with three small corners and one large one.

| Corner | Radius | Conf. | Notes |
|---|---|---|---|
| Top-left, top-right | **≈3 mm** | M | PH-F: the edge reaches full width ~24 px from the top. |
| Bottom-left | **≈3 mm** | M | |
| Bottom-right | **≈20 mm** (fit range 20–23) | M | PH-F: the right edge leaves the straight line at y ≈ −54 and the bottom edge at x ≈ +24.6, i.e. ~20 mm each way. The curve is slightly "squarish" (a superellipse fits better than a circle; a pure circle of R = 20 is a good approximation). |

The back shell repeats the same outline (the large radius is at the **bottom-left** when seen from behind).

### 1.3 Thickness profile and edge fillets

| Item | Value | Conf. | Notes |
|---|---|---|---|
| Thickness | **32 mm, essentially constant** (the DMG is a slab) | H/M | PH-L/PH-R show straight top and bottom silhouettes. |
| Shell split line | horizontal seam all around at **≈14 mm behind the front face** (z ≈ +2) | M | PH-L: front band 13.4 mm of 30.2 mm measured (scaled to 32 → ~14). |
| Front face perimeter fillet | ≈1.5–2 mm | L | |
| Back perimeter fillet | ≈2–3 mm (softer than the front) | L | |
| Top front "lip" | A shallow groove runs across the front ≈7 mm below the top (y ≈ +67), joined by two short vertical grooves at x ≈ ±38 that run up to the top edge. The "OFF•ON" marking sits in this band, top-left. | M | PH-F |
| Side grip ribs | 11 horizontal ribs on each side, on the back shell, from **≈73 to ≈127 mm below the top** (y ≈ +1 … −53); they wrap round onto the back edges (13–14 ribs visible on the back). Pitch ≈5 mm, depth ≈0.8 mm. | M/L | PH-L, PH-R, PH-B |
| Feet | Low strip under the battery cover on the back (PH-L shows a 1–2 mm darker band) | L | |

---

## 2. Front layout (all centres in mm, origin = body centre)

| Part | Centre (x, y) | Size | Conf. | Notes |
|---|---|---|---|---|
| **Screen bezel** (dark plate) | (0, **+32.8**) | **77 × 58 mm** (x −38.5…+38.6, y +3.7…+61.8) | M | PH-F. Corners: top-left, top-right, bottom-left r ≈ 2 mm; **bottom-right r ≈ 15 mm** (echoes the body corner). Flush with or ~0.3 mm proud of the face; it is the printed/clear lens. |
| **Screen window** (dark opening in the bezel showing the LCD) | (0, +32.8) | **≈48 × 44 mm** (x ±24.0, y +10.9…+54.7) | M | PH-F, the green/dark-bordered area. Square corners (r < 1 mm). |
| Visible LCD pixel area | (0, ≈+32.8) | **47 × 43 mm** panel; 160 × 144 px | H | WP (one WP figure gives 45.5 × 41.5 mm as the playable area). Sits ≈1.5 mm behind the lens. |
| Lens | covers the whole bezel | thickness **1.0 mm** (replacement) | M | HHL |
| **Power LED** | (**−32.6, +39.7**) | Ø ≈2.5–3 mm | M | PH-F, left of the window. Red when on. |
| **Decorative lines** | see §2.1 | | M | |
| **D-pad** | (**−26.9, −26.3**) | span **≈22.8 mm**, arm width **≈8.3 mm** | M | PH-F. One-piece cross, sits in a cross-shaped well ≈0.5 mm larger. Protrudes ≈3 mm (L). Arms have slightly bevelled tips. |
| **A button** | (**+32.9, −20.8**) | Ø ≈10.3 mm (hole **10.75 mm**) | M / M | PH-F; hole diameter SUDO (two shells measured). Domed, protrudes ≈2.5 mm (L). |
| **B button** | (**+18.2, −28.0**) | same | M | Distance A–B centres **16.4 mm**, the A–B axis rises **≈26°** to the right. |
| A/B cradle | (+25.6, −24.4), axis 26° | stadium ≈31 × 15 mm | L/M | A shallow stadium-shaped recess around both buttons, aligned with their axis. |
| **SELECT** | (**−12.4, −48.1**) | rubber pill ≈10.4 × 3.1 mm visible; hole ≈11.5 × 4.0 mm, fully rounded ends | M | Tilted **≈22–25°** (rising to the right). |
| **START** | (**+3.0, −47.2**) | same | M | Same tilt. Each pill sits in a faint round/oval dish ≈14 mm across (L). Centres 15.4 mm apart. |
| **Speaker grille** | slot centres on a line from (+16.4, −64.7) to (+37.5, −51.3) | **6 through-slots**, each ≈14 × 2 mm, round ends | M | PH-F. Pitch ≈5 mm along the row; the row rises ≈32° to the right. Each slot's long axis points **≈−55 to −60°** (top end towards upper-left), i.e. roughly perpendicular to the row. The lowest slots run close to the large corner. |
| Brand text "Nintendo GAME BOY™" | y ≈ −1, x ≈ −39…+13 | — | M | (Text; omit or use a decal.) |
| Headphone jack plate | bottom edge, x ≈ **−4.4** | oval plate ≈14 mm wide at the front-bottom edge; 3.5 mm stereo jack on the bottom face | M/L | PH-F shows the "PHONES" oval; jack centred in depth on the bottom face. |

### 2.1 Bezel decorative lines (no text)

Two thin parallel horizontal lines across the top of the bezel, broken in the middle by the "DOT MATRIX WITH
STEREO SOUND" text.

| Item | Value | Conf. |
|---|---|---|
| Upper line | **dark magenta / maroon**, centre y ≈ **+59.0** (≈3 mm below the bezel top) | M |
| Lower line | **dark navy blue**, centre y ≈ **+57.8** | M |
| Line thickness | ≈0.5–0.6 mm each, centre spacing ≈1.2 mm | M |
| Left segment | x **−34.7 … −14.8** | M |
| Right segment | x **+26.0 … +35.0** | M |
| Gap (text) | x −14.8 … +26.0 | M |

The lines stop ≈3.7 mm inside the bezel's left and right edges. (The photo is rotated ~0.5°; values are averaged.)

---

## 3. Sides, top, bottom, back

| Part | Location | Size | Conf. | Source |
|---|---|---|---|---|
| **Power switch** (OFF/ON slider, also locks the cartridge) | top face, **left**: knob at x ≈ −36 … −29 (ribbed grip, 3 ribs) | knob ≈7 × 6 mm, travel ≈8 mm towards +x (OFF left → ON right) | M | PH-T (top view: viewed from above with the front at the top, so left/right are mirrored in the image) |
| Cartridge lock tab | back half of the top, next to the switch, x ≈ −34 | ≈5 × 5 mm grey tab; slides into the cart's corner notch when ON | M | PH-T |
| **Contrast wheel** | **left side**, y ≈ +38.5 … +20 (window 35–54 mm below the top), in the front shell just above the seam | wheel window ≈18 × 5 mm; wheel protrudes ≈1.5 mm | M | PH-L |
| **DC IN 6V jack** | **left side**, ≈25 mm below the top (y ≈ +49), just behind the seam (z ≈ −3) | round, outer Ø ≈5.5 mm | M | PH-L; WP says DC jack on the left |
| **Volume wheel** | **right side**, y ≈ +28 … +13, just above/at the seam | window ≈15 × 5 mm | M | PH-R |
| **Link port (EXT. CONNECTOR)** | **right side**, y ≈ +50 … +36, in the back shell just below the seam | opening ≈11 × 6 mm in a ≈14 × 9 mm frame | M | PH-R; WP says right side |
| Side ribs | both sides, y ≈ +1 … −53 | 11 ribs | M | PH-L, PH-R |
| **Battery cover** | back, **centred in x**, from y ≈ **0 to −67** | **≈65 × 67 mm**, 9 horizontal grooves; latch tab at top centre (≈15 mm wide) | L/M | PH-B (perspective-corrected) |
| Upper back plate | back, from 23 mm below the top down to the battery cover; holds the model sticker | full width minus ≈12 mm margins | L | PH-B |
| Screws | 4 visible tri-wing screws on the back near x ≈ ±35, y ≈ +61 and x ≈ ±37, y ≈ +18; 2 more inside the battery bay | | L | PH-B; Game Boy Museum article |

## 4. Cartridge slot and how the cart sits

| Item | Value | Conf. | Source |
|---|---|---|---|
| Slot | opening in the top of the **back shell**, centred in x; the cart slides straight down | ≈58 × 9 mm opening | M | PH-T |
| Inserted cart, x | centred (x ≈ 0, measured +0.2) | | M | PH-T |
| Inserted cart, z | **back face flush with the console back** (z = −16 … ≈ −8.3) | | M | PH-T |
| Inserted cart, y | **top flush with the console top** (y = +74); connector edge at y ≈ +8.5 | | M | PH-B, PH-T |
| Exposure | the cart's top **≈23 mm** (grip ridges + top of the label) is visible from behind through an open recess; the rest is hidden by the back plate | | M/L | PH-B |

## 5. Colours (sRGB) and finish

Photo-sampled values are from PH-F (underexposed, yellowed) and PH-C. The "model" column is a corrected value for
rendering. **No official colour codes exist (searched).** Confidence: L for all.

| Part | Photo sample | Model value | Finish |
|---|---|---|---|
| Body (grey ABS) | #979187 | **#C4C0B8** (warm light grey) | matte, fine texture |
| Screen bezel | #3A393E / #424147 | **#4B4C58** (slate blue-grey) | gloss (lens) |
| Upper line | #422935 | **#7A1E4C** (maroon/magenta) | print |
| Lower line | #1A152E | **#1F2668** (navy) | print |
| Bezel text & "GAME BOY" logo | #2C2E50 | **#2C3270** (navy) on the body; light grey on the bezel | print |
| A / B | #876B76 (lit), #532F38 (shade) | **#9C2355** (magenta-maroon) | slight gloss |
| D-pad | #0E0E10 / #22211F | **#1E1E20** | semi-gloss |
| Start / Select rubber | #43403A | **#6F6E78** (grey with a blue cast) | matte rubber |
| LED | #FEAFB6 (lit) | on **#FF2A2A**, off **#4A1A1E** | |
| LCD (off) | — | **#8E9A5E** olive-grey; pixels on **#1F3A1F** | |
| Cart (grey) | #BDB0BB (white-balance tint) | **#A9A7AC** | matte |

---

## 6. Game Pak (DMG cartridge)

**Coordinates (cart):** origin at the centre of the 57 × 65.5 label face, x right, y up, z towards the viewer
(label face). Top edge y = +32.75, connector edge y = −32.75. Values measured on PH-C are given as distances from the
top edge (**"↓ from top"**) and converted.

### 6.1 Size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Width | **57 mm** | H | WP, iFx, PR |
| Height | **65.5 mm** (PR: 65) | H | WP, iFx |
| Thickness | **7.5 mm** (WP) / 7.75 (PR) / **9 mm** (iFx) | M | conflict; PH-T measured ≈7.7 mm. Use **7.7 mm**. |

### 6.2 Outline

| Feature | Value | Conf. | Notes |
|---|---|---|---|
| Corners | r ≈1 mm on all four corners | M | PH-C |
| **Cut corner (notch)** | **top-right corner when viewed from the label side**: a rectangular step **≈5.7 mm wide × ≈2.6 mm deep** | M | PH-C. The console's lock tab enters this notch when power is ON. |
| Side step | very slight (<0.3 mm) step on the left edge ≈27 mm below the top | L | Optional. |

### 6.3 Front (label side) details

| Feature | Position | Size | Conf. |
|---|---|---|---|
| Top recessed stadium (holds embossed "Nintendo GAME BOY™") | centre ≈11.2 mm ↓ from top (y ≈ +21.5), x ≈ +0.6 | **44.7 × 13.1 mm**, fully rounded ends (r ≈6.5) | M |
| Grip ridges | two groups, left (x −28.5…−22.5) and right (x +22.5…+28.5), from 5.1 to 13.4 mm ↓ from top | 5 horizontal ridges each, pitch ≈1.85 mm | M |
| **Label recess** | from **18.1 to 58.1 mm ↓ from top** (y +14.6 … −25.4), x centred (+0.8) | **45.1 × 40.0 mm**, corner r ≈0.9 mm, depth ≈0.3–0.5 mm (L) | M |
| Label sticker | inside the recess, ≈1 mm inset | ≈43.5 × 38.5 mm | L |
| **Arrow** | **down-pointing triangle**, top at 59.3 mm ↓ from top (y ≈ −26.6), x ≈ +1.5 | **≈9.0 wide × 5.2 tall** (recessed) | M |

### 6.4 Back and bottom

| Feature | Value | Conf. |
|---|---|---|
| Screw | one 3.8 mm security screw on the back, centred, in the lower half | M (iFx/Epilogue) / L for position |
| Connector opening | bottom edge, back side, exposing the 32-pin PCB edge (≈2.5 mm deep cut-out) | L |
| Colour | grey (see §5); special editions yellow/red/blue/black | H (WP) |

---

## 7. Reference photos (Wikimedia Commons)

Front / back / sides (grey DMG):
- Front, straight-on, transparent PNG (**best layout ref**): https://commons.wikimedia.org/wiki/File:Геймбой.png
- Left side: https://commons.wikimedia.org/wiki/File:Геймбой_(левая_боковина).png
- Right side: https://commons.wikimedia.org/wiki/File:Геймбой_(правая_боковина).png
- Top, switch off (with cart): https://commons.wikimedia.org/wiki/File:Геймбой_(верхний_торец,_выключено).png
- Top, switch on: https://commons.wikimedia.org/wiki/File:Геймбой_(верхний_торец,_включено).png
- Bottom: https://commons.wikimedia.org/wiki/File:Геймбой_(нижний_торец).png
- Back with a cart inserted: https://commons.wikimedia.org/wiki/File:Геймбой_со_вставленным_картриджем.png
- Battery bay: https://commons.wikimedia.org/wiki/File:Батарейный_отсек_Геймбоя.jpg
- Evan-Amos 3/4 views: https://commons.wikimedia.org/wiki/File:Game-Boy-FL.png ,
  https://commons.wikimedia.org/wiki/File:Game-Boy-FR.jpg , https://commons.wikimedia.org/wiki/File:Game-Boy-BL.jpg ,
  https://commons.wikimedia.org/wiki/File:Game-Boy-BR.jpg
- Category with teardown photos: https://commons.wikimedia.org/wiki/Category:Disassembled_Game_Boy_DMG-01

Cartridge:
- Grey DMG cart, blank label, beside GBC and GBA carts (**best cart ref**): https://commons.wikimedia.org/wiki/File:Game_Boy_Game_Paks.jpg
- https://commons.wikimedia.org/wiki/File:Nintendo-Game-Boy-Cartridge.jpg
- Front, straight-on (GBC-era cart, same shell): https://commons.wikimedia.org/wiki/File:Картридж_Геймбоя.png
- Bottom/connector: https://commons.wikimedia.org/wiki/File:Картридж_Геймбоя_(донная_часть).jpg

## 8. Gaps / open questions

- No official dimensioned drawing or design patent drawing was found for the DMG (only utility patent US 5,184,830).
  Every layout figure is photo-derived (M).
- Button heights above the face, fillet radii and the battery-cover size are estimates (L).
- The DC jack being *above* the contrast wheel on the left side comes from PH-L only (single source).
- Colour values are corrected estimates; no measured colour data was found.
