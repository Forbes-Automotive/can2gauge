# rev15: backlight on 12V for the Saef SF-TC210B-8947A3N

The Saef backlight is 4 parallel dual-chip LEDs: Vf 5.4–6.6 V, IF 80 mA typ, no max given. A buck from 5V can't
drive it, so the AL8860 now runs from 12V.

| Ref | rev14 | rev15 |
|---|---|---|
| U_BL.5 VIN, C27.1, D_BL.1 K, R_SENSE_A.1 | 5V | **12V** (the two 5V flags in the block are 12V flags now) |
| R_SENSE_A | 0R47 TBD | **FRC1206F1R30TS, C2907365**: 1R3 → 77 mA (1R2 C2907364 → 83 mA if you want the typ) |
| R_SENSE_B | 0R47 n.f. | **removed** (one 1206 dissipates 8 mW) |
| L2 | FNR4030S470MT 47 µH | **FNR4030S151MT 150 µH, C167898** (same footprint). 47 µH would switch at ~2 MHz with tON 0.28 µs; 150 µH gives 0.6–0.8 MHz, tON ≥ 0.6 µs at 9–16 V (AL8860: ≤ 1 MHz, tON ≥ 0.5 µs) |
| C27, C8 | CL21A106KAYNNNE 25 V | **GRM21BR61H106KE43L 10 µF 50 V, C440198** (both sit on 12V now) |
| R_KP | 510R TBD | **1206W4F5100T5E, C17991** |

## PCB

- The old 5V chain from the USB/buck ideal diodes (U10/U11) to the inner 5V loads ran through C27.1, U_BL.5 and
  R_SENSE_B. It is now split:
  - The backlight half, D_BL.1 → R_SENSE_A.1 (via) → C27.1 → U_BL.5 → the bottom trace up to C8, is relabelled
    12V. A 1.7 mm bottom stub joins it to the C8.1/U7 12V trace.
  - The left 5V group is re-joined to the inner 5V trunk. The new route runs on the bottom from the U11.2 trace under
    R25, then a via at (−11.5, −8.35), then inner2 on a 45° line to the trunk. It is 0.38 mm wide.
- The inner2 5V path to R_SENSE_B, its vias and the LEDA stub were deleted.
- Checks:
  - Clearance: no new gaps under 0.2 mm (the list is identical to rev14).
  - Nets: every net is track-connected, except $1N10 and GND, which the pours join as before.
  - GND after carving the new copper out of the old fills: still one group.
  - Schematic vs PCB: no new mismatches.
- **Stale pour fills were dropped. Re-pour (Rebuild all copper areas) before DRC or gerbers.**

## Behaviour changes

- On USB power alone the panel stays dark (6.4 V LEDs); bench-test on 12V.
- Keep BL PWM ≤ 500 Hz (firmware already uses 250 Hz).
- Ask Saef for their ST7701S init sequence: the firmware still uses the Waveshare/UGB 2.1" one.
- Power-on: backlight ≥ 200 ms after the panel is on (spec 5.2).
