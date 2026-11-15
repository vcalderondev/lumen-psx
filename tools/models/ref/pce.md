# PC Engine (NEC PI-TG001, original white, Japan) + HuCard: geometric reference

Research only. No 3D model files were downloaded or copied. Overall size is from published specs. The top layout was
**measured by pixel scan of a straight-down Wikimedia Commons photo**, scaled to 140 × 140 mm. Front/back/side
details come from 3/4 studio photos (lower confidence).

**Confidence key:** **H** = published spec or several sources agree. **M** = measured from a near-orthographic
photo scaled to a known dimension (about ±1–1.5 mm). **L** = estimated from perspective photos or inferred.

**Coordinate system (console):** origin at the centre of the 140 × 140 plan, on the table plane. +x right (seen
from the front), +y up, +z towards the front. Front edge z = +70, rear z = −70, sides x = ±70, top y ≈ 38.

Primary sources:
- **WP-en:** Wikipedia "PC Engine": "14 by 14 by 3.5 centimeters", one controller port, RF output only, rear
  expansion port, power switch locks the card.
- **jisaku:** jisaku.com glossary "PC Engine (NEC)": 14 × 14 × 3.8 cm, about 500 g.
- **RC:** retro-community.pages.dev PC Engine hardware page: 14 × 14 × 3.8 cm; HuCard ≈85 × 54 mm, ≈1.7 mm thick.
- **CM:** ConsoleMods wiki "TG16 Model Differences" (via search excerpt): single mini-DIN controller port on the
  front, power input on the left, RF out on the right, large expansion port at the rear, "HuCard slot on the top
  front". "Dimensions for Game Cartridges": HuCard 84 × 53 × 2 mm.
- **WP-talk:** Talk:TurboGrafx-16 archive: HuCard 85 × 54 × 2 mm.
- **MW:** magweasel.com "PC Engine (White)" (via search excerpt): all white except the **green** power switch; card
  slot at the dead centre; push the power switch **to the right** and the guard closes over the card.
- **PH-T:** `File:PC Engine (top view).jpg` (1793², straight down). Silhouette 505 × 495 px → sx = 0.2772,
  sz = 0.2828 mm/px, centre px (399.5, 397.5). **Main layout source.**
- **PH-FL / PH-BL:** `File:NEC-PC-Engine-Console-FL.png`, `File:NEC-PC-Engine-Console-BL.jpg` (Evan-Amos studio).
- **PH-H:** `File:PC Engine HuCARD.jpg` (card back, straight-on).

---

## 1. Overall shape

### 1.1 Size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Width (x) | **140 mm** | H | WP-en, jisaku, RC |
| Depth (z) | **140 mm** | H | same |
| Height (y) | **38 mm** | M | jisaku, RC (3.8 cm) |
| Mass | ≈500 g | L | jisaku only |

**Conflicts:** WP-en gives **35 mm** height; one feature article (search excerpt) gives 135 × 130 × 35 mm. Most
sources and the photo proportions support 140 × 140 × 38. Possibly 35 mm is the body without the raised top/arch.

### 1.2 Plan outline and edges

| Item | Value | Conf. |
|---|---|---|
| Plan | square 140 × 140 | H |
| Plan corner radius (vertical edges) | ≈4 mm | M (PH-T) |
| Top perimeter fillet | ≈2–3 mm | L |
| Shell split | horizontal seam all round at **y ≈ 12–13 mm** (lower base / upper cover) | L (PH-FL) |
| Front face | **not flat**: the central arch section runs right out to the front edge; the two front corner "wings" (with the vents) have their upper front edges stepped/chamfered back by ≈3–5 mm | L (PH-FL) |

## 2. Top layout (centres in mm; x right, z towards the front)

