# can2gauge

**can2gauge** is a 52 mm round digital gauge with a 2.1" 480 × 480 IPS screen. It reads the **car's CAN bus**, the
**K-line**, a **Bluetooth or WiFi ELM327 OBD dongle**, an **OpenHaldex** controller over WiFi, an **Ignitron,
MaxxECU, Link, Haltech or EMU PRO** through **usb2ip**, or a **0-5 V sensor expander**. Everything is set from a
phone over WiFi: no laptop, no app. With the optional **can2gauge Link** Android app it also shows **Google Maps'
next turn**.

Built as the companion display for [OpenHaldex](#openhaldex), it works just as well as a boost / oil / coolant gauge
on any car with a CAN bus or an OBD port. ESP32-S3 (16 MB flash, 8 MB PSRAM), Arduino and **LVGL 9**; firmware, web
UI, PCB and enclosure are all open source.

<p align="center">
  <img src="images/unit-iso.png" alt="The assembled can2gauge: 52 mm gauge body, bezel and the round screen showing the RPM needle page" width="430">
  &nbsp;
  <img src="images/glass-hero.gif" alt="can2gauge on a Mk4 pull: RPM needle, boost arc, Custom page with coolant / oil / battery, then the Haldex lock" width="310">
</p>

![can2gauge web UI: dashboard, gauge pages, Haldex, CAN / DBC, K-line, OBD adapter, display / themes and OTA](images/can2gaugeUI.png)

![can2gauge web UI: OEM oil, DSG, ADC expander, triggers, output, ME7, daisy chain and boot logos](images/can2gaugeUI-2.png)

> [!NOTE]
> **Status (firmware 0.38.2, not yet released; board V3.4 "V1.0").** The board is routed and reviewed and the
> firmware is feature complete for a first release. Some paths still need their first test on a real car: see
> [Status and Bench Items](#status-and-bench-items).

---

## Contents

- [Features at a Glance](#features-at-a-glance)
- [On the Screen](#on-the-screen): page styles, setting up a page, the menu, the drag timer, themes, boot logos, alerts, navigation
- [Where the Data Comes From](#where-the-data-comes-from): CAN, K-line, OBD dongles, OpenHaldex, gearbox, Scan My Car, diagnostics, oil, ADC, ECU over WiFi, daisy chain
- [Data Logger and Service](#data-logger-and-service)
- [OBD Dongle Support](#obd-dongle-support)
- [Customising It](#customising-it)
- [Web Interface](#web-interface)
- [Hardware](#hardware)
- [Updating](#updating)
- [Status and Bench Items](#status-and-bench-items)
- [Credits and Licences](#credits-and-licences)

---

## Features at a Glance

| Feature | Detail |
|---|---|
| Display | 2.1" round 480 × 480 IPS (ST7701S), about 1000 nit. Runs without a screen too: everything but the drawing still works |
| Screen rotation | **Upside Down**, and **Rotation Trim** (±15°) for a gauge that sits crooked in its pod; touch follows |
| Page styles | 12: Needle, Arc, Bar, Graph, Single, Dual, Triple, Quad, Haldex, Gear (DSG), Oil (PQ), Custom; any page can show its **sprite** instead |
| Sprites | **Rev**, the can2gauge mascot: 22 animated clips that follow rpm, boost, Haldex lock and oil temperature |
| Screensaver | Rev over the gauge once the car is still and the screen untouched (off by default) |
| Permanent power | Ignition from CAN terminal 15, the illumination wire or another gauge: shutdown animation, standby, deep sleep |
| Pages | Up to 8, swiped left / right, each with its own source and values |
| Sources per page | CAN (the gauge's own decode or **any DBC signal**), K-line, OBD, ADC, **any ECU channel** over usb2ip, diagnostics, or the gauge itself |
| Gauge feel | **Analog** (needles move like real pointers) or **Digital** (every reading at once); 50 Hz refresh |
| CAN | Classic CAN, 500 kbps, switchable 120 Ω (**Auto**). Profiles: VAG PQ, MQB, **MLB**, OBD-II, **MaxxECU**, listen-only; OBD-II can run **alongside** a profile |
| DBC library | **108 opendbc DBCs** plus EMU Black, MaxxECU, Link and Haltech; car identified from the bus; **three DBCs at once**, which can **feed the gauge's own values** |
| K-line | KWP1281, **KWP2000 VAG**, KWP2000 / OBD-II (with **Fast OBD**), an **ME7 logger** (~34/s, or ~50/s **Fast**) and a read-only **ME7 flash dump** |
| Block labels | Field names, block titles and specs for **501 modules**, picked by part number |
| OBD dongles | ELM327-type over **Bluetooth LE or WiFi**: VIN, every standard PID, the car's **CAN broadcast**, VW **TP2.0**, and **UDS to any module** on 11 or 29-bit CAN |
| Scan My Car | One button finds every value the car can show (OBD PIDs, UDS values from each module, VW measuring blocks over TP2.0 or the K-line, **OBD-II over the K-line on any make**) and lists them as the **Car** source |
| Diagnostics | Up to 32 measuring values from any modules, any make (UDS, or TP2.0 on VW), over CAN **or the dongle**, with a **DID library** of 21 car groups |
| OpenHaldex | Lock / target / mode over CAN **or WiFi**, mode change from the screen, full status in the web UI |
| Gearbox | DSG and **ZF 8HP** (Audi / VW and BMW): selector, gear, lock-up, input speed, temperature |
| OEM oil | Mk4-era G266 oil temperature **and level** from the cluster |
| ADC | Up to 16 × 0-5 V sensors from two [adc2can](#adc-the-adc2can-expander) boards, over CAN or WiFi |
| ECU over WiFi | [usb2ip](#ecu-over-wifi-usb2ip) sends an **Ignitron, MaxxECU, Link, Haltech or EMU PRO**'s data with no wiring; **every channel** can go on a page |
| Data logger | Up to **16 values** at 1-50 a second: **graphs** on the phone, **CSV** for Excel, MegaLogViewer or Datazap |
| Service | **Read / clear fault codes** (K-line, CAN or the dongle; **OBD-II on any make** over the K-line) with **descriptions**, **Scan Modules**, **throttle body adaptation** |
| Drag timer | **0-100 km/h** (or 0-60 mph) from a 5 s countdown: the flag drops, the ring fills, the chequered flag lifts; a **scoreboard** of the best 10 |
| Daisy chain | Gauges share values over CAN or WiFi; one polls the car, the rest follow; set up and update every gauge from any one |
| Navigation | **Google Maps turn-by-turn** from the **can2gauge Link** Android app over Bluetooth LE |
| Triggers | 8 set-points raising an alert and / or switching the output, on any value the gauge has |
| Output | One 12 V output, **high-side or low-side**, protected |
| Night dimming | The dash-light wire switches to the night brightness |
| Themes | **71**: 65 manufacturer cluster colours across 31 makes, house palettes, Custom, and **Match car** from the VIN |
| Your car | The **VIN** from the dongle, the CAN or the K-line; make, model and year confirmed, with a **picture of the car** in its paint |
| Boot logos | can2gauge, Forbes, OpenHaldex, your text, **33 make logos**, **My Car**, or any image |
| First Boot | A new gauge checks every source and suggests the whole setup, all changeable before applying |
| Web UI | 18 tabs on the gauge's own WiFi, shown to suit the car, **Basic / Advanced** |
| Updates | Over the air from GitHub or from files, with a 60 s rollback guard |

---

## On the Screen

### Page Styles

Every page is one of twelve styles. Any value can go on any page, and each page has its own range, redline, decimals
and label.

![The twelve page styles: Needle, Arc, Bar, Graph, Single, Dual, Triple, Quad, Haldex, Gear (DSG), Oil (PQ) and Custom](images/glass-pages.png)

| Style | What it shows |
|---|---|
| **Needle** | A classic dial with an anti-aliased needle |
| **Arc** | A sweep with a tick scale and a big number |
| **Bar** | A horizontal bar with the value above |
| **Graph** | The last 30 seconds as a rolling line, Min and Max beside it, the redline across it. It keeps recording while another page is showing |
| **Single** | One value, large, with its icon |
| **Dual** | Two values, each with a bar and an icon |
| **Triple** | One value large on top, two side by side below |
| **Quad** | Four values in a 2 × 2 grid |
| **Haldex** | Target and actual lock arcs, the mode, and an optional second value |
| **Gear (DSG)** | P R N D S M strip, the gear huge (D3, S4, M2 ...) and the speed |
| **Oil (PQ)** | The OEM oil-temperature arc with **LEVEL OK / LOW** |
| **Custom** | An arc with three icon readouts stacked in the centre (see [below](#custom-and-quad-pages)) |
| *Sprite* | Any page can show a sprite pack (Rev) in a ring that shows its reading; tap it for a reaction |

A page waiting for data says why under the value (*"Waking module 0x01"*, *"No OBD adapter paired"*, *"0x280 not on
the bus"*), with a spinner while it connects. A value that stops arriving shows `--`. Sources run in the background,
so swiping to a page shows a live value straight away.

### The Pages a New Gauge Starts With

| # | Label | Style | Shows | On |
|---|---|---|---|---|
| 1 | HALDEX | Haldex | OpenHaldex lock | Yes |
| 2 | BOOST | Arc | Boost | Yes |
| 3 | RPM | Needle | Engine speed | Yes |
| 4 | COOLANT | Arc | Coolant temperature | Yes |
| 5 | ENGINE | Quad | Coolant, intake, oil temperature, boost | Yes |
| 6 | SPEED | Bar | Road speed | No |
| 7 | BATTERY | Single | Battery voltage | No |
| 8 | OIL TEMP | Arc | Oil temperature | No |

[First Boot Setup](#first-boot-setup) replaces these with gauges for what it finds on the car.

### Setting Up a Page

Pages are set on the web UI's **Gauges** tab (join the gauge's WiFi, open `http://192.168.1.1` or `http://c2g.local`).
The screen rebuilds as you go and every change saves by itself.

1. **Open the gauge**: tap its line. The switch puts it in or out of the swipe.
2. **Style**: one of the twelve above.
3. **Source**: where the values come from.
   - **Car**: everything [Scan My Car](#scan-my-car) found, grouped by module, with a Find box. Only values that
     answered are listed. In Basic this is the main source (with ADC and ECU when one is heard, and Specifics); the
     others below show with **Advanced** on.
   - **CAN**: the gauge's own decode, or any signal of a loaded DBC.
   - **K-line**: a module, block and field (or an ME7 logger variable).
   - **OBD**: any mode 01 PID, through the dongle, the CAN or the K-line.
   - **ADC (adc2can)**: an adc2can sensor.
   - **ECU (usb2ip)**: any channel of the ECU, with a Find box.
   - **Diagnostics (UDS / TP2.0)**: one of the [diagnostics values](#diagnostics-uds--tp20).
   - **Specifics**: the gauge's own values and the OEM oil temperature and level.
4. **Label**: up to 15 characters.
5. **The value**: picking one fills in a sensible range, redline and decimals.
6. **More Options**: Min, Max, Redline, Decimals and Sprite.
7. **Move Up** / **Move Down** set the swipe order.

### Custom and Quad Pages

Both show four values: the page's value plus three slots. **Triple** uses the first two slots.

**Custom** is the arc page with three rows in the middle (**Top**, **Middle**, **Bottom**), each an icon, a value and a
unit. An "ENGINE" page with coolant on the arc and oil temperature, intake and battery in the middle:

| Setting | Value |
|---|---|
| Style | Custom |
| Source | CAN |
| Label | ENGINE |
| Value (the arc) | Coolant, Min 0, Max 140, Redline 110 |
| Top / Middle / Bottom | Oil temperature / Intake air / Battery |

**Quad** puts the four values in a 2 × 2 grid with their names and units. Quad and Triple pages are titled by their
source (CAN, OBD, ADC, or the ECU's name), which you can change.

<p align="center">
  <img src="images/glass-page-custom.png" alt="A Custom page: the boost arc with coolant, oil temperature and battery stacked in the centre" width="300">
  &nbsp;&nbsp;
  <img src="images/glass-page-quad.png" alt="A Quad page: coolant, intake, oil temperature and boost in a 2 by 2 grid" width="300">
</p>

### Value Icons

Single, Dual, Triple, Quad and Custom pages show an icon with each value, in the theme's accent colour.

![The value icons](images/value-icons.png)

The gauge's own values have fixed icons (coolant, oil, battery, boost, intake, outside air, fuel, speed, rpm, lambda,
gearbox, Haldex, throttle, gear, temperature, pressure, voltage, ignition, injector, brake, engine warning light). Anything else (a DBC
signal, a PID, a K-line field, an ECU channel, an adc2can sensor) gets its icon from its **name and unit**: an
adc2can sensor called "Oil temp" in °C gets the oil can, "Distance MIL on" the engine warning light. A value that
matches nothing has no icon. The menu and settings icons are drawings too, with no text in them.

### Decimals

Every value shows the decimals it has, on every page and in the web UI's live values: lambda reads **1.00**, not 1.
That comes from an adc2can sensor's or ECU channel's own setting, a DBC signal's or diagnostics value's scaling
(0.01 = two places), otherwise its unit (two for lambda and bar, one for volts, none for rpm, temperatures and speed).
A value shown in another unit takes that unit's (23.2 psi, 160 kPa, 14.7 AFR). The page's **Decimals** can add places
to its main value, never take them away.

### The Menu

Hold the screen for 2 s (or swipe up / down) for the menu: one item per screen with an icon and a **live status**.
Swipe for the next, tap to open, hold or wait 10 s to go back. Settings > Device > Menu and Tabs hides any item (and
its web UI tab).

<p align="center">
  <img src="images/glass-menu.gif" alt="The mode menu: Gauges, Haldex, CAN bus, K-line, Output / Illumination, Gearbox, ADC, Bluetooth OBD, Daisy chain, Drag Timer, Settings" width="300">
</p>

### Mode Screens

Behind each menu item is a live screen. Opening one claims its source (the K-line, Bluetooth), released 3 s after
you leave.

![The mode screens: menu, Haldex, CAN bus, K-line, Bluetooth OBD, Gearbox, ADC, Output / Illumination, Daisy chain, Settings](images/glass-modes.png)

| Screen | What it does |
|---|---|
| **Haldex** | The Haldex gauge; **swipe to change the OpenHaldex mode** |
| **CAN bus** | Every ID on the bus, decoded with the car's DBC, or the raw bytes with changes lit |
| **K-line** | Wakes the module picked in the web UI, then one measuring block at a time; swipe for the next |
| **Bluetooth OBD** | Connects to the paired adapter, then pages of live PIDs |
| **Gearbox** | Selector and gear full screen, speed, temperature and lock-up |
| **ADC** | The adc2can sensors, four to a page |
| **Output / Illumination** | The output, its control, any active trigger and the dash-light state |
| **Daisy chain** | Every gauge on the link, the master and this gauge's number |
| **Drag Timer** | 0-100 km/h (0-60 mph) with a countdown and a scoreboard, see below |
| **Settings** | Brightness, CAN termination, units, theme, reboot |

### Drag Timer

**Menu > Drag Timer** times **0-100 km/h**, or **0-60 mph** when Speed is set to mph.

- **Tap** to start, with the car standing still. A green flag goes up and the screen counts down from 5.
- The **flag drops** at the end of the countdown and the clock runs. If the car moves first, the flag drops and the
  clock starts the moment it does.
- The **ring** round the edge fills with the speed. At 100 (or 60) the **chequered flag** lifts and the time shows,
  with its place on the board ("New Best!").
- **Tap** while it counts down or runs to cancel; nothing is saved. **Hold** for the menu.
- **Double-tap** for the **scoreboard**: the best 10 times, with **Clear Scores** (tap it twice). The Log tab in the
  web UI shows them too, with **Delete** for one time and **Clear All**.

Screens without touch: **Dashboard > Gauge Control** (Advanced) works the screen the way a finger would. **Previous
Gauge** / **Next Gauge** swipe (pages, or the menu's items), and **Single Tap**, **Double Tap** and **Long Press** press
the middle. So Long Press, Next Gauge to Drag Timer, Single Tap goes in, and the same buttons run it.

The finish is worked out between the two speed readings either side of the target, so it doesn't depend on the
screen's frame rate. How exact it is depends on how often the speed comes: many times a second over CAN, a few a
second over OBD.

![The drag timer: ready, the countdown under the green flag, running with the ring part full, finished under the chequered flag, and the scoreboard](images/glass-drag.png)

<p align="center">
  <img src="images/ui-drag.png" alt="Log: the Drag Timer card, the best 0-100 km/h times with Delete, and Clear All" width="260">
</p>

### Themes

71 themes: 65 manufacturer cluster colours across 31 makes, one per era (VW green → blue → white, Audi red → white, BMW
amber ...), four house palettes, **Custom** and **Match car**, which picks the make and era from the VIN. Pick one in
the web UI (with a live preview) or on the screen.

![The RPM needle page in twelve themes: Forbes teal, VW blue, VW green, Audi red, BMW amber, Skoda green, Mercedes, Porsche, Mazda red, Subaru red, Nissan amber, Saab green](images/glass-themes.png)

<p align="center">
  <img src="images/glass-themes.gif" alt="The needle page cycling through manufacturer themes" width="300">
</p>

### Screen Rotation

Settings > Display > **Screen Rotation**: **Upside Down** for a gauge fitted the other way up, and **Rotation Trim**
(up to 15° either way in half-degree steps) for one that sits crooked. Taps and swipes are turned to match. It costs a
little frame rate and needs Direct drawing mode.

### Boot Logos

The gauge boots to the **can2gauge** logo (its dial sweeps like a cluster at ignition on), Forbes Automotive,
OpenHaldex, your own text, one of **33 make logos**, **My Car** (a picture of [your car](#your-car) in its paint), or
**any image**: the web UI converts a PNG / JPG / SVG in the browser and uploads it. **Show It Now** plays it without a
reboot. In a [daisy chain](#daisy-chain) all the gauges light up and sweep together.

<p align="center">
  <img src="images/glass-boot-can2gauge.png" alt="The can2gauge boot logo: a dial reading OBD / CAN / K-LINE / ADC over the wordmark" width="300">
  &nbsp;&nbsp;
  <img src="images/glass-bootlogos.gif" alt="Boot logos cycling: can2gauge, Forbes, VW, Audi, Seat, Skoda, Porsche, BMW, Mercedes, Ford and more" width="300">
</p>

<details>
<summary>All the logos on the gauge</summary>

![Every boot logo: can2gauge, Forbes and 33 makes](images/glass-bootlogos.png)
</details>

### Alerts

A [trigger](#customising-it)'s alert comes up over any page: a red ring, a red wash and a pulsing warning card with
the trigger's label. It clears when the value comes back; a latched one waits for a tap. The Dashboard has **Clear
Alerts** and **Test Alert**.

<p align="center">
  <img src="images/glass-alert.gif" alt="A coolant alert pulsing red over the coolant arc" width="300">
</p>

### Turn-by-Turn Navigation

Google Maps' next turn, laid out like Android Auto: the arrow and distance on an accent band, the road, the arrival
time, and a ring round the edge filling as the journey goes.

- **The app**: **can2gauge Link** for Android reads the navigation notification Maps already shows and sends it over
  **Bluetooth LE**. Maps isn't changed and Android Auto isn't needed. Download it from
  **[can2gauge.com/app](https://can2gauge.com/app)** (Android 8.0+), and keep the app and the gauge on the same version.
- **When it shows**: **pops up near a turn** (300 m by default) or for the **whole route**; a tap hides it until the
  next turn.
- **Setup**: web UI > **Navigation** > switch **Nav Link** on; in the app allow notification access and Bluetooth,
  and set battery use to Unrestricted. It reconnects by itself at every ignition on. **Show Test Turn** tries the
  card with no phone.
- In a daisy chain, switch Nav Link on in one gauge only. It runs alongside a Bluetooth OBD adapter: switched on while
  the adapter is in use, it starts after a restart (**Restart Now** on the Navigation tab).
- Android only: iOS doesn't let apps read other apps' notifications.

<p align="center">
  <img src="images/glass-nav.png" alt="The nav card: a right-turn arrow and 250 m on the accent band, Turn right onto Test Street, the arrival time" width="300">
</p>

### Sprites, the Screensaver and Rev

Turn **Sprite** on for a page and it shows an animated character instead of the dial. **Rev**, the can2gauge mascot,
is a white figure whose quiff is a gauge needle and whose glow takes the theme's colour. The ring and the number stay
the gauge's own.

![Rev in each mood: idle, running, sprinting, REDLINE, BOOST, Haldex LOCK, oil hot, oil cold, the screensaver and goodbye at ignition off](images/glass-rev.png)

| Mood | When it plays |
|---|---|
| Idle / Running / Sprinting | An rpm page: under 1300, 1300-4800, over 4800 rpm |
| **REDLINE** | Over 6500 rpm: Rev bounces off the limiter |
| **BOOST** | Over 1.4 bar: Rev inflates |
| **PSSH** | Boost falling fast: the blow-off, on any page |
| **LOCK** | A Haldex page: Rev braces, LOCK slams in at 92 % |
| Oil hot / cold | Over 125 °C or under 20 °C, on any page |
| Screensaver | The car still and the screen untouched |
| Goodbye | Ignition off on permanent power: Rev waves, yawns and lies down |

<p align="center">
  <img src="images/glass-rev.gif" alt="Rev on an rpm page during a pull: running with the revs, then a PSSH blow-off as the boost drops" width="300">
</p>

The **screensaver** (Settings > Logo & Sprites) brings Rev up once the car is still; **double-tap** it for **Rev
Runner**, a little jump-the-cones game.

### Ignition on Permanent Power

With the gauge on a permanent feed, the ignition comes from **CAN terminal 15** (VAG PQ, MQB, any bit you enter, or
just bus traffic) or the **illumination wire on an ignition feed**. Ignition off plays the goodbye clip, fades the
screen and goes to a quiet standby with the output off; after 10 minutes it deep-sleeps until the ignition returns.
V3.4 still draws a few mA asleep, so check before leaving one on for weeks.

In a [daisy chain](#daisy-chain) only one gauge needs the ignition source: the others follow it by themselves.

### Night Dimming

Wire the dash-light feed (VAG 58d, PWM or dimmed 12 V, or 58) to J_OUT pin 3. With the lights on, the screen drops to
the **night brightness**. In a daisy chain one gauge's wire dims them all.

---

## Where the Data Comes From

Everything the gauge can show, trigger on or log is a **signal** in one catalogue, and every source writes into it.

```
 car CAN ──── VAG PQ / MQB / MLB decode, OBD-II, OpenHaldex, any DBC ─────────┐
 K-line ───── KWP1281 blocks, KWP2000, ME7 logger, cluster oil ───────────────┤
 OBD dongle ─ BLE / WiFi ELM327: PIDs, VIN, UDS / TP2.0 ──────────────────────┤
 OpenHaldex ─ ESP-NOW broadcast (no CAN wire needed) ─────────────────────────┼──▶ signals ──▶ pages, triggers,
 adc2can ──── 16 × 0-5 V over CAN or WiFi ────────────────────────────────────┤               the logger, the web UI,
 usb2ip ───── an aftermarket ECU over WiFi: every channel ────────────────────┤               the daisy chain
 other gauges  daisy chain over CAN or WiFi ──────────────────────────────────┤
 this gauge ─ dash lights, output, board temperature ─────────────────────────┘
```

### CAN

- **Classic CAN**, 500 kbps by default.
- **Profiles** (all listen-only unless noted):
  - **VAG PQ**: rpm, pedal, coolant, outside temp, speed, fuel, gear and selector, oil temperature, oil-pressure warning.
  - **VAG MQB**: rpm, speed, pedal, intake, coolant, fuel, outside temp, selector and gear, gearbox temperature,
    shifting, lock-up and input speed.
  - **VAG MLB** (Audi A6 / A7 C7, A8 D4, Q5 8R with the ZF 8HP): much the same as MQB, on the drivetrain CAN.
  - **OBD-II** (asks): up to six PIDs per request, the next request the moment the last answer is in. See
    [OBD-II Polling](#obd-ii-polling).
  - **MaxxECU**: its "Default CAN output" from a configurable base ID.
- **OpenHaldex** (0x6B0) is always decoded.
- **Termination**: **Auto** (default) switches the 120 Ω on only when nothing else terminates the bus.
- A quiet bus gets a gentle "knock" once a second to wake a sleepy gateway, and the gauge recovers from bus-off by
  itself.

**Any DBC.** All 108 opendbc DBCs are on the gauge (the VW / Audi ones translated to English). The web UI **ranks them
against the IDs on the bus** to identify the car, or converts your own `.dbc` in the browser. Any signal of a loaded DBC
can drive a page.

**DBCs that feed the gauge.** A DBC can map its signals onto the gauge's own values (rpm, coolant, gear ...), so
loading it is all the setup. Included for **EMU Black, MaxxECU, Link and Haltech**, plus overlays for opendbc's **VW
MQB and Audi MLB**. **Up to three DBCs at once** (car, gearbox, extra), with a moved base ID per slot.

### K-line

An L9637D on J_OUT pin 2 (OBD pin 7).

- **KWP1281**: the VAG measuring blocks with the full formula table. Text blocks show as text.
- **KWP2000 VAG**: the same blocks, faster, falling back to KWP1281 by itself.
- **K-line mode**: browse **any module**, one block at a time, on the screen or in the web UI. **Find...** jumps to a
  value by name; **Show Formula and Spec** adds the formula and the spec under each one.
- **Block labels**: field names and specs from the module's label file, picked by its part number (501 on the gauge).
- **K-line pages**: one module stays connected for every page that uses it.
- **KWP2000 / OBD-II**: fast init, falling back to the 5-baud wake-up for ISO 9141-2 ECUs; about 7-8 answers a second,
  roughly twice that with **Fast OBD**. Any make: it also reads the **VIN** (mode 09) when the ECU has it, and
  [Service](#service) and [Scan My Car](#scan-my-car) use it for fault codes and values.
- **ME7 logger**: fast logging for Bosch ME7. Upload the `.ecu` from ME7Info and tick the variables to log. **Normal**
  reads about 34 samples a second; **Fast** runs a tiny routine in the ECU's RAM (never the flash) for about 50.
  **Test Link** proves Fast on a new ECU first. Logs download as CSV.
- **ME7 flash read**: a read-only dump of the 1 MB flash to a `.bin`. Nothing is ever written. A modified flash needs
  the ECU's **Permit** value. Engine off, ignition on, a charger on the battery.
- **Fault codes and basic settings**: see [Service](#service).

### OBD Dongles

A Bluetooth LE or WiFi **ELM327-type** adapter gives the gauge the car's data with no wiring. See
[OBD Dongle Support](#obd-dongle-support).

- **Auto-connect** at power-up, so OBD pages have values as soon as the logo clears. A new car is tried on ISO 15765
  (CAN) first, the adapter's slower search through every protocol only if that gets no answer. An adapter that isn't there
  stops being looked for after 5 tries (about a minute on Bluetooth), until **Scan**, a settings change, the OBD
  screen being opened or the next ignition on.
- **Adapter Source**: OBD PIDs (the usual), **VAG K-line measuring blocks** (set on the K-line tab, **K-line Through
  the OBD Adapter**), or **CAN broadcast** (the adapter just listens and the gauge decodes the frames).
- **11-bit and 29-bit CAN**: on a 29-bit car (e.g. a 2025 Tiguan) the gauge finds the engine's address by itself.
- **VIN**: make, model and the **Match car** theme.
- **Standard PIDs**, up to six per request on CAN. **Fast OBD** (on by default) makes the adapter answer sooner, and
  drops back to standard timing by itself if answers go missing. On a K-line car a
  clone adapter manages about 5 requests a second (the protocol's limit); the gauge's own K-line does 8-15.
- **Read / Clear DTCs**, and **UDS to any module** for [Diagnostics](#diagnostics-uds--tp20) and
  [Service](#service): the adapter switches between 11 and 29-bit for each module, with OBD values carrying on in between.
- **Raw terminal**, **TP2.0 through the adapter** and **bench tests** for any new adapter.

### OBD-II Polling

The wired CAN, the gauge's K-line and the dongle share one PID picker:

- **Every standard PID up to 0x60** (80 values) can go on a page, by any route. Only the PIDs the ECU has are asked for.
- **Fast** PIDs (rpm, boost, speed, throttle, timing, lambda, and the page on screen) are asked every time; the rest
  take turns.
- **Throttle follows the pedal** when the engine reports a pedal PID: on a diesel the throttle valve PID is the intake
  flap, which sits mostly open whatever your foot does. Otherwise it's the throttle valve, as before.
- **OBD-II over CAN** (CAN tab) adds PIDs next to a VAG or MaxxECU profile, for values the car's frames don't carry.
- **Which ECU**: the CAN tab lists every ECU that answered; the engine by default.

### OpenHaldex

can2gauge started life as the OpenHaldex companion display.

- **Over CAN**: lock target / actual, mode, speed and pedal; the mode buttons send 0x6A0.
- **Over WiFi**: OpenHaldex V9 broadcasts its full state ten times a second, with **no pairing and no CAN wire**,
  including generation, force modes, learn progress and clutch / oil temperature.
- **Control**: change mode by swiping the Haldex screen or from the web UI. The Haldex tab can also set the
  generation and run Learn, signed with the OpenHaldex's WiFi password, and only with the car stopped.

### Gearbox (DSG and ZF 8HP)

- **Selector and gear** from the CAN broadcast (PQ, MQB, MLB), with no requests sent. MQB and MLB add shifting,
  lock-up and input speed, and the gauge works out converter slip.
- **Gearbox type**: DSG, **ZF 8HP Audi / VW** or **ZF 8HP BMW**; picking one fills in the diagnostic reads for it
  (BMW over extended addressing, on the CAN or through the adapter).
- **Diagnostic reads** (optional): UDS or TP2.0, on the CAN or through the dongle, for speed, gear, oil temperature,
  lock-up and selector.

### Scan My Car

The **Scan My Car** tab (also in First Boot) asks the car what it can show, so nobody needs to know about
PIDs, DIDs or measuring blocks. Ignition on; it takes from a few seconds to a few minutes:

1. **The way in**: OBD on the gauge's own CAN wire (11 or 29-bit), else the OBD dongle (a Bluetooth adapter it finds
   is paired there and then, and the scan waits until it has found the car), else one quick wake-up of the engine on
   the K-line (skipped when First Boot or the K-line already found nothing). If nothing answers it stops and says **Connect an ECU** (or, when the car's CAN is busy but answers no
   questions, that its values are already under Live Now) rather than trying everything.
2. **The car**: its make from the VIN picks the right part of the DID library. Until the make is known the scan is
   plain ISO (OBD), never a guess.
3. **OBD PIDs from every ECU** that answers: the engine's, and the gearbox's (or any other's) own, read from that ECU.
4. **Modules** that answer UDS (engine, gearbox, AWD, brakes ...; BMW's by name, DME, EGS, KOMBI ...), then each
   module's **DIDs** from the library: only the ones that answer with enough data are kept. BMW modules through a dongle
   need one that takes extended addressing (many cheap clones don't). A VW engine that answers like a **Simos 18**
   (EA888 gen 3 on MQB: Golf R / GTI, S3, TT ...) gets the Simos 18 list instead: 46 values (boost, AFR, knock per
   cylinder, turbo speed, fuel pressures, the torque model ...) with names and scaling proven on these engines. Putting
   one on a page turns **Turbo** on in groups of 8, so they're read many times a second.
5. **Older VWs**: the modules that open a **TP2.0** channel (PQ era), or wake on the **K-line** (Mk4 and older), and
   their measuring blocks, named from the label files.
6. **Older cars of any make**: no engine on the CAN or the dongle and no VW on the K-line, so it asks for **OBD-II
   over the K-line** (ISO 9141-2 or KWP2000): the engine's PID lists and the VIN. The K-line tab is switched to its
   OBD-II mode for those values (unless it's set to a VW protocol).

The gauge's screen shows the scan as it goes (**SCANNING**, then **SCANNED** and the number of values; a red **NO WAY
IN** when nothing answered), then fades. The result stays on the gauge and becomes the **Car** source on every page. A value from a module takes one of the 32
diagnostics slots by itself.

<p align="center">
  <img src="images/ui-carscan.png" alt="The Scan My Car tab after a scan of a Mk4: OBD and seven K-line modules, 299 values, and the scan's log" width="260">
  &nbsp;
  <img src="images/ui-gauges-car.png" alt="Gauges tab: gauge 1 on Source = Car showing oil temperature" width="260">
</p>

### Enable Other Scanners

**Settings > Device** and the **Diagnostics** tab have one switch, **Enable Other Scanners**. While it's on, the gauge
asks the car nothing: no diagnostics values, K-line, gearbox reads, OBD on the CAN or scans. That leaves the car free for
another scan tool. Gauges that need those values show no data meanwhile. It's off again after a restart.

### Diagnostics (UDS / TP2.0)

The **Diagnostics** tab (Advanced) reads up to **32 measuring values**, any make, each from its own module: **UDS** on
modern modules (VW MQB era, BMW and the other groups below), **KWP2000 blocks over TP2.0** on VW PQ-era ones. Only the
values a gauge page or trigger uses are asked for (all of them while the tab is open).

- **Connection**: the gauge's CAN, or the **OBD dongle** (UDS only).
- **Any module, any ID**: a VW address from the list, or any request / answer ID, 11 or 29-bit, with BMW extended
  addressing.
- **Any run of bits** from the answer, signed or not, either byte order, then scale and offset. Or an **OBD PID** asked
  of that module (the gearbox's own PIDs, which the engine's OBD polling never asks).
- **Mix anything**: pages can show K-line blocks, TP2.0 blocks, UDS DIDs and OBD PIDs side by side. Everything that
  asks questions on the CAN (the OBD polling, these values, the gearbox reads) takes turns, one question and its
  answer at a time, so two never talk to the same ECU at once; the K-line is its own wire. A TP2.0 value from the
  module the gearbox reads over TP2.0 shares the gearbox's connection (a module only takes one) and uses its answers
  when it's the same block.
- **Turbo** asks back to back and packs several values into one request (up to 8; a Simos 18 takes all 8), and
  the module sends a long answer without gaps. If that's too fast it steps back by itself: gaps between the frames
  first, then smaller groups, growing again once the answers come in cleanly.
- **Stops when there's nothing to ask**: no requests while the dongle isn't connected or the car's OBD isn't CAN, and
  after 3 tries with no answer (one if nothing else is heard on the CAN) until **Restart Session**, a settings change,
  the next ignition on or the car showing up on the CAN.
- Shows the module's part number, name and dataset, each value with its raw bytes, and a **Try a DID** box. Any value
  can go on a gauge page (Source = Diagnostics).
- **DID Library**: the values [OBDb](https://github.com/OBDb) lists for 21 car groups (the VW group alone has 3,555,
  most confirmed on a real car; also Stellantis, Renault, Mazda, Hyundai / Kia, Toyota, Volvo, Nissan, GM, Ford,
  Honda, BMW, Mercedes, Subaru and more). Pick the group (your car's comes first), the module and a few words; **Add**
  fills in the next free slot with that value's own module, so values from several modules run together. It then
  shows on the Gauges tab under Source = Diagnostics. A search that finds
  nothing on the module picked looks in every module, and says how many matches **Confirmed Only** hides (the values
  OBDb is still trying out).

### OEM Oil Temperature and Level

Mk4-era VAG cars with the **G266** sump sender have oil temperature and level that only the cluster decodes. The
**Oil (PQ)** page reads it from **CAN** (temperature only) or over the **K-line** from the cluster (temperature and
level, only while the page is on screen).

### ADC: the adc2can Expander

The board has no analog inputs of its own. Up to **two adc2can boards** (8 × 0-5 V each, 16 sensors) add them over
the CAN chain, or over WiFi with just power. A board's sensors appear in the web UI's lists once it's been heard, as
"Board 1 - Sensor 1 (V)" (or with the sensor's name).

- **Settings live on the adc2can**: each sensor's scaling comes across by itself, and the gauge's ADC tab edits it in
  place (over CAN).
- **Dial range**: each sensor has a **Dial Min / Max** on the adc2can, Auto by default (plain volts 0.00 to 5.50 V, a
  pressure sender over its span, an NTC -20 to 150). A page showing the sensor always uses that range, so the page and
  the adc2can's own dial agree.
- A sensor can also be scaled on the gauge only (presets for pressure senders, 3 bar MAP, wideband and EGT).
- Over WiFi it's one-way: changes need CAN or the adc2can's own web UI.

### ECU Over WiFi (usb2ip)

**usb2ip** is Forbes Automotive's wireless USB bridge for an **Ignitron, MaxxECU, Link (G4X / G5), Haltech (Elite /
Nexus)** or **ECUMaster EMU PRO**. It also broadcasts the ECU's data over WiFi, so a gauge with just power shows the
engine with no wiring.

- **Everyday values** (rpm, boost, temperatures, lambda, pressures, gear ...) fill the gauge's own signals when nothing
  else provides them, so every page and trigger just works.
- **Every channel** (380 Ignitron, 596 MaxxECU, 831 Link, 936 Haltech, 1,409 EMU PRO) can go on a page: Source = ECU,
  then pick from a grouped list with a Find box.
- **The usb2ip tab** shows the ECU found, the signal and which values arrive.

> [!WARNING]
> Link, Haltech and EMU PRO support is new and untested on a car; EMU PRO channel names are still unconfirmed.

### Daisy Chain

Several gauges share one set of sources. The two bus plugs are wired straight through, so the gauges chain on one
4-wire harness.

- On from new, over WiFi, so a new gauge finds the others by itself. One **master** polls the car; the rest follow.
- **Gauge Numbers**: from any gauge, give every gauge its place in the row and its role. **Show Numbers** puts each
  number on its screen.
- **Row wave**: at ignition off, numbered gauges wave goodbye along the row.
- Values go over **CAN** (once allowed) or **WiFi**. **Follow page** and **follow night** keep them in step.
- **Shared settings**: ignition, shutdown, screensaver, theme, brightness, units and boot logo match on every gauge;
  change them on any one.
- **Update Other Gauges**: update one gauge and it passes the update on (see [Updating](#updating)).

<p align="center">
  <img src="images/glass-number.png" alt="Show Numbers: the gauge's number in the row, 2, big on its screen with its name and role" width="300">
  &nbsp;
  <img src="images/ui-chain.png" alt="Daisy Chain: Gauge Numbers, each gauge's number and role set from one gauge" width="260">
</p>

---

## Data Logger and Service

### Data Logger

The **Log** tab records up to **16 values**, **1 to 50 times a second**, from any source.

- **Active Gauges** (default) logs whatever your pages show; **Chosen Below** logs the 16 you pick.
- **Start / Stop**, or **Auto Start** at power-up.
- Up to 4 MB in memory: about 1 h 40 min of 16 values at 10 a second; then the oldest are overwritten. A power cut
  loses it, so download first.
- **Graphs** on one time line, live while recording, with zoom, a cursor and min / average / max.
- **Download CSV** for Excel, MegaLogViewer or Datazap.

### Service

The **Service** tab talks to the car's modules from the gauge's own wiring or the dongle.

- **Read / Clear Faults** for any module: K-line (KWP1281, KWP2000), CAN (UDS, TP2.0) or the dongle (UDS).
- **OBD-II (any make)** on the K-line: the stored and pending codes of every emissions ECU (modes 03 / 07), and clear
  (mode 04), the way a generic scan tool does it.
- **Descriptions** for the standard codes (about 9,500: P0, P2, P3, B0, C0, U0, U3). A maker's own codes (P1 ... and
  VW fault numbers) show as the code.
- **Scan Modules**: every module's part number, name and fault count.
- **Throttle body adaptation**: basic setting **060** or **098** (or any group) over the K-line, with the fields shown
  live. It won't start with the engine running.

> [!WARNING]
> Ignition on, engine off, foot off the pedal for an adaptation. Clearing an engine's faults also resets its emissions
> readiness monitors.

<p align="center">
  <img src="images/ui-log.png" alt="Log: recording controls and the graphs of a pull, RPM, speed and boost on one time line" width="260">
  &nbsp;
  <img src="images/ui-log-channels.png" alt="Log: the 16 channel pickers, samples a second and Auto Start" width="260">
  &nbsp;
  <img src="images/ui-service.png" alt="Service: OBD-II fault codes read over the K-line, stored and pending, with their descriptions, and the throttle body adaptation card" width="260">
</p>

---

## OBD Dongle Support

The ESP32-S3 has **Bluetooth LE only**, so a Bluetooth dongle must be a BLE ("iPhone compatible") adapter; SPP-only
"Android" adapters and the OBDLink MX+ / LX won't work. **WiFi adapters** work too, with the gauge's own WiFi staying
up.

| Adapter | Link | Notes |
|---|---|---|
| **Vgate iCar Pro BLE 4.0** | BLE | The reference; raw CAN, TP2.0 and K-line paths built for it |
| **OBDLink CX** | BLE | STN chip, recommended |
| **Veepeak OBDCheck BLE / BLE+** | BLE | |
| **vLinker MC+ / BM+ / FD** | BLE | "FD" means Ford MS-CAN, not CAN FD |
| Generic BLE ELM327 clones | BLE | Slow but fine for two or three gauges; can't wake VAG modules |
| Any **WiFi ELM327** | WiFi | Usually 192.168.0.10:35000 |

| Protocol | Through a dongle | On the gauge's own wiring |
|---|---|---|
| OBD-II PIDs, VIN | Yes | Yes |
| UDS (any module's values) | Yes, 11 and 29-bit | Yes (CAN) |
| VW TP2.0 | If the adapter does raw CAN | Yes (CAN) |
| VAG KWP2000 on K-line | Yes (engine blocks) | Yes |
| **VAG KWP1281** (Mk4 blocks, cluster oil level) | **No**: no ELM adapter can do it | Yes |
| ME7 logger / flash read | No | Yes |
| Fault codes, module scan | Yes (UDS modules) | Yes |
| Throttle body adaptation | No | Yes (K-line) |

Run the **bench tests** (Bluetooth OBD tab) with any new adapter.

---

## Customising It

Almost everything is set in the web UI and saved on the gauge; Settings > Device downloads a backup and imports one.

- **Pages**: up to 8, each with its own style, source, values, label, range, redline and decimals.
- **Triggers**: 8 set-points on any value: above / below, threshold, hysteresis, hold time and latch, raising an
  alert, switching the output, or both. Picking a value suggests a threshold. **No Reading** decides what happens when
  a sensor fails (ignore, or trigger). The value list is grouped by source and only offers what the gauge has,
  including any page's own value; a trigger keeps its value read even when its page isn't showing.
- **The output**: high-side or low-side, Off / On or Auto (triggers), with a fault light. Held off in standby.
- **Themes**, **boot logos**, **brightness**, **swipe animation** and the WiFi name and password.
- **Menu and Tabs**: hide what your car doesn't have; the screen's menu item and the web UI tab go together.
- **Units**: speed, boost, pressure, temperature, mixture (lambda / AFR) and distance, each metric or imperial. The
  dials follow, and every gauge's range is converted.
- **Your own DBC** and your own **label file** per module.

---

## Web Interface

Join the gauge's WiFi, **`can2gauge-XXXX`** (no password until you set one), and open **`http://192.168.1.1`** or
**`http://c2g.local`**. Changes save automatically. Phones keep their mobile data while connected.

The same pages work on a phone and a laptop. On a phone the tabs fold into a menu button, grouped as Live, Car Data,
Set Up and Tools.

> [!NOTE]
> Some Android phones don't find `.local` names while on mobile data: use `http://192.168.1.1` there.

### Basic and Advanced

Each card shows the everyday settings; the fine-tuning sits behind **More Options**, and help behind the **(i)** on
its title. The **Advanced** switch opens every More Options at once and shows the tool tabs (**Diagnostics**,
**System**) and the inspection cards.

<p align="center">
  <img src="images/ui-menu.png" alt="The phone menu: the tabs in groups, the Advanced switch at the bottom" width="260">
  &nbsp;
  <img src="images/ui-gauges.png" alt="Gauges: one line per gauge, the first one open with style, source, label and value" width="260">
  &nbsp;
  <img src="images/ui-settings.png" alt="Settings, Device section: the menu and tabs switches, the Advanced switch and the device buttons" width="260">
</p>

### First Boot Setup

A new gauge asks on the screen: **Set this gauge up from your phone?** **Yes** shows the WiFi name and address. The web
UI then opens straight on its **First Boot** page (the rest loads behind it):

1. **What is connected**: a tick or a cross for the CAN (and which car), OBD-II, OpenHaldex, adc2can, usb2ip, the
   K-line, a Bluetooth or WiFi OBD adapter and other gauges. Have the ignition on. A Bluetooth adapter it finds is
   paired there and then, and the check waits until it has found the car (then the WiFi adapter row isn't needed).
2. **Suggested setup**: one row per setting with the reason: CAN profile and the car's DBC, OBD, K-line, oil, termination,
   power, dimming, units, colours, boot logo, sprites, and which menu items and tabs to show.
3. **Gauges**: a starter set, all ticked: only gauges the car can actually show, from what was found (OpenHaldex, the
   OBD-II values, the K-line engine's measuring blocks, adc2can sensors). When there's a way in to the car, the car scan runs
   as part of the checks and the list comes up with the values it found in other modules (oil temperature, gear, boost,
   EGT, gearbox temperature ...). **Scan Again** if the car has changed. Untick or reorder any.
4. **Your car**: the car the VIN says it is, with its picture; check it and press **This Is My Car**.

While the setup runs on the phone, the gauge's screen shows a **SETTING UP** ring with what it's doing (checking,
scanning which module) and how far it has got, until **Apply** (it says **SET UP**) or **Skip**, then fades out.

Nothing changes until **Apply**. **Settings > Device > First Boot Setup** runs it again.

<p align="center">
  <img src="images/glass-firstboot-ask.png" alt="The screen on a new gauge: Welcome, set this gauge up from your phone? Not now / Yes" width="300">
  &nbsp;
  <img src="images/glass-firstboot-login.png" alt="The screen after Yes: join the WiFi, then open 192.168.1.1 or c2g.local" width="300">
</p>

<p align="center">
  <img src="images/ui-firstboot.png" alt="First Boot: what is connected, a tick or a cross per source" width="260">
  &nbsp;
  <img src="images/ui-firstboot-setup.png" alt="First Boot: the suggested setup, one row per setting with the reason" width="260">
  &nbsp;
  <img src="images/ui-firstboot-pages.png" alt="First Boot: the starter set of gauges, only ones the car can show" width="260">
</p>

### Your Car

First Boot and **Settings > Device > Your Car** show which car the gauge is in.

- **The VIN** comes from the OBD adapter, the CAN (MQB / MLB gateways broadcast it; otherwise the gauge asks once) or
  an ME7's K-line block 081.
- **The make** from the VIN for any car (over 200 maker codes); on VW-group cars the model and year too, and on BMWs
  the models seen so far (X5 E70 / F15 / G05).
- **The picture**: the car's body type in the paint you pick.
- The first VIN read saves the car by itself if none is saved; **This Is My Car** confirms it. It decides **Match car**
  and the DID Library's car group, and survives a settings reset.
- Pick **My Car** as the boot logo to boot to its picture.

<p align="center">
  <img src="images/ui-firstboot-car.png" alt="First Boot step 4: Your car, the VIN read over the OBD adapter, a picture of the Golf IV in red, and its make, model, year and paint" width="260">
</p>

### Tabs

Tabs for things your car doesn't have stay hidden (First Boot sets them; Settings > Device > Menu and Tabs brings any
back). Diagnostics and System show with **Advanced** on.

| Tab | Purpose |
|---|---|
| **Dashboard** | Haldex buttons, a live tile per menu item, the live data on screen, gauge controls |
| **Log** | The [data logger](#data-logger), and the [drag timer](#drag-timer)'s scores |
| **Navigation** | The nav link, pop-up distance, connection, **Show Test Turn** |
| **Gauges** | The page editor |
| **CAN** | Bus settings, termination, OBD-II over CAN, car identification and the **DBC library** (three slots) |
| **OpenHaldex** | Live lock, mode buttons and the controller settings |
| **ADC** | The adc2can sensors: live values, scaling and dial range |
| **usb2ip** | The ECU over WiFi link |
| **K-line / ME7** | Protocol, K-line mode, the K-line through the OBD adapter, the ME7 logger and flash read |
| **Oil** | OEM oil source and live values |
| **Gearbox** | Live selector and gear, the gearbox type and its reads |
| **Diagnostics** | Advanced. Up to 32 [measuring values](#diagnostics-uds--tp20) from any module, and the **DID Library** |
| **Service** | [Fault codes](#service) with descriptions (OBD-II on any make over the K-line), **Scan Modules**, throttle body adaptation |
| **Bluetooth OBD** | Connection, pairing, Fast OBD, supported PIDs, terminal, bench tests |
| **Triggers** | The 8 set-points |
| **Daisy Chain** | Gauge Numbers, follow options, the gauges heard, **Update Other Gauges** |
| **Scan My Car** | Finds every value the car can show, listed by module with their readings; they become the **Car** source on the Gauges tab (modules first, then the gauge's own live values) |
| **Settings** | **Display** (brightness, units, themes, gauge feel, rotation), **Logo & Sprites**, **Power & Wiring** (output, illumination, ignition), **WiFi**, **Device** (Your Car, Menu and Tabs, Advanced, Enable Other Scanners, backup, First Boot, factory reset) |
| **System** | Advanced. The gauge's own health: buses, links, power, memory, display and drawing |
| **OTA** | Versions and updates |

<p align="center">
  <img src="images/ui-dashboard.png" alt="Dashboard: Haldex buttons, menu tiles and the live data grid" width="260">
  &nbsp;
  <img src="images/ui-haldex.png" alt="Haldex: live lock, mode buttons and the OpenHaldex controls over WiFi" width="260">
  &nbsp;
  <img src="images/ui-display.png" alt="Settings, Display section: brightness, theme with its live preview, units, and the Screen Rotation card" width="260">
</p>

<details>
<summary>More web UI screenshots</summary>

<p align="center">
  <img src="images/ui-can.png" alt="CAN: bus settings, car identification against the DBC library, and the IDs on the bus" width="260">
  &nbsp;
  <img src="images/ui-ecu.png" alt="ECU over WiFi: the usb2ip link with an Ignitron, the channels arriving and the everyday values" width="260">
  &nbsp;
  <img src="images/ui-gauges-ecu.png" alt="Gauges: a page with Source = ECU over WiFi, knock retard on the arc and injector duty, lambda target and intake in the centre" width="260">
</p>

<p align="center">
  <img src="images/ui-kline.png" alt="K-line mode: the module, Read Blocks Here with the block number, Find... with its matches, and a live block with its formula and spec lines" width="260">
  &nbsp;
  <img src="images/ui-obd.png" alt="Bluetooth OBD: connection, adapter and vehicle, terminal" width="260">
  &nbsp;
  <img src="images/ui-oil.png" alt="Oil: OEM oil temperature and level, source and raw fields" width="260">
</p>

<p align="center">
  <img src="images/ui-dsg.png" alt="DSG: live selector and gear mimic, and the gearbox source" width="260">
  &nbsp;
  <img src="images/ui-adc.png" alt="ADC: the adc2can sensors with their scaling" width="260">
  &nbsp;
  <img src="images/ui-triggers.png" alt="Triggers: set-points with threshold, hysteresis, hold, action and latch" width="260">
</p>

<p align="center">
  <img src="images/ui-output.png" alt="Settings: the output (wiring and control) and the illumination input" width="260">
  &nbsp;
  <img src="images/ui-me7.png" alt="ME7: the logger, and the K-line status with the module's part number" width="260">
  &nbsp;
  <img src="images/ui-bootlogos.png" alt="Settings, Logo and Sprites section: the boot logo, and your own boot logo with the in-browser converter" width="260">
</p>

<p align="center">
  <img src="images/ui-chain.png" alt="Daisy Chain: Gauge Numbers (every gauge's number and role, set from this one) and the row wave" width="260">
  &nbsp;
  <img src="images/ui-can-frames.png" alt="CAN: statistics and recent frames (Advanced)" width="260">
  &nbsp;
  <img src="images/ui-obd-bench.png" alt="Bluetooth OBD: bench tests" width="260">
</p>

<p align="center">
  <img src="images/ui-ota.png" alt="OTA: device, update from GitHub, Home WiFi bridge, update from files" width="260">
  &nbsp;
  <img src="images/ui-diag.png" alt="System: the gauge, communication and power blocks at a glance (Advanced)" width="260">
  &nbsp;
  <img src="images/ui-chain-update.png" alt="Daisy Chain: Update Other Gauges, each gauge's version and how its update went" width="260">
</p>

<p align="center">
  <img src="images/ui-nav.png" alt="Navigation tab: the nav link, pop-up distance, the phone connected and the next turn" width="260">
  &nbsp;
  <img src="images/ui-diagnostics.png" alt="Diagnostics: a 2025 Tiguan's engine read through the Bluetooth adapter on 29-bit ids, and the DID Library searched for oil temperature" width="260">
  &nbsp;
  <img src="images/ui-service-scan.png" alt="Service: Scan Modules through the OBD adapter, each module's part number, name, ids and fault count" width="260">
</p>

</details>

---

## Hardware

### The Unit

A standard **52 mm gauge**: the screen and bezel sit on the dash, the body goes through the hole, and everything plugs
into the rear cap.

<p align="center">
  <img src="images/unit-turntable.gif" alt="The assembled gauge turning: the screen, the side with the head and body, the rear cap with the plugs and the U-bracket" width="360">
</p>

![Rear of the gauge: J_BUS1, J_BUS2 and J_OUT through their windows, USB-C, BOOT and RESET pin holes, power LED, three M2 screws and the two M3 bracket studs](images/unit-rear.png)

![Exploded view: U-bracket, M2 screws, rear cap, PCB, head and body, the 2.1-inch screen and the bezel ring](images/unit-exploded.png)

### The Board

A Ø48 mm, 4-layer board behind the screen, with all the plugs and the USB-C on the rear face.

<p align="center">
  <img src="images/pcb-top.png" alt="The routed can2gauge V3.4 board (silkscreen can2gauge V1.0)" width="520">
</p>

| Block | Parts | Notes |
|---|---|---|
| 12 V in | Pass-through plugs, input PTC, SMF24CA TVS, SS54 | Power and CAN arrive together on either plug |
| 5 V / 3V3 | TPS62933 buck, CH213K OR-ing with USB, AMS1117-3.3 | Runs from USB on the bench |
| MCU | ESP32-S3-WROOM-1-**N16R8**, USB-C, BOOT / RESET | USB console |
| CAN | TCAN1044V, PESD2CAN, switched 120 Ω | Classic CAN only |
| I/O expander | TCA9554 | LCD and touch reset, termination, output, dash-light and fault inputs |
| Display | YouriTech ET021CR01-Z, 2.1" 480 × 480 ST7701S | Touch pins wired for a touch version |
| Backlight | AL8860 constant-current **from 12 V** | Stays dark on USB power alone |
| Output | DRV8876 half-bridge | High- **or** low-side, 1.5 A limit, protected |
| K-line | L9637D, tester pull-up | Fast enough for ME7 at 56000 baud |
| Dash lights | Peak detector | 12 V, dimmed 12 V or 58d PWM |

Analog sensors come in through the [adc2can](#adc-the-adc2can-expander). The same firmware also runs on the
**Waveshare ESP32-S3-Touch-LCD-2.1** development board.

<details>
<summary>Schematic</summary>

![can2gauge V3.4 schematic](images/pcb-schematic.png)

The EasyEDA Pro project and the BOM are in [PCB/](PCB/).
</details>

### Plugs

Three JST PA (2.0 mm) plugs on the rear.

![Rear plugs: J_BUS1 / J_BUS2 PA-4 (12 V, GND, CAN-H, CAN-L) and J_OUT PA-3 (OUT, K-line, dash lights)](images/pcb-connectors.png)

| Plug | Pin | Signal | Notes |
|---|---|---|---|
| **J_BUS1 / J_BUS2** (PA-4) | 1 | 12 V | Switched / ignition feed |
| | 2 | GND | |
| | 3 | CAN-H | Car CAN, or the gauge chain |
| | 4 | CAN-L | |
| **J_OUT** (PA-3) | 1 | OUT | 12 V high-side **or** ground low-side |
| | 2 | K-LINE | OBD pin 7 |
| | 3 | DASH LIGHTS | 58d / 58 |

The two bus plugs are wired straight through for chaining. Switch the 120 Ω on only on the gauge at the end of the
chain (or leave it on Auto).

### ESP32-S3 Pin Map

| Group | Signal | GPIO |
|---|---|---|
| LCD RGB | DATA0-15 (B1-5, G0-5, R1-5) | 42, 45, 48, 47, 21 · 14, 13, 12, 11, 10, 9 · 46, 3, 8, 18, 17 |
| LCD timing | HSYNC / VSYNC / DE / PCLK | 38 / 39 / 40 / 41 |
| LCD init (3-wire SPI) | SCK / SDA | 2 / 1 |
| Backlight | AL8860 CTRL | 6 |
| I2C | SDA / SCL (TCA9554 0x20, touch 0x15) | 15 / 7 |
| Touch | INT | 16 |
| CAN | TX / RX | 4 / 5 |
| K-line | TX / RX | 44 / 43 |
| USB | D- / D+ | 19 / 20 |
| Boot | BOOT button | 0 |

| TCA9554 port | P0 | P1 | P2 | P3 | P4 / P5 | P6 (in) | P7 (in) |
|---|---|---|---|---|---|---|---|
| Signal | LCD_RST | TP_RST | LCD_CS | CAN_TERM_EN (low = 120 Ω in) | DRV8876 IN1 / IN2 | Dash lights | Output fault |

### The 52 mm Housing

A standard 52 mm DIN gauge: a Ø51.8 body, a Ø76.4 head carrying the screen, a rear cap with a window per plug, and a
U-bracket. Printable in PETG or ASA; the files and assembly notes are in [Enclosures/](Enclosures/README.md).

<p align="center">
  <img src="images/unit-front.png" alt="The gauge from the front: bezel ring and the round screen" width="400">
  &nbsp;
  <img src="images/unit-open.png" alt="Rear cap off: the ESP32-S3 module, J_LCD flex connector, USB-C, BOOT / RESET and the three plugs inside the body" width="400">
</p>

---

## Updating

**Over the air**, from the **OTA** tab:

1. **Update from GitHub**: the gauge fetches the latest release (directly, or through the **Home WiFi bridge**).
2. **Update from Files**: upload `littlefs.bin` then `firmware.bin`.

A new firmware has to confirm itself within 60 s or the gauge rolls back.

**Gauge to gauge**: update one gauge, then press **Update Other Gauges** on its Daisy Chain tab. It updates the others
over WiFi one at a time (about a minute each), sending only the files that differ; a power cut leaves a gauge on its
old firmware. The car must be parked.

---

## Status and Bench Items

Still to be checked on a real car or bench:

- **Board bring-up**: first picture on the screen 2026-10-26; touch is untested (the supplied screens have no touch
  chip), and the sprite frame rate (6-8 fps so far) is being worked on.
- **OBD speed**: wired OBD-II polling on a car, and both **Fast OBD** options (off by default until then).
- **Dongle paths**: TP2.0 and K-line through the adapter, WiFi adapters, and a CAN car through a dongle.
- **K-line**: the ME7 Fast logger on more ECUs, the flash read on an unmodified ECU, KWP2000 VAG blocks, and the cluster
  oil level on a G266 car.
- **CAN**: Kombi_2 oil temperature, the MQB gearbox speed read, and chain bus load.
- **Daisy chain**: Gauge Numbers, the row wave, shared settings and ignition follow across three gauges; boot logos
  in step; **Update Other Gauges** including a power cut half way. It works on the bench; still to check: no lines on
  the gauge being updated with the 12 MHz update clock (0.36.13), and the backlight going off before the restart.
- **Radio**: WiFi range through the dash, and the AP, Bluetooth and WiFi links all at once.
- **First Boot** and **Your car** on a real car.
- **ECU over WiFi** on a car with each supported ECU.
- **Data logger** memory with everything running, and CSV download on iPhone and Android.
- **Nav link** on a real phone and route.
- **DBCs feeding the gauge** on an MQB car, a MaxxECU and an EMU Black / Link / Haltech.
- **8HP gearboxes**: a BMW X5 F15 through the adapter, an Audi / VW 8HP on the CAN, an MLB car.
- **Scan My Car**: on an MQB car (CAN wire and dongle), a PQ-era VW (TP2.0), a Mk4 (K-line) and a non-VW car; values
  from several modules on one page, mixed with OBD PIDs, a TP2.0 module and the DSG reads; Enable Other Scanners with
  another scan tool connected.
- **Simos 18**: the scan spotting one, every value's scaling (speed against GPS), and the read rate in Turbo.
- **K-line OBD-II on other makes**: fault codes (several messages, two ECUs), the VIN and Scan My Car on a non-VW
  K-line car; so far only the bench ME7.5 (ISO 9141-2).
- **Drag timer** against a GPS timer, over CAN and over OBD.
- **Web UI first load**: app.js now goes zipped; First Boot opens before the rest has loaded.
- **Diagnostics and Service**: UDS through the dongle on a 2025 Tiguan (29-bit), fault read / clear on K-line and CAN,
  and the throttle body adaptation on an ME7 1.8T.
- **Screen rotation** frame-rate cost, and **units** on the screen.

---

## Credits and Licences

- **opendbc** (comma.ai, MIT): the DBC library.
- **OBDb** ([github.com/OBDb](https://github.com/OBDb), CC BY-SA 4.0): the DID library, shared under the same licence.
- **OBDex** ([github.com/foerbsnavi/OBDex](https://github.com/foerbsnavi/OBDex), CC0): the fault code descriptions.
- **Waveshare ESP32-S3-Touch-LCD-2.1** (MIT): the ST7701S start-up sequence and display pin map the board started from.
- **NefMoto ME7 flasher** (GPLv3): the read path behind the ME7 flash dump, ported read-only.
- **LVGL**, **NimBLE-Arduino**, **ESPAsyncWebServer / AsyncTCP** (mathieucarbou) and **ArduinoJson**.
- **uPlot** (Leon Sorokin, MIT): the Log tab's graphs.
- **Espressif esp-bsp / esp_lvgl_port** (Apache-2.0): the ESP32-S3 SIMD blend routines.
- **MaxxECU**: its published "Default CAN output" specification.
- **Kile Thomson at Rokketek** (RokketGauge): the Simos 18 values, their scaling and how fast the engine can be asked.
- Thanks to **Garage Tinkering's Ultimate Gauge Board** for the inspiration.

## Disclaimer

Forbes Automotive accepts no responsibility for any incidents arising from the use of this gauge. It reads, and in a
few places requests, data on the car's diagnostic buses. IDs, scalings and protocols vary by vehicle, so check them
against your own car before relying on any value, trigger or output, and never run diagnostics while driving.
