# can2gauge

**can2gauge** is a 52 mm round digital gauge that reads almost anything a car will tell it and shows it on a
2.1" 480 × 480 IPS display. It sits in a standard 52 mm gauge pod and takes its data from the **car's CAN bus**, the
**K-line**, a **Bluetooth or WiFi ELM327 OBD dongle**, an **OpenHaldex** controller over WiFi, an **Ignitron, MaxxECU, Link,
Haltech or EMU PRO** over WiFi through **usb2ip**, or a **0-5 V sensor expander**. Every page, source, colour and logo is set from a phone over WiFi, with no laptop or app needed. With the optional
**can2gauge Link** Android app it also shows **Google Maps' next turn**, laid out like Android Auto.

It was built as the companion display for [OpenHaldex](#openhaldex), and it is just as happy as a boost / oil /
coolant gauge on any car with a CAN bus or an OBD port. It runs on an **ESP32-S3** (16 MB flash, 8 MB PSRAM) under
the Arduino framework with **LVGL 9**, and the whole project (firmware, web UI, PCB, enclosure, simulator and tools)
is open source.

<p align="center">
  <img src="images/unit-iso.png" alt="The assembled can2gauge: 52 mm gauge body, bezel and the round screen showing the RPM needle page" width="430">
  &nbsp;
  <img src="images/glass-hero.gif" alt="can2gauge on a simulated Mk4 pull: RPM needle, boost arc, Custom page with coolant / oil / battery, then the Haldex lock" width="310">
</p>

![can2gauge web UI: dashboard, gauge pages, Haldex, CAN / DBC, K-line, OBD adapter, display / themes and OTA](images/can2gaugeUI.png)

![can2gauge web UI: OEM oil, DSG, ADC expander, triggers, output, ME7, daisy chain and boot logos](images/can2gaugeUI-2.png)

> [!NOTE]
> **Status (firmware 0.36.0, not yet released; board V3.4 "V1.0").** The board is routed and reviewed and the firmware
> is feature complete for a first release. Everything on the screen and in the web UI runs today in the bundled
> [simulator](#simulator). Several paths still need their first sitting on a real car or bench: see
> [Bench items](#status-and-bench-items).

---

## Contents

- [Features at a glance](#features-at-a-glance)
- [On the screen](#on-the-screen) - page styles, setting up a page, the menu, mode screens, themes, boot logos, alerts, navigation, night dimming
- [Where the data comes from](#where-the-data-comes-from) - CAN, K-line, OBD dongles, OBD-II polling, OpenHaldex, DSG, OEM oil, ADC, ECU over WiFi, the daisy chain
- [Data logger and service](#data-logger-and-service) - graphs and CSV, fault codes, throttle body adaptation
- [OBD dongle support](#obd-dongle-support)
- [Customising it](#customising-it)
- [Web interface](#web-interface)
- [Hardware](#hardware) - the unit, the PCB, plugs, pin map, the 52 mm housing
- [Simulator](#simulator)
- [Updating](#updating)
- [Repository layout](#repository-layout)
- [Status and bench items](#status-and-bench-items)
- [Credits and licences](#credits-and-licences)

---

## Features at a glance

| Feature | Detail |
|---|---|
| Display | 2.1" round 480 × 480 IPS (ST7701S, RGB565), constant-current backlight, about 1000 nit, touch-ready. Runs without the screen too (bench, or a failed panel): everything but the drawing still works |
| Screen rotation | **Upside Down** for a gauge fitted the other way up, and **Rotation Trim** (±15° in half-degree steps) to straighten one that sits slightly crooked in its pod; touch follows the picture |
| Page styles | 12: Needle, Arc, Bar, Graph (the last 30 s as a rolling line), Single, Dual, Triple, Quad, Haldex, Gear (DSG), Oil (PQ), Custom (arc + three icon readouts); any page can switch to its **sprite** |
| Sprites | **Rev**, the can2gauge mascot: 22 animated clips that follow rpm, boost, Haldex lock and oil temperature, take the theme's colour and react to a tap. Any art converts to a `.c2gs` pack ([docs/SPRITES.md](docs/SPRITES.md)) |
| Screensaver | The mascot over the gauge once the car is still and the screen untouched (30 s, adjustable, off by default) |
| Permanent power | Ignition from CAN terminal 15 (PQ, MQB, any bit, or bus traffic), the illumination wire, or another gauge in the chain: shutdown animation, dark standby, deep sleep |
| Pages | Up to 8, swiped left / right (five are on when the gauge is new); each one has its own **source** and values, and is set up from the Gauges tab ([how](#setting-up-a-page)) |
| Sources per page | CAN (the gauge's own decode, or **any signal of any DBC**), K-line (any module, block and field), OBD (any mode 01 PID), ADC, **any channel of an Ignitron, MaxxECU, Link, Haltech or EMU PRO** (over WiFi from usb2ip), or the gauge itself |
| Gauge feel | **Analog**: needles, arcs and bars move like real pointers, a damped spring tuned to each signal's update rate. **Digital**: every reading shown the moment it arrives. 50 Hz refresh, anti-aliased needle in 0.1° steps |
| CAN | Classic CAN (TCAN1044V), 500 kbps by default, switchable 120 Ω termination (**Auto**: on only when nothing else terminates the bus). Profiles: VAG PQ, VAG MQB, **VAG MLB**, OBD-II, **MaxxECU**, listen-only. Every frame decoded the moment it arrives; OBD-II polls up to 6 PIDs per request, the next request straight after each answer, and can run **alongside** VAG PQ / MQB / MaxxECU |
| DBC library | All **108 opendbc DBCs** plus the gauge's own ECU DBCs (EMU Black, MaxxECU, Link, Haltech) on the gauge, VW / Audi translated to English; the web UI identifies the car from the bus; up to **three DBCs at once**, and DBCs that **feed the gauge's own values** directly |
| K-line | L9637D: KWP1281 (VAG measuring blocks), **KWP2000 VAG** (the same blocks, faster), KWP2000 / OBD-II (with a **Fast OBD** option), an **ME7 logger** (Normal ~34/s, or a **Fast** RAM-routine mode ~50/s), and a read-only **ME7 flash dump** |
| VCDS labels | Field names, block titles and specs from **501 VCDS label files**, picked by the module's part number |
| OBD dongles | ELM327-type adapters over **Bluetooth LE or WiFi**: VIN auto-ID, every standard PID up to 0x60 (with a **Fast OBD** option), the car's **CAN broadcast** read through the adapter, raw terminal, VW **TP2.0** through the adapter, and **UDS to any module** (measuring values, fault codes, module scan) on 11-bit or 29-bit CAN |
| Measuring values | Up to 12 values from any module, the way VCDS reads them (UDS or KWP2000 over TP2.0), over the gauge's CAN **or the OBD dongle**, with a **DID library** of 3,555 VW-group values (and 20 more car groups) to pick from |
| OpenHaldex | Live lock / target / mode over CAN **or WiFi (ESP-NOW)**, mode change from the screen, full controller status in the web UI |
| Gearbox | DSG and **ZF 8HP** (Audi / VW and BMW): selector and gear from the CAN broadcast (PQ, MQB, MLB), lock-up and input speed; gearbox speed, oil temperature, lock-up and selector by UDS (BMW extended addressing too), TP2.0 or the dongle |
| OEM oil | Mk4-era G266 oil temperature **and level** from the cluster (CAN or K-line) |
| ADC | Up to 16 × 0-5 V sensors from two [adc2can](#adc-the-adc2can-expander) boards on the CAN or over WiFi, auto-scaled |
| ECU over WiFi | [usb2ip](#ecu-over-wifi-usb2ip) on an **Ignitron**, **MaxxECU**, **Link**, **Haltech** or **ECUMaster EMU PRO** sends the ECU's data with no wiring: the everyday values fill the gauge's own signals, and **every channel** (380 Ignitron, 596 MaxxECU, 831 Link, 936 Haltech, 1,409 EMU PRO) can go on a page |
| Data logger | The **active gauges** (or up to **16 values** you pick) from any source at 1-50 samples a second, recorded in the gauge's memory: **graphs** on the phone, **CSV** download (Excel, MegaLogViewer, Datazap) |
| Service | **Read / clear fault codes** over the gauge's own K-line (KWP1281, KWP2000) or CAN (UDS, TP2.0), or UDS through the OBD dongle; **Scan Modules** (every module's part number, name and fault count); **throttle body adaptation** (basic setting 060 / 098) |
| Daisy chain | Gauges share values over CAN or ESP-NOW; one master polls the car, followers can mirror its page and night mode; number and set up every gauge from any one of them (Show Numbers puts each gauge's number on its screen); update one gauge and it passes the update on to the others |
| Navigation | **Google Maps turn-by-turn**, Android Auto style: the turn arrow, distance, road and arrival time, from the **can2gauge Link** Android app over Bluetooth LE. Pops up as a turn gets close (or stays up for the whole route); a tap hides it until the next turn |
| Triggers | 8 set-points (above / below, hysteresis, hold, latch, what to do with no reading) raising an on-screen alert and / or switching the output |
| Output | One 12 V output, **high-side or low-side** (chosen in the web UI), protected half-bridge |
| Night dimming | Dash-light input (58d PWM, dimmed 12 V or 58) switches to the night brightness |
| Themes | **71**: 65 manufacturer cluster colours across 31 makes, 4 house palettes, Custom, and **Match car** from the VIN |
| Your car | The **VIN** read from the OBD dongle, the CAN (the MQB / MLB gateway's broadcast, or OBD over the gauge's CAN) or the K-line (ME7 block 081); make, model and year confirmed on First Boot or in Settings, with a **picture of the car** in its paint colour |
| Boot logos | can2gauge (its dial sweeps at ignition on), Forbes, OpenHaldex, custom text, **33 make logos**, or any image converted in the browser |
| First Boot | A new gauge asks on the screen to be set up from a phone. The web UI then checks every source, ticks off what it found and suggests the CAN profile, OBD adapter, dimming, boot logo, sprites, menu and gauge pages, all changeable before applying |
| Web UI | 18 tabs on the gauge's own WiFi (including a live **Diag** view), shown to suit the car (First Boot sets them, Settings brings any back), plus the First Boot page. **Basic / Advanced**: the everyday options first, the rest behind **More Options** or the **Advanced** switch; one line per gauge; Settings in sections; shared Forbes Automotive theme |
| Updates | Over the air from GitHub or from files (web UI + firmware), 60 s rollback guard |
| Simulator | The real web UI and a drawn screen running against a simulated Mk4 on your PC (Python only) |

---

## On the screen

### Page styles

Every page is one of twelve styles, listed in this order in the web UI's Style box. Any signal can go on any page, and each page has its own range, redline, decimals
and label.

![The twelve page styles: Needle, Arc, Bar, Graph, Single, Dual, Triple, Quad, Haldex, Gear (DSG), Oil (PQ) and Custom](images/glass-pages.png)

| Style | What it shows |
|---|---|
| **Needle** | A classic dial with a counterweighted, anti-aliased needle that sweeps over the name and number, like a printed dial |
| **Arc** | A sweep with a tick scale and a big number; redline colour above the set point |
| **Bar** | A horizontal bar with the value above |
| **Graph** | The value over the last 30 seconds as a rolling line, Min at the bottom and Max at the top, with the redline drawn across it and the value and unit above. Every Graph page keeps recording while another page is showing, so its last 30 s are there as soon as you swipe to it (they start again when the page's settings change) |
| **Single** | The value large, with its icon above the label |
| **Dual** | Two values, each with its own bar and an icon beside it |
| **Triple** | Three values: the page's value large on top, **Value 2** and **Value 3** side by side below, each with its icon and name above and unit below |
| **Quad** | Four values in a 2 × 2 grid, each with its icon and name above and unit below: the page's value top left, then **Value 2-4** |
| **Haldex** | Target and actual lock arcs, the mode, and an optional second value |
| **Gear (DSG)** | P R N D S M strip, the engaged gear huge (D3, S4, M2 ...), and the speed |
| **Oil (PQ)** | The OEM oil-temperature arc with **LEVEL OK / LOW** and the source's live status |
| **Custom** | The arc as the main gauge (its value and unit high in the dial, the label at the bottom), with three icon readouts stacked in the centre: **Top**, **Middle** and **Bottom** (see [below](#custom-and-quad-pages)) |
| *Sprite switch* | Any page (Haldex, RPM, Boost ...) can show a sprite pack (Rev by default) instead, inside a ring that shows its reading; the pack picks its clip from the car. Tap it for a reaction |

A page waiting for data says why, under the value: *"Waking module 0x01 (5-baud init)"*, *"No OBD adapter paired - see
the web UI"*, *"0x280 not on the bus"*, with a small spinner while a K-line, OBD adapter or VCDS-style source connects.
A value that stops arriving goes stale and shows `--` (CAN 2 s, Haldex 1.5 s, ADC 1 s). Text that changes (page names,
status lines, the menu's live status) is cut short with "..." where it would run off the round edge.

**Sources are ready before you swipe.** A page's source doesn't wait for the page to come on screen: the gauge keeps
one K-line module connected for all the pages that use it, and runs the OBD adapter and the VCDS-style reads while any
enabled page needs them, so swiping to a page shows a live value straight away.

**Page changes.** A swipe slides the next page in at the screen's full frame rate (800 ms by default, Settings >
Display > Swipe animation; 0 swaps the page at once). The page dots along the bottom show how many pages are on and
which one is showing.

### The pages a new gauge starts with

| # | Label | Style | Shows | On from new |
|---|---|---|---|---|
| 1 | HALDEX | Haldex | OpenHaldex lock (actual and target) | Yes |
| 2 | BOOST | Arc | Boost | Yes |
| 3 | RPM | Needle | Engine speed | Yes |
| 4 | COOLANT | Arc | Coolant temperature | Yes |
| 5 | ENGINE | Quad | Coolant, intake, oil temperature and boost | Yes |
| 6 | SPEED | Bar | Road speed | No |
| 7 | BATTERY | Single | Battery voltage | No |
| 8 | OIL TEMP | Arc | Oil temperature | No |

[First Boot Setup](#first-boot-setup) replaces these with gauges for what it actually finds on the car. Pages that are
off keep their settings: switch one on and it joins the swipe.

### Setting up a page

Every page is set on the web UI's **Gauges** tab (join the gauge's WiFi, open `http://192.168.1.1` or
`http://c2g.local`). The screen rebuilds as you change things, and every change saves by itself.

1. **Open the gauge.** The tab has one line per page: its number, label, style and value, with an on / off switch.
   Tap a line to open it. The switch puts the page in or takes it out of the swipe.
2. **Style**: one of the ten above.
3. **Source**: where the page's values come from.
   - **CAN**: the gauge's own decode (the CAN profile, OpenHaldex, the daisy chain), or any signal of a loaded DBC.
   - **K-line**: a module, block and field, named from its VCDS label file; with the ME7 logger running, any logged
     ME7 variable.
   - **OBD**: any mode 01 PID, through the dongle, the wired CAN or the K-line.
   - **ADC (adc2can)**: an adc2can sensor.
   - **ECU (usb2ip)**: any channel of the ECU [usb2ip](#ecu-over-wifi-usb2ip) reads, with a Find box.
   - **VCDS (UDS / TP2.0)**: one of the [VCDS-style measuring values](#vcds-style-measuring-values-uds--tp20).
   - **Specifics**: the gauge's own values (board temperature, uptime, dash lights, the output) and the OEM oil
     temperature and level.

   All the values on one page come from the same source.
4. **Label**: the name on the screen, up to 15 characters.
5. **The value.** Picking one fills in a sensible range, redline and decimals for it (an adc2can sensor or ECU channel
   brings its own). Dual and Haldex pages get a second value; Quad and Custom pages get three more (below).
6. **More Options** (at the bottom of the open gauge): **Min**, **Max**, **Redline** (the arc, bar and ticks turn the
   redline colour above it), **Decimals** (0-2) and **Sprite** (show the page as a [sprite](#sprites-the-screensaver-and-rev)).
7. **Move Up** / **Move Down** change where it sits in the swipe.

On the gauge, swipe left or right to reach the page. With the [daisy chain](#daisy-chain)'s Follow page on, the
followers swipe with the master.

### Custom and Quad pages

Both show four values: the page's own value plus three slots, picked under the value on the Gauges tab.
A **Triple** page uses the first two of those slots (**Value 2** and **Value 3**); the third picker is hidden for it.

**Custom** is the arc page with a stack of three readouts in the middle. The arc is the main value, with its range and
redline; the slots are the **Top**, **Middle** and **Bottom** rows, each an icon, the value (right-aligned) and its unit,
with a thin line between the rows (see [Value icons](#value-icons)).

For example, an "ENGINE" screen with coolant on the arc (92 °C) and oil temperature, intake air and battery stacked in
the middle is a Custom page set up like this:

| Setting | Value |
|---|---|
| Style | Custom |
| Source | CAN |
| Label | ENGINE |
| Value (the arc) | Coolant, Min 0, Max 140, Redline 110 |
| Top | Oil temperature |
| Middle | Intake air temperature |
| Bottom | Battery voltage |

A new Custom page starts with coolant, battery and oil pressure in its three rows, ready to change.

**Quad** puts the four values in a 2 × 2 grid, each with its name above and its unit below: the page's value top left,
then **Value 2** top right, **Value 3** bottom left and **Value 4** bottom right. A long name is shortened to stay in its
corner. The page's title is where its values come from (CAN, OBD, ADC, or the ECU's name, such as IGNITRON or HALTECH),
filled in when you pick the style, source or first value; the same goes for Triple. You can still type your own.

<p align="center">
  <img src="images/glass-page-custom.png" alt="A Custom page: the boost arc with coolant, oil temperature and battery stacked in the centre" width="300">
  &nbsp;&nbsp;
  <img src="images/glass-page-quad.png" alt="A Quad page: coolant, intake, oil temperature and boost in a 2 by 2 grid" width="300">
</p>

### Value icons

**Single**, **Dual**, **Triple** and **Quad** pages show an icon with each value (above its name, or left of its bar on
Dual), and Custom uses the same set for its rows. Every icon is drawn in the same thin-line style and takes the
theme's accent colour.

![The value icons, in the order of the table below](images/value-icons.png)

The gauge's own signals have a fixed icon:

| Icon | Signals |
|---|---|
| Coolant | Coolant temperature |
| Oil temperature / pressure / level | Oil temperature; oil pressure and level (pressure and level that are a warning switch read **OK** / **LOW**) |
| Battery | Battery voltage, the OpenHaldex supply |
| Boost | Boost (relative or absolute) |
| Intake | Intake air temperature, air mass |
| Outside | Outside air temperature |
| Fuel | Fuel level, ethanol content |
| Speed | Road speed, gearbox speed, OpenHaldex speed |
| RPM | Engine speed, gearbox input speed |
| Lambda | Lambda, lambda target |
| Gearbox temperature | Gearbox oil temperature |
| Haldex | Haldex lock, target, mode, clutch and oil temperature |
| Throttle | Throttle, pedal, engine load |
| Gear | Gear, selector, shift and lock-up state |
| Temperature | EGT, board temperature, and any other temperature |
| Pressure | Fuel pressure, barometric pressure, and any other pressure |
| Voltage | A voltage that isn't the battery (a raw ADC sensor) |
| Ignition | Ignition timing, knock retard |
| Injector | Injector duty |
| Brake | Brake values (from a DBC, an ECU ...) |

Any other value (a DBC signal, an OBD PID, a K-line field, an ECU channel, an ME7 variable or an ADC sensor) gets
its icon from its **name and unit**, and follows the name when it arrives a few seconds after start-up. An adc2can
sensor named "Oil temp" in °C gets the oil can, "Fuel pressure" gets the pressure gauge, "EGT" the thermometer, and an
unnamed sensor reading volts the voltage icon. A value that matches nothing has no icon. On a Custom page the three
generic icons (temperature, pressure, voltage) give way to the value's short name, so "ADC 2" and "ADC 3" can still be
told apart. A slot set to **None** leaves its row blank.

### Decimals

A page's main value uses the page's **Decimals**. Every other value on a page (Quad and Triple values 2-4, the
second value on Dual and Haldex pages, the Custom rows) shows the decimals that suit it, worked out the same way
everywhere: an ADC sensor's own setting (two places by default), an ECU channel's or ME7 variable's own, otherwise from
its unit (two for lambda, ratios and bar; one for volts, degrees and milliseconds; whole numbers for rpm,
temperatures, speed, mbar and %).

### The menu

Hold a finger on the screen for 2 s (or swipe up / down) for the mode menu: one item per screen with a two-colour icon, a line of text
and a **live status** (bus rate, link state, connection). Swipe for the next item, tap to open it, hold or wait
10 s to go back. Any item except Settings can be hidden from Settings > Device > Menu and Tabs, which also hides its tab in
the web UI.

<p align="center">
  <img src="images/glass-menu.gif" alt="The mode menu: Gauges, Haldex, CAN bus, K-line, Output / Illumination, Gearbox, ADC, Bluetooth OBD, Daisy chain, Settings" width="300">
</p>

### Mode screens

Behind each menu item is a live screen. Opening one is what claims its source: the K-line session or the Bluetooth
link runs while the screen shows and is released 3 s after you leave.

![The mode screens: menu, Haldex, CAN bus, K-line, Bluetooth OBD, Gearbox, ADC, Output / Illumination, Daisy chain, Settings](images/glass-modes.png)

| Screen | What it does |
|---|---|
| **Haldex** | The Haldex gauge with a dot per mode; **swipe to change the OpenHaldex mode** (confirmed by its broadcast) |
| **CAN bus** | Every ID on the bus, decoded with the car's DBC five signals at a time (counters and checksums left out), or the raw eight-byte grid with changed bytes lit |
| **K-line** | Wakes the module picked in the web UI, shows each step and the part number, then one measuring block at a time with VCDS names; swipe for the next block |
| **Bluetooth OBD** | Connects to the paired adapter, says what it is doing, then pages of live PIDs |
| **Gearbox** | The selector and gear full screen, plus speed, gearbox temperature and the converter lock-up (locked / slipping) |
| **ADC** | The adc2can sensors four to a page with the board's supply and fault state |
| **Output / Illumination** | ON / OFF, how the output is wired, its control and any active trigger, the dash-light state and brightness |
| **Daisy chain** | A circle per gauge on the link, who is master, this gauge's number in the row, frames and follow options |
| **Settings** | Brightness (10 % steps), CAN termination, units (Metric / Imperial; Custom is set in the web UI), theme picker, reboot. Everything else is in the web UI |

### Themes

71 themes. 65 are manufacturer cluster colours across 31 makes, one per era where the make's illumination
changed (VW green → blue → white, Audi red → white, BMW amber ...). The rest are four house palettes, **Custom** (two
colour pickers) and **Match car**, which reads the VIN through the OBD dongle and picks the make and era for you,
then remembers it so the next boot starts in the right colours. Pick from a grouped dropdown with a live preview in
the web UI, or from Settings > Theme on the screen.

![The RPM needle page in twelve themes: Forbes teal, VW blue, VW green, Audi red, BMW amber, Skoda green, Mercedes, Porsche, Mazda red, Subaru red, Nissan amber, Saab green](images/glass-themes.png)

<p align="center">
  <img src="images/glass-themes.gif" alt="The needle page cycling through manufacturer themes" width="300">
</p>

### Screen rotation

Settings > Display > **Screen Rotation** turns the whole picture round the middle of the screen, menus and alerts
included. **Upside Down** is for a gauge fitted the other way up. **Rotation Trim** turns it up to 15° either way, in
half-degree steps (+ is clockwise), for a gauge that sits slightly crooked in its pod. Touches are turned back the
same way, so taps and swipes still land on what's drawn, and an upside-down gauge's page slides still go the right
way. Both apply as soon as they're changed.

While either is set, the gauge draws each frame into a spare picture and turns it onto the screen, so frames cost a
little more (Diag > Drawing shows it under Copy to panel). With both at 0 the picture goes straight to the screen as
before. It needs Direct drawing mode and about 450 kB of PSRAM. If a rotation is first set after the gauge has
started and that memory is already in use, the card asks for a restart.

### Boot logos

The gauge can boot to the **can2gauge** logo (the default), Forbes Automotive, OpenHaldex, your own text, a
**make logo** (33 included: the theme's make, the VIN's make on Match car, or one you pick), or **any image you
like**: pick **Your own image** as the boot logo and a card appears where the web UI converts a PNG / JPG / SVG in the
browser, previews it on a round screen and uploads it as `/logo.bin`. On the
gauge logos are stored as **C2GL** (RLE-packed RGB565), typically 10-60 kB each, unpacked once into PSRAM.
The can2gauge logo's own dial sweeps up to full and settles back, its ticks lighting as it passes, like a cluster's
needles at ignition on. The text logos (OpenHaldex, your own text) sweep a ring round the edge of the screen. The sweep
takes 3 s and the logo is held a moment before fading into the first page (Display > Boot fade, 2000 ms by default).
**Show it now** in the web UI plays it without a reboot.

In a [daisy chain](#daisy-chain) the gauges' logos come up and sweep together. Each gauge takes a slightly different
time to start, so they agree over CAN first: each one builds its logo with the screen dark and says it's ready, and
once every gauge has, they count down together and all light up on the same moment. A gauge that is missing doesn't
keep the others dark for more than 1.5 s, and each gauge remembers how many it found for next time. For the first few
seconds of the logo they keep comparing clocks, and one that is behind jumps forward to match. This happens whether
or not link frames are allowed on CAN; after it the gauge goes back to that setting.

<p align="center">
  <img src="images/glass-boot-can2gauge.png" alt="The can2gauge boot logo: a dial reading OBD / CAN / K-LINE / ADC over the wordmark, its sweep settled at 72 %" width="300">
  &nbsp;&nbsp;
  <img src="images/glass-bootlogos.gif" alt="Boot logos cycling: can2gauge, Forbes, VW, Audi, Seat, Skoda, Porsche, BMW, Mercedes, Ford and more" width="300">
</p>

<details>
<summary>All the logos on the gauge</summary>

![Every boot logo in the file system: can2gauge, Forbes and 33 makes](images/glass-bootlogos.png)

Make logos are the trademarks of their owners and are included only so the gauge can match the car it is fitted to;
where each one came from is in [docs/LOGO_SOURCES.md](docs/LOGO_SOURCES.md).
</details>

### Alerts

A [trigger](#customising-it)'s alert comes up over whatever page is showing: a steady red ring round the edge, a red
wash over the gauge, and a warning card in the middle with the trigger's label, pulsing between dark and bright red
about once every 1.2 s. An alert that isn't latched goes when the value comes back past its hysteresis; a latched one
says **Tap to acknowledge** and waits for a tap. A tapped-away alert stays away until its value has come back past the
threshold and hysteresis, then the trigger re-arms; an output switched by a latched trigger stays on until then too.
The Dashboard's Gauge Control card has **Clear Alerts** (the same as tapping it away, from the phone) and **Test
Alert** (shows one to check it), next to **Previous Gauge** / **Next Gauge**.

<p align="center">
  <img src="images/glass-alert.gif" alt="A coolant alert pulsing red over the coolant arc" width="300">
</p>

### Turn-by-turn navigation

Google Maps' next turn on the screen, laid out like Android Auto: the turn arrow and the distance on a
band in the theme's accent colour, the road under it and the arrival time at the bottom. A three-quarter ring round
the edge, in the theme's second colour, shows the whole journey: it fills from a start dot (bottom left, where the
route began) over the top to a chequered flag (bottom right, the destination). The app works it out from what Maps says
is left (distance, or time). It comes up over whatever the gauge is showing.

- **Where it comes from:** the **can2gauge Link** Android app (`android/`) reads the navigation notification Google
  Maps already shows while it navigates, and sends the turn to the gauge over **Bluetooth LE**. Maps isn't changed and
  Android Auto isn't needed (both can run side by side). The phone can stay in a pocket, locked.
- **Getting the app:** download it from **[can2gauge.com/app](https://can2gauge.com/app)** (a signed APK, Android 8.0
  or later; allow the browser to install apps when asked), or build it from `android/` in Android Studio. Keep the
  app and the gauge on the same version (the app's version is the firmware it goes with): they share the packet
  layout. Every release of the app is signed with the same Forbes Automotive key, so a new one installs over the old.
- **When it shows:** **Pops up near a turn** (300 m by default, 10 m - 2.5 km) and goes again after it, or **Whole
  route**. Rerouting and arriving always show. A **tap** hides the card until the next turn.
- **Setting it up:** web UI > **Navigation** tab (always there, next to Log): switch the **Nav Link** on, then in
  the app grant notification access and Bluetooth and set battery use to Unrestricted. The app keeps searching until
  it finds the gauge, then reconnects by itself every time the ignition comes on. The tab says whether the phone can
  see the gauge and whether it's connected. **Show Test Turn** puts a test card on the screen with no phone.
- **Daisy chain:** the app links to the first gauge it finds with Nav Link on, so switch it on in one gauge only
  (**Forget Gauge and Search Again** in the app after changing which). Runs alongside a Bluetooth OBD adapter: the
  gauge holds both connections at once.
- **The app's screen** (dark, in the web UI's colours): **From Google Maps** (Maps' own banner, rebuilt from its
  notification: its arrow picture and words) above **On the Gauge** (the turn as the gauge gets it, and which arrow it
  uses), so a wrong arrow or a cut-off road name is easy to spot. Below them: status, setup buttons, **Send Test
  Turn** (fills both cards with a pretend Maps turn) and the raw text.
- **Arrows**: kept on the gauge like the boot logos (`data/nav/`, about 17 KB): straight on, left, right,
  slight left / right, U-turn, roundabout 1st to 5th exit (clockwise, as in the UK) and arrived. The app picks one
  from Maps' instruction ("take the 2nd exit"). For wording it doesn't know, it sends Maps' own arrow picture instead.
- **Text:** distances come in the phone's own units ("250 m", "0.2 mi"). The gauge's fonts are plain English letters,
  so the app takes accents off road names (Eglise for Église).
- **Bluetooth:** the gauge is a Bluetooth client to the OBD adapter and a server to the phone at the same time (two
  connections). While the nav link is on, Bluetooth starts at power-up. Android only: iOS doesn't let apps read
  other apps' notifications. Protocol, app build steps and limits: [docs/nav-link/NAV_LINK.md](docs/nav-link/NAV_LINK.md).

<p align="center">
  <img src="images/glass-nav.png" alt="The nav card: a right-turn arrow and 250 m on the accent band, Turn right onto Test Street, the arrival time" width="300">
</p>

### Sprites, the screensaver and Rev

Turn **Sprite** on for a page (Gauges tab) and it swaps the dial for an animated character. The first pack is **Rev**, the can2gauge mascot
([docs/mascot/MASCOT.md](docs/mascot/MASCOT.md)): a white figure whose quiff is a gauge needle and whose glow takes the
theme's colour. On an rpm page it idles, jogs, sprints and bounces off the limiter; on a boost page it inflates; on a
Haldex page it braces as the lock climbs, and LOCK slams in at 92 %. Hot or cold oil overrides any page, a fast boost
drop plays a blow-off, and a tap startles it. The ring and the number are the gauge's own.

![Rev in each mood: idle, running, sprinting, REDLINE, BOOST, Haldex LOCK, oil hot, oil cold, the screensaver and goodbye at ignition off](images/glass-rev.png)

| Mood | When it plays |
|---|---|
| Idle / Running / Sprinting | An rpm page: under 1300, 1300-4800 and over 4800 rpm |
| **REDLINE** | An rpm page over 6500 rpm: Rev bounces off the limiter |
| **BOOST** | A boost page over 1.4 bar: Rev inflates (with its own clips from 0.6 bar, and for vacuum) |
| **PSSH** | Boost falling fast from over 0.9 bar: the blow-off, on any page |
| **LOCK** | A Haldex page: Rev braces as the lock climbs, and LOCK slams in at 92 % |
| Oil hot / Oil cold | Oil over 125 °C or under 20 °C, on any page (it wins over the others) |
| Screensaver | The car still and the screen untouched for the delay |
| Goodbye | Ignition off on permanent power: Rev waves, yawns and lies down (in a numbered row, they wave to each other first) |

<p align="center">
  <img src="images/glass-rev.gif" alt="Rev on an rpm page during a pull: running with the revs, then a PSSH blow-off as the boost drops" width="300">
</p>

The **screensaver** (Settings > Logo & Sprites, off by default) brings Rev up over the gauge once the car is still and the
screen untouched for the delay (30 s), and goes at a touch, moving off, an alert or the readings moving.
**Double-tap it** and Rev wakes up for **Rev Runner**, a little Chrome-dino style game: tap to jump the cones and speed
bumps, hold to leave; the high score is kept. Any picture
sequence can become a pack with `tools/sprite_convert.py`; see [docs/SPRITES.md](docs/SPRITES.md).

Sprite frames are stored zipped and unzipped just before they're drawn, on the second core. The clip that's playing
is kept unzipped in PSRAM after its first time round, so its loops don't touch the flash. When the gauge can't keep up
with a clip's speed, it shows every 2nd (3rd, 4th) frame at an even pace rather than jumping. Diag > Drawing shows the
sprite frame rate and how long each frame takes to unzip and decode.

### Ignition on permanent power

Settings > Power & Wiring > Ignition (permanent power). With the gauge on a permanent feed, the ignition comes from **CAN terminal 15**
(VAG PQ 0x570 / 0x572, MQB 0x3C0, any bit you enter, or simply bus traffic) or from the **illumination input wired to
an ignition feed**. Ignition off plays the shutdown clip (Rev waves, yawns and lies down), fades the screen out and puts
the gauge in a standby that sends nothing on the car's bus (the shutdown sprite is Rev by default, and can be changed or
set to None in Settings > Power & Wiring; on switched power it never plays, as the power goes first); after 10 minutes it deep-sleeps until the ignition returns,
then Rev springs up with a needle sweep. In standby the output is switched off, whatever its setting, so nothing runs
from the battery with the ignition off. V3.4 still draws a few mA asleep (the CAN transceiver and regulators can't be
switched off), so check the current before leaving one on a permanent feed for weeks.

In a [daisy chain](#daisy-chain), only one gauge needs the CAN or wire ignition: set it on that gauge's page and
every other gauge switches itself to **Another gauge in the chain**, whatever it was set to before, and sleeps and
wakes with it. The last gauge given an ignition is the one they all follow, all playing their goodbye together
so the row wave lines up. Deep sleep needs the chain on CAN, as that is what wakes them; with the chain on WiFi only
they stay dark in standby.

### Night dimming

Wire the car's dash-light feed (VAG 58d, PWM or a plain dimmed 12 V, or the side lights on 58) to J_OUT pin 3. While
the lights are on, the backlight drops to the **night brightness** and the theme dims with the hue kept. On a daisy
chain only one gauge needs the wire: the master passes the night state on. When the wire is used as the ignition
(above), it no longer dims the screen.

---

## Where the data comes from

Everything that can be shown, triggered on or read in the web UI is a **signal** in one catalogue, and every
producer writes into it. A page, a trigger or a Custom-page slot simply picks a signal.

```
 car CAN ──── CANLine: VAG PQ / MQB decode, OBD-II, OpenHaldex 0x6B0, any DBC ─┐
 K-line ───── KWP1281 blocks, KWP2000 PIDs, ME7 logger, cluster oil session ──┤
 OBD dongle ─ BLE / WiFi ELM327: PIDs, VIN, UDS / TP2.0 gearbox, KWP2000 ─────┤
 OpenHaldex ─ ESP-NOW status broadcast (no CAN wire needed) ──────────────────┼──▶ signal catalogue ──▶ pages, Custom slots,
 adc2can ──── 16 × 0-5 V over CAN, scaled on the expander ────────────────────┤                          triggers, web UI,
 usb2ip ───── an aftermarket ECU over ESP-NOW: every channel ─────────────────┤                          the daisy chain
 other gauges  daisy chain over CAN or ESP-NOW ───────────────────────────────┤
 this gauge ─ dash lights, output, board temperature ─────────────────────────┘
```

### CAN

- **Classic CAN** on a TCAN1044V, 500 kbps by default (set in the web UI), RX on its own task.
- **Profiles:**
  - **VAG PQ (listen)**: rpm, pedal, torque, coolant, outside temp, speed, fuel, gear and selector, oil temperature
    (Kombi_2), the oil-pressure warning, decoded per the PQ35/46 K-matrix.
  - **VAG MQB (listen)**: rpm, speed, pedal, intake and coolant, fuel, outside temp, selector and gear, gearbox sump
    temperature, shift in progress, the clutch / torque converter lock-up and the gearbox input speed.
  - **VAG MLB (listen)**: the Audi A6 / A7 C7, A8 D4 and Q5 8R drivetrain CAN with the ZF 8HP: rpm, speed,
    pedal, coolant, fuel, outside temp, selector and gear, gearbox oil temperature, shift in progress, lock-up and input
    speed (from the opendbc `vw_mlb` K-matrix extract). On the drivetrain CAN at the gateway, not the OBD port.
  - **OBD-II**: finds the engine ECU and the PIDs it has with one `0100` to 0x7DF, then asks it directly (0x7E0) for up to
    six PIDs per request. Each request goes out the moment the last answer is complete, so it runs as fast as the ECU
    answers, and an ECU that can't take six is asked one at a time. See [OBD-II polling](#obd-ii-polling).
  - **MaxxECU (listen)**: its "Default CAN output" 1.2 / 1.3 from a configurable base id (0x520): rpm, throttle, MAP
    and boost, lambda, timing, speed, battery, intake, coolant, ethanol, EGT, gear, oil pressure and temperature, fuel
    pressure, gearbox temperature. Built from MaxxECU's published protocol.
  - **Listen only**.
- **The OpenHaldex broadcast** (0x6B0) is always decoded and its mode buttons send 0x6A0.
- **Termination**: a FET switches the 120 Ω in and out from the web UI or the screen. **Auto** (the default) listens
  at power-up: any CAN frame heard means the bus is terminated already (the car's wiring has its own), so it stays
  off. Nothing heard (gauges on their own, on the bench) and the first gauge to give up listening switches its 120 Ω on
  and says so on CAN, so the others hear a live bus and leave theirs off. If car frames turn up later it goes off
  again. **On** is for the gauge at the end of a chain with nothing else terminating it.
- **A quiet bus**: with nothing heard for 5 s, the gauge sends one single-shot "knock" frame a second (enough to wake
  a gateway that only talks when spoken to) instead of resending for ever and piling up bus errors.
- **Bus-off recovery**: if the CAN controller shuts itself off after too many errors (a wiring fault, the wrong bit
  rate), the gauge restarts it by itself.

**Any DBC.** All 108 opendbc DBCs ship on the gauge (current, generated and legacy, compressed to 165 kB), with the
six German VW / Audi files translated to English and the German names kept in the comments. The web UI **ranks the
DBCs against the IDs on the bus** to identify the car, loads the one you pick, or converts your own `.dbc` in the
browser. Once a DBC is loaded, any of its signals can drive a page (Intel / Motorola, signed and multiplexed signals
are all decoded), and it is decoded the moment its frame arrives. See [docs/DBC_library.md](docs/DBC_library.md).

**DBCs that feed the gauge** ([docs/CAN_DBC_plan.md](docs/CAN_DBC_plan.md)). A DBC can say which of the gauge's
own values each signal is (`GaugeKey`: rpm, coolant, gear ...), so loading it is all the setup: the pages, alerts,
logger and daisy chain get those values as if the gauge decoded them itself, converted to the gauge's units (kPa to
mbar and so on), with gear and selector codes read from the DBC's value labels. The library has the gauge's own DBCs
for **ECUMaster EMU Black, MaxxECU, Link (Generic Dash 1 and 2) and Haltech**, written from the makers' published
tables, plus overlays that do the same for opendbc's **VW MQB and Audi MLB** files. **Up to three DBCs at once**
(car / engine, gearbox, extra - e.g. a MaxxECU and an 8HP on one bus); two can't share an ID. An ECU whose CAN base ID
was moved in its own software (EMU Black, MaxxECU) gets its base set on its slot. With the ECU's own baro on the bus,
boost is worked out against it instead of a standard 1013 mbar. The CAN profiles (PQ, MQB, MLB, MaxxECU) stay as they
are until each DBC has been checked against them on a car.

### K-line

An L9637D on the gauge's own K-line (J_OUT pin 2, OBD pin 7), with the tester pull-up on board.

- **KWP1281**: the VAG measuring blocks. 5-baud init, identification, then the blocks, with the full VAG formula
  table. The default engine map suits a Mk4 1.8T ME7 (blocks 1, 3, 4, 115). Text blocks (ME7 block 81, the VIN) show
  as text. Reconnecting to a module takes about 6 s, and a session already open to the same module is carried on
  rather than started again.
- **KWP2000 VAG**: the same measuring blocks and gauges over KWP2000 (woken with a double slow init), which answers
  faster on modules that speak it. It drops back to KWP1281 by itself if the module won't come up.
- **K-line mode**: browse **any module** (engine, ABS, instruments, AWD ... or any address) from the screen or the
  web UI, one block at a time. In the web UI, **Find...** finds a value by name (type part of it, e.g. "boost") from
  the module's label file and goes straight to its block. Each value shows as a name / value line, and **Show
  Formula and Spec** adds the formula, its a / b bytes and the label file's specification under each one.
- **VCDS labels**: field names, block titles and specifications come from the VCDS label file for the module, picked
  the way VCDS picks it (part number and index, REDIRECTs, engine-code variants, wildcard and chassis files).
  501 plain-text `.lbl` files are packed onto the gauge; where several fit, the web UI lets you choose.
- **K-line pages**: a page can show any field of any module. The gauge keeps one module connected for all the pages
  that use it (not just the one on screen), so a swipe shows a live value at once; a Dual page reads two fields back
  to back, and Quad and Custom pages read their slots too.
- **Nothing connected**: if no module has answered since power-up, the gauge stops waking the K-line after three
  tries (each one ties the line up for about three seconds). A K-line page coming on screen, a change on the K-line
  tab, **Reconnect**, or the ignition going off and on starts it again. Once a module has answered, it keeps trying.
- **KWP2000 / OBD-II** on K-line: fast init first, and the 5-baud wake-up when that gets no answer, so ISO 9141-2 ECUs
  (most 1999-2004 VAG petrol ECUs, e.g. the 1.8T ME7.5) work too. It reads which PIDs the ECU has, then asks for them
  back to back at the standard 55 ms gap, about 7-8 answers a second. **Fast OBD** (K-line tab) shortens the gaps for
  roughly twice that, and drops back to standard by itself if the ECU starts missing answers.
- **ME7 logger**: fast logging for Bosch ME7 ECUs, which have no logging service of their own. Upload the `.ecu`
  from ME7Info (or nyetlabs/me7-logger) and a `.cfg` of variables (up to 32); the `.ecu` maps each variable name to
  its RAM address and scaling. Pick which variables to log with **checkboxes** in the ME7 logger card (Find adds any
  `.ecu` variable, untick drops one) - no hand-editing the `.cfg`. The gauge logs the values, maps well-known names (`nmot`, `tmot`,
  `pvdkds_w` ...) onto the main signals, feeds any gauge page with a `me7:<name>` source, and writes CSV logs you
  download from the web UI. Two speeds, chosen in the K-line tab:
  - **Normal** - standard KWP2000 reads (`2C` dynamically-defined local IDs, or `23` ReadMemoryByAddress as a
    fallback). The ECU answers on its own ~10 ms tick, so touching variables are grouped into spans read a few at a
    time - about **34 samples/s** for a typical 11-variable set.
  - **Fast** - writes a tiny routine into the ECU's RAM once and hooks it in, so a single request a sample returns
    the whole block: about **50 samples/s**, the configured maximum. The routine is can2gauge's own clean-room code;
    it lives only in RAM, is unhooked when logging stops and clears at ignition-off. If it won't install, logging
    carries on with Normal reads. **Test link** (a one-off choice) installs it and checks the ECU answers `C2G!`
    without logging anything, to prove it on a new ECU first. How it works and how to rebuild it is in
    [docs/me7logger.md](docs/me7logger.md).
  - The logger follows the `.ecu` file's `Communicate=` setting, so it connects to ME7.5 ECUs as well, and the K-line
    tab shows its samples a second and requests a sample.
- **ME7 flash read**: a read-only dump of a Bosch ME7's full 1 MB flash to a `.bin` over K-line, ported from the
  NefMoto flasher's read path. Nothing is ever written to the flash. A stock ECU whose flash checksums are good reads
  straight from the flash window with no login. A modified flash needs the ECU's **Permit** value (K-line tab; the
  usual ones are in its help), set in the ECU's RAM only, which clears at ignition-off. When the gauge has no room to
  save the dump, it keeps it in memory and the download comes from there. Read with the engine off, the ignition on
  and a charger on the battery, and cycle the key afterwards.
- **Fault codes and basic settings** (Service tab): read and clear any module's faults, and run a basic setting
  such as the throttle body adaptation. See [Data logger and service](#data-logger-and-service).
- **One owner at a time**: the oil page, the K-line screen, the Service tab and the configured protocol share the
  line, and each session closes cleanly before the next starts.

### OBD dongles

A Bluetooth LE or WiFi **ELM327-type** adapter in the OBD port gives the gauge the car's data with no wiring to
the car's CAN at all. See [OBD dongle support](#obd-dongle-support) for which adapters and what each can do.

- **Auto-connect** at power-up, during the boot logo, so OBD pages have values as soon as the logo clears (or only
  connect while an OBD screen or page is showing). Pairing an adapter turns auto-connect on.
- **Bluetooth and WiFi share the radio.** While it looks for a missing adapter, Bluetooth leaves most of the radio to
  WiFi and only looks every 30 s, so the web UI stays quick. Bluetooth has to start before WiFi takes its memory, so
  switching Bluetooth OBD on after the gauge has started asks for a restart (**Restart Now**). After a scan with no
  adapter in use, Bluetooth switches off again.
- **Adapter Source** (Bluetooth OBD tab): **OBD PIDs / DSG as set** (the usual), **VAG K-line measuring blocks**
  (the engine's blocks through the adapter's K-line; KWP2000 modules only), or **CAN broadcast into the gauges**: the
  adapter just listens to the car's CAN frames and the gauge decodes them with the CAN tab's profile or DBC, for cars
  that broadcast on CAN but don't answer OBD on CAN.
- **11-bit and 29-bit CAN**: on a car that answers OBD on 29-bit CAN (protocol 7, e.g. the 2025 Tiguan) the
  gauge learns the engine's address from its first answer and asks it there (`18 DA 01 F1` on the Tiguan); 11-bit cars
  stay on `7E0`. The DSG / gearbox read through the adapter follows it (`18 DA 02 F1` on the Tiguan, the gearbox's
  address taken from its own answer).
- **VIN auto-ID**: make, model (VW-group model code, e.g. 1K Golf V) and **Match car** theme. The dongle is one of
  four places the VIN comes from: see [Your car](#your-car).
- **Standard PIDs**, up to six per request on CAN, picked and decoded the same way as the wired OBD profile.
  **Fast OBD** (Bluetooth OBD tab) makes the adapter hand each answer back without waiting for other ECUs and uses its
  quicker adaptive timing; either part turns itself off if answers come back short. On a K-line car (ISO 9141-2 /
  KWP2000) the "one answer" part is always on: it is what sets the speed there.
- **Speed on a K-line car** (bench: ME7.5 8N0906018J on ISO 9141-2, ELM327 v2.3 BLE clone): 5.4 requests a second,
  and the value on the screen about 4 times a second - the page on screen gets three requests in four. ISO 9141-2 itself caps it there (10.4 kbaud and a 55 ms gap after every answer the adapter can't
  shorten, ~180 ms a request); one PID per request, the ECU refuses more. The gauge's own K-line does 8-15 a second
  on the same ECU. On CAN a dongle asks for up to six PIDs at once.
- **Steady link**: writes without response where the adapter allows, a 7.5 ms connection interval and a 4 s
  supervision timeout. Reading or clearing codes on a K-line car doesn't reset the adapter afterwards (that would wake
  the ECU while it still held the session and drop the link), and a K-line protocol gets a second try after 5.5 s.
- **Read DTCs / Clear DTCs** on the Bluetooth OBD tab, through the adapter: the OBD codes of every emissions ECU,
  and the engine's and gearbox's own UDS fault codes, on 11-bit cars and on 29-bit ones (a 2025 Tiguan: `18 DA 01 F1`
  and `18 DA 02 F1`).
- **UDS to any module** for the [VCDS tab](#vcds-style-measuring-values-uds--tp20) and the [Service tab](#service):
  measuring values, fault codes and the module scan go through the adapter, with no wire to the car's CAN. The adapter
  is pointed at the module (`ATSH`, `ATCRA`, `ATCP` for a 29-bit id's first byte) and, when the module uses the other
  id length than the car's OBD, switches between 11-bit and 29-bit CAN for it: an MQB Evo car answers OBD on 29-bit,
  but most of its modules (gateway, cluster, ABS, AWD ...) on 11-bit. Its own OBD values carry on in between, taking
  turns with the module's requests. BMW-style extended addressing works too (`ATCEA`), where the adapter has it.
- **Raw terminal** in the web UI (AT / OBD commands, 40-line log).
- **TP2.0 through the adapter** (raw CAN mode, `ATCAF0`): a PQ35 DSG or any TP2.0 module can be read with no wire to
  the car's CAN.
- **VAG KWP2000 over the adapter's K-line**: the engine blocks of Mk4-era cars into the same gauges.
- **Bench tests**: probe the adapter, check raw CAN, read and clear DTCs, sweep the TP2.0 timing, test the K-line,
  and time 15 requests per adapter timing choice.
  Each writes a full log to the gauge (`/logs/elm_<test>_NNN.txt`); `tools/elm_bench.py` runs them from a laptop.

### OBD-II polling

The wired CAN, the gauge's own K-line and the OBD dongle share one PID picker:

- **Every standard OBD value up to PID 0x60** (80 of them: fuel trims, rail pressure, catalyst temperatures, O2
  sensors, pedal, ethanol, fuel rate ...) can go on a page and shows in the live list, by any route (adapter, wired
  CAN, K-line). The less common ones are only asked for while a page or the web UI wants them, so the usual gauges
  stay as quick.
- Only the PIDs the ECU says it has are asked for.
- **Fast** PIDs (rpm, boost, speed, throttle, load, timing, lambda, plus anything on the page on screen) are asked for
  every time; the rest take turns in one slow place, so other gauges in a chain still get everything.
- On CAN up to six PIDs go in one request (five fast + one slow); on K-line it's one per request, a slow one every fourth.
  With a page on the screen its PIDs get three K-line requests in four and everything else shares the fourth,
  so the value you're looking at is the fresh one.
- **OBD-II over CAN** (CAN tab) asks the engine for PIDs on 0x7DF while the profile keeps listening to the car's own
  frames, for the values those frames don't carry (intake air, timing, MAF, load ...). The OBD-II profile always asks.
- **Which ECU**: every ECU that speaks OBD-II answers the first request (0x7E8 the engine, 0x7E9 the gearbox, others
  by car). The CAN tab lists the ones heard and reads the one picked (the engine by default).

| Path | How it runs | Where to see the rate |
|---|---|---|
| Wired CAN (profile OBD-II, or **OBD-II over CAN** next to a VAG PQ / MQB / MaxxECU profile) | Next request the moment the answer is complete; six PIDs per request, ISO-TP flow control for the long answers | CAN tab > OBD-II over CAN and Statistics > OBD PIDs, menu > CAN |
| Gauge K-line (KWP2000) | 55 ms gap after each answer (Fast OBD: 10 ms) | K-line tab > Samples /s |
| OBD dongle | Next request as soon as the adapter answers; battery voltage read every 10 s. K-line car: ~5.4 /s through an ELM327 clone | Bluetooth OBD tab > req/s |

### OpenHaldex

can2gauge started life as the OpenHaldex companion display.

- **Over CAN**: the 0x6B0 broadcast gives lock target / actual, mode, speed and pedal; the mode buttons send 0x6A0.
- **Over WiFi (ESP-NOW)**: OpenHaldex V9 with *Gauges over WiFi* broadcasts its full state ten times a second. Every
  gauge in range hears it with **no pairing and no CAN wire**, and the gauge's own WiFi keeps working. Over WiFi the
  gauge also gets the generation, force modes, brake / handbrake, temperature protection, wheel slip, learn progress,
  clutch / oil temperature and supply voltage, and can fill in speed / rpm / boost / pedal when nothing else provides
  them.
- **Control**: change mode by swiping the Haldex screen or from the web UI (over CAN when OpenHaldex is heard there,
  otherwise over WiFi). The web UI's Haldex tab can also set the generation, switch the controller on / off and run
  Learn, Long learn or Clear, with each command signed with the OpenHaldex's WiFi password and refused by the
  controller unless the car is stopped.

### Gearbox (DSG and ZF 8HP)

- **Selector and gear** come from the passive CAN broadcast (PQ: the cluster's gear display, the TCU, the lever;
  MQB: Getriebe_11; MLB: Getriebe_03), with no requests sent. MQB and MLB also give shift in progress, the torque
  converter lock-up (the clutch on a DSG) and the gearbox input speed, and the gauge works out the converter slip.
- **Gearbox type**: DSG, **ZF 8HP in an Audi / VW**, or **ZF 8HP in a BMW** (F / G-series). Picking one fills
  in the diagnostic reads for it, all still editable:
  - Audi / VW 8HP: module 02 on 0x7E1 / 0x7E9, ATF temperature (22 2104), gear (22 210F) and lock-up (22 7078).
  - BMW 8HP: the EGS (0x18) with BMW's **extended addressing** (its address in front of every frame, on 0x6F1 / 0x618),
    ATF temperature (22 DA12), lock-up (22 DA22) and the selector with Sport (22 DA2E). BMW has no gear read, so the
    gear isn't shown. It works on the OBD port's CAN, or through the Bluetooth adapter (which does the addressing
    itself with `ATCEA`, put back after each pass so engine polling carries on).
- **Diagnostic reads** are optional and active, by any of four routes: **UDS on CAN** (every field editable), **TP2.0
  on CAN** (PQ35 DQ250, sequence from a VCDS capture), **UDS via the dongle**, or **TP2.0 via the dongle** in raw CAN
  mode. Up to five reads: speed, gear, oil temperature (every 10th pass), lock-up and selector.
- The web UI's Gearbox tab shows only the settings of the read in use, the raw fields and raw lock-up / selector
  numbers (for working out another gearbox's codes), and a gear mimic like the gauge's own.

### VCDS-style measuring values (UDS / TP2.0)

- Up to **12 values from one module** (engine, gearbox, AWD, ...), read the way **VCDS** (Ross-Tech) reads its
  advanced measuring values: **UDS** (`22` + DID) on MQB-era modules, **KWP2000 measuring blocks** over **TP2.0** on
  PQ-era modules. Auto finds which one the module talks.
- **Connection**: the gauge's own CAN wire, or the **OBD dongle** (UDS only; TP2.0 needs the wire). Through the dongle
  Turbo groups at most 3 values a request (an ELM327 sends one CAN frame at a time).
- **Any module, any id**: pick a VW address (the gauge knows the UDS ids of 16 of them) or type the request and answer
  ids, **11-bit** (`7E0`) or **29-bit** (`17FC0076`, VW's own ids for an MQB Evo engine; `18DA01F1`, the OBD-II ones),
  with **extended addressing** for BMW (out `18`, back `F1` for the EGS).
- **Any run of bits**: a value is the bits from **Bit** for **Bits** long in the DID's data (byte 2 = bit 16), signed or
  not, high or low byte first, then x scale + offset. Flags and values that don't start on a byte read right.
- **Turbo** asks again as soon as an answer is in and packs several UDS DIDs into one request, like VCDS's "Turbo" and
  "Group UDS requests" options.
- The tab shows the module's part number, name and **ASAM dataset**, each value with its raw bytes, the answers per
  second, and a **Try a DID** box. Any value can go on a gauge page (Source = VAG diag).
- **DID Library**: the values [OBDb](https://github.com/OBDb) lists for each car group, ready to add. Pick the car
  group (the one the dongle's VIN points to comes up first), the module and type a few words (oil temp, clutch,
  boost ...); **Add** puts the value in the first free slot with its DID, bits, scaling and unit filled in, and moves
  the tab to that value's module if it was on another one.
  - 21 car groups: the **VW group** (VW, Audi, Skoda, SEAT, Cupra, Porsche, Bentley, Lamborghini: 1,977 DIDs, 3,555
    values, 3,163 of them confirmed on a real car), Stellantis, Renault / Dacia, Mazda, Hyundai / Kia, Toyota / Lexus,
    Volvo, Nissan, GM, Ford, Honda, BMW / MINI, Mercedes, Subaru, MG, JLR, Suzuki, BYD, Haval, Mitsubishi and Aiways.
    About 180 KB on the gauge, zipped.
  - **Confirmed Only** (on by default) hides the values OBDb is still trying out, which may read wrong.
  - The engine and gearbox lists are long because they cover every engine in the group. A DID your engine doesn't
    have is refused (NRC 31) and the tab stops asking for it.
  - Rebuilt from OBDb with `tools/vdid/obdb_to_c2g.py` whenever OBDb grows.
- Anything OBDb doesn't have: type the DID and its layout in, from your own captures (VCDS, OBDeleven, a CAN sniffer)
  and **Try a DID**. See `docs/VCDS_protocols.md`.

### OEM oil temperature and level

Mk4-generation VAG cars with the **G266** sump sender (cluster coding, 4th digit 1 or 2) have oil temperature and
level that only the cluster decodes. The **Oil (PQ)** page reads it two ways:

- **CAN (passive)**: Kombi_2 `0x420`, temperature only.
- **K-line**: a KWP1281 session to the cluster (0x17) for **temperature and level**. Block 003 or 050 is picked from
  the cluster's part number. The session is open only while the oil page is on screen and closes cleanly 3 s after
  you leave it.

The web UI's Oil tab picks the source, shows the live and raw fields, and explains which cars have the sender.

### ADC: the adc2can expander

The V3.4 board has no analog inputs of its own. Instead, up to **two adc2can boards** (8 × 0-5 V each, 16 sensors) sit
on the CAN chain. With adc2can firmware 1.1 every channel's scaling comes across the bus by itself, and the gauge's ADC
tab edits it in place (sent over CAN, saved on the adc2can). Pick an ADC sensor for a page and its range, redline,
decimals and name come from its scaling. A sensor can also be scaled on the gauge only (presets for 0.5-4.5 V pressure
senders, 3 bar MAP, wideband AFR / lambda and EGT amplifiers).

**No CAN wire needed (adc2can firmware 1.3):** the adc2can also broadcasts everything it sends on CAN over WiFi
(ESP-NOW, no pairing), so a gauge with just power shows its sensors, names and units. Over WiFi it's one-way: the
adc2can's settings show on the ADC tab (in full after about 25 s) but changing them needs CAN or the adc2can's own web
UI. Both must be on the same WiFi channel (both start on 1); a Home WiFi bridge or a WiFi OBD adapter on the gauge moves
its channel. A board that's also on CAN uses CAN.

### ECU over WiFi (usb2ip)

**usb2ip** is Forbes Automotive's wireless USB bridge for an **Ignitron**, **MaxxECU**, **Link** (G4X / G5),
**Haltech** (Elite / Nexus) or **ECUMaster EMU PRO** (through ECUMaster's USBtoCAN adapter). From usb2ip 2.0 it also
broadcasts the ECU's data over WiFi (ESP-NOW, no pairing), so a gauge with just power shows the engine's values with no
CAN wire, OBD dongle or K-line.

- **Everyday values** (20 a second): rpm, speed, coolant, intake, MAP / boost, pedal, throttle, load, lambda, timing,
  battery, oil temperature and pressure, fuel pressure, EGT, ethanol and gear fill the gauge's own signals whenever
  nothing else on the gauge (its CAN decode, OBD or K-line) provides them, so every page and trigger that uses them just
  works. Knock retard, injector duty and lambda target are new signals of their own.
- **Every channel of the ECU** (10 times a second): all 380 Ignitron, 596 MaxxECU, 831 Link, 936 Haltech or 1,409
  EMU PRO channels. On the Gauges tab, pick
  Source = **ECU over WiFi** and choose any of them from a list grouped as Engine, Air & Fuel, Temperatures,
  Electrical, Vehicle and Diagnostics, with a Find box. The page takes its name, unit, range and decimals from the
  channel.
- **The ECU tab** shows which usb2ip is followed (the first one heard; others in range can be picked),
  the ECU it found, signal strength, packets lost in the air, how many channels are arriving, and the everyday values
  with the ones another source provides greyed out. Both start on WiFi channel 1; a Home WiFi bridge or a WiFi OBD
  adapter moves the gauge's.

> [!NOTE]
> The gauge decodes the channels with its own copy of usb2ip's channel list. If the card says the lists differ, one of
> the two is older: update can2gauge and usb2ip together. The everyday values keep working meanwhile.

> [!WARNING]
> usb2ip's Link, Haltech and EMU PRO support is new and not yet tested on a car. On an EMU PRO, which channel is which
> is still a best guess until it has been checked against EMU PRO's own values: treat its channel names as unconfirmed.

### Daisy chain

Several gauges can share one set of sources. J_BUS1 and J_BUS2 are wired straight through, so the gauges chain on
the same 4-wire harness.

- The chain is on from first boot, over ESP-NOW only, so a new gauge finds the others by itself.
- Node IDs are automatic; a **master** is elected (or forced) and is the only gauge that polls the car, reads the
  gearbox or holds the dongle.
- **Gauge Numbers**: from any gauge's Daisy Chain tab, give every gauge its number (its place in the row,
  1 = leftmost as you sit in the car) and its role (Auto, Master or Follower). Only one gauge can be set to Master.
  **Show Numbers** puts each gauge's number big on its own screen for 15 s, and a gauge whose number or role is
  changed shows its new one for a few seconds, so you can see which gauge is which while you number them.
- **Row wave**: at ignition off, numbered gauges with a shutdown sprite say goodbye along the row. The first two turn
  to each other and wave, then the second and third, and so on while the others watch; then they all wave to the
  driver and lie down together. **Play It Now** plays it on every numbered gauge, and says which ones sit out because
  they have no shutdown sprite. Rev is every gauge's shutdown sprite from new, so a fresh row waves straight away.
- Values are shared over **CAN** (two signals a frame, off by default until you allow link frames on the car's bus)
  or **ESP-NOW** (one broadcast every 50 ms).
- **Follow page** and **follow night**: the followers swipe with the master and dim with it.
- **Shared settings**: the ignition and shutdown (off delay, deep sleep, shutdown sprite), the screensaver, the look
  (theme and colours, day and night brightness, gauge feel, units) and the boot logo are the same on every gauge in
  the chain. Change one on any gauge's web page and the rest follow within a second or two; a gauge that was asleep
  or out of range catches up when it's next heard. Only the gauge with the ignition wire (or terminal 15) needs an
  ignition source: the others take theirs from it by themselves, and giving the ignition to a different gauge moves
  them all over to that one. What each page shows, the number, name, role,
  rotation, WiFi, outputs and diagnostics stay per gauge.
- **Boot logos in step**: the gauges agree over CAN at power-up and light up and sweep together (see
  [Boot logos](#boot-logos)).
- The **Gauge Numbers** list shows the end of each gauge's MAC address (the same four characters as on the end of
  its WiFi name, `can2gauge-XXXX`) and which one has the ignition, to tell them apart.
- Wireless commands and chain updates are signed with a key built into every can2gauge, so there is nothing to set up: gauges in the same group work together.
- **Update Other Gauges**: update one gauge as usual, then it sends its firmware and web pages on to the
  others over ESP-NOW, one gauge at a time (see [Updating](#updating)).

<p align="center">
  <img src="images/glass-number.png" alt="Show Numbers: the gauge's number in the row, 2, big on its screen with its name and role" width="300">
  &nbsp;
  <img src="images/ui-chain.png" alt="Daisy Chain: Gauge Numbers, each gauge's number and role set from one gauge" width="260">
</p>

---

## Data logger and service

### Data logger

The **Log** tab records up to **16 values** side by side, **1, 2, 5, 10, 20 or 50 times a second**. A value can be
anything the gauge has: CAN, K-line, OBD, VCDS, ME7, the adc2can sensors, usb2ip, or a gauge page's own value.

- **Channels**: **Active Gauges** (the default) records the values your gauge pages show, in gauge order (gauge 1
  first), so whatever is on the screen is logged without picking anything - an ME7, K-line or OBD value a page shows is
  logged by name, and the graphs come out in gauge order. **Chosen Below** instead records the 16 channels you pick.
- **Start Recording** / **Stop Recording**, or **Auto Start** from power-up.
- The recording lives in the gauge's memory (PSRAM, up to 4 MB). That is about 1 h 40 min of 16 values at 10 a
  second, or longer with fewer. When it's full, the oldest samples are written over.
- **Recording memory**: a bar and a line under the buttons show the room before you start (how long the channels and
  rate picked will fit), then how much is used and how long is left while it records.
- A new recording (Start after changing the channels) draws its graphs again with the new channels.
- **Nothing is saved on the gauge**: a power cut loses the recording, so download it first.
- **Graphs**: one per value, all on one time line, live while it records. Zoom and pan with the buttons (or drag
  with a mouse); every graph shows its value at the cursor and its min / average / max for what's on screen. A gap
  means that value's source wasn't sending.
- **Download CSV** saves the whole recording to the phone. It opens in Excel, MegaLogViewer or Datazap.
- The viewer is shared by the Forbes Automotive products (`_SharedTheme/logview.js`, drawn with uPlot).

### Service

The **Service** tab talks to the car's modules straight from the gauge's own wiring, or through the OBD dongle.

- **Read Faults** / **Clear Faults** for any module (engine, gearbox, ABS, instruments, AWD ... or any address):
  - K-line tries KWP1281 (Mk4 era), then KWP2000.
  - CAN tries UDS (MQB era), then KWP2000 over TP2.0 (PQ era).
  - The OBD dongle speaks UDS (MQB era). On a car that answers OBD on 29-bit CAN, the engine and gearbox are asked
    at the 29-bit ids they answered OBD on.
  - Fault numbers show as VCDS shows them. A clear is followed by a read, so a fault that is still there shows
    straight away.
- **Scan Modules**, like a VCDS auto-scan: every module the gauge knows the UDS ids of, then the 29-bit engine and
  gearbox ids (OBD-II's `18 DA xx F1` and VW's `17 FC 00 xx`), each asked for its part number, its own name and how
  many faults it has stored. Over CAN or the dongle; it only reads. **Read Faults** on a row reads that module. PQ-era
  modules speak TP2.0 and don't show up: read those one by one over CAN.
- **Throttle body adaptation**: basic setting group **060** (drive-by-wire, ME7 and later) or **098** (cable
  throttle), or any group, over the K-line.
  - The ECU learns the throttle's end stops itself while the gauge shows the group's fields live, named from the
    VCDS label file.
  - It refuses to start with the engine running. It stops after 60 s, if the engine starts or the car moves, or
    when the page closes.

> [!WARNING]
> Ignition on, engine off, foot off the pedal for an adaptation. Clearing an engine's faults also resets its emissions
> readiness monitors.

<p align="center">
  <img src="images/ui-log.png" alt="Log: recording controls and the graphs of a pull, RPM, speed and boost on one time line" width="260">
  &nbsp;
  <img src="images/ui-log-channels.png" alt="Log: the 16 channel pickers, samples a second and Auto Start" width="260">
  &nbsp;
  <img src="images/ui-service.png" alt="Service: fault codes read from the engine over the K-line, and the throttle body adaptation card" width="260">
</p>

---

## OBD dongle support

The ESP32-S3 has **Bluetooth LE only, not Bluetooth Classic**. A Bluetooth dongle must therefore be a BLE ("iPhone
compatible") adapter; SPP-only "Android" adapters and the OBDLink MX+ / LX can't be used. **WiFi adapters** work
too: the gauge joins the adapter's network as a station while keeping its own WiFi up, so the web UI stays
reachable.

| Adapter | Link | Notes |
|---|---|---|
| **Vgate iCar Pro BLE 4.0** (fw 4.1.x) | BLE (FFE0 on v1/v2, FFF0 on v3) | The adapter the dongle support was written against. Raw CAN, TP2.0 and K-line paths are built for its ELM v2 command set |
| **OBDLink CX** | BLE (FFF0) | STN chip, the recommended reference adapter |
| **Veepeak OBDCheck BLE / BLE+** | BLE (FFF0 or FFE0) | |
| **vLinker MC+ / BM+ / FD** | BLE (18F0 / 2AF0, or the Vgate "iPhone" service) | "FD" means Ford MS-CAN, not CAN FD |
| Generic BLE ELM327 clones | BLE (usually FFF0 / FFE0) | Slow (5-10 PIDs/s) and sometimes flaky, but fine for two or three gauges. A v2.3 clone held 5.4 requests/s on an ISO 9141-2 ME7.5 for a full bench session with no drop-outs; it can't wake VAG module addresses (0x01 / 0x11) or do a fast init |
| Any **WiFi ELM327** | WiFi (usually 192.168.0.10:35000) | Joins the adapter's AP; the gauge's AP moves to its channel, and phones reconnect after a few seconds |

**How the gauge finds a BLE adapter:** it searches for the FFF0, FFE0, 18F0 and Vgate E781... services, then any
custom service with one notify and one write characteristic, and picks the RX / TX characteristics by their
properties rather than by UUID. The adapter's MAC is saved once paired.

**What a dongle can and can't do:**

| Protocol | Through a dongle | On the gauge's own wiring |
|---|---|---|
| OBD-II mode 01 PIDs, VIN | Yes | Yes (CAN OBD profile; KWP2000 on K-line) |
| UDS reads (e.g. DSG speed, any module's measuring values) | Yes, 11-bit and 29-bit | Yes (CAN) |
| VW TP2.0 (PQ35 gearbox etc.) | Yes, if the adapter does raw CAN (`ATCAF0`) | Yes (CAN) |
| VAG KWP2000 on K-line | Yes (engine blocks) | Yes |
| **VAG KWP1281** (Mk4 measuring blocks, cluster oil level) | **No.** KWP1281 needs every byte echoed inverted, which no ELM-type adapter can do | Yes, gauge K-line only |
| ME7 logger / flash read | No | Yes, gauge K-line only |
| Read / clear fault codes | Yes (Service tab, UDS modules; Bluetooth OBD tab > bench tests) | Yes (Service tab: K-line and CAN) |
| Module scan | Yes (UDS modules) | Yes (CAN, UDS modules) |
| Basic settings (throttle body adaptation) | No | Yes, gauge K-line (KWP1281 modules) |

Run the **bench tests** (Bluetooth OBD tab) with any new adapter. They show whether it does raw CAN at all, the
`ATST` / `ATAT` timing TP2.0 needs, and which protocol a K-line module speaks.

---

## Customising it

Almost everything is set in the web UI and saved on the gauge. Settings > Device downloads them as a backup file
and imports one again.

- **Pages** (Gauges tab): up to 8 pages in swipe order. For each one you set the style, **source** (CAN / K-line /
  OBD / ADC / ECU over WiFi / this gauge), the value(s), label, range, redline and decimals, plus the Quad and Custom slots. The
  screen rebuilds live as you change them.
- **Any signal from any source**: a gauge-decoded signal, `dbc:<id>:<signal>` from the loaded DBC,
  `kl:<addr>:<block>:<field>` from a K-line module, `pid:<n>` from the OBD dongle, `ecu:<kind>:<channel>` from usb2ip,
  or an ADC channel.
- **Triggers**: 8 set-points on any signal, each with above / below, threshold, hysteresis, hold time and latch,
  raising an alert, switching the output, or both. **No Reading** says what a trigger does when its value has none (a
  sensor unplugged, shorted or faulted, the bus gone quiet): **Ignore** waits for a reading, **Trigger** counts it as
  the condition met, so a low-pressure alarm also goes off when its sender fails (from 5 s after power-on, so the
  values have time to arrive). The output follows triggers while its control is **Auto**. Picking a signal sets a sensible threshold for it (its redline,
  or three quarters of its usual range) with the right step: 0.01 for lambda, whole numbers for rpm. Gear and
  Haldex mode pick from a list (gear 1-7), and on / off values read "is On" / "is Low" instead of a number.
  The **Signal** list is grouped by where the value comes from (Engine and car, Gearbox, Page values, ADC, ECU, Haldex,
  This gauge) and only offers what the gauge has: a page's own value (whatever a page reads from a DBC, K-line, OBD,
  usb2ip, VCDS or ME7, listed as e.g. "Page 3: Boost (K-line)") once a page has one set up, and the adc2can, usb2ip
  and OpenHaldex values once they've been heard. A switched-on trigger keeps its value read even when its page isn't
  showing or is switched off, so it still goes off. K-line is one wire to one module at a time: when the pages and
  triggers use more than one module, a trigger's K-line value is read only while a page on the same module is showing.
- **The output**: high-side (load to ground) or low-side (load to 12 V), with manual Off / On or Auto (triggers).
  "Output: On / Off / Fault": the dot is green while it's switched on, red while it's off, and flashes orange when the
  driver has shut it down (short circuit, overload or too hot; it retries by itself). With the ignition on permanent power, the output is held
  off while the ignition is off (standby), even when set to On, and the card says so.
- **Themes**: pick any of the 71, set your own dial and needle colours, or let Match car choose from the VIN.
- **Boot logos**: upload any image (converted to C2GL in the browser), or batch-convert SVG / PNG with
  `tools/logo_convert.py`.
- **Menu and Tabs**: hide what your car doesn't have (K-line, Gearbox ...); the on-screen menu entry, the web UI tab and the
  dashboard tile go together. First Boot sets them from what it finds; Settings > Device > Menu and Tabs brings any back.
- **Units per quantity**: speed (km/h / mph), boost (bar / psi / kPa), other pressures (bar / psi / kPa),
  temperature (°C / °F), mixture (lambda / AFR) and distance (km / miles). **Metric** or **Imperial** sets them all
  (lambda stays lambda); **Custom** picks each, e.g. mph with bar. The dials follow: the needle, arc, bar, redline and
  scale all work in the shown unit, and changing a unit turns every gauge's Min / Max / Redline over and rounds them
  (280 km/h becomes 175 mph; switching back and forth can move an end by one step). Logs and triggers stay in the
  gauge's own units. The screen's Units button switches Metric / Imperial.
- **Brightness, night brightness, swipe animation, boot fade, long-press time**, WiFi name and password.
- **Your own DBC** (converted in the browser) and your own **VCDS label choice** per module.
- **Your own signals** in firmware: add one enum entry and one table row in `src/can2gauge_signals.cpp`, and every
  dropdown in the web UI picks it up. Append new IDs only, because the enum order is the daisy chain's wire format.

---

## Web interface

Join the gauge's WiFi, **`can2gauge-XXXX`** (the last four hex digits of its MAC; no password until you set one on
the Settings tab; a password has to be at least 8 characters), and open **`http://192.168.1.1`** (the same address as
every Forbes Automotive product) or **`http://c2g.local`**. Changes save automatically. The AP hands out no gateway, so
a phone keeps its mobile data while connected.

The UI uses the shared Forbes Automotive theme, and the same pages work on a phone and on a laptop. On a phone the tab
row becomes a menu button with the tabs in groups (Live, Car data, Set up, Tools), every dropdown gets its own line,
number boxes and lone buttons stretch to the width, and the status badges become coloured dots that open out when
tapped. On a laptop the fields sit side by side. Live states read as short "Name: value" lines with a coloured dot
("Output: On", "Recording: On"), and button text never wraps.

> [!NOTE]
> Some Android phones don't look up `.local` names while they're also on mobile data: use `http://192.168.1.1` there.

### Basic and Advanced

The web UI opens in **Basic**: each card shows the settings most people change, and the fine-tuning (ranges and
redlines, ID numbers, delays, animation timings, backup import, factory reset ...) sits behind a **More Options** link at
the bottom of the card. A card's help text is behind the **(i)** button on its title. The **Advanced** switch (at the
bottom of the phone menu, and on Settings > Device) opens every More Options at once, and also shows the tool tabs (**VCDS**,
**Diag**) and the inspection cards (CAN: IDs on the bus, statistics, recent frames; OBD: terminal; ME7: read ECU flash).
The browser remembers the choice; nothing on the gauge changes with it.

- **Gauges**: one line per gauge (number, label, style, value and range) with its on / off switch. Tap a line to open
  that gauge (one at a time): style, source, label and the value; picking a value fills in its range and decimals, and
  More Options holds min / max, redline, decimals and the sprite. **Move Up** / **Move Down** change the swipe order.
- **Settings** is in five sections, picked with the chips along the top and shown one at a time: **Display**, **Logo &
  Sprites**, **Power & Wiring** (output, illumination, ignition), **WiFi** and **Device** (menu and tabs, Advanced,
  reboot, backup, First Boot Setup).

<p align="center">
  <img src="images/ui-menu.png" alt="The phone menu: the tabs in groups, the Advanced switch at the bottom" width="260">
  &nbsp;
  <img src="images/ui-gauges.png" alt="Gauges: one line per gauge, the first one open with style, source, label and value" width="260">
  &nbsp;
  <img src="images/ui-settings.png" alt="Settings, Device section: the menu and tabs switches, the Advanced switch and the device buttons" width="260">
</p>

### First Boot Setup

A new gauge (or one after a factory reset) asks on the screen, after the boot logo: **Set this gauge up from your
phone?** **Yes** shows the WiFi name, the password (if any) and the address to open; **Not Now** doesn't ask again.
A screen without touch shows the WiFi name and address straight away. When the web UI opens, its **First Boot** page
comes up by itself (every time, until **Apply** or **Skip** finishes it) and runs four steps:

1. **What is connected**: a tick or a cross for the wired CAN (and which car: VAG PQ, VAG MQB or MaxxECU, plus the
   closest DBC), OBD-II over CAN (the gauge asks the engine for a few seconds), CAN over WiFi, OpenHaldex, adc2can, usb2ip, the K-line (it wakes engine module 01), a Bluetooth OBD
   adapter (a 5 s scan), a WiFi OBD adapter and other gauges, with every live value seen. Have the ignition on.
2. **Suggested setup**: one row per setting with the suggestion picked and why. Everything it found is set up with
   its defaults: the CAN profile and the **car's DBC** (the closest match to the IDs on the bus, loaded onto the gauge),
   OBD-II over CAN and **which ECU to read** (every ECU that answered is listed), KWP1281 on the K-line when the engine
   answered, the oil temperature from CAN or the cluster, the daisy chain when other gauges are heard; then CAN
   termination, the OBD adapter, K-line logging, OpenHaldex and usb2ip listening, how the gauge is powered, what dims it (the illumination
   input's live state is shown, to check it with the headlights), units, colours (Match car once a VIN is read),
   boot logo, sprites, the screensaver, and the **menu and tabs**: CAN, Haldex, ADC, ECU, K-line and Bluetooth OBD
   when found, Oil when CAN or the K-line is, DSG when CAN is, Daisy Chain when other gauges are heard.
3. **Gauges**: the gauges to show, ticked when their value was seen live (adc2can sensors under their own names),
   up to 8, in the order set with the arrows (Move Up / Move Down on the Gauges tab changes it later).
4. **Your car**: the car the VIN says it is, with its picture: check it, change what's wrong, pick the paint and
   press **This Is My Car** (see [Your car](#your-car)). With no VIN, pick the car by hand.

Nothing changes until **Apply**; **Skip** keeps the current settings. A gauge that was already set up before this
arrived is never asked. **Settings > Device > First Boot Setup** runs it again, and sets the tabs again from what it
finds then.

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
  <img src="images/ui-firstboot-pages.png" alt="First Boot: the gauges to show, live values ticked, in order" width="260">
</p>

### Your car

Step 4 of First Boot and **Settings > Device > Your Car** show which car the gauge is in.

- **The VIN** comes from whichever source has it:
  - the **OBD adapter** (OBD `09 02`, else UDS `22 F190` on the engine);
  - the **CAN broadcast**: MQB, MQB Evo and MLB gateways send the VIN by themselves (`VIN_01`, 0x6B4) on the
    drivetrain CAN, so the gauge just listens;
  - **OBD over the gauge's CAN**: First Boot (and **Detect Again**) ask the engine once, `22 F190` then `09 02`, on
    `7E0` and on the 29-bit `18DA01F1` / `18DA10F1`;
  - the gauge's **K-line**: on an ME7 the engine's measuring block 081 is the VIN and the immobiliser ID, and First
    Boot shows it for a moment while it has the engine awake.
- **What it says**: the make from the VIN's first three characters, and on VW-group cars the model generation
  (characters 7-8, e.g. `1J` Golf IV, `AD` Tiguan II) and the model year.
- **The picture**: a drawing of the car's body type (hatchback, saloon, estate, SUV, coupe, convertible, MPV, van,
  pickup; the VW-group models in the VIN table know theirs), in the paint you pick (twelve common colours or any
  other). A drawing of the model itself can be added one model at a time (`data/cars/index.json`).
- **This Is My Car** saves it (make, model, year, body, paint), whatever the VIN said. The confirmed car decides
  **Match car** (the theme and boot logo) and the DID Library's car group. It's kept apart from the settings, so a
  settings reset or a backup import doesn't lose it.

<p align="center">
  <img src="images/ui-firstboot-car.png" alt="First Boot step 4: Your car, the VIN read over the OBD adapter, a picture of the Golf IV in red, and its make, model, year and paint" width="260">
</p>

### Tabs

CAN, OpenHaldex, ADC, usb2ip, K-line / ME7, Oil, DSG, Bluetooth OBD, Triggers and Daisy Chain show only when the car has them:
First Boot sets them from what it finds, and Settings > Device > Menu and Tabs turns any of them back on. VCDS and Diag
are tools, so they show with **Advanced** on (above).

| Tab | Purpose |
|---|---|
| **Dashboard** | Haldex mode buttons and lock, a live tile per menu item (CAN, K-line, Output, DSG, ADC, OBD, chain), **Live data** that follows whatever the screen shows (adc2can sensors with their own decimals), and page controls |
| **Log** | The [data logger](#data-logger): Start / Stop, the recording memory (room before Start, used while recording), graphs on one time line, Download CSV, and the channels (**Active Gauges** or 16 picked) with the rate and Auto Start |
| **Navigation** | Google Maps turns: the nav link (Off / pops up near a turn / whole route), pop-up distance, whether the phone can see the gauge and is connected, the turn it last sent, **Show Test Turn**. Always shown, next to Log |
| **Gauges** | The gauge editor, one line per gauge (tap to open, Move Up / Move Down to change the order): style, source, values, slots, and under More Options range, redline, decimals and sprite; with Source = ECU over WiFi, every channel of the ECU usb2ip reads (Ignitron, MaxxECU, Link, Haltech or EMU PRO) with a Find box |
| **CAN** | Bus settings and termination (relay and OpenHaldex / MaxxECU IDs under More Options), **OBD-II over CAN** next to the profile, **car identification and the DBC library** with **three DBC slots** (car, gearbox, extra; a base ID for a moved ECU); in Advanced the IDs on the bus, statistics (with OBD PIDs per second) and recent frames |
| **OpenHaldex** | Live lock / target, mode buttons, state chips, the OpenHaldex controls over WiFi and which controller to follow |
| **ADC** | The adc2can boards and their 16 channels: live volts and values, scaling edited in place |
| **usb2ip** | The **ECU over WiFi (usb2ip)** link: which usb2ip is followed, the ECU it found, the channels arriving and the everyday values |
| **K-line / ME7** | Protocol and address, Fast OBD for KWP2000, the K-line mode module and live blocks with VCDS names (Find... by name, Show Formula and Spec), the ME7 logger and (Advanced) the ECU flash read |
| **Oil** | OEM oil source, live temperature and level, raw fields, which cars have the sender |
| **Gearbox** | Live selector / gear / speed, temperature, lock-up and input speed with a gear mimic, the **gearbox type** (DSG, ZF 8HP Audi / VW, ZF 8HP BMW) and the diagnostic reads |
| **VCDS** | Advanced only. Up to 12 [measuring values](#vcds-style-measuring-values-uds--tp20) from one module over CAN or the OBD dongle (UDS / TP2.0, 11 or 29-bit ids), Turbo, the module's identification, the **DID Library**, Try a DID |
| **Service** | [Fault codes](#service) read and cleared over the gauge's K-line or CAN or the OBD dongle, **Scan Modules**, and the throttle body adaptation |
| **Bluetooth OBD** | Connection (Bluetooth / WiFi), scan and pair, Fast OBD, adapter and vehicle, supported PIDs, terminal (Advanced), bench tests |
| **Triggers** | The 8 set-points, each on any signal the gauge has (grouped by source; page values from a DBC, K-line, OBD, usb2ip, VCDS or ME7 included) |
| **Daisy Chain** | Node name, group, transport, follow options; **Gauge Numbers** (every gauge's number and role, set from here, and **Show Numbers**); the row wave's **Play It Now**; the gauges heard (with their versions); and **Update Other Gauges** |
| **Settings** | Five sections, one at a time. **Display**: brightness, units, **themes with live preview**, **gauge feel: Analog / Digital**, **Screen Rotation** (Upside Down, Rotation Trim). **Logo & Sprites**: **boot logos**, **the screensaver and sprite packs**. **Power & Wiring**: the **output** (wiring, Off / On / Auto, a green / red dot, flashing orange on a fault), **illumination** (its live state, night dimming), **ignition on permanent power**. **WiFi**. **Device**: **Your Car** (the VIN, make, model, year and paint, with its picture), **Menu and Tabs**, the **Advanced** switch, reboot, settings backup download and import, **First Boot Setup**, factory reset |
| **First Boot** | Hidden until it's wanted: the [First Boot Setup](#first-boot-setup) (checks, suggested setup, gauges) |
| **Diag** | Advanced only. Live: CAN (rate, errors, listen-only), ECU over WiFi, K-line, OBD adapter, chain, WiFi, ignition and power state, illumination / dimmed, uptime, last reset, memory (free, lowest since boot, and **RAM ran out**: what the last failed memory request was and in which task, kept through a reset); **display health** (how far the panel bring-up got, frames sent to the screen, backlight, IO expander, LVGL memory) and a **touch test** (controller found, presses and the last position, look for it again); drawing (frame rate, frame time, core load, sprite decode and unzip time), the benchmark and the drawing options |
| **OTA** | Firmware and web UI versions, update from GitHub, Home WiFi bridge, update from files |

<p align="center">
  <img src="images/ui-dashboard.png" alt="Dashboard: Haldex buttons, menu tiles and the live data grid" width="260">
  &nbsp;
  <img src="images/ui-haldex.png" alt="Haldex: live lock, mode buttons and the OpenHaldex controls over WiFi" width="260">
  &nbsp;
  <img src="images/ui-display.png" alt="Settings, Display section: brightness, theme with its live preview, units, and the Screen Rotation card" width="260">
</p>

*Dashboard (left): the Haldex buttons, a live tile per menu item and the live data the screen is showing. Haldex
(middle): the live state and the OpenHaldex controls. Settings > Display (right): brightness, the theme picker and Screen Rotation.*

<details>
<summary>More web UI screenshots (CAN, ECU over WiFi, K-line, OBD, oil, DSG, ADC, triggers, output, ME7, boot logos, daisy chain, OTA, Diag, navigation)</summary>

<p align="center">
  <img src="images/ui-can.png" alt="CAN: bus settings, car identification against the DBC library, and the IDs on the bus" width="260">
  &nbsp;
  <img src="images/ui-ecu.png" alt="ECU over WiFi: the usb2ip link with an Ignitron, the channels arriving and the everyday values" width="260">
  &nbsp;
  <img src="images/ui-gauges-ecu.png" alt="Gauges: a page with Source = ECU over WiFi, knock retard on the arc and injector duty, lambda target and intake in the centre" width="260">
</p>

*CAN (left): the car identified from its bus and its DBC loaded. ECU (middle): usb2ip with an
Ignitron heard, every channel arriving. A gauge page from the ECU (right): any of its channels, found by name.*

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
  <img src="images/ui-diag.png" alt="Diag: the gauge, communication and power blocks at a glance (Advanced)" width="260">
  &nbsp;
  <img src="images/ui-chain-update.png" alt="Daisy Chain: Update Other Gauges, each gauge's version and how its update went" width="260">
</p>

<p align="center">
  <img src="images/ui-nav.png" alt="Navigation tab: the nav link, pop-up distance, the phone connected and the next turn" width="260">
  &nbsp;
  <img src="images/ui-vcds.png" alt="VCDS: a 2025 Tiguan's engine read through the Bluetooth adapter on 29-bit ids, and the DID Library searched for oil temperature" width="260">
  &nbsp;
  <img src="images/ui-service-scan.png" alt="Service: Scan Modules through the OBD adapter, each module's part number, name, ids and fault count" width="260">
</p>

</details>

---

## Hardware

### The unit

A standard **52 mm gauge**: the screen and bezel sit on the dash, the body goes through the hole, and everything plugs
into the rear cap. The renders below are made from the printable housing and the routed board (every component at its
real position and size).

<p align="center">
  <img src="images/unit-turntable.gif" alt="The assembled gauge turning: the screen, the side with the head and body, the rear cap with the plugs and the U-bracket" width="360">
</p>

![Rear of the gauge: J_BUS1, J_BUS2 and J_OUT through their windows, USB-C, BOOT and RESET pin holes, power LED, three M2 screws and the two M3 bracket studs](images/unit-rear.png)

*The rear: the three plugs through their cap windows, USB-C for flashing and the console, the BOOT and RESET pin holes,
the power LED, the three screws that hold it together, and the studs for the U-bracket.*

![Exploded view: U-bracket, M2 screws, rear cap, PCB, head and body, the 2.1-inch screen and the bezel ring](images/unit-exploded.png)

### The board

A Ø48 mm, 4-layer board that fits behind the screen inside a 52 mm gauge body. All the plugs and the USB-C are on the
rear face.

<p align="center">
  <img src="images/pcb-top.png" alt="The routed can2gauge V3.4 board (silkscreen can2gauge V1.0)" width="520">
</p>

| Block | Parts | Notes |
|---|---|---|
| 12 V in + protection | J_BUS1 / J_BUS2 (pass-through), input PTC, SMF24CA TVS, SS54, bulk caps | Power and CAN arrive together on either plug |
| 5 V | TPS62933 buck, CH213K ideal-diode OR-ing with USB VBUS | Runs from USB on the bench |
| 3V3 | AMS1117-3.3 | |
| MCU | ESP32-S3-WROOM-1-**N16R8** (16 MB flash, 8 MB octal PSRAM), USB-C (native USB), BOOT / RESET | USB-CDC console at 500000 |
| CAN | TCAN1044V, PESD2CAN, AO3401A-switched 120 Ω | Classic CAN only, no CAN FD |
| I/O expander | TCA9554 @ 0x20 | LCD reset / CS, touch reset, CAN termination, output IN1 / IN2, dash-light and fault inputs |
| Display | 40-pin 0.5 mm FPC: YouriTech ET021CR01-Z, 2.1" 480 × 480 ST7701S, 18-bit panel driven RGB565 + 3-wire SPI init with YouriTech's register set | Touch pins (CST8xx, I2C 0x15) wired for a touch version of the screen. FPC pinout checked against YouriTech's sheet (matches); backlight 5-7 V / 80 mA from 12V |
| Backlight | AL8860 constant-current buck **from 12 V**, 77 mA (4 parallel LED pairs, Vf 5.4-6.6 V), 250 Hz PWM dimming, pull-down holds it dark at power-up | On USB power alone the screen stays dark |
| Output | DRV8876 half-bridge (PWM mode, OUT1) | High- **or** low-side, current limit 1.5 A (R40 2.2 k), over-current / thermal / UVLO protection with auto-retry |
| K-line | L9637D, 510 Ω tester pull-up, SMF24A | Rated > 50 kbaud (ME7 at 56000) |
| Dash lights | 10 k + BAT54S peak detector, 10 µF / 100 k hold | Reads 12 V, dimmed 12 V or 58d PWM down to about 5 % duty |

There is **no on-board ADC** on V3.4. Analog sensors come in through the [adc2can](#adc-the-adc2can-expander) over
CAN.

The same firmware also runs on the **Waveshare ESP32-S3-Touch-LCD-2.1** development board (its own build in
`platformio.ini`, with Waveshare's screen set-up), handy for trying it without a can2gauge board.

<details>
<summary>Schematic</summary>

![can2gauge V3.4 schematic](images/pcb-schematic.png)

The EasyEDA Pro project is [pcb/ProPrj_can2gauge_v3.4-claude-kfix_2026-09-29.epro2](pcb/); the BOM with LCSC numbers
is [pcb/can2gauge_BOM.csv](pcb/can2gauge_BOM.csv).
</details>

### Plugs

Three JST PA (2.0 mm) plugs on the rear, all taking the same crimps.

![Rear plugs: J_BUS1 / J_BUS2 PA-4 (12 V, GND, CAN-H, CAN-L) and J_OUT PA-3 (OUT, K-line, dash lights)](images/pcb-connectors.png)

| Plug | Pin | Signal | Notes |
|---|---|---|---|
| **J_BUS1 / J_BUS2** (PA-4) | 1 | 12 V | Switched / ignition feed |
| | 2 | GND | |
| | 3 | CAN-H | Car CAN, or the gauge chain |
| | 4 | CAN-L | |
| **J_OUT** (PA-3) | 1 | OUT | 12 V high-side **or** ground low-side, set in the web UI |
| | 2 | K-LINE | OBD pin 7 |
| | 3 | DASH LIGHTS | 58d / 58 |

The two bus plugs are wired straight through, so a harness can loop from one gauge to the next. Fit the 120 Ω (from
the web UI or the screen) only on the gauge at the end of the chain. The output takes its load's 12 V or ground from the
harness. In high-side mode the load current also passes through the input PTC.

### ESP32-S3 pin map

Defined in [include/can2gauge_pins.h](include/can2gauge_pins.h):

| Group | Signal | GPIO |
|---|---|---|
| LCD RGB | DATA0-15 (B1-5, G0-5, R1-5) | 42, 45, 48, 47, 21 · 14, 13, 12, 11, 10, 9 · 46, 3, 8, 18, 17 |
| LCD timing | HSYNC / VSYNC / DE / PCLK | 38 / 39 / 40 / 41 |
| LCD init (3-wire SPI) | SCK / SDA (CS and RESET on the TCA9554) | 2 / 1 |
| Backlight | AL8860 CTRL (PWM ≤ 500 Hz) | 6 |
| I2C | SDA / SCL (TCA9554 0x20, touch 0x15) | 15 / 7 |
| Touch | INT (RESET on the TCA9554) | 16 |
| CAN | TX / RX | 4 / 5 |
| K-line | TX / RX (UART1 remapped) | 44 / 43 |
| USB | D- / D+ | 19 / 20 |
| Boot | BOOT button | 0 |

| TCA9554 port | P0 | P1 | P2 | P3 | P4 / P5 | P6 (in) | P7 (in) |
|---|---|---|---|---|---|---|---|
| Signal | LCD_RST | TP_RST | LCD_CS | CAN_TERM_EN (low = 120 Ω in) | DRV8876 IN1 / IN2 | Dash lights | Output fault |

### The 52 mm housing

A standard 52 mm DIN gauge: a Ø51.8 body through the dash hole, a Ø76.4 head carrying the screen, a rear cap with a
window per plug and slots for USB-C, BOOT and RESET, and a U-bracket clamping it to the panel. Printable in PETG or
ASA; the current files are `enclosure/can2gauge_52mm_STL_V3-6.zip`. Assembly, the flex fold and the stack-up are in
[enclosure/README.md](enclosure/README.md).

<p align="center">
  <img src="images/unit-front.png" alt="The gauge from the front: bezel ring and the round screen" width="400">
  &nbsp;
  <img src="images/unit-open.png" alt="Rear cap off: the ESP32-S3 module, J_LCD flex connector, USB-C, BOOT / RESET and the three plugs inside the body" width="400">
</p>

*Front (left), and the rear with the cap off (right): the ESP32-S3 module, the flex connector from the screen, USB-C,
BOOT / RESET and the plugs. Cutaway sections of the stack are in [enclosure/README.md](enclosure/README.md).*

---

## Simulator

Try every page, source and setting before anything is flashed, with Python 3 only (no compiler and no hardware):

```
python sim/run.py                 opens http://127.0.0.1:8080/sim/
python sim/run.py --first-boot    the same, as a brand-new gauge (the screen asks to set it up)
```

- **`/sim/`** is the screen, drawn the way GaugeUI draws it, plus a bench to drive the car.
- **`/`** is the **real web UI** (`data/`, unchanged) talking to the simulated gauge.
- The car is a Mk4 1.8T 4motion with OpenHaldex and a DSG. It puts real `vw_pq.dbc` frames on a virtual bus and
  answers K-line sessions as the engine, ABS, cluster, AWD and other modules with real part numbers. It also runs an
  adc2can, a Bluetooth adapter that reads a Golf IV VIN, an OpenHaldex heard over WiFi, and a usb2ip sending an
  Ignitron's channels over WiFi.
- It covers drive-cycle, manual and parked scenarios, faults, pinned values, car ID against all 108 DBCs, and a DBC
  browser.
- The Log tab records the simulated car, and the Service tab reads its stored faults (the engine has two) and runs
  the throttle body adaptation (park the car on the bench first).
- The nav card shows with **Show Test Turn**; there's no phone in the sim.
- `/sim/reel.html` is a 1080 × 1920 portrait frame (the screen plus the web UI) for screen recordings.

Every image in this README was made from it; see [sim/README.md](sim/README.md) and
[Regenerating the README images](#regenerating-the-readme-images).

---

## Updating

**Over the air**, from the **OTA** tab (the shared Forbes Automotive OTA):

1. **Update from GitHub**: the gauge fetches the release list from `Forbes-Automotive/can2gauge` (directly, or
   through the **Home WiFi bridge**) and installs the web UI then the firmware.
2. **Update from Files**: upload `littlefs.bin` (web UI) and then `firmware.bin`.

Uploads are checked before anything is swapped, and the file system is never formatted. A new firmware has to
confirm itself within 60 s or the gauge rolls back. Updates are refused while an ECU flash read has the K-line.

**Gauge to gauge**, for a daisy chain: update one gauge as above, wait a minute for it to confirm its new
firmware, then press **Update Other Gauges** on its Daisy Chain tab. It works through the other gauges one at a time
over ESP-NOW, about a minute each:

- The firmware goes straight from the updated gauge's running slot into the other gauge's spare slot. That gauge only
  switches over once the whole image is in and its fingerprint matches, so a power cut on the way leaves it on its old
  firmware. A gauge that already runs the same firmware skips this part.
- The web pages, logos, DBCs and sprites: only the files that differ are sent (`fs_manifest.txt`, written when the
  image is built, lists the release's own files). Each gauge keeps its own files (.ecu, log.cfg, logs, uploads).
- Then the gauge restarts on the new version. The next one only starts once it is heard back, so a bad build stops
  at one gauge.
- Every gauge needs the chain on with the same group (the job and each file are signed with the built-in key), the
  ESP-NOW transport, and the car parked (no RPM, no road speed). A gauge never takes older firmware this way.

> [!NOTE]
> The flash layout is 16 MB: two 3.25 MB firmware slots and 9.4 MB for the web UI, logos, DBCs, label files, the Rev
> sprite pack, ME7 files and logs. The filesystem image is packed when it is built (`tools/fs_pack.py`: the web scripts and styles, make logos and VCDS label index zipped, the Rev sprite pack
> with every frame zipped), so it takes about 5 MB of the 9.4 MB and leaves room for ME7 files, logs and uploads.
> `data/` itself stays plain, and the simulator serves it as it is.

---

## Repository layout

```
can2gauge/
  platformio.ini, partitions_16mb.csv
  include/, src/              firmware (signal catalogue, CAN / K-line / ELM / DSG / chain / OHX / ECU over WiFi / themes /
                              web API ...); ecunow_proto.h + ecunow_library.h are shared with usb2ip
  lib/                        GaugeUI (LVGL front end), CANLine, KLine, ME7Logger, ME7Read, Display_ST7701, CST8xx_Touch,
                              TCA9554, IOX, LVGL_Driver, FsUnzip (files that ship zipped)
  data/                       web UI (index.html, app.js, style.css, ota.js, logtab.js, service.js, vdid.js, car.js, the
                              shared logview.js + uPlot), logos/, dbc/, lbl/ (labels.pak), me7/, sprites/, nav/ (turn
                              arrows), vdid/ (the VCDS tab's DID library, from OBDb), cars/ (the car pictures)
  sim/                        the simulator (Python, standard library)
  android/                    can2gauge Link, the Android app for the nav link (Kotlin, Android Studio)
  dbc/                        the opendbc library + English VW / Audi translations, index and fingerprints
  tools/
    dbc/dbc_tool.py           DBC index, car matching, pack for the gauge, German -> English
    labels/lbl_tool.py        pack VCDS label files for the gauge, show which file a module would use
    logo_convert.py           SVG / PNG -> C2GL boot logos
    sprite_convert.py         frames / the Rev layers -> C2GS sprite packs (docs/SPRITES.md)
    c2gs_pack.py              zip a sprite pack's frames (C2GS version 2, about half the size)
    fs_pack.py                builds the filesystem image from a packed copy of data/ (run by PlatformIO)
    vdid/obdb_to_c2g.py       OBDb's vehicle profiles -> data/vdid/ (download, then build)
    cars/                     the body-type pictures in data/cars (gen.py from models.py's sizes; preview.py draws them)
    icons/                    page-slot and menu icons (one source each, rendered into lib/GaugeUI)
    elm_bench.py              run the OBD dongle bench tests from a laptop
    nav_arrows.py             the nav card's turn arrows -> data/nav/*.bin (and the app's preview pictures)
    make_release.py           OTA release builder
    readme_shots.py, readme_shots/   the README images (see below)
  docs/                       FIRMWARE.md (architecture), DBC library, OBD / BLE / chain design notes, nav-link/ (the
                              phone app link), DSG UDS report,
                              display spec, HISTORY.md (the design log and changelog)
  pcb/                        EasyEDA Pro project, BOM, netlist, generator scripts, previous revisions
  enclosure/                  the 52 mm housing (STLs, model scripts, assembly check)
```

### Regenerating the README images

```
py tools/readme_shots/glass_shots.py          screen stills, posters and GIFs from the simulator   -> images/glass-*
py tools/readme_shots/can2gauge_api.py        record the simulator's API into the web UI mock
py tools/readme_shots.py tools/readme_shots/can2gauge.json   web UI shots and posters  -> images/ui-*, can2gaugeUI*.png
py tools/readme_shots/pcb_images.py           board render, schematic, plug pinout from the newest pcb/*.epro2
py tools/readme_shots/unit_renders.py         the assembled unit (iso, front, rear, cap off, exploded, turntable) -> images/unit-*
```

They need Python 3 with Pillow and Chrome or Edge (`unit_renders.py` also loads three.js from jsDelivr, and uses the
screen stills from `glass_shots.py` for the gauge's face). `readme_shots.py` is the shared Forbes Automotive tool
(`_SharedTheme/tools`), copied in unchanged.

---

## Status and bench items

The firmware has grown in the simulator ahead of the hardware. These still want their first real-world check:

- **Board bring-up**: YouriTech's ST7701S init on the ET021CR01-Z and its RGB timing (the datasheet's typical porches,
  17 MHz pixel clock), backlight current, and touch on a real screen (the boot log lists the I2C bus when the touch
  chip doesn't answer at 0x15). Checked against YouriTech's sheet (docs/ET021CR01-Z.pdf) on 2026-10-26: the FPC pinout
  matches J_LCD pin for pin, and the backlight is 5-7 V at 80 mA (12V supply: it only glows on USB power). First
  picture on the screen 2026-10-26. The supplied screens have no touch chip, so touch is still untested. Screensaver /
  sprite frame rate on the screen: 6-8 fps so far (Rev: ~96 ms to decode a frame, ~155 ms to draw it), being worked
  on. The firmware runs on a bare board with no screen fitted (WiFi, web UI, OTA, CAN, K-line all come up), and
  Diag > Display shows how far the panel got and tests the touch.
- **OBD speed**: the wired CAN OBD-II polling (six PIDs per request, flow control) on a first car, and both **Fast OBD**
  options (gauge K-line, dongle) against real ECUs and adapters. Both are off by default until then.
- **Dongle paths**: TP2.0 through the adapter, WiFi adapters, KWP2000 over the adapter's K-line and the bench tests,
  all written against the Vgate iCar Pro BLE and still to be run on one. A CAN car through a dongle (six PIDs a
  request).
- **K-line**: the ME7 logger (Normal and the Fast RAM-routine mode) runs on a bench `8N0906018J` at ~34 and ~50
  samples/s; still to try the Fast mode across more ME7 part numbers, and the ME7 flash read on an unmodified ECU
  (the bench one is tuned, so its flash read is gated). KWP2000 VAG blocks on a module that speaks it. Also the
  cluster oil-level encoding on a car with G266.
- **CAN**: Kombi_2 oil temperature on a Mk4 with G266, the MQB TCU's answer to the gearbox speed read, and daisy-chain
  bus load on the drivetrain CAN.
- **Gauge Numbers**: numbering three gauges and moving the master from one gauge's web page, Show Numbers on every
  screen, and Play It Now's row wave across the three. Ignition from another gauge in the chain: the followers
  switching to it by themselves, then sleeping and waking with the gauge that has the ignition. Shared settings: a
  theme, units, screensaver or shutdown sprite change on one gauge reaching the other two, from the master and from a
  follower.
- **Radio**: ESP-NOW range through the dash and bulkhead, and the AP, BLE and ESP-NOW all running together with the web
  UI open.
- **First Boot**: the question and login cards on a real screen and touch, and the checks with a real car: the
  K-line wake-up, the Bluetooth scan and the WiFi scan while the phone is on the gauge's AP.
- **ECU over WiFi**: the usb2ip link on a car with an Ignitron, a MaxxECU, a Link, a Haltech and an EMU PRO (everyday
  values, a few channels on gauge pages, the loss figure with the USB/IP bridge busy), so far only run in the simulator.
- **Data logger**: the PSRAM a real gauge has to spare with the Rev pack and the web UI in use, and Download CSV on an
  iPhone and an Android phone.
- **Bluetooth OBD**: the drop-out seen with the web UI open on a phone (the bench PC has no WiFi to load the gauge's
  AP); the 4 s supervision timeout should ride it out. Internal RAM with the adapter connected: ~27 KB free, 17 KB at
  the lowest, biggest piece 9.7 KB - steady over 15 min, but check an OTA update from GitHub (HTTPS) with an adapter
  connected.
- **Packed filesystem**: Rev's decode time per frame on Diag > Drawing with the zipped frames (it should stay well
  inside the 15 fps slot), the make logos and the VCDS label index loading from their zipped files, and the web UI
  loading zipped scripts on an iPhone and an Android phone.
- **Basic / Advanced web UI**: the gauge lines, More Options folds, (i) help and Settings sections on a real phone
  (iPhone and Android) against the gauge, and that every setting still saves from inside a fold.
- **29-bit Bluetooth OBD**: on the 2025 Tiguan the terminal shows `ATSHDA01F1 OK` after "Detected", No data stays flat
  and the PIDs fill in; with the DSG source on Bluetooth OBD, the gearbox read goes to `18DA02F1` and engine polling
  carries on after it, and after Read DTCs. The ME7 and the X5 still on `ATSH7E0` and still working.
- **Nav link**, untested: the stored arrows need the filesystem image flashed too (Upload Filesystem Image). Which
  arrow the app picks for each Maps instruction on a real route (the raw dump shows the words). The app on a real
  phone with a Google Maps route, checking the raw notification dump on the app's main screen against what reaches
  the gauge (distance, road, arrival time, the arrow). Internal RAM with Bluetooth started for the nav link and the
  OBD adapter connected too (Diag, boot log "BLE: started, used .. KB"): two connections and advertising cost more
  than the adapter alone. The app finding the gauge again after the ignition goes off and on, the card popping up at
  the set distance, a tap hiding it until the next turn, and an arrow sent across Bluetooth in two parts (295 bytes,
  more than one packet).
- **DBCs feeding the gauge**, in the order of [docs/CAN_DBC_plan.md](docs/CAN_DBC_plan.md) section 5: an MQB car with
  the VAG MQB profile and vw_mqb in slot 1 (rpm, speed, coolant, gear, selector, lock-up the same either way - set the
  profile to OpenHaldex only to see the DBC alone); a MaxxECU on the bench with maxxecu_default and its base ID; an EMU
  Black / Link / Haltech against the ECU's own live view. Two DBCs with a clashing ID: slot 2 says which ID. Slots
  survive a reboot.
- **Units**: Custom (mph with bar, AFR) on the screen and in Live data, the screen's Units button turning the ranges
  over, and a backup import keeping its own ranges.
- **8HP gearboxes**, per [docs/8HP_research.md](docs/8HP_research.md) section 6:
  - X5 F15 through the Bluetooth adapter, Gearbox type ZF 8HP BMW: does the clone answer `OK` to `ATCEA18` (the tab
    says "not supported" if not), do DA12 / DA22 / DA2E come back, and does the ATF temperature read sensibly (cold
    start: close to coolant and outside air)? Engine PIDs must keep coming between gearbox passes.
  - An Audi / VW 8HP on the gauge's CAN, type ZF 8HP Audi / VW: what 210F reads in each gear (raw, or raw - 2) and
    the raw 7078 lock-up numbers at a steady 60-80 km/h.
  - An MLB car (A6 / A7 C7, Q5 8R) on the drivetrain CAN with the VAG MLB profile: 0x102 selector and gear, 0x441
    oil temperature, and whether 0x082's clutch state really is the converter lock-up.
  - An MQB car with a DSG: the lock-up (clutch) and shift values look sane.
- **Internal RAM**: Bluetooth OBD connected and a phone on the web UI for an hour; Diag > RAM ran out stays "No". If it
  freezes, the next boot's log and Diag show what ran out. Open: the task watchdog only watches core 0's idle task, so
  a stuck task hangs the gauge instead of resetting it - watching more tasks needs bench time (Bluetooth and K-line
  waits of 5-15 s are normal and mustn't trip it).
- **Boot logo timing**: the 3 s sweep still looks right, and the 2 s fade stays smooth on the screen. Three gauges
  in a row light up and sweep together at power-up (the boot sync handshake over CAN), the second power-up too (the
  first after flashing only learns how many gauges there are), and one gauge unplugged from CAN costs the others at
  most 1.5 s of dark screen.
- **Screen rotation**: Upside Down and a few degrees of Rotation Trim on both screens: the frame-rate cost (Diag >
  Drawing, needle page and Rev), taps and swipes landing right, page slides the right way round upside down, and
  setting it after boot (memory already taken by the sprites: the card should ask for a restart).
- **Update Other Gauges**: two gauges: firmware and file transfer time, a power cut half way (the gauge must stay on
  its old firmware), Stop part way, and a gauge with a different group (not updated).
- **Service tab**: fault read / clear over the K-line (Mk4 engine and cluster) and CAN (MQB UDS, PQ35 TP2.0), and the
  throttle body adaptation on an ME7 1.8T, which also settles how the result text in field 4 is coded.
- **UDS through the dongle** (all untested), on the 2025 Tiguan first:
  - Bluetooth OBD > Read DTCs: the UDS part now runs on 29-bit (`18DA01F1`, `18DA02F1`) and lists the VAG codes.
  - Scan Modules over the adapter: which modules answer on 11-bit (the gateway, cluster, ABS, AWD ... should) and
    which on 29-bit, and that the adapter takes `ATSP6` / `ATSP7` and is back on its OBD values afterwards (No data
    stays flat).
  - VCDS tab with Connection = OBD adapter: the engine at `18DA01F1` and at VW's `17FC0076` (one of them should open
    10 03); a few values from the DID Library, the answers per second with and without Turbo, and the OBD pages still
    updating in between. An OBDb 29-bit entry with no tester address is built with the answer's last byte
    (`17FE0076` -> `17FC0076`): this is the check of that rule.
  - A clone's answer to `ATCEA` (BMW extended addressing; the X5's EGS from the DID Library's BMW group).
  - On the gauge's own CAN: the same VCDS values and the scan on 29-bit ids (extended CAN frames out of the gauge).
- **Your car**: the VIN from each source - the Tiguan through the adapter (`09 02` or `22 F190`) and on the gauge's
  CAN (the gateway's `VIN_01`, or the one-off `22 F190` / `09 02`); the ME7 1.8T's block 081 over the K-line during
  First Boot (the K-line session goes back to block 1 afterwards). The Tiguan's model code (VIN characters 7-8, a Mk3
  isn't in the table yet) to add with its picture. The confirmed car surviving a settings reset and an OTA update.

The full lists are under "Bench items" in [docs/FIRMWARE.md](docs/FIRMWARE.md).

---

## Credits and licences

- **opendbc** (comma.ai, MIT): the DBC library in `dbc/` and `data/dbc/`.
- **OBDb** ([github.com/OBDb](https://github.com/OBDb), CC BY-SA 4.0): the DID library in `data/vdid/`, built from its
  vehicle profiles by `tools/vdid/obdb_to_c2g.py`. The library files are shared under the same licence.
- **Garage Tinkering's Ultimate Gauge Board** and the **Waveshare ESP32-S3-Touch-LCD-2.1** (MIT): the ST7701S init
  sequence and the display pin map the board started from.
- **NefMoto ME7 flasher** (GPLv3): the read path behind the ME7 flash dump, ported read-only.
- **Ross-Tech VCDS** label files: field names and block layouts for the K-line screens.
- **LVGL**, **NimBLE-Arduino**, **ESPAsyncWebServer / AsyncTCP** (mathieucarbou) and **ArduinoJson**.
- **uPlot** (Leon Sorokin, MIT): the graphs in the Log tab.
- **Espressif esp-bsp / esp_lvgl_port** (Apache-2.0): the ESP32-S3 SIMD blend routines in `src/simd/`.
- **MaxxECU**: the CAN protocol is MaxxECU's published "Default CAN output" specification.
- Make logos are the trademarks of their owners ([docs/LOGO_SOURCES.md](docs/LOGO_SOURCES.md)).

## Disclaimer

Forbes Automotive accepts no responsibility for any incidents arising from the use of this gauge. It reads, and in a
few places requests, data on the car's diagnostic buses. Bus IDs, scalings and protocols are vehicle-specific, so check
them against your own car before relying on any value, trigger or output, and never run diagnostics while driving
that could distract you from the road.
