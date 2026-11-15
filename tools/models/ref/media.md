# Game media geometric reference: PS1 jewel case + disc, DVD keep case, UMD + UMD case

Research notes for 1:1 Blender (Python) models. All dimensions are in mm.
Confidence: **H** = standard or several agreeing primary sources; **M** = several secondary
sources agree or derived from H values; **L** = inferred or remembered, not confirmed in a source,
so check it against photos or a physical object before treating it as final.

No 3D model files were downloaded or used. Photos are linked by URL only.

Primary sources used:
- **[ECMA-130]** ECMA-130, 2nd ed. (June 1996), *Data interchange on read-only 120 mm optical data disks (CD-ROM)*, §8 (dimensional characteristics). https://ecma-international.org/wp-content/uploads/ECMA-130_2nd_edition_june_1996.pdf
- **[ECMA-365]** ECMA-365, 1st ed. (June 2005), *Data Interchange on 60 mm Read-Only ODC, Capacity 1,8 Gbytes (UMD)*, §10 (case), §11 to 12 (disk). https://ecma-international.org/wp-content/uploads/ECMA-365_1st_edition_june_2005.pdf
- **[WP-ODP]** Wikipedia, *Optical disc packaging*. https://en.wikipedia.org/wiki/Optical_disc_packaging
- **[WP-KC]** Wikipedia, *Keep case*. https://en.wikipedia.org/wiki/Keep_case
- **[WP-UMD]** Wikipedia, *Universal Media Disc*. https://en.wikipedia.org/wiki/Universal_Media_Disc
- **[WP-CD]** Wikipedia, *Compact disc*. https://en.wikipedia.org/wiki/Compact_disc
- **[US5938021]** US patent 5,938,021, *Hinged housing for holding a disc-shaped information carrier* (describes the jewel-case corner hinge: arms, pins and bores). https://patents.google.com/patent/US5938021
- Supplier and print-template pages (cdrom2go, Blank Media Printing, 4over4, Walmart listings), cited inline.

---

## A) PlayStation 1 CD jewel case (standard 10.4 mm, black tray) + PS1 disc

### A.1 Overall case

| Item | Value | Conf. | Source / note |
|---|---|---|---|
| Width (hinge edge to opening edge) | **142.0** | H | WP-ODP "142 by 125 by 10 mm"; cdrom2go same. Retail listings give 142 to 145 (for example "143 x 124 x 10.4", "145 x 125 x 10.4"), so the exact value varies by mould. Use 142.0. |
| Height | **125.0** | H | WP-ODP; cdrom2go. |
| Depth (closed) | **10.4** | H | Industry name "10.4 mm standard jewel case" (supplier listings). WP-ODP rounds it to 10. |
| Material | general-purpose polystyrene (GPPS), injection moulded, 3 parts | H | WP-ODP. |
| Mass | 68 to 100 g (empty case) | M | cdrom2go. |
| Wall thickness (lid face, base floor) | ~1.0 to 1.3 | L | Not published. Typical for GPPS mouldings. |
| Outer corner radius (plan view) | ~0.5 to 1.0 (nearly sharp) | L | Jewel cases have almost square corners. The hinge corners on the left have rounded knuckles, see A.2. |

**Region notes for PS1 (important for "which case"):**
- NTSC-U and NTSC-J PS1 games use the **standard 10.4 mm jewel case with a black tray** (M: WP-ODP and Video game packaging article: "standard North American and Japanese CD cases").
- PAL PS1 games use a **thicker case** for multilingual manuals. WP-ODP gives it as "18 mm thick", with a black tray and the PlayStation logo embossed on the front left of the tray, standardised by Sony from 1997 (M/L: the 18 mm figure is not well sourced, so check it against a real PAL case). Japanese releases add a paper "spine card" (obi) clipped over the hinge edge.
- A GameSpot collector thread reports that the PS1 tray is hinged on the **opposite side** from some generic replacement cases (L). Model the lid hinge on the **left** edge (viewed from the front, closed), which is the normal orientation.

### A.2 Parts and how they nest

The case has three injection-moulded parts (WP-ODP, US 7,299,920 background).

