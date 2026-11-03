# can2gauge 52 mm housing CT V4.2: Saef SFTO210XC-7478A-CT touch panel

## V4.2 (29 Oct 2026): bezel back, ribbon cavity opened

- **Snap-on full-face bezel is back.** No tape: the lens sits straight on the recess floor (recess now 0.70 deep, so the lens front is flush). The bezel clamps it.
  - The bezel window is Ø68.0, with a 1.6 mm lip over the lens edge and a 0.1 mm bead under the lip, so the lens is held all round.
  - It snaps on with the four hooks at 30 / 150 / 210 / 330°. Those are well clear of the ribbon's fold corners at about 245° and 295°.
  - It keeps the slimmer V4.1 head (Ø76.0), so the overall size is **Ø79.2** (V4.0 was 79.8).
- **Ribbon cavity opened into the bore.** This is the same fix as non-touch V3.9: the two ledges at the +y end of the run either side of the +z drop are gone, so the wide part of the ribbon has room to bow.
- **Unchanged from V4.1:** stack, pockets, flex fold, board fit, cap (supportless) and bracket. The trim ring is dropped; the bezel replaces it.

![ribbon opening, before / after, and the bezel / lens section](can2gauge_CT_V4-2_ribbon_opening.png)

**Assembly:** put the panel in from the front, neck at 6 o'clock, lens down flat in its recess. Then snap the bezel on.

**Print:** body head-down, bezel face-down, cap back-face-down (no supports), bracket flat.

Print `can2gauge_52mm_STL_CT_V4-2.zip` (`pod10.py` is inside: `ENC_RES=0.2 python pod10.py out/`; the bezel was meshed at `ENC_RES=0.15` for the bead).

---

# CT V4.1 notes (superseded: no bezel, taped lens)
## V4.1 (28 Oct 2026): as slim as the panel allows

The overall diameter is down from **Ø79.8 to Ø76.0**, and the face sits 1.5 mm lower.

- **No snap bezel.** The lens is the face: it sits flush in its Ø71.87 recess, held by the tape ring. Use about 0.15 mm double-sided / adhesive-transfer tape, such as 3M 468MP at 0.13 mm.
  - The head shows a 2.0 mm rim round the lens.
  - The lens's own black border does the bezel's job.
  - The bezel's snap pockets in the head are gone.
- **Head Ø76.0**, down from 76.6. Two things set this:
  - the lens recess plus a 1.2 mm rim, which would allow Ø74.3;
  - the two corners of the flex where it folds back behind the display, which are the real limit. The wide part (IC and touch pads) is x −14.5 to +15.3 at the fold apex, which puts the corners at r 36.8.
  - The fold cavity stops 0.35 mm past those corners, with a 0.8 mm wall behind them. That's the thinnest wall in the part, and it's only at those two corners.
  - The fold is already the tightest the datasheet allows: 0.5 mm straight from the TFT edge, then R1.2 to clear the 2.48 LCM.
- **Optional trim ring** (`_trim-ring-optional_`): glue it onto the head face if you want a mechanical lip as well as the tape.
  - It's 0.6 mm thick, with the same OD as the head, so it adds nothing to the diameter.
  - It covers 0.8 mm of the lens edge.
- **Unchanged from V4.0:** stack, pockets, flex, board fit, cap (supportless) and bracket.

![V4.0 vs V4.1](can2gauge_CT_V4-1_slim.png)
![sections](can2gauge_CT_V4-1_sections.png)

**Assembly change:** lay the tape ring on the recess floor (r 30.5 to 35.9; it can't go over the fold cavity at 6 o'clock), then press the lens down flush. That's it: there's no bezel to snap on.

**Print:** body head-down, cap back-face-down (no supports), bracket and trim ring flat.

Print `can2gauge_52mm_STL_CT_V4-1.zip` (`pod9.py` is inside: `ENC_RES=0.2 python pod9.py out/`).

---

# CT V4.0 notes (still valid apart from the bezel and head size)
This is a separate version for the touch panel. It's built from V3.8, so it keeps the opened-out flex cavity, the V3.7 supportless cap and the inside bracket bosses. It's for the rev15 board (V3.4, M2 holes). Keep using V3.8 for the non-touch SF-TC210B panel.

It's built from the SFTO210XC-7478A-CT drawing (rev 01, 2026-03-06):

