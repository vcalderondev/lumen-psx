# Game Boy Advance (AGB-001, original horizontal, Indigo) + GBA Game Pak: geometric reference

Research only. No 3D model files were downloaded or copied. Overall sizes are from published specs. The outline and
layout were **measured by pixel scans of near-orthographic Wikimedia Commons photos**, scaled to the published size.

**Confidence key:** **H** = published spec or several sources agree. **M** = measured from a near-orthographic
photo scaled to a known dimension (about ±1 mm). **L** = estimated from perspective photos or inferred (±2–3 mm or
worse).

**Coordinate system (console):** origin at the centre of the 144.5 × 82 front-outline bounding box, at mid-depth.
+x right (seen from the front), +y up, +z towards the viewer.

Primary sources:
- **WP:** Wikipedia "Game Boy Advance" (82 × 144.5 × 24.5 mm, 140 g, screen 40.8 × 61.2 mm, 240 × 160 px,
  2.9 in) and "Game Boy Game Pak" (GBA pak 35 × 57 × 7.5 mm; "top is wider (6 cm) than the bottom (5.8 cm)").
- **D.com:** dimensions.com/element/game-boy-advance-gba (82 × 144 × 24.5 mm, 0.14 kg).
- **iFx:** iFixit Device "Game Boy Advance" (145 × 82 × 24 mm, rounded).
- **PH-F:** `File:GBA Controls detailed.png` (800 × 472, Arctic unit, straight-on front, transparent background).
  Silhouette 758 × 419 px → scales sx = 0.1906 mm/px, sy = 0.1957 mm/px; centre px (403, 246.5). **Main layout source.**
- **PH-I:** `File:Nintendo-Game-Boy-Advance-Purple-FL.png` (Indigo, 3/4 front, Evan-Amos) — colours, bottom-edge ports.
- **PH-IF / PH-IB:** `File:Game Boy Advance front.jpg`, `File:Game Boy Advance back.jpg` (Indigo).
- **PH-RC:** `File:Game Boy Advance rear 20240213.jpg` (clear unit, back, **GBA cart inserted**).
- **PH-GBC:** `File:Gameboyadvance gbccart by zeartul.JPG` (front, **GBC cart inserted**, shows protrusion).
- **PH-C:** `File:Game Boy Game Paks.jpg` (GBA pak, near-orthographic, blank label). Scale 0.0836 mm/px.

---

## 1. Console overall shape

### 1.1 Overall size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Width (x) | **144.5 mm** | H | WP, D.com (144), iFx (145) |
| Height (y) | **82 mm** | H | WP, D.com, iFx |
| Depth (z), maximum (at the grips) | **24.5 mm** | H | WP, D.com (iFx: 24) |
| Mass | 140 g | H | WP, D.com |

**Note:** PH-F gives a silhouette ratio of 1.81 vs 144.5 / 82 = 1.76, so PH-F was scaled separately in x and y.

### 1.2 Front outline (polygon, right half; mirror for the left — measured left side matches within ~1.5 mm)

| x (mm) | y (mm) | Where | Conf. |
|---|---|---|---|
| 0 | +41.0 | top centre ("Nintendo" plateau) | M |
| +30 | +41.0 | plateau ends | M |
| +37.5 | +39.4 | start of the slope | M |
| +45.2 | +35.9 | slope down to the shoulder | M |
| +49.0 | +34.7 | | M |
| +55 | ≈+33 | shoulder (R button region) | L/M |
| +60.4 | +32.2 | | M |
| +64.2 | +29.3 | top-right corner turns down | M |
| +65.6 | +24.8 | right end (rounded end) | M |
| +66.9 | +18.9 | | M |
| +68.2 | +13.0 | | M |
| +69.4 | +7.1 | | M |
| +70.3 | +1.3 | | M |
| +71.1 | −4.6 | | M |
| +71.9 | −10.5 | | M |
| **+72.2** | **−16.3** | **widest point (below centre)** | M |
| +71.9 | −19.3 | | M |
| +70.5 | −22.2 | | M |
| +67.9 | −25.1 | bottom-right corner | M |
| +61.8 | −28.1 | | M |
| +53.0 | −31.0 | | M |
| +45.7 | −33.9 | | M |
| +38.7 | −36.9 | bottom edge (shallow arc) | M |
| +29.9 | −38.7 | | M |
| +18.5 | −40.2 | | M |
| 0 | **−41.0** | bottom centre | M |

