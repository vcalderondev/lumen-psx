# Sega Mega Drive model 1 (JP / PAL) + Mega Drive cartridge: geometric reference

Research only. No 3D model files were downloaded or copied. Unlike the NES, **no orthographic design drawing was found**
for the Mega Drive. Sega's US design patents for this machine did not turn up, and Sega Retro and ConsoleMods block
automated access. Overall dimensions are published. Feature positions are estimated from perspective and near-front
Commons photos, a 150 px top-view thumbnail (Vectronics), and pixel sampling. Expect ±5 mm on most feature
positions.

**Confidence key:** **H** = published spec, or several sources agree. **M** = one good source, or a measurement
consistent across two photos. **L** = estimated from perspective photos, or inferred.

**Console coordinates:** the origin is the centre of the 280 × 212 plan box, on the table plane. +x points right (seen
from the front), +z points to the front, and +y points up (y = 0 is the table). The sides are at x = ±140, the front at
z = +106 and the back at z = −106.

**Sources:**
- **RTC:** retrotechwiki.com "Sega Mega Drive / Genesis (Model 1)", per search excerpt: 280 W × 210 D × 70 H mm,
  1.2 kg. The fetched page lists: front headphone socket with volume slider, rear 8-pin DIN A/V and RF out, two 9-pin
  controller ports, and the Mega-CD expansion edge.
- **Vec:** vectronicscollections.org/consoles/segagenesis.php (Genesis model 1, VA-series): 2.25 H × 10.95 W ×
  8.45 D in = **57 × 278 × 215 mm**. It has front, back, side, top and 3/4 photos (thumbnails only).
- **iFixit:** iFixit Sega Genesis topic, per search excerpt: 10.95 × 8.45 × 2.25 in. This is the same figures as Vec,
  so probably not independent.
- **CHM:** computerhistory.org/revolution/artifact/210/890 (Genesis): 2½ × 11 × 8 in = 64 × 279 × 203 mm.
- **WP:** Wikipedia "Sega Genesis": cartridges are inserted in the top; stereo headphone jack on the front; RF and
  8-pin DIN on the back; the expansion edge connector is at the bottom right (for the Mega-CD).
- **SR (search excerpts only; segaretro.org is behind a bot wall):**
  - Asian, Japanese and Korean consoles have a **cartridge lock** that stops cartridges being removed with the power on.
    That is why Eastern carts have a **cut-out on the left side**.
  - Japanese-style carts take a more rounded approach, with ridges.
  - Labels are layered over the top and front of the cartridge.
- **YT-cart:** YouTube video "Sega Megadrive/Genesis Game Cartridge Dimensions" (xUD5FnGvVXk) description: width 109,
  height 70, depth 17 mm.
- **CM:** consolemods.org "Dimensions for Game Cartridges" (search excerpt; the page itself is Cloudflare-blocked):
  - PAL / Genesis carts: 70 high × 109 wide × 17 thick.
  - Japanese Mega Drive carts: 67 high × 93 wide × 17 thick.
- **SF:** shinforce.com article (403 to fetch; search excerpt only): says the JP body is about 113 mm vs about 102 mm
  for the US shell. The axis isn't stated, and it conflicts with CM.
- **Photos:** Commons, Evan-Amos JP and EU model 1 studio shots, plus the Science Museum near-front photo. See §6.

---

## 1. Console overall

| Quantity | Value | Conf. | Source / conflicts |
|---|---|---|---|
| Width (x) | **280** | H | RTC 280, Vec 278, CHM 279. |
| Depth (z) | **212** | M | RTC 210, Vec 215, CHM 203 (CHM rounds to whole inches). We split the difference between RTC and Vec. |
| Height of main deck (top surface around the disc) | **≈ 47** at the rear | L | From photos: the deck is about 4/5 of the total height. |
| Height to top of the round disc | **≈ 57–60** | M | Vec 57 (2.25 in). **Conflicts:** CHM 64, RTC 70 (may include feet, or be rounded up). |
| Feet | about 3 mm tall rubber feet, 4 of them near the corners | L | Not visible in the photos used. |

### Plan-view outline and body

1. **Main body:** a block 280 × 212 with vertical corner radius about 6 mm (L).
   - **Both front corners are cut at roughly 45° in plan.** This shows clearly on the right front in the JP and EU
     photos: the "MEGA DRIVE / SEGA" lettering ends where the bevel begins. Make the cut about 18 mm along each edge
     (L).
   - The rear corners are plain radiused corners (L).
