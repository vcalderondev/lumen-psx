# Super Famicom (SHVC-001) / PAL Super Nintendo (SNSP-001A) + cartridge: geometric reference

Research only. No 3D model files were downloaded or copied. Numbers come from published specs and from
Wikimedia Commons photos, which were measured with pixel scans and scaled to the published overall size.
The Japanese SFC and the PAL SNES share the same shell. Only the printed text differs: the PAL slot reads
"PAL VERSION" and the front band carries the "SUPER NINTENDO" wordmark. Model either one from this sheet.

**Confidence key:**
- **H**: published spec, or several sources agree.
- **M**: measured from a near-orthographic photo or scan and scaled to a known dimension (about ±3 mm).
- **L**: estimated from perspective photos (about ±5 mm or worse), or inferred.

**Coordinate system (console):** origin at the centre of the 200 × 242 plan bounding box, on the table plane.
+x points right (seen from the front), +z points to the front, and +y points up (y = 0 is the table).
So the rear edge is at z = −121, the front at z = +121, and the outer sides at x = ±100.

**Facts that correct the brief:**
- **The SFC and PAL consoles have no purple or lavender parts.** The lavender and purple buttons belong only to
  the US model. On the SFC/PAL model:
  - POWER and RESET are **dark grey**.
  - EJECT is **mid grey**, roughly the colour of the top panel.
  - The only colour is the four-dot logo (red, yellow, green, blue) and the red power LED.
- **The slot is behind the centre** (centre at about z = −25), not in the middle. The POWER, EJECT and RESET row is
  in front of the centre.
- **POWER is a slide switch, RESET is a square push button, and EJECT is a wide pivoting lever.** Also, on the
  SFC/PAL the POWER slider **locks the cartridge**: a claw enters the slot when the power is on (ja.WP).
- **The cartridge has no "side grip notches" in the NTSC sense.** The SFC/PAL cartridge is a plain rounded block
  without the NTSC bottom grooves. Its grip features are **five short horizontal slots on each side edge near the
  rounded top**. Section 5 gives details.

**Sources (abbreviations used below):**
- **ja.WP:** Japanese Wikipedia, スーパーファミコン. Lists 200 mm (W) × 242 mm (D) × 72 mm (H), about 600 g
  (the weight looks low), 28-pin EXT connector, power lamp, and the power-switch cart lock.
  https://ja.wikipedia.org/wiki/スーパーファミコン
- **RTC:** Retrotech Collection wiki: "about 200 × 242 × 72 mm, varies by region", about 1.1 kg.
  https://wiki.retrotechcollection.com/Super_Famicom
- **MP:** manuals.plus listing for SHVC-001: 7.87 × 9.53 × 2.83 in (= 200 × 242 × 72 mm), 2.87 lb.
  https://manuals.plus/asin/B0001PW264
- **CHM:** Computer History Museum catalogue 102747863: 2 × 7 5/8 × 9 1/2 in (51 × 194 × 241 mm). This
  disagrees on height and width; probably measured without the top/feet or loosely. Not used.