- **Cover lens:** Ø71.27 ± 0.1 × 0.7 mm.
- **Touch film:** G+F, Ø60.08, with a flat bottom 62.19 mm from its top edge (about 32 mm wide).
- **Depth:** 3.61 ± 0.2 overall, fully laminated. The LCM alone is 2.48 ± 0.2.
- **LCM:** BL O/D 56.18. The neck end is at the same place as before, and the neck is about 31.4 mm wide.
- **FPC:** 26.90 ± 0.5 long, 20.50 wide, with a 4.50 ± 0.3 stiffener.

![sections](can2gauge_CT_V4-0_sections.png)

## What's different from V3.8

- **Lens recess.** The front of the head has a Ø71.87 × 0.85 recess, and the lens front sits flush with the head face.
  - The lens's outer ring (outside the touch film) rests on a **0.15 mm double-sided tape ring** on the recess floor. Use a ring about 5 mm wide between r 30.5 and 35.9.
  - The tape is part of the stack. Without it, the lens sits 0.15 mm low and the bezel won't clamp it.
- **Touch film / LCM pocket.** This is one pocket shaped as the hull of the Ø60.08 film and its bottom flat, with tangent sides. It has 0.2 tolerance plus 0.25 clearance all round, so there's no corner anywhere for the panel to bind on.
  - The neck pocket from V3.8 (0.4 mm a side, R3 blend) is still in there.
  - The pocket floor is 0.2 mm behind the worst-case LCM back, so the lens carries the module and the LCM never bottoms out.
- **Stack.** The panel is 1.35 mm deeper than the old one, so everything behind it moves back about 1.6 mm:

  | | Old | Now |
  |---|---|---|
  | PCB | 6.0–7.6 | 7.6–9.2 |
  | Dash face (head rear) | 9.8 | 11.4 |
  | Cap floor | 16.3–19.3 | 17.9–20.9 |

  - Relative to the PCB, nothing has changed. The glass-side parts get 3.8 mm (3.0 needed), and the bosses, inserts, screws and cap are the same.
  - The head stands 11.4 mm proud of the dash, plus 1.5 for the bezel.
  - The bracket legs are generated for a 3 mm panel, as before.
- **Flex.**
  - The 180° fold is now R1.2, so the run comes back at z 4.33, clear of the thicker LCM.
  - Needed: 24.3 mm of the worst-case 26.4 (2.1 mm of slack).
  - The stiffener is now worst-case 4.8 mm, and it stays straight.
  - The fold / touch-tail cavity starts behind the lens ring and goes back to 1.83 mm short of the dash face.
- **Head size.** The head is Ø76.6, up from 76.4, because of the bigger fold. The lens recess leaves a 2.3 mm wall.
- **Bezel.**
  - The window is now **Ø68.0**: a 1.6 mm lip over the lens edge, so the lens's black border shows.
  - A 0.1 mm bead under the lip, at r 34.3–35.3, clamps the lens into its tape.
  - The snap hooks are unchanged.

## Board check (rev15)

- **Touch lines.** J_LCD already matches this panel's touch pins:

  | Pin | Panel | rev15 net |
  |---|---|---|
  | 35 | TP_INT | TP_INT |
  | 36 | TP_SDA | SDA |
  | 37 | TP_SCL | SCL |
  | 38 | TP_RESET | TP_RST |
  | 39 | TP_VCI | 3V3 |
  | 40 | GND | GND |

  Pins 1–34 match too: LEDA, LEDK × 2, GND, VCI, RESET, SPI, RGB, with DB0 / DB12 tied to GND for RGB565.
- **Backlight.** It's 80 mA at 5–7 V (dual-chip LEDs), which is what the V3.4 review assumed.

## Assembly

1. Press the inserts: 3 × M2 × 3 in the head bosses (flush), and 2 × M3 × 4 into the cap from its back face.
2. Lay the tape ring in the lens recess, then peel it. Put the panel in from the front with the neck at 6 o'clock and press the lens down onto the tape. Snap the bezel on.
3. Plug the flex into J_LCD. Lay the PCB on the bosses with the fold in the cavity under the neck. The touch tail sits in the same cavity.
4. Fit the cap with 3 × M2 × 12, snugged only. Fit the bracket with 2 × M3 × 6.

## Print

- **Body:** head (lens face) down. The pocket and recess ledges need the same supports as V3.x.
- **Bezel:** face down.
- **Cap:** back face down, no supports.
- **Bracket:** flat.
- **Material:** PETG or ASA.

The meshes are watertight and manifold.

## Files

- `can2gauge_52mm_CT_body_V4-0.stl`, `_bezel_`, `_cap_`, `_bracket_`
- `pod8.py`: the model. Run `ENC_RES=0.2 python pod8.py out/`. It needs `geom.py` and `cap4.py`.
- `can2gauge_CT_V4-0_sections.png`