Reading the shape:
- **Rounded ends:** each end is a tall, slightly convex curve that **leans outward towards the bottom**
  (x = 64 at y = +30, max x = 72.2 at y = −16), then turns into the bottom edge through a corner of r ≈ 10–12 mm.
  The top-end corner is tighter, r ≈ 6–8 mm. (The "rounded ends" of the brief are not semicircles.)
- **Top edge:** a flat central plateau 60 mm wide (x ±30) at y = +41, sloping ~8 mm down to the shoulders, where
  L/R sit.
- **Bottom edge:** one shallow convex arc: y = −41 at the centre, −37 at x = ±39, −28 at x = ±62 (approx. circle
  R ≈ 200 mm in the middle portion).

### 1.3 Thickness profile and fillets

| Item | Value | Conf. | Notes |
|---|---|---|---|
| Grips (ends, x beyond ±45) | **24.5 mm** (max) | H/L | WP gives the maximum; the grips are the thickest part — the back curves out into two rounded hand grips (PH-IB). |
| Centre section (behind the screen, battery and slot) | ≈20–22 mm | L | Visual estimate from PH-IB/PH-I; the back is flatter in the middle and the battery cover sits on this plateau. |
| Front face | flat; the bezel is ~0.5 mm proud | L | |
| Front perimeter fillet | ≈2 mm | L | |
| Back-to-side blend at the grips | large, r ≈8–10 mm (round "rolled" grips) | L | PH-IB |
| Shell split | seam around the sides ≈9–10 mm behind the front face | L | PH-I |

---

## 2. Front layout (centres in mm, origin = outline centre)

| Part | Centre (x, y) | Size | Conf. | Notes |
|---|---|---|---|---|
| **Screen bezel** (black lens plate) | (≈0, ≈+0.5) | **≈80.5 × 65 mm** (x −40.6…+39.8, y +33.0…−32.0) | M | PH-F. Top corners r ≈5 mm; the **bottom edge bulges down slightly** (convex) and the bottom corners are large, r ≈12 mm. "GAME BOY ADVANCE" printed in the lower band at y ≈ −21.5. |
| **Screen window** (opening in the printed lens) | (0, **+5.7**) | **≈68 × 44 mm** (y +27.7…−16.3) | M | Square-ish corners r ≈1 mm. |
| Active LCD area | (0, ≈+5.7) | **61.2 × 40.8 mm**, 240 × 160 px | H | WP |
| **D-pad** | (**−53.9, +9.3**) | span **≈19.1 mm**, arm width **≈6.5 mm** | M | Light grey, protrudes ≈2.5 mm (L), shallow centre dimple, arrow embosses on the arms. |
| **B** | (**+46.5, +8.5**) | Ø **≈9.2 mm** | M | Domed, protrudes ≈2 mm (L). |
| **A** | (**+59.3, +13.0**) | Ø ≈9.2 mm | M | A–B centre distance **13.6 mm**, axis rises **≈19°** to the right. |
| **START** (upper) | button (**−45.4, −10.5**) | round, Ø **≈4.6 mm** | M | Each button sits at the right end of a horizontal stadium-shaped recessed label plate ≈15.5 × 5.8 mm extending to the left (to x ≈ −58). |
| **SELECT** (lower) | button (**−45.9, −19.3**) | Ø ≈4.6 mm | M | Same plate. Centre spacing 8.8 mm. |
| **Power LED** | (**+44.8, +29.3**) | Ø ≈2.3 mm | M | Green when on (red when batteries are low). "POWER" printed to its right. |
| **Speaker grille** | centre (**+51.3, −12.8**) | **6 slots**, each ≈15 × 1.2 mm, pitch ≈2.9 mm | M | Slots are parallel, tilted ≈12–15° (rising to the right), stacked vertically in a block ≈16 × 17 mm. |
| "Nintendo" oval emboss | (0, ≈+38) | ≈14 × 3.5 mm | M | On the top plateau. |