- **D.com:** dimensions.com "SNES": 68 × 203.2 × 254 mm. This is the US model. **Do not use it.**
- **Mini:** Nintendo Classic Mini Super Famicom spec, 110 × 133 × 40.5 mm (https://www.nintendo.co.jp/clvs/spec/).
  It is a 0.55-scale replica. Its plan ratio (1.209) matches 242/200 = 1.21 exactly, so it is a good proportion check.
- **WP-cart:** English Wikipedia "Super Nintendo Entertainment System Game Pak". PAL/JP cartridge
  86 (H) × 130 (W) × 20 (T) mm; NTSC 88 × 136 × 20. https://en.wikipedia.org/wiki/Super_Nintendo_Entertainment_System_Game_Pak
- **Forum-cart:** a forum poster's measurement of a cart: 127.75 × 87 mm overall, 97.75 mm across the base opening
  (from search snippets of retropie.org.uk / shmups threads; the threads themselves would not load).
- **iFixit:** Super Famicom teardown https://www.ifixit.com/Teardown/Super+Famicom+Teardown/96507. Six 4.5 mm
  Gamebit screws on the underside; the eject mechanism is a spring-loaded lever on an axle.
- **BitBuilt scan thread** (https://bitbuilt.net/forums/threads/super-famicom-pal-snes-console-scan.5499/), used
  for **part breakdown only** (files not downloaded). The parts are: top shell, bottom shell, "top cover" (the
  dark panel), "front cover" (the logo band), controller-port cover, eject button, reset button, power switch,
  and EXT port cover.
- **Photo measurements:**
  - **[EUROTOP]** Commons "Super NES European versio.jpg": a PAL Classic Mini shot almost straight down (camera
    tilt about 23°, corrected).
  - **[PALVER]** Commons "Nintendo SNES - PAL Version.jpg": a real PAL console, near top-down, perspective
    corrected using the convergence of the side edges.
  - **[EA-FL/FR/BR]** Evan-Amos studio photos.
  - **[ANGEL]** a flatbed scan of an SFC cartridge (orthographic).
  - **[BOTTOM]** "Super Famicom SHVC-001 bottom 20090614.jpg".

  Where the two top-view photos disagree, both values are given.
- **Possible cartridge drawing:** US Des. 320,203, "Cartridge for game machine", K. Ashida (Nintendo), JP priority
  19 Jun 1989, granted 24 Sep 1991. It has 27 orthographic figures (front, rear, top, bottom, side).
  The timing and inventor fit the SFC cassette, but I did not confirm this from the drawings themselves. **Check
  it first:** if it is the SFC cart, it is the best source for the cart outline.
  https://patents.google.com/patent/USD320203S/en.
  I found no equivalent design patent for the SFC console. US Des. 333,686 is the **US** SNES (Lance Barr), not this shell.

---

## 1. Console overall shape

### 1.1 Overall size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Width (x) | **200 mm** | H | ja.WP, RTC, MP, Mini ratio |
| Depth (z) | **242 mm** | H | ja.WP, RTC, MP, Mini ratio |
| Height (y), table to the highest point of the top deck, feet included | **72 mm** | H | ja.WP, RTC, MP |
| Mass | ~1.1–1.3 kg (ja.WP says ~600 g, which looks wrong) | L | RTC / MP / ja.WP |

The CHM height (51 mm) and the US-model figures (68 × 203 × 254) are rejected, as explained in the source list.

### 1.2 Construction (what to build)

The shell is a **bottom shell** (tray) plus a **top shell** (a light-grey frame) with three inserts in it:
1. The large **dark-grey top cover** (the deck panel holding the slot, EJECT, POWER, RESET and logo dot).
2. The light-grey **front cover**, a sloped band carrying the wordmark and LED.
3. A pill-shaped **controller-port cover** on the front face.

All the inserts are separated from the frame by thin seam grooves, about 0.8–1 mm wide (L).

### 1.3 Plan-view outline (top view)

| Element | Value | Conf. | Source |
|---|---|---|---|
| Outline | Rounded rectangle 200 × 242 | H | |
| Rear corner radius (plan) | R ≈ 9–10 mm | L | EUROTOP |
| Front corner radius (plan) | R ≈ 12–15 mm. Visibly larger and softer than the rear corners. | L | EUROTOP, EA-FR close-up |
| Top shell vs bottom shell | The top shell overhangs the bottom shell by about 1.5–2.5 mm all round; there is a visible step at the parting line. | L | EA-FR close-up |

### 1.4 Top surface zones, front to back (z values are console coordinates)

| Zone | z from → to | x extent | Description | Conf. | Source |
|---|---|---|---|---|---|
| Rear strip (light) | −121 → −99 | full width | About 21 mm deep. It curves or slopes down into the rear vent face. Two longitudinal **seam grooves** at x ≈ ±68 run from the dark panel to the rear edge. | M/L | EUROTOP 20.4×1.085 = 22 mm; PALVER ≈ 22 mm |
| Dark top cover (deck) | −99 → +73 | x ≈ −86 … +86 (≈ 171–172 mm wide) | **Flat, and the highest plane** (y ≈ 70–72). It is about 172 mm deep, with small corner radii (R ≈ 3–5). It sits flush or within about 1 mm of the frame, inside a seam groove. | M | EUROTOP: back edge 22 mm and front edge 193.7 mm from the rear; PALVER front edge 192 mm from the rear |
| Side rails (light) | full depth | x ±86 → ±100 | About 14 mm each, including the large shoulder fillet that rolls down into the side walls. | M/L | EUROTOP (dark panel ≈ 171 mm wide) |
| Front band / front cover (light, wordmark + LED) | +73 → +113 | about x −80 … +80, bounded left and right by seam grooves that continue the side-rail lines | **Slopes down towards the front** (a convex or inclined band, not flat). Estimated drop is 20–25 mm over about 40 mm, about 25–30° average; see 1.5. | M (z) / L (slope) | EUROTOP 195–234 mm from the rear; tieto/wikipal perspective analysis |
| Front nose fillet | +113 → +121 | full width | Rounds into the vertical front face. | L | |

### 1.5 Height profile (side section at x = 0), as buildable shapes

All the heights in this section are **L** (perspective estimates) except the 72 mm total.
- **Feet:** about 2–3 mm. The body underside sits at y ≈ 2–3.
- **Bottom shell (tray):** from the underside to the parting line at **y ≈ 25–28**. Vertical walls, inset about 2 mm
  inside the top-shell outline. Plan corner radius is about 2 mm less than the top shell's.
- **Top shell walls:** vertical from the parting line to about **y ≈ 55–60** on the sides and back. They then roll
  into the top through large shoulder fillets: **R ≈ 12–15 mm on the sides** and **R ≈ 6–8 mm at the back**.
- **Front face:** essentially vertical from the bottom to about **y ≈ 38–42**. The front-top nose fillet is
  **R ≈ 8–10**. Above it, the **front band rises at about 25–30°** to the front edge of the dark deck at
  y ≈ 70–72 (z ≈ +73).
- **Deck (dark top cover):** flat at y ≈ 70–72 from z ≈ −99 to +73.
- **Rear:** the rear strip slopes or curves down from the deck (z −99) to the rear edge. The rear face is about
  55–60 mm tall and carries the vent slots (§3.10). Its lower-left area holds the connector bay (§3.11).

A simple build recipe:
1. **Base:** rounded box 196 × 238 × 26, plan R 8/12, at y 2 → 28 (the bottom shell).
2. **Top shell:** rounded box 200 × 242, plan R 10 (rear) / 14 (front), from y 26 to 58. Bevel the side and rear
   top edges with R 13 (sides) and R 7 (rear).
3. **Deck:** a slab 172 × 172, top at y = 72, centred at z = −13 (its front at +73). Loft or blend it down to the
   frame, and add the front wedge: from the deck front edge (y 72, z +73) down to the nose (y ≈ 50, z ≈ +113),
   then fillet R 9 into the front face.

**Check these heights against the side and 3/4 photos in §6 before final modelling. They are the weakest numbers
in this sheet.**

---

## 2. Cartridge slot

| Item | Value | Conf. | Source |
|---|---|---|---|
| Position | Centred in x. Centre at **z ≈ −25** (EUROTOP −27, PALVER −22), so about 96 mm from the rear edge and 146 mm from the front. | M | both top views |
| Outer recess (the darker rounded-end tray containing the flaps) | **≈ 137–150 mm long (x) × 21–23 mm wide (z)**, stadium-shaped ends (end radius ≈ 10). EUROTOP: x −69…+68, z −38.5…−15.2. PALVER: about 150 × 20. | M | |
| Bevelled light rim around the recess | About 3 mm wide (EUROTOP z −41…−12) | L | |
| Actual opening for the cart | Must pass a 128–130 × 20 mm cart, so **≈ 131–132 × 21 mm** (inferred) | L | |
| Dust flaps | Two spring flaps meeting at the slot's centreline; they swing down and in when a cart is pushed in. At rest they sit about 3–5 mm below the deck, flush with the recess floor. | L | photos, general knowledge |
| Markings | A small **"V" arrow notch** at the centre of the slot's front edge. Two short transverse tick grooves at **x ≈ ±55** inside the recess. Printed line ("USE CASSETTE WITH SUPER FAMICOM MARK ONLY" / "PAL VERSION"), not to be modelled. | M | EUROTOP, EA-FL |
| Cart lock | When POWER is ON, a claw protrudes into the slot (no visible geometry needed) | H | ja.WP |

---

## 3. Controls and features (centre positions; x right, z front, y up)

| # | Feature | Centre (x, z) | Size (x × z) | Height / other | Conf. | Source / notes |
|---|---|---|---|---|---|---|
| 3.1 | **POWER** bezel (light-grey raised ring around a square well) | (**−53**, **+42**) | **≈ 31–33 × 34–37** outer | The ring is about 2–2.5 mm wide and about 1–2 mm above the deck | M | EUROTOP (−48 raw, recentred by −5 using EJECT symmetry), PALVER (−55 to −60, +41) |
| 3.1a | POWER slider | in the well | A dark plate ≈ 26 × 20, with a **curled grip tab** (a ski-jump lip ≈ 4–5 mm high) across its rear | Slides along z. **OFF = forward, ON = rearward**; travel ≈ 5–6 mm. "ON"/"OFF" legends and two indicator dots sit just left of the bezel (x ≈ −72). The "POWER" text is on the plate's front half. | M (shape) / L (travel) | EA-FL close-up |
| 3.2 | **EJECT** lever | (**≈ 0**, **+42**) | **≈ 58 × 31–34** | A rounded rectangle (R ≈ 4) with a slightly domed or inclined top, standing about 4–6 mm proud of the deck at the front edge. Pivots on a transverse axle at its rear (iFixit): press the front down ≈ 6–8 mm to lift the cart. | M (size) / L (travel) | EUROTOP 58 wide; PALVER ≈ 60 × 29 |
| 3.3 | **RESET** bezel | (**+53**, **+40**) | **≈ 31 × 34–37** | Same light ring as POWER | M | EUROTOP (+57.5 raw, recentred) |
| 3.3a | RESET button cap | in the bezel | ≈ 24 × 26, square with R ≈ 2 | Flat or slightly concave textured top. Dark grey. Proud of the deck by about 2 mm. Push travel about 3 mm. "RESET" text at its front edge. | M/L | |
| 3.4 | **Logo dot area** (do not model the logo; note position only) | (**≈ +55**, **≈ −77**) | Circle **Ø ≈ 22–25** on the dark deck | A slightly lighter disc carrying 4 coloured ovals (blue at top, red right, yellow bottom, green left), each ≈ 7 × 5 mm. | M | EUROTOP, PALVER |
| 3.5 | **Power LED** | (**≈ +55**, **≈ +100**) on the sloped front band | Pill slot **≈ 10–11 × 3.5–4** | Translucent red lens recessed in a slightly raised oval surround about 13 × 6. "POWER" printed above (behind) it. | M | EUROTOP z +99, PALVER z +101 |
| 3.6 | **Wordmark area** (do not draw text) | x ≈ −75 … −5, z ≈ +85 … +105 on the front band | ≈ 70 × 20 | — | M | photos |
| 3.7 | **Controller-port cover** ("pill" panel) on the front face | x centred, z = +121 (front face) | **≈ 148 mm long × ≈ 18–20 mm tall**, stadium ends | Centred at about y ≈ 18–22. A separate insert surrounded by a seam groove and roughly flush. Raised or engraved circled **"1"** (left end, x ≈ −64) and **"2"** (right end, x ≈ +64), each Ø ≈ 9. | M (length, from the tieto frontal view) / L (height) | |
| 3.8 | **Controller sockets** ×2 | **x = −33 and +33** (±3), y ≈ 20 | Socket recess **≈ 36–39 × 12–14** | The recess is about 6–8 mm deep. The outline is a rectangle with **one end square and one end semicircular**. **The rounded end faces outward** on both ports (port 1 rounded on the left, port 2 rounded on the right), so the pair is mirror-symmetric. Inside are two separate light-grey pin blocks, about 1.5 mm apart: a **3-hole block with a rounded end** (outer side) ≈ 11 × 7, and a **4-hole block with a square end** (inner side) ≈ 15 × 7. Hole Ø ≈ 2.5, pitch ≈ 3.5. | M (layout, from EA-FL close-ups) / L (mm) | Port pinouts are mirrored between ports 1 and 2 (snes.nesdev.org Controller_connector), which matches the mirrored sockets |
| 3.9 | Seam grooves | — | ≈ 0.8–1 mm wide, 0.5–1 mm deep | Around the dark deck, around the front band, the two rear-strip grooves at x ≈ ±68, and around the port pill | L | photos |
| 3.10 | **Rear vents** | rear face, z = −121 | **~22 vertical slots**, each ≈ 3 × 20–24 mm, pitch ≈ 7 mm, spanning about 150–165 mm of the upper rear face | — | M (count) / L (sizes) | EA-BR edge count = 22 |
| 3.11 | **Rear connector bay** | Lower rear, **x ≈ −95 … 0** (left half when seen from the front) | ≈ 95 wide × 20–25 tall, recessed ≈ 10–15 mm into the bottom shell | From the console centre outward, left to right as seen from the front: **MULTI OUT** (x ≈ −10…−25), **RF OUT** jack (x ≈ −40), **CH1/CH2 slide switch** (x ≈ −50), **DC IN** (x ≈ −85). Dark grey interior. | M (layout) / L (mm) | EA-BR and BOTTOM agree on the order and on the left-half placement |
| 3.12 | **Feet** | Front pair at (**±72, ≈ +100**). Rear pair at (**±75, ≈ −80**). | Front: black rubber discs Ø ≈ 15. Rear: grey moulded ring bosses Ø ≈ 20 (outer). | About 2–3 mm tall | L | BOTTOM |
| 3.13 | **EXT (expansion) port cover**, underside | (**≈ +50, ≈ −45**) | ≈ 68 × 17 | A flush snap-in cover with a "◁" tab at its outer (+x) end. It covers the 28-pin connector (ja.WP). | L | BOTTOM |
| 3.14 | Underside other | — | — | A row of about 20 short vent slots along the underside near the front edge. A large shallow rectangular label recess at the centre. 6 screw holes (4.5 mm Gamebit, iFixit). | L | BOTTOM |

The POWER, EJECT and RESET row sits in front of the console centre (z ≈ +40 to +42). The slot is behind it
(z ≈ −25). The logo disc is at the right-rear of the deck.

---

## 4. Colours (sRGB) and finish

Values were sampled from the neutral-white studio Evan-Amos photos and checked against outdoor photos. The real
plastic is slightly warmer, and **old units yellow** towards #D6D2C4 or beyond. If the scene should look "new",
use the base values.

| Part | Base hex | Range seen | Finish |
|---|---|---|---|
| Body: top-shell frame, rear strip, side rails, front band, bottom shell, port pill | **#D2D2CF** | #C8C8C8 – #D7D7D7 (shaded faces read about #A8ACAC) | Matte with a fine stipple texture (satin highlights on large radii) |
| Dark top cover (deck panel) | **#A0A1A2** | #868A8E (outdoor) – #A8A8A8 (studio) | Matte, fine texture |
| Slot recess, flaps | **#868686** | #808080 – #8A8A8A | Matte |
| EJECT lever | **#AAAEAC** | ≈ deck grey, slightly lighter and cooler | Semi-matte |
| POWER slider plate and RESET cap | **#686C6D** | #666A6C – #6C7170 | Semi-matte (RESET top lightly textured) |
| POWER/RESET bezels | **#D2D2CF** (body colour) | — | Matte |
| Controller-socket pin blocks | **#A8A8A8** | — | Matte |
| Logo dots | red **#E62E1E**, blue **#1C64C8**, green **#3FBF4A**, yellow **#F0D246** | sampled #F12F1A / #0373CF / #43D64C / #EFD547 (studio, saturated) | Printed, slight gloss |
| Logo disc base | **#B0B2B2** | — | Matte |
| LED lens | off **#6E4A48**, on **#FF3020** (emissive) | — | Translucent gloss |
| Rear connector bay interior | **#5A5B5B** | — | Matte |
| Feet (front rubber) | **#2A2A2A** | — | Rubber matte |
| Cartridge shell | **#CDD0D1** | #C4C9CB – #D6D9D6 (scan) | Matte, fine texture |
| Cartridge screws | brass/gold-tone **#B6AC7E** | — | Metallic |

Wordmark and legends are dark grey/black prints (#3A3A3A). Not to be modelled.

---

## 5. Cartridge (Super Famicom / PAL)

### 5.1 Size

| Dimension | Value | Conf. | Source |
|---|---|---|---|
| Width (x) | **128–130 mm** | H/M | WP-cart 130; Forum-cart 127.75 |
| Height (y, long axis, connector edge → rounded top) | **86–88 mm** | H/M | WP-cart 86; Forum-cart 87 |
| Thickness (z) | **20 mm** | H | WP-cart |

**Conflict:** the orthographic scan [ANGEL] has a height/width ratio of 0.70, which gives ≈ 90 mm at 128 wide. The
scanner may not use square pixels. **Use 128 × 87 × 20.**

### 5.2 Outline and shape

Fractions are from [ANGEL]; mm assume 128 wide and 87 tall.
- **Front view:** a rectangle with a **rounded top edge**. The front face rolls back into the top surface with
  **R ≈ 6 mm**: the top 6.6% of the front view, about 5.7 mm, is this curved band.
- **Back-top edge:** smaller radius, R ≈ 1.5–2.
- **Plan view (looking down on the top):** front corners rounded **R ≈ 6**, back corners about R 1–2
  (from the bottom-view photo "SNES Cartridge Comparison bottom.jpg"). M/L.
- **Front face layout:** three vertical zones:
  - **Side rails** each **≈ 21 mm wide** (x ±43.8 → ±64).
  - A **centre panel ≈ 87.5 mm wide** below the label.
  - Seam lines at x ≈ ±43.8 run from the top edge down to the label, and again below it. M.
- **Grip slots (the "side grips"):** **5 horizontal slots per side**, cut into the side edges and wrapping around
  the front corners:
  - Each slot is ≈ 4.5 mm long on the front face, ≈ 1.5–1.8 mm tall and ≈ 1 mm deep in silhouette.
  - Pitch ≈ 4 mm.
  - They span from ≈ 7 mm to ≈ 25 mm below the top.
  - They continue as grooves along the 20 mm side faces. M.
- **No NTSC bottom grooves or notches.** The sides are straight down to the connector edge. H (WP-cart: "smooth
  curved front and no grooves").

### 5.3 Label area (front)

- A recess **≈ 107 × 36–37 mm** with corner radius ≈ 3. M.
- Its top edge is ≈ 6 mm below the top of the cart. It is centred in x and spans x −53.4 → +53.4.
- Recess depth ≈ 0.5 mm. L.

### 5.4 Front details

- **Finger-grip recess:** a horizontal stadium **≈ 72 × 6 mm**, centred in x, with its centre **≈ 57 mm below the
  top** (≈ 30 mm above the connector edge). It is ≈ 1.5 mm deep. Inside are fine horizontal ribs in three segments
  separated at x ≈ ±12. M.
- **Two screws:** at **x = ±56.4**, **≈ 6 mm above the bottom edge**. Visible head Ø ≈ 4–4.5 mm. They are
  3.8 mm security (Gamebit) screws, brass-coloured on many SFC carts. M (position) / H (screw type, common
  knowledge).

### 5.5 Back details

- **Raised side rails** ≈ 21 mm wide each, matching the front rails.
- A **recessed centre panel** ≈ 87 mm wide holding a small warning label, ≈ 72 × 15 mm, near the bottom.
- Two small round holes near the bottom corners, aligned with the screws. M/L from the PAL back photo.

### 5.6 Bottom edge

- A connector opening **≈ 98–99 mm wide**, centred (Forum-cart 97.75; photo ≈ 98.8). M.
- About 10–12 mm deep (z) between the shells. L.
- The PCB edge sits recessed ≈ 5–8 mm inside. L.

### 5.7 Inserted height

- The cart sits down until roughly the **bottom edge of its label** reaches the deck. In both inserted-cart photos
  (SHVC-001 with Lethal Enforcers; PAL with Mario All-Stars), only about 3–5 mm of shell is visible below the label.
- **Visible above the deck ≈ 45–48 mm** (top ≈ y 117–120). **Insertion depth ≈ 40 mm.** M/L.

---

## 6. Reference photos

All of these are on Wikimedia Commons unless noted.

**Console (SFC):**
- Front-left 3/4 (studio, the best overall view): https://commons.wikimedia.org/wiki/File:Nintendo-Super-Famicom-Console-FL.jpg
- Front-right 3/4: https://commons.wikimedia.org/wiki/File:Nintendo-Super-Famicom-Console-FR.jpg
- Back-left 3/4: https://commons.wikimedia.org/wiki/File:Nintendo-Super-Famicom-Console-BL.jpg
- Rear 3/4 (vents and connector bay): https://commons.wikimedia.org/wiki/File:Nintendo-Super-Famicom-Console-BR.jpg
- Reset close-up: https://commons.wikimedia.org/wiki/File:Nintendo-Super-Famicom-Console-FL_(Reset_Button).jpg
- Console set: https://commons.wikimedia.org/wiki/File:Nintendo-Super-Famicom-Set-FL.jpg
- Underside (feet, EXT cover, rear bay): https://commons.wikimedia.org/wiki/File:Super_Famicom_SHVC-001_bottom_20090614.jpg
- Almost frontal (front-face proportions): https://commons.wikimedia.org/wiki/File:Nintendo_Super_Famicom_Tietokonemuseo.JPG
- With a cart inserted: https://commons.wikimedia.org/wiki/File:SHVC-001_with_Lethal_Enforcers_cartridge_20191228.jpg

**Console (PAL):**
- Near top-down: https://commons.wikimedia.org/wiki/File:Nintendo_SNES_-_PAL_Version.jpg
- 3/4 studio: https://commons.wikimedia.org/wiki/File:Wikipedia_SNES_PAL.jpg
- With a cart inserted: https://commons.wikimedia.org/wiki/File:PAL-SNES.png
- **Top-down plan** (Classic Mini, 0.55 scale, the cleanest plan view): https://commons.wikimedia.org/wiki/File:Super_NES_European_versio.jpg
- Other PAL photos: https://commons.wikimedia.org/wiki/Category:Super_Nintendo_Entertainment_System_(SNSP-001)

**Controller plug** (mates with the sockets): https://commons.wikimedia.org/wiki/File:Nintendo-Super-NES-Controller-Plug.jpg

**Cartridge:**
- Orthographic front scan: https://commons.wikimedia.org/wiki/File:Super-Famicom-Angelique-1994-Cartridge.jpg
- Front 3/4: https://commons.wikimedia.org/wiki/File:SFC_Super_Mario_World_cassette_20090828.jpg
- PAL bottom edge: https://commons.wikimedia.org/wiki/File:SNES_Cartridge_PAL.jpg
- PAL vs NTSC bottom: https://commons.wikimedia.org/wiki/File:SNES_Cartridge_Comparison_bottom.jpg
- SFC vs NTSC shells: https://commons.wikimedia.org/wiki/File:SNES-SFAM-Cartridges.jpg
- Possible orthographic drawings: US Des. 320,203 sheets on Google Patents (link in the source list)

**Disassembly and part breakdown:**
- iFixit teardown: https://www.ifixit.com/Teardown/Super+Famicom+Teardown/96507
- iFixit device page and guides: https://www.ifixit.com/Device/Nintendo_Super_Famicom_SHVC-001

---

## 7. Gaps and caveats

- **No published side elevation.** The height profile in §1.5 (wall heights, front-band slope, fillet radii) is
  photo-estimated (L). It is the main thing to verify against EA-FL/FR/BR before final modelling.
- No published drawing of the console (no design patent found). Plan positions come from two independent
  near-top views. They agree within about 5 mm on every feature (deck edges, slot, button row, LED).
- Controller-socket mm sizes, eject travel, flap depth, EXT cover and feet positions are L.
- Cartridge height conflict: 86 (WP) vs 87 (forum) vs ≈ 90 (scan ratio). 87 is recommended. D320203 may settle it.