1. **Lid (front), clear GPPS.** A flat front plate, 142 x 125, with shallow top and bottom side walls that overlap the base's top and bottom walls when closed. The left (hinge) end has **two hinge arms**, one at the top-left corner and one at the bottom-left corner. Each arm is a short rounded "knuckle" that wraps around the base's hinge corner. On the inside of each arm a small **pin** points inward (toward the case's centre along the height axis). The pin snaps into a **bore** in the matching hinge wall of the base (US5938021: "pins (19a) on the inner side of the free end portions engage bores (24a) in the hinge walls"; stops (27) limit opening to about 180 to 185 degrees). Hinge axis: parallel to the 125 mm edge, near the back-left edge of the case, about 1.5 to 3 mm in from both the left face and the back face (L).
   - **Booklet tabs:** WP-ODP says lids have "two, four, or six tabs" that hold the liner notes. In the common 2-tab lid the tabs are small rectangular hooks, about 10 x 2 mm and about 1 mm proud of the inner face, moulded into the inside of the lid on the **opening (right) edge**, near the top and bottom corners. The booklet's other edge sits against the hinge-side lip. (Tab position and size: L. Check against `CD Jewel Case (Front).jpg` and `Jewel_Case_02.jpg`.) 4-tab lids add a matching pair on the hinge side.
   - **Booklet:** **120 x 120** (H, WP-ODP), or a 242 x 120 sheet folded in half. Thickness 0.3 (1 leaf) to about 3 (thick PAL manual).
2. **Base (back), clear GPPS.** A flat back plate, 142 x 125, with top and bottom walls running the full width. The left edge carries the **hinge walls** at the top and bottom corners (the bored posts the lid pins click into). Between them is the **spine**: a hollow, downward-open channel the full height of the case, its outer face flat and clear (US5938021 calls this the "box-shaped compartment (23)" between the corner recesses). The right edge is open. The base is the part that is 10.4 deep at the spine. The clear base walls let the inlay's **spine flaps** show through on the left (spine) and right ends.
3. **Tray (insert), black on PS1, opaque GPPS (glossy or satin).** About **136 to 138 x 118 to 121, about 8 to 9 tall** (L, derived: it must fit inside the base and hold a 120 mm disc). It snaps into the base on top of the back inlay card and traps that card.
   - **Centre hub "rosette":** a raised ring that grips the 15 mm centre hole of the disc. It is a circle of about **8 to 12 flexible teeth (petals)** split by radial slots, about 15.0 to 15.2 across the tips, with a slightly domed centre button you press to release the disc. The teeth are about 3 to 4 tall (L; WP-ODP: "central hub of spring-loaded teeth grips the disc by its hole").
   - **Disc well:** a shallow recess about **120.5 to 121** across, about 1.5 to 2 deep. A raised annular **support ring** around the hub, about 33 to 36 across, holds the disc by its clamping area (L).
   - **Finger cut-outs:** two **thumb and finger notches**, one on each side of the disc well (3 and 9 o'clock, that is, toward the hinge and toward the opening edge). Each is a curved recess about 20 to 25 long and 6 to 8 deep in plan, which lets you grip the disc rim (L/M). Some trays have 2 extra small notches.
   - **Corner areas:** the four corners outside the disc well are flat or ribbed triangular lands. On PAL cases the PlayStation logo is embossed at the front left (WP-ODP).
   - **Snap features:** small latches on the tray's top and bottom long edges clip under the base's top and bottom walls (L).
   - The right edge of the tray often has a small raised catch that the lid's right edge clicks over when you close it (L).
4. **Back inlay (tray card):** **150 x 118** including both spine flaps (H, WP-ODP and print templates). Folded into a flattened U: **back panel about 137 to 138 x 118** with **two spine flaps about 6 to 6.5 x 118** that stand up at the left and right ends, inside the base's clear end walls (M: print templates give spines of about 6.35 mm or 0.25 in). The flaps are visible through the closed case's spine.

**Nesting order (back to front):** base, then inlay card (back panel on the base floor, flaps up at the ends), then tray (snapped in, trapping the card), then disc (on the hub, label side up), then booklet (under the lid tabs), then lid (hinged at the left corners). The closed stack is 10.4.

**Open animation pivot:** the lid rotates about the hinge axis (parallel to the Y/125 mm edge) at the back-left corner. It opens to a hard stop at about 180 to 185 degrees (US5938021).

### A.3 Colours and finish

- Lid and base: water-clear, high-gloss GPPS (IOR about 1.59). Light birefringence shows as stress patterns near the hub: see `Photoelasticity - Clear CD case centre.jpg`.
- PS1 tray: opaque black GPPS, gloss to semi-gloss.
- Inlay and booklet: printed coated paper or card.

### A.4 Disc (CD-ROM, PS1)

| Item | Value | Conf. | Source |
|---|---|---|---|
| Outer diameter d10 | **120.0 ± 0.3** | H | ECMA-130 §8.7 |
| Centre hole d1 | **15.0 (+0.1 / -0.0)** | H | ECMA-130 §8.2 |
| Thickness (substrate, label surface height) | **1.2 (+0.3 / -0.1)**; substrate e = 1.2 ± 0.1 | H | ECMA-130 §8.4 and §8.6 |
| First transition area (hub region) | d1 to d2 = **20 (+6 / -0)**; may be thinned 0.2 to 0.4 per side | H | ECMA-130 §8.3 |
| **Clamping area** | **26 to 33 (min)** diameter, flat, full 1.2 thickness | H | ECMA-130 §8.4 |
| Second transition area (stacking ring or mirror band) | 33 to **44 max**; surface may deviate ±0.4 (this is where the moulded **stacking ring** sits) | H | ECMA-130 §8.5 |
| Information area | **44 to 118 max** | H | ECMA-130 §8.6 |
| Inner buffer and lead-in start | ≤ 46; user data from **50.0 (+0 / -0.4)** to ≤ **116** | H | ECMA-130 §8.6 |
| Rim area | 118 to 120, edge may thin (h16 - h15 ≥ 0.6) | H | ECMA-130 §8.7 |
| Edge of centre hole | 45° chamfer ≤ 0.1 or radius ≤ 0.1 | H | ECMA-130 §8.2 |
| Mass | ~15 to 16 g | M | general CD figure |

Model tip: make a 1.2 mm disc with a 15 mm hole. Add a stacking ring as a raised annulus about 0.2 tall, roughly 33 to 38 in diameter, on the data side. Make the clamping area flat from 26 to 33. Use a separate material zone for the information area (46 to 117).

**PS1 colours:**
- **Data (read) side:** **black-tinted polycarbonate**. Through the black substrate the pressed area looks very dark grey or black with a faint purple-blue sheen. The inner hub (15 to about 44) is plain clear or black plastic (H that PS1 discs are black on the bottom: Commons "Playstation disc.jpg", svg.com, Hackaday). Some PAL SLES discs are "black and silver" (vgcollect forum).
- **Label (top) side:** silk-screened lacquer. Most PS1 labels are printed on a silver (bare metallised) or white base, with the PlayStation logo and licence text in a ring near the rim.
- Reflectance of the CD metal layer is ≥ 70 % (ECMA-130). On the PS1 the black tint means the data side reads as dark under visible light.

---

## B) DVD keep case (Amaray-style, 14 mm)

| Item | Value | Conf. | Source |
|---|---|---|---|
| Height | **190** | H | WP-ODP, WP-KC ("fixed at 190 mm and 135 mm"), Walmart and supplier listings |
| Width (front panel) | **135** | H | same |
| Depth (spine) | **14** (WP-KC says "typical 15"; Wii and Wii U 14) | H/M | Supplier listings "14 mm spine"; WP-ODP 14 |
| Material, shell | polypropylene (soft, flexible), single piece, **two living-hinge creases** either side of the spine | H | WP-ODP, WP-KC |
| Material, outer sleeve | thin clear **polyethylene** film welded along the outside of front, spine and back, open at the opening edge so the insert slides in | H | WP-KC |
| Cover insert, full wrap | **273 x 183** trim (129.5 front + 14 spine + 129.5 back) | H | WP-KC, 4over4 |
| (alternative WP figure) | "284 x 184" paper label | L | WP-ODP. Probably includes wrap tolerance; prefer 273 x 183. |
| Booklet or leaflet max | **120 x 180** | H | WP-KC |
| Corner radius (plan view, outer) | ~**4 to 6**, generously rounded | L | Not published. Measure from `Amaray-case.jpg`. |
| Edge profile radius | ~1.5 to 2 on all outer edges | L | |
| Wall thickness | ~1.2 to 1.5 (PP) | L | |

**Structure (open, inside view: left = front-cover panel, right = back panel with disc):**
- **Spine:** 14 wide, flat, between two creases (living hinges). The outer sleeve continues over it. Inside the spine there is often a stiffening rib.
- **Hub (right panel):** in the centre of the right half, a raised **push-button rosette**: a ring of flexible teeth that grips the 15 mm hole. Press the centre to release (WP-KC: "pluck hubs" and "push-to-release hubs"; the original Amaray has a "crown of teeth"). The hub has a raised support ring for the disc of about 34 to 40 diameter. The disc sits in a shallow circular well, about 121 to 122 across, made by a low raised ridge, with **notches or finger cut-outs** at the edges (L for dimensions).
- **Booklet clips (left panel):** two (sometimes four) moulded **clips** on the inner face of the front panel, usually near the hinge side at top and bottom, or a pair of tabs on the outer edge, that hold a manual up to 120 x 180 (H that clips exist, WP-KC; positions L). PS2 cases also have a **memory-card slot** moulded into the left panel (WP-KC).
- **Latches:** small snap bumps near the opening edge, top and bottom, hold it closed (M).
- **"Window" and edge cut-outs:** on the opening (right) edge of the closed case there is a **finger-grip recess** ("Griffmulde"). On many cases it is a fake or vestigial dimple (Commons `DVD-Griffmulde1.JPG` / `-2.JPG`: "fake grasping recess at a DVD case"). Original Amaray cases had no side finger slot and rounder contours round the disc area (DVDTalk forum, L). Many keep cases also have hollow **rectangular openings ("windows") in the top and bottom edges or the front plate** behind the sleeve, which save material. These show as dark rectangles through the clear sleeve (L, check a photo).
- **Colours:** black opaque PP is standard for DVD-Video. Game variants: PS2 is navy blue or black (blue in PAL from 2001), Xbox is lime green, Wii is white, PSP is clear (WP-KC). Black PP has a satin or slightly textured finish. The sleeve is glossy clear.

**Open animation pivot:** two parallel creases 14 apart. The front cover rotates about the front crease (about 180 degrees). For a simple rig, treat the back panel and spine as fixed.

---

## C) UMD (PSP Universal Media Disc) cartridge, disc, and UMD keep case

### C.1 Cartridge (shell), per ECMA-365 §10.3 (H unless noted)

Coordinate frame used below (my derivation from ECMA reference planes): origin = **disc centre**, +Y toward the **round (semicircular) end**, X across the width, Z = thickness. ECMA "top of the case" = the round end. Reference Plane X is the horizontal line through the location hole. Reference Plane Y is the vertical line through the location hole.

| ECMA ref | Meaning | Value | Derived position (origin at disc centre) |
|---|---|---|---|
| L1 | total length | **65.00 ± 0.15** | from y = +32.0 (round end) to y = -33.0 (flat end, on centreline) |
| L3 | total width | **64.00 ± 0.10** | x = ±32.0 |
| R1 | round end ("top") arc | **R 32.00 ± 0.10**, concentric with the disc | a **full semicircle** over the whole width |
| R2 | flat end ("bottom") arc | **R 70.00 ± 0.20**, **convex** (bulging outward) | arc centre at (0, +37.0); meets the sides at y ≈ -25.26 (sagitta ≈ 7.7) before rounding |
| R3 | corners where R2 meets the straight sides | **R 5.00 ± 0.15** | two bottom corners |
| — | straight side edges | — | from y = 0 down to the R3 blends (about 25 mm long) |
| L2 | top to Plane X | 58.30 ± 0.10 | Plane X at y = -26.3 |
| L4 | right side to Plane Y | 11.50 ± 0.10 | Plane Y at x = +20.5 (right in the figure-7 view) |
| L6 / L7 | disc-centre offsets from planes | L6 = 20.50 ± 0.05 (from Y), L7 = 26.30 ± 0.05 (from X) | consistent with the origin |
| L9 | thickness | **4.20 ± 0.10** outside D1; **4.35 max** inside D1 = **Ø 50.00** centred on the disc | the disc region may bulge 0.15 |
| L5, R4, L8 | **crescent bevel** on the round end: the area between R1 and R4 = **R 29.90 ± 0.10** (concentric), within L5 = 3.00 of the top | chamfer depth **0.40 ± 0.10** | a 2.1 mm-wide bevelled band round the semicircular end on one face |
| R5 | all outline edges rounded | **R 0.30 ± 0.10** | |
| D2 | **location hole** (round) | **Ø 3.60 (+0.07/-0)**, depth ≥ 3.60 (L10) | at (+20.5, -26.3) |
| L11, L12, L13, R8 | **alignment hole** (slot) | 41.00 ± 0.10 from Plane Y along X; slot **3.60 x 4.60**, ends R ≥ 1.80 | at (-20.5, -26.3), mirror of the location hole |
| R6 / R7 | hole lead-in / bottom radius | 0.20 / ≤ 0.50 | |
| D3 / D4, L14, L15 | reference pads S1 and S2 (Ø 5.60) on the two holes; S3 and S4 (Ø 4.00) at 47.0 from X, ±20.5 from the centreline | | S3, S4 at (±20.5, +20.7) |
| D5 | **spindle window** (round, centred on the disc) | **Ø 18.00 (+0.08/-0)**, edge R 0.20 | the drive spindle enters here from the **back** |
| L16, L17, L18 | **head window** (laser slot), roughly rectangular | from 17.40 to 35.80 above Plane X (y = -8.9 to +9.5, about 18.4 tall); inner edge 9.80 from Plane Y; open to the **side edge** of the case | slot about 21 wide from about 10.7 off the centre axis out to the side edge, beside the spindle window. Which side it sits on depends on bottom versus top view: ECMA Fig. 9 is a bottom view, where "left" is mirrored (M) |
| L19 | recess next to the head window | ≥ 2.20 deep | |
| L20 to L24, R10 to R13 | head-window chamfers and radii | chamfers 0.60; L24 ≥ 1.50; R10 1.00, R11 ≥ 1.00, R12 ≤ 0.50, R13 0.20 | |
| §10.3.7 | **mis-insert detent**, a notch on one side edge | at 17.30 above Plane X; recess 3.60 x 2.20, 3.40 deep from Plane Z | |
| §10.3.8 | **safe-eject detent**, a rhombic recess at one bottom corner | about 3.4 x 1.4 to 1.8, corner R 1.00 | |
| §10.3.9 | **autoload slots**, one on each side edge | width 2.80, at 0.70 from Plane Z, at 42.30 from Plane X (y = +16.0); detent arc R 31.50 concentric with the disc | small grooves on both straight sides |
| §10.3.10 | **shutter-opener recess**, on the left side | stepped profile 6.5 to 13.8 above Plane X, corners R 2.00 | |

**Plan outline recipe for Blender** (2D curve, origin at disc centre, mm):
1. Semicircle R32 centred at (0,0) from (32,0) through (0,32) to (-32,0).
2. Straight line (-32,0) to (-32, about -25.3).
3. Arc R70 centred at (0,37) from (-32,-25.26) through (0,-33) to (32,-25.26), with both junctions with the sides filleted R5.
4. Straight line back up to (32,0).
Extrude 4.20. Bevel 0.30. Add a 0.40-deep crescent chamfer between R29.9 and R32 on the label face. Cut Ø3.6 and 3.6 x 4.6 holes on the back at (±20.5,-26.3), a Ø18 spindle window and the head-window slot on the back. The Ø50 central area may be 4.35.

**Shell construction and look (M/L, not in ECMA):**
- Two plastic halves (WP-UMD Talk: "is in 2 halves and can be broken open and reassembled"), ultrasonically welded or clipped.
- **Front (label face):** the round-end region shows the disc through a **clear or translucent window** in the shell. Retail game UMDs show the disc's printed label art under the clear upper shell, with the "UMD" logo moulded or printed (check `UMD front.JPG`, `Cars UMD.jpg`, `Psp umd disk.jpg`). The flat-end area carries the Sony and PSP legal text.
- **Back:** opaque shell (most games: black; some titles and UMD Video use coloured or clear shells). It has the Ø18 spindle window, the laser head window (the disc data side is visible through it), and the two locating holes.
- **Shutter:** ECMA says the windows "may be covered by a shutter" and reserves a shutter-opener recess. WP-UMD frames the shutter as something that "would shield" the disc. Shipped retail UMDs are generally described as having **no moving shutter**, with an open head window (L/M: confirm against a photo of the back, for example `UMDDISCK.JPG`). For the animation an open window is the safe choice.
- ECMA-365 §10.4.1 gives the material only as "any suitable" (H). In practice it is PC or ABS-like moulding.

### C.2 UMD disc (ECMA-365 §11 to 12)

| Item | Value | Conf. |
|---|---|---|
| Outer diameter d1 | **60.00 ± 0.30** (Wikipedia's "64 mm" is the shell width, not the disc) | H |
| Substrate centre hole d2 | **≈ 11.0** (11.025 ± 0.025 as extracted) | H/M |
| First transition area d3 | ≤ 11.50 | H |
| Second transition area d4 | ≤ 12.0 | H |
| **Clamping zone** | d4 to **d5 ≥ 16.2** | H |
| Clamping plate (magnetic, ferritic stainless X6Cr17) | cylindrical cup, inner Ø ≥ 11.0, top Ø ≥ 10.5, plate thickness ≈ 0.30, central hole **Ø 1.50 ± 0.10**. The disc is held **magnetically**, with no hub hole visible from outside. | H |
| Disc thickness in data zone (incl. spacer and label) e1 | **≈ 0.84** (+0.09/-0.075 as extracted) | M (text extraction was garbled) |
| Substrate 0 thickness | about 0.38 to 0.48 depending on IOR (Fig. 19) | M |
| Spacer (bond layer) | 36 to 54 µm | H |
| Label thickness | ≤ 40 µm | H |
| Mass | **2.8 to 3.5 g** | H |
| Structure | two substrates bonded by a spacer (DVD-like, 660 nm, NA 0.64) | H |

Colours: the data side is a gold or silver DVD-style metallised layer seen through clear polycarbonate. The label side is printed art (visible through the front window).

### C.3 UMD keep case (retail PSP game case)

| Item | Value | Conf. | Source |
|---|---|---|---|
| Outer size H x W x D | **177 x 104 x 14** | M | WP-UMD ("The case dimensions for UMD discs are 177×104×14mm") |
| (conflicting figure) | 168 x 99 x 14 "PlayStation Portable (clear)" | L | WP-KC table. Possibly inner or print size. Use 177 x 104 x 14 unless you can measure. |
| Colour / material | **clear** (transparent) PP keep case with a clear outer sleeve over a printed wrap insert (WP-KC: "PSP: clear"). A collector forum claims some PAL releases used coloured cases. | M |
| Corner radius | ~4 to 5 | L |
| Insert (wrap) | about 2 x 100 + 14 by about 173 (derived, L; no published template found) | L |

**Structure (M/L, from seller and reviewer descriptions):** a miniature Amaray-style keep case: a single PP shell with two creases and a 14 mm spine, with a clear sleeve outside. On the **right panel** a moulded **UMD-shaped pocket** (the outline in C.1 plus clearance) holds the UMD by friction ribs and a **clip that bears on the top (round end) of the UMD** (reviews note "older cases had a clip on the top of the UMD", which can break). Orientation: label face up, round end toward the top of the case (L). On the **left panel** there are plastic clips or tabs that hold the manual. "Cases have clips inside to hold the game manual and UMD" (seller copy, stoneagegamer, M).

---

## Reference photos (Wikimedia Commons unless noted; visual reference only)

PS1 / CD / jewel case
- Open jewel case with disc (CC0, 6000x4000): https://upload.wikimedia.org/wikipedia/commons/f/f4/Audio_CD_with_Copy_Protection_%28Open_Jewel_Case_%E2%80%93_With_Compact_Disc%29.jpg
- Jewel case front (CC0): https://upload.wikimedia.org/wikipedia/commons/6/6f/CD_Jewel_Case_%28Front%29.jpg
- Jewel case back (CC0): https://upload.wikimedia.org/wikipedia/commons/d/de/CD_Jewel_Case_%28Back%29.jpg
- Jewel case (CC BY 2.5): https://upload.wikimedia.org/wikipedia/commons/e/ef/Jewel_Case_02.jpg
- Jewel case, empty, right angle (CC BY-SA 3.0): https://upload.wikimedia.org/wikipedia/commons/a/a4/Cd_jewel_case%2C_no_art%2C_right_angle.jpg
- Jewel case (CC BY-SA 3.0): https://upload.wikimedia.org/wikipedia/commons/c/c3/CD_Jewel_Case.JPG
- Hub area, photoelastic (CC BY-SA 4.0): https://upload.wikimedia.org/wikipedia/commons/5/52/Photoelasticity_-_Clear_CD_case_centre.jpg
- Original PlayStation disc, black side (CC BY-SA 4.0): https://upload.wikimedia.org/wikipedia/commons/6/6a/Playstation_disc.jpg
- Super jewel case: https://commons.wikimedia.org/wiki/File:SuperJewelCase.jpg
- PS1 game case photos (non-Commons, scholarly site), Chrono Trigger PS1 case and contents: https://scalar.usc.edu/works/chronicles/photos-of-the-chrono-trigger-playstation-cd-case-and-its-contents

DVD keep case
- Standard DVD case (PD): https://upload.wikimedia.org/wikipedia/commons/6/69/Amaray-case.jpg
- Amaray box (CC BY-SA 3.0): https://upload.wikimedia.org/wikipedia/commons/f/fa/Amaray-Box.jpg
- Keep case, Xbox green (PD): https://upload.wikimedia.org/wikipedia/commons/2/2c/Keep-case-xbox.jpg
- Edge finger recess (CC BY-SA 4.0): https://upload.wikimedia.org/wikipedia/commons/8/8d/DVD-Griffmulde1.JPG and https://upload.wikimedia.org/wikipedia/commons/8/83/DVD-Griffmulde2.JPG
- PS2 case inside, labelled components (CC BY-SA 4.0, 4032x3024): https://upload.wikimedia.org/wikipedia/commons/c/c4/PlayStation_2_Game_Case_-_Inside.jpg
- Blu-ray keep case (for comparison): https://commons.wikimedia.org/wiki/File:Blu-ray_keep_case.jpg

UMD
- UMD front (CC BY-SA 3.0, 3648x2736): https://upload.wikimedia.org/wikipedia/commons/2/24/UMD_front.JPG
- UMD plastic shell (PD): https://upload.wikimedia.org/wikipedia/commons/b/b1/Universal_Media_Disc_plastic_shell.jpg
- UMD infobox photo (CC BY-SA 3.0): https://upload.wikimedia.org/wikipedia/commons/d/d9/Universal_Media_Disc%2C_an_optical_disc_medium_developed_by_Sony_for_use_on_the_PlayStation_Portable.jpg
- UMD game disc (CC BY-SA 4.0, 1800x4000): https://upload.wikimedia.org/wikipedia/commons/8/8e/Psp_umd_disk.jpg
- UMD disc (CC BY-SA 4.0): https://upload.wikimedia.org/wikipedia/commons/d/db/UMDDISCK.JPG
- "Cars" UMD (CC0, 2326x2197): https://upload.wikimedia.org/wikipedia/commons/5/58/Cars_UMD.jpg
- Size comparison UMD / MiniDisc / cassette (CC BY 2.0): https://upload.wikimedia.org/wikipedia/commons/7/7d/Universal_Media_Disc%2C_Minidisc_and_Compact_Cassette_-_size_comparison.jpg
- UMD in PSP-2000 (CC BY-SA 3.0): https://upload.wikimedia.org/wikipedia/commons/5/53/PSP2000_UMDPlacement.jpg
- UMD replacement case, clear (CC BY-SA 3.0, small): https://upload.wikimedia.org/wikipedia/commons/1/18/Clearcase.jpg

---

## Gaps and what to verify with calipers or photos

1. **Jewel case internals** (L): lid side-wall height, hinge-pin diameter and axis position, tray outer size and height, hub tooth count and size, finger-notch geometry, booklet tab positions. None of these are published. Patents give topology only, not mm.
2. **PAL PS1 case depth**: the "18 mm" figure from WP-ODP is not well sourced.
3. **DVD keep case corner radii, wall thickness, hub ring diameter, clip positions, edge "windows"**: not published (L). Measure from a physical Amaray if possible.
4. **UMD shell material, colours, front window outline, shutter presence** are not in ECMA (M/L). ECMA does fix the outline, thickness, holes and windows to ±0.1 mm.
5. **UMD keep case**: 177 x 104 x 14 is a single Wikipedia claim and conflicts with the 168 x 99 x 14 keep-case table. Internal pocket and clip geometry are unpublished.
6. ECMA-365 tolerances on several values (d2, e1, D2, L5) came from garbled PDF text extraction. The nominal values are reliable; the +/- splits are approximate.