## 3. Shoulders, edges, back

| Part | Location | Size | Conf. | Source |
|---|---|---|---|---|
| **L / R buttons** | top-left / top-right corners, on the **back half** of the shoulders, x ≈ ±(46…68) | ≈22–24 mm along the top edge, ≈9–10 mm deep (z), wrapping round the corner ≈10 mm down the end | L | PH-I, PH-IF, PH-IB (light grey parts visible at both top corners) |
| **Cartridge slot** | top edge, **back half**, centred in x | opening ≈59 × 9 mm (fits the 57–58.5 mm pak) | L | PH-RC |
| **Link port (EXT)** | top edge, between the slot and R, x ≈ +30…+38 | ≈11 × 6 mm | L | Inferred (not confirmed by a citable source; WP only says "top edge") |
| **Headphone jack (3.5 mm stereo)** | **bottom edge**, right of centre, x ≈ +20…+25 | Ø 3.5 mm in a small frame, "PHONES" printed | L | PH-I |
| **Volume control** | **bottom edge**, right of the jack, x ≈ +32…+40 | slider/wheel window ≈8 × 3 mm, "VOLUME" printed | L | PH-I; WP says "volume slider" |
| **Power switch** | **bottom edge, left grip**, x ≈ −55…−48 | slider ≈7 × 4 mm | L | PH-I shows a control there; not confirmed by text sources |
| **Battery cover** | back, centred in x (≈+1 to +2 from the front view), lower half: y ≈ −1 … −40 | **≈60 × 45 mm**, latch tab at top centre, embossed "GAME BOY" plate on it | L | PH-IB, PH-RC |
| Model sticker | back, directly above the battery cover, full width of the cover | ≈70 × 17 mm | L | PH-IB |
| Screws | back: 2 near the top corners (x ≈ ±47, y ≈ +27), 2 near the bottom corners (x ≈ ±62, y ≈ −22) | tri-wing | L | PH-IB |

## 4. Cartridge sitting position

| Item | Value | Conf. | Source |
|---|---|---|---|
| Cart orientation | label faces the back; connector down | H | |
| GBA pak protrusion | **≈5 mm above the local rim of the slot cut-out** in the back shell; its top is **≈8 mm below the front "Nintendo" plateau**, so it is **not visible from the front** | L | PH-RC (back) and the front photo `Gameboyadvance gbacart by zeartul.JPG` (no cart visible) |
| Visible area from the back | top ≈23 mm of the pak (grip + top of label) | L | PH-RC |
| Estimated pak position | top at y ≈ +33, bottom at y ≈ −2, back face ≈1 mm inside the back surface | L | Derived: a GBC cart (65.5 mm) sticks out **≈22–23 mm** above the plateau in PH-GBC; both cart types bottom out on the same connector, so the 35 mm pak's top is 30.5 mm lower. |
| GB/GBC cart in a GBA | sticks out ≈22–23 mm above the top plateau | L/M | PH-GBC |

---

## 5. Colours (sRGB) and finish — Indigo unit

No official colour codes found. Photo samples are from PH-I (studio light). Confidence: L.

| Part | Photo sample | Model value | Finish |
|---|---|---|---|
| Body (Indigo) | #695C9F (lit), #4F447A (shade) | **#5E5296** | satin, slight sheen |
| Screen lens / bezel | #3C3E3E | **#2B2D2E** (near-black) | gloss |
| Bezel print ("GAME BOY ADVANCE") | — | #E8E8E8 | print |
| D-pad, A, B, Start, Select | #C5C6C4 / #C9C9C8 | **#C8C8C6** (light grey) | matte |
| L / R | — | **#C6C7C5** (same light grey) | matte |
| LCD (off) | #72746F | **#5E625C** | |
| LED | — | on **#38E048**, off **#3A3F38** | |
| GBA pak | #535857 | **#4C5052** (dark grey) | matte |

Other launch colours (WP): Arctic, Black, Orange (JP), Fuchsia (translucent), Glacier (translucent).