| Part | Position | Size | Conf. | Notes |
|---|---|---|---|---|
| **Central raised "tombstone" plateau** | x ±39.4; rear end is a **semicircle r ≈39.4** centred at z ≈ −20.7 (apex z ≈ −60.1); straight sides from z −20.7 to the front edge | 78.7 mm wide, ≈130 mm long | M | Raised ≈1.5–2 mm above the surrounding top (L). The "PC Engine" logo sits on it at (0, z ≈ −10). |
| **HuCard tray (recess)** | x ±28.4, from z **+10.6** to the front edge (+70) | **56.8 wide × 59.4 deep** | M | Open-topped tray, floor ≈3–4 mm below the plateau (L). Floor has fine fore-aft ribs and an embossed arrow pointing back. |
| **Card slot mouth** | at the back of the tray, z ≈ +10.6 … +13.4 | ≈56 × 3 mm opening | M/L | The card slides **backwards** into it, label up. |
| **Finger scoop** | front of the tray, x ±16, from z ≈ +38.9 to the front edge; rear end a semicircle r ≈16 | 31.9 wide | M | Goes down through the front of the tray (deep U-shaped notch in the front, PH-FL). |
| **Card guard (lock bar)** | inside the tray, front-left, x ≈ −28…−19, z ≈ +62…+67 | small black L-shaped bar | M | Moves over the card when the power switch is ON. |
| **Vent grooves (left)** | x −66.4 … −39.2, z +31.8 … +51.0 | **5 grooves**, ≈27 mm long, pitch ≈4.2 mm, ≈2 mm wide | M | |
| **Vent grooves (right)** | x +39.2 … +66.6, same z | same | M | |
| Left label boss ("AC ADAPTER") | x −66 … −49, z −31 … −1 | rounded rectangle, ≈1 mm relief | M | Marks the DC jack on the left side below. |
| Right label boss ("ANT SWITCH") | x +49 … +66, z −31 … −1 | same | M | Marks the RF jack and channel switch on the right side. |
| "POWER ON/OFF" marking | x −58 … −44, z +55 … +68 | | M | Above the front-left power slider. |
| "PAD" marking | x ≈ +52, z +55 … +68 | | M | Above the front controller port. |
| "NEC" logo | (−48, −56) | | M | Text, optional. |
| Hudson "HE" mark | (+50, +24) | | M | Text, optional. |

## 3. Front, sides, back

| Part | Location | Size | Conf. | Source |
|---|---|---|---|---|
| **Power slider** | **front face, left**, x ≈ −50, at the seam height (y ≈ 8–18) | green knob ≈8 mm wide × 10 mm tall, protrudes ≈3 mm, vertical grip ribs | M (x) / L (size) | PH-T, PH-FL, MW. Slides horizontally; **to the right = ON**, which also pushes the guard over the HuCard so it cannot be removed (WP-en, MW). |
| **Controller port** (8-pin mini-DIN, "PAD") | **front face, right of centre**, x ≈ **+45…+52**, y ≈ 10 | port Ø ≈9.5 mm in a round recess Ø ≈13 mm | L | PH-FL, CM, top marking |
| **DC power input** | **left side**, z ≈ −16 (under the "AC ADAPTER" boss) | round jack Ø ≈8 mm surround | L | CM, PH-T |
| **RF output + channel switch** | **right side**, z ≈ −16 (under "ANT SWITCH") | RCA-style jack + small slide switch | L | CM, PH-T, PH-FL/PH-BL |
| **Expansion connector** | **rear face, centred**, low | opening **≈76 mm wide × ≈9 mm tall**, bottom edge ≈3 mm above the table; gold card-edge contacts visible inside | M (width, PH-T) / L (height) | PH-T, PH-BL; WP-en |
| Rear grip ribs | rear face, upper band above the connector | fine vertical ribs | L | PH-BL |

## 4. HuCard sitting position

| Item | Value | Conf. |
|---|---|---|
| Orientation | flat, label up, contact end first, sliding towards the rear | H (WP-en: "front slot") / M |
| Inserted depth | ≈26 mm inside the slot mouth; **≈59 mm of the card stays visible in the tray** (85 − 59 ≈ 26) | L/M (tray depth from PH-T) |
| Height | card lies on the tray floor, ≈3–4 mm below the plateau | L |

---

## 5. Colours (sRGB) and finish