2. **Height profile (side view, front to back):**
   - Base plinth: y 0 … 6. It is set back about 3 mm all round, giving a shadow line (L).
   - **Lower front band:** a vertical face from y ≈ 6 to y ≈ 22, recessed about 4 mm from the upper front. This is the
     dark horizontal channel that holds the controller ports (L, Science Museum and JP photos).
   - **Upper front face:** from y ≈ 22 to y ≈ 40. It slopes back about 15° from vertical (the "sloped front"), and
     there is a fine horizontal groove at about y = 30 (L).
   - **Front top edge:** a large rounded edge, R ≈ 8–10 mm, from the front face into the deck (L).
   - **Deck:** rises gently from y ≈ 42 at the front edge to y ≈ 47 at the back (nearly flat; L).
   - Rear face: vertical. Rear top edge R ≈ 4 mm (L).
3. **The round disc assembly** (this defines the look). It is three stacked cylinders, centred at **x ≈ +35, z ≈ −4**
   (L/M: top-view thumbnail gives x +33, z ≈ 0; Science Museum front photo gives x ≈ +38):
   - **Plinth ring:** Ø ≈ **160**, rising about 3 mm above the deck, with a 45° chamfered outer edge. Its flat top
     annulus (about 10 mm wide) carries printed text in the JP version ("AV INTELLIGENT TERMINAL" on the left arc,
     "HIGH GRADE MULTIPURPOSE USE" on the right arc). The EU ring has no text (L).
   - **Raised disc (cap):** Ø ≈ **140**, top at y ≈ 57–60, with a bevelled/rounded edge R ≈ 4 mm. The top is
     **glossy** and slightly domed (about 1 mm crown; L).
   - At the front-right, where the plinth meets the deck, there is a **wedge-shaped ridge (fairing)**. It runs from
     the plinth's front-right (about 4 o'clock) outward toward the right front corner and dies into the deck about
     40 mm from the plinth. It is about 6 mm tall at the plinth. The same fairing appears faintly at the front-left
     (about 8 o'clock), where the deck meets the control panel. These are the "ridges" visible in all 3/4 photos (L).
4. **Vent block:** a raised rectangular block **behind the control panel on the left side**. It is about 40 W × 120 D,
   at x ≈ −120 … −80, z ≈ −80 … +40, and stands about 4 mm above the deck (L). Its top has about **13 transverse
   slats** (running along x) with about 8 mm pitch, slot width about 4 mm and depth to dark about 4 mm (L).
   - Conflict: the Science Museum front view places it at x ≈ −100 … −50. That view has perspective shrink toward the
     centre, so the thumbnail-based x range was preferred.

## 2. Cartridge slot (on the disc)

| Feature | Value | Conf. | Notes |
|---|---|---|---|
| Slot position | In the **rear half** of the disc. Centreline at z ≈ disc centre − 30 (so z ≈ −34), running along x, centred on the disc (x ≈ +35) | L/M (JP and EU photos) | |
| Slot opening | ≈ **114 × 22 mm** with rounded ends (R ≈ 4) | L | It must pass the 109 × 17 PAL/US cart. The JP slot is the same size. |
| Dust flaps | Two spring-loaded black flaps meeting at the slot centreline (they open downward and inward). They sit about 3 mm below the disc top. | M (photos show the twin-flap seam) | |
| "16-BIT" plate | A trapezoid set into the disc **in front of the slot**, running from just in front of the slot to the plinth ring's outer edge: back edge about 40 wide, front edge about 55 wide, about 50 deep (z ≈ −10 … +40 relative to the console origin). Flush or slightly recessed (0.5 mm), glossy black. Gold "16-BIT" lettering (not drawn). | L | |
| Coloured band at the front of the plate | The front about 12 mm of the trapezoid, crossing the plinth ring. **JP: magenta. EU: off-white.** It carries "POWER ● ON" printing. | M (colour), L (size) | |
| Power LED | In that band, at about x = disc centre − 3, z ≈ +38, Ø ≈ 3 mm | L | Red. |
| Cartridge lock (JP only) | Third position of the power slider ("ON / CARTRIDGE LOCK / OFF"). A tab inside the slot engages the cut-out on the left side of JP carts. Not visible from outside. | H (SR + JP photo text) | PAL power slider has only ON / OFF. |

**Inserted cartridge:** it stands upright in the slot with its label face toward the front (+z).
- The slot connector is roughly 30 mm below the disc top. So a 70 mm PAL cart **sticks out about 40 mm above the disc
  top** (y ≈ 98–100).
- A JP cart (67 high) sticks out about 37 mm.
- Confidence L: estimated from the EU demo-unit and Science Museum photos. No published figure was found.

## 3. Visible features (console coordinates, mm)

| Feature | Position / size | Conf. | Notes |
|---|---|---|---|
| **Control panel recess** (front-left of deck) | x ≈ −119 … −49 (70 W), z ≈ +52 … +100 (48 D), about 4 mm deep, vertical walls, inner corner R ≈ 2. The raised rim at its right edge steps down into the deck. | L/M (Science Museum front photo gives the x span; top thumbnail gives the depth) | Printed labels, not drawn: "PHONES VOL" or (EU) "VOLUME", "POWER", "RESET". |
| Volume slider | Left part of the panel. A slot running **front-to-back** (along z), about 8 × 32 mm, centre x ≈ −108, z ≈ +76. Ridged black knob about 10 W × 12 D × 8 H. Scale printing 0 / 5 / 10 beside it. | L | Travel about 20 mm. |
| Power slider | Back-right of the panel. A slot running **left-right** (along x), about 28 × 10 mm, centre x ≈ −72, z ≈ +64. Black knob about 10 × 8 × 7 with a **red stripe** on top. | L | JP has 3 positions (ON, CARTRIDGE LOCK, OFF), travel about 14 mm. PAL has 2 positions, travel about 10 mm. |
| Reset button | Front-right of the panel. A rectangle about **20 W × 10 D**, centre x ≈ −72, z ≈ +86. It stands about 2 mm proud of the panel floor and has a fine dot texture. | L | **JP: blue. EU: light grey.** |
| Headphone jack (3.5 mm stereo) | On the upper sloped front face, below the panel: x ≈ **−100**, y ≈ 30. Bezel about Ø 8. | L | Model 1 only. |
| Controller ports ×2 | On the lower front band: centres at x ≈ **+28** and **+63** (about 35 mm pitch), y ≈ 14. Each is a DE-9 male in a trapezoid recess, about **22 W × 12 H** outer, 8 mm deep. | L | Printed "CONTROL 1 / CONTROL 2" below them (not drawn). |
| Front lettering area "MEGA DRIVE   SEGA" | On the deck's front edge strip, right half: x ≈ +55 … +125, z ≈ +95, about 8 mm tall | L | Position only. White text. |
| Ring lettering (JP) | On the plinth annulus, left and right arcs | M | Position only. Red/magenta text. |
| Expansion port (Mega-CD) | **Right side face**, lower rear portion: a removable cover plate about 70 × 20 mm, at x = +140, y ≈ 6 … 26, z ≈ −60 … +10 | L | WP: "edge connector on the bottom right". |
| Rear panel | Left to right as seen from behind: DC 9 V jack, RF out, 8-pin DIN A/V. All in the lower half, around y ≈ 20. Exact spacing was not measured. | M (presence), L (positions) | RTC/WP. |
| EXT 9-pin port (early units) | Rear face | M (WP) | |

## 4. Colours and finish

Sampled from the Commons JP studio photo (Evan-Amos, bright lighting, so the sampled blacks read lighter than the
real plastic). Suggested base colours are tuned for neutral lighting.

| Part | Sampled sRGB | Suggested base | Finish | Conf. |
|---|---|---|---|---|
| Body (deck, front, sides) | #303030 … #444346 (lit) / #2E2E2E (front) | **#1C1C1E** | **matte**, fine sand texture | M |
| Raised disc top, 16-BIT plate | #131313 | **#0E0E0E** | **gloss** (clear reflections in all photos) | M |
| Plinth ring | #2E2F2F | #1A1A1A | satin | M |
| Vent block slats | #3A3A3A | #1C1C1E (same as the body) | matte | M |
| JP band (front of 16-BIT plate) | #992353 (lit) / #6B2C43 (shade) | **#8E1F4F** (magenta) | printed, gloss | M |
| EU band | photo: off-white | **#E6E2D8** | printed, gloss | M |
| "16-BIT" lettering (not drawn) | #CCA775 | **#C9A46A** (gold) | metallic print | M |
| JP reset button | #0066B3 | **#0062AE** | satin | M |
| EU reset button | photo: light grey | **#C6C6C6** | satin | M |
| Power slider red mark | photo | **#D0202A** | gloss | L |
| JP ring lettering | photo | #B0303A | print | L |
| LED | photo | **#FF2A1A** (emissive) | gloss | L |
| Controller-port recess | n/a | #0A0A0A | matte | L |

## 5. Cartridges

### PAL / Genesis-style Sega cartridge (also fits JP consoles physically)

| Quantity | Value | Conf. | Source |
|---|---|---|---|
| Width | **109** | M/H | YT-cart, CM |
| Height | **70** | M/H | YT-cart, CM. It is consistent with a flash-cart project note that the PCB must be under about 60 mm tall to fit the case. |
| Thickness | **17** | M | YT-cart, CM |
| Outline (face) | The lower portion (the part that sits in the slot) is a plain rectangle 109 wide. The upper portion has **large rounded shoulders** (R ≈ 15) and a gently curved top edge. | L | J-cart and EU demo-unit photos |
| Grip | Four horizontal finger slots on each side, near the left and right edges of the front face, in the upper half. Each is about 12 × 2.5 mm, at about 5 mm pitch. | L | Commons Mega_drive_j-cart.jpg (Codemasters shell, but this matches Sega's PAL shell styling) |
| Label | Front face label about 80 W × 50 H, centred, in a recess about 0.5 mm deep. A second label wraps over the top edge onto the top end (about 15 mm). | L | SR excerpt ("labels layered over the top and front"); photos |
| Top end | Rounded crown; no separate lip | L | |
| Colour | Black **#1A1A1A**, satin/matte. Label glossy. | M | |
| Protrusion when inserted | About 40 mm above the disc top | L | §2 |

### Japanese Mega Drive cartridge

| Quantity | Value | Conf. | Source |
|---|---|---|---|
| Width | **93** | L/M | CM. **Conflict:** SF says about 113 (axis unclear). |
| Height | **67** | L/M | CM |
| Thickness | **17** | M | CM |
| Outline | Rounder than PAL. The top edge is a continuous arc and the side edges are softly rounded, with horizontal grip ridges on the upper sides. There is a **notch cut into the left side** (for the cartridge lock). | M (notch: SR), L (shape) | SR excerpts |
| Label | Front plus top wrap (same scheme as PAL) | M | SR |
| Colour | Black, satin | M | |
| Protrusion when inserted | About 37 mm above the disc top | L | |

**Recommendation for the animation:** use the PAL cart (109 × 70 × 17) unless the frontend needs JP authenticity.
Its dimensions have two consistent sources.

## 6. Reference photos

- JP model 1, front-left 3/4 (best overall reference, sharp detail on the panel, disc text and magenta band):
  https://commons.wikimedia.org/wiki/File:Sega-Mega-Drive-JP-Mk1-Console-Set.jpg
- EU (PAL) model 1, front-left 3/4 (white band, grey reset button, ON/OFF-only power slider):
  https://commons.wikimedia.org/wiki/File:Sega-Mega-Drive-EU-Mk1-wController-FL.jpg
- Genesis model 1 bare (same body, US trim): https://commons.wikimedia.org/wiki/File:Sega-Genesis-Mod1-Bare.jpg
- **Near-front view** with a cart inserted (Science Museum; good for the front-face layering and port positions):
  https://commons.wikimedia.org/wiki/File:Sega_Mega_Drive_with_J-Cart_in_the_Science_Museum_-_2025-08-14.jpg
- PAL unit with carts inserted (protrusion reference): https://commons.wikimedia.org/wiki/File:Mega_drive_demo_ds-16.JPG
- PAL-style cartridge face (J-Cart): https://commons.wikimedia.org/wiki/File:Mega_drive_j-cart.jpg
- Top, sides, back and front photos (Genesis model 1; thumbnails only):
  https://vectronicscollections.org/consoles/segagenesis.php
- Visual-only, not downloaded: the CGTrader / Sketchfab / TurboSquid preview images of "Sega Mega Drive" models can be
  used to check the disc and fairing proportions.

## 7. Gaps and conflicts

- **Height:** 57 (Vec) vs 64 (CHM) vs 70 (RTC). We used deck about 47 and disc top about 58. This needs a real
  measurement.
- **Depth:** 203 / 210 / 215. We used 212.
- **No orthographic drawing**, so these are all L:
  - Disc diameters (160 / 140)
  - Disc centre
  - Panel, vent and port positions
- **Biggest single improvements:**
  - A real top-down photo (the Vectronics full-size image was not retrievable).
  - A Japanese design registration (意匠登録) for the Mega Drive (HAA-2510), which was not searched successfully.
- **JP cart size:** CM 93 × 67 vs the SF claim of about 113. Not resolved.
- **Cart insertion depth:** inferred, not published.
- **Blocked sources:**
  - segaretro.org (Anubis bot wall)
  - consolemods.org (Cloudflare)
  - shinforce.com (403)