---

## 6. GBA Game Pak

**Coordinates (pak):** origin at the centre of the 57 × 35 label face, x right, y up, z towards the label.
Measured on PH-C; "↓ from top" = distance from the top edge.

### 6.1 Size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Width (body) | **57 mm** | H | WP |
| Width (top band) | **≈58.3 mm** measured; WP text says 60 vs 58 | M | PH-C ratio 698 / 682 px = 1.023. **Conflict:** WP prose "6 cm top vs 5.8 cm bottom". Use 57 / 58.5. |
| Height | **35 mm** | H | WP (PH-C: 35.4) |
| Thickness | **7.5 mm** | H/M | WP; some listings 8 mm |

### 6.2 Outline

| Feature | Value | Conf. |
|---|---|---|
| **Raised / wider top end** | a top band **≈7.6 mm tall** that is ≈0.7 mm wider on each side than the body; its lower boundary is a soft curve rising towards the sides | M |
| Top corners | rounded r ≈2.5–3 mm | M |
| Bottom corners | r ≈1 mm; on the **back, bottom corner**, a small notch (for the GBA mode-detect switch) | H (WP) for the notch; L for its size (≈3 × 3 mm) |
| Top band thickness | possibly slightly thicker than the body (grip lip) | L — not confirmed |

### 6.3 Front details

| Feature | Position | Size | Conf. |
|---|---|---|---|
| Embossed "GAME BOY ADVANCE" logo plate | top band, centred, ≈4–5 mm ↓ from top | ≈28 × 3 mm | M (text, optional) |
| **Label recess** | **8.4 to 30.8 mm ↓ from top** (y +9.1 … −13.3), centred | **43.8 × 22.4 mm**, corner r ≈1 mm | M |
| **Arrow** | below the label, 32.0–33.8 mm ↓ from top, centred | flat down-pointing triangle **≈10.4 × 1.8 mm** | M |
| Back | one tri-wing screw; connector opening at the bottom edge | — | H (Epilogue) / L |

---

## 7. Reference photos (Wikimedia Commons)

- Front, straight-on (Arctic, transparent, **best layout ref**): https://commons.wikimedia.org/wiki/File:GBA_Controls_detailed.png
- Indigo 3/4 front (Evan-Amos, studio): https://commons.wikimedia.org/wiki/File:Nintendo-Game-Boy-Advance-Purple-FL.png
- Indigo front (slightly from below): https://commons.wikimedia.org/wiki/File:Game_Boy_Advance_front.jpg
- Indigo back: https://commons.wikimedia.org/wiki/File:Game_Boy_Advance_back.jpg
- Back with a GBA pak inserted: https://commons.wikimedia.org/wiki/File:Game_Boy_Advance_rear_20240213.jpg
- Front with a GBA pak inserted: https://commons.wikimedia.org/wiki/File:Gameboyadvance_gbacart_by_zeartul.JPG
- Front with a GBC cart inserted: https://commons.wikimedia.org/wiki/File:Gameboyadvance_gbccart_by_zeartul.JPG
- Other colours (same geometry): https://commons.wikimedia.org/wiki/File:Nintendo-Game-Boy-Advance-Black-FL.png ,
  https://commons.wikimedia.org/wiki/File:Nintendo-Game-Boy-Advance-White-FL.jpg
- Disassembled: https://commons.wikimedia.org/wiki/File:Game_Boy_Advance_desmontada.jpg
- Paks: https://commons.wikimedia.org/wiki/File:Game_Boy_Game_Paks.jpg ,
  https://commons.wikimedia.org/wiki/File:Nintendo-Game-Boy-Advance-Cartridge.jpg ,
  https://commons.wikimedia.org/wiki/File:Gba-cartridge.png

## 8. Gaps / open questions

- No orthographic **top, bottom or side** photo was found; edge-port positions (link port, power switch, volume,
  headphones) and the L/R geometry are L.
- The thickness at the centre vs the grips is a visual estimate; only the 24.5 mm maximum is published.
- The pak's protrusion is derived, not measured directly.
- Battery cover size is from perspective photos (L).