Photo samples from PH-FL (studio, clean unit). No official codes found. Confidence: L.

| Part | Photo sample | Model value | Finish |
|---|---|---|---|
| Body (top and base) | #EBEBE6 / #E7E6E1, base #DBDCD6 | **#E8E7E1** (slightly warm white) | matte |
| "PC Engine" logo | (sample contaminated) | **#E4472B** (red-orange outline logo) | print |
| Power slider | #398156 (shaded) | **#5DB07E** (mint green) | matte |
| Controller port interior | #30302D | **#2E2E2C** | |
| Card guard, slot interior | — | **#151515** | |
| Expansion contacts | orange-gold | **#C8873C** | metal |

---

## 6. HuCard

**Coordinates (card):** origin at the card centre, x across the 54 mm width, y along the 85 mm length (contact end
at y = +42.5), z towards the label (front).

### 6.1 Size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Length | **85 mm** (CM: 84) | H | RC, WP-talk, CM |
| Width | **54 mm** (CM: 53) | H | RC, WP-talk, CM |
| Thickness | **≈2 mm** (RC: 1.7) | M | CM, WP-talk / RC |
| Corner radius | ≈2.5 mm | M | PH-H |

For comparison only: an ISO ID-1 credit card is 85.60 × 53.98 × 0.76 mm (HuCards share the footprint, not the
thickness).

### 6.2 Back side (PH-H)

| Feature | Position | Size | Conf. |
|---|---|---|---|
| Contact strip (38 contacts) | along the contact end, ≈1.7 mm from the edge | **≈50.5 × 4.9 mm** | M (count H, WP "HuCard") |
| Black protected area over the chip (glossy polymer) | from the contact end down to ≈31 mm from it, x ±25.7 | **≈51.5 × 31 mm**, lower corners r ≈3 mm | M |
| Grip ridges | far end, centred, ≈1–6 mm from the edge | ≈16 × 5 mm arched group of ridges | M/L |
| Rest of the card | plain card plastic | — | M |

### 6.3 Front (label side)

| Feature | Value | Conf. |
|---|---|---|
| Label | printed sticker covering most of the face (≈50 × 75 mm, ~2 mm margins) | L — no source with dimensions found |
| Colour | card body usually white/off-white **#F2F1EC**; black area **#111111**; contacts gold **#C9A24E** | L |

---

## 7. Reference photos (Wikimedia Commons)

- Top, straight-down (**best layout ref**): https://commons.wikimedia.org/wiki/File:PC_Engine_(top_view).jpg
- Front 3/4 (studio): https://commons.wikimedia.org/wiki/File:NEC-PC-Engine-Console-FL.png ,
  https://commons.wikimedia.org/wiki/File:NEC-PC-Engine-Console-FR.jpg
- Back 3/4 (expansion port): https://commons.wikimedia.org/wiki/File:NEC-PC-Engine-Console-BL.jpg ,
  https://commons.wikimedia.org/wiki/File:NEC-PC-Engine-Console-BR.jpg
- Other angles: https://commons.wikimedia.org/wiki/File:PC_Engine_(3-4_right_view).jpg ,
  https://commons.wikimedia.org/wiki/File:PC_Engine_(high_angle).jpg ,
  https://commons.wikimedia.org/wiki/File:PCEngine_FirstModel.jpg
- Set with controller: https://commons.wikimedia.org/wiki/File:PC-Engine-Console-Set.jpg
- Category: https://commons.wikimedia.org/wiki/Category:PC_Engine_(PI-TG001)
- HuCard (back): https://commons.wikimedia.org/wiki/File:PC_Engine_HuCARD.jpg

## 8. Gaps / open questions

- No straight-on front, side or rear photo, and no dimensioned drawing, was found. Heights, port sizes and
  side-jack positions are L.
- Height conflict 35 vs 38 mm (WP-en vs jisaku/RC).
- HuCard label size and the exact inserted depth are not documented.
- ConsoleMods and Magweasel could not be fetched directly (403/500); their content is from search excerpts.
