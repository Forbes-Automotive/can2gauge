#pragma once

// can2gauge firmware version (semantic: major.minor.patch) - bump on every release, mirrored in data/version.json
#define CAN2GAUGE_VERSION "0.39.7"  // 0.39.7 - Fixes: a Tiguan scanned (and First Boot's Your Car filled in) as a BMW -
                                    //          My Car, saved apart from the settings, survived Factory Reset and won
                                    //          over the VIN read. Factory Reset now forgets My Car and the last car
                                    //          scan too; Scan My Car and the Your Car form go by the VIN read when it
                                    //          isn't My Car's (the form says so). The paired OBD adapter is kept: also
                                    //          saved under its own name (back after a settings layout change), an
                                    //          empty one in a backup no longer unpairs it, and the web UI's Reboot
                                    //          saves a change not yet written first. Not compiled.
                                    // 0.39.6 - The K-line screen (menu > K-line, the K-line tab's live view): when the
                                    //          engine doesn't answer at 0x01, OBD-II at 0x33 is tried (as First Boot
                                    //          and Scan My Car do) and its PIDs show four to a page, a swipe for the
                                    //          next ("Page 2 / 5"); the values feed the gauge too. Worked last time =
                                    //          asked first next time. The Bluetooth OBD tab's K-line bench test ends
                                    //          with OBD-II at 0x33 (ISO 9141-2, KWP2000 5-baud, fast init). Not compiled.
                                    // 0.39.5 - Update All Gauges (was Update Other Gauges) works from any gauge: the gauge
                                    //          with the newest firmware updates the others, the one pressed on too
                                    //          (a new signed PLEASE request asks it; versions come from the heartbeats).
                                    //          The screensaver comes up over any screen (a menu screen, Haldex,
                                    //          Gearbox, Settings, the drag timer when it isn't timing) and goes back
                                    //          there; leaving it is done by the LVGL task (the stack, as 0.39.4).
                                    //          Output on the screen kept short: menu "Output", screen "OUTPUT",
                                    //          Control Auto / On / Off, the trigger's name on the line above; other
                                    //          menu screens' values shortened to fit the value column. Not compiled.
                                    // 0.39.4 - Fix: the menu going back by itself after 10 s untouched, to a menu screen
                                    //          (Navigation, CAN ...: the screen it was opened from), overflowed the
                                    //          uiTick task's 6 KB stack and restarted the gauge (coredump 2026-11-03,
                                    //          0.39.2: the slide draws that whole screen in the task asking for it).
                                    //          The idle close is now done by the LVGL task (10 KB), as a tap's is.
                                    //          Not compiled.
                                    // 0.39.3 - Menu > Navigation: a swipe rolls over at the ends (past Route back to Off,
                                    //          before Off to Route); it stopped there. Not compiled.
                                    // 0.39.2 - Every Menu and Tabs switch is a menu item on the screen, each with its own
                                    //          icon, in the switches' order: new Navigation, usb2ip, Oil and Triggers
                                    //          screens. Navigation: the link's state (phone, route, next turn, last
                                    //          update) and Off / Pop Up / Route buttons (a tap, or a swipe steps them;
                                    //          the phone link starts / stops in the house task). A tap or a long press
                                    //          on the nav card goes back to the gauges: Pop Up brings it back at the
                                    //          next turn, Route not for the rest of the route (or until Route is
                                    //          picked again). tabsShown is gone (the four tab switches are menuMask
                                    //          bits now): SETTINGS_VERSION 32, the update resets every setting again
                                    //          (Download Backup / Import Backup). Menu icons the glass never shows are
                                    //          left out of the firmware (~105 KB of flash). Not compiled.
                                    // 0.39.1 - Settings > Device > Menu and Tabs has a Navigation switch: it hides the
                                    //          Navigation tab like the Oil / usb2ip / Triggers ones (tabsShown bit
                                    //          TAB_SHOW_NAV, JSON menu "nav"; First Boot keeps it as it is). On a gauge
                                    //          that ran 0.39.0 the switch starts off: switch it on. The USB bench
                                    //          console sends the screen's two frame buffers (FILEGET /api/screen/glass
                                    //          and /back; tools/diag/screen_grab.py makes PNGs) to look into the marks
                                    //          left on the RPM dial after Show Test Turn and the accent colour at the
                                    //          bottom of the nav card. Not compiled.
                                    // 0.39.0 - Tidy, batch 1 (no change in what the gauge does): can2gauge_defs.h, main.cpp,
                                    //          the tasks and the small files (OTA, RAM, WiFi, ME7 read / fast, drag
                                    //          timer, CAN relay, triggers, WiFi OBD, IO, First Boot, shared settings,
                                    //          themes, console, logger, units, boot sync) laid out the same way: the
                                    //          file's settings and variables (grouped by type), a list of its functions,
                                    //          setupX() / the task first, the details below; short comments without
                                    //          version tags. The Settings fields go by type within each section:
                                    //          SETTINGS_VERSION 31, so the update puts every setting back to its default
                                    //          (Download Backup before, Import Backup after: the backup goes by name).
                                    //          unitsSanitize -> unitsSanitise. Not compiled.
                                    // 0.38.8 - Fix: a page rebuild (a settings save, shared settings from another gauge, an
                                    //          adc2can range) while LVGL was sliding to another page itself crashed the
                                    //          gauge: LVGL still held the deleted page as the screen it was going to, and
                                    //          the next screen load moved it (LoadProhibited in lv_screen_load_anim).
                                    //          Found on the bench with tools/diag/anim_rebuild_test.py over USB: a crash
                                    //          every round on 0.38.7. lvglFinishScreenAnimation() ends such a slide at
                                    //          once before the pages, the menu or the theme are rebuilt. Not compiled.
                                    // 0.38.7 - First Boot: with other gauges heard on the chain, "Set Up the Other Gauges
                                    //          the Same" (ticked) at Finish. After this gauge's own setup (and the car's
                                    //          DBC), POST /api/chain/setup saves what the others get (/fbsetup.json:
                                    //          pages, triggers, CAN profile, OBD, OpenHaldex, usb2ip, menu, the diag
                                    //          slots, the oil over CAN) and the chain update task sends it, the car scan
                                    //          and the DBC slots to each gauge over ESP-NOW: a setup job (UPD_SETUP
                                    //          hello, signed files, FINISH applies them as a web UI save would - no
                                    //          firmware, no restart; only between gauges on the same version). Each
                                    //          gauge starts on its own page by its place in the row (chainSetupStartPage)
                                    //          and its First Boot question goes. Kept per gauge: WiFi, row number, name,
                                    //          CAN termination, the OBD dongle, the K-line, the dash-light wire (theme,
                                    //          units, screensaver, ignition and boot logo travel as shared settings
                                    //          already). The settings save (web UI, USB console, a setup) is one at a
                                    //          time now (settingsApplyMutex). Untested, not compiled.
                                    // 0.38.6 - First Boot (web UI): no VW engine at 01 on the K-line, and it asks OBD-II at
                                    //          33 too (any make: one wake-up and the PID lists, as Scan My Car does) -
                                    //          its own row in the checks. Before, it only waited for 01, so an older car
                                    //          with K-line OBD-II only found nothing, and the car scan didn't run either
                                    //          (no way in). 33 answering counts as a way in (the scan runs), suggests
                                    //          K-line KWP2000 OBD-II at address 33, and the OBD gauges. Untested.
                                    // 0.38.5 - Update Other Gauges: a web file that arrives with the wrong fingerprint is
                                    //          sent again (3 tries in all) instead of failing the whole gauge - before,
                                    //          the gauge being updated threw away the firmware it had already checked
                                    //          and kept the files already in place (new web UI, old firmware) until the
                                    //          update was run again. The sender's own flash reads are checked now (a
                                    //          short read is tried once more, then the job stops with "couldn't read
                                    //          our own files / firmware"), where before a short read went out as it was.
                                    //          Untested, not compiled.
                                    // 0.38.4 - K-line KWP2000 / OBD-II: no answer or no VIN at the address set (01 by
                                    //          default) and it tries the OBD-II address 33 by itself, keeping whichever
                                    //          works (nothing to type in). Fuel in litres is its own value, "Fuel tank"
                                    //          (fuelLitres, L): the VAG cluster's tank litres (PQ mKombi_1, MQB / MLB
                                    //          Kombi_02) went into OBD's Fuel %. Scan My Car (web UI): VW's own 29-bit
                                    //          addresses asked too (the 2025 Tiguan answers 18DAF101 / 18DAF102, not
                                    //          at 10 / 18), for OBD and for the engine / gearbox diagnostics lists;
                                    //          modules named "(OBD)" or "(diagnostics)"; the log ends with what came
                                    //          from where; Add to Gauge on every value; Suggested Gauges and Triggers
                                    //          under the scan, the same lists First Boot makes. First Boot's starter
                                    //          set: every source offered (the engine: rpm, speed, oil temp / pressure,
                                    //          coolant, boost, battery ...; the gearbox: gear, its and the clutch's
                                    //          temperature; adc2can with its dial range and warning; usb2ip; OpenHaldex;
                                    //          one value from each other module), the 8 ticks one per source first;
                                    //          suggested triggers (coolant, rpm, boost, battery low / high, oil,
                                    //          gearbox, EGT, the adc2can sensors' own warning limits). A scanned value's
                                    //          gauge range from its own unit. Fix: car.js and carscan.js both had
                                    //          carAddStyle / carSetup, so Scan My Car's styles were never added (its
                                    //          progress bar never showed). tools/diag/anim_rebuild_test.py drives the
                                    //          page-rebuild-during-an-animation test. Untested, not compiled.
                                    // 0.38.3 - Bug review of the whole firmware (nine reviews by area), the clear bugs fixed.
                                    //          Crashes: a Service job over CAN / the adapter ran out of the VAG diag
                                    //          task's stack (3 KB on a 4 KB stack); a diag value's Bit near 65535
                                    //          wrapped past the length check; a K-line OBD answer longer than 8 bytes;
                                    //          the ME7 logger's 64-byte answers read with a 250-byte limit; Recent
                                    //          Frames with a 0-byte or 9-15 length frame; Rev Runner on a pack that
                                    //          failed to open; a sprite mood band narrower than 1 (divide by zero).
                                    //          Car / power: CAN RX left on the wake circuit after a deep-sleep wake on
                                    //          CAN (rtc_gpio_deinit); the output switched on during each night-time
                                    //          wake check; a trigger's output stayed on after the trigger was switched
                                    //          off; triggers ran in standby (No Reading fired as the bus slept);
                                    //          standby set first so the backlight can't come back during the fade.
                                    //          Values: MQB / MLB init / error / not-fitted codes no longer shown as
                                    //          readings (76.5 C outside temp, 142 C coolant at ignition-on); MaxxECU
                                    //          1.2 oil from base + 0x16 only once a 1.3 frame is heard; OBD polling
                                    //          moves on when a later PID list is refused; a chain follower learns the
                                    //          pedal PID for Throttle; OpenHaldex / usb2ip car signals and the chain's
                                    //          "alive" checks no longer trip on a 1 ms time stamp slip; every shared
                                    //          signal goes out over ESP-NOW (was the first 40); Match Car only follows
                                    //          My Car's own VIN. Scan / diag: Scan My Car and the CAN VIN read get their
                                    //          turn while diag values poll; the VIN read obeys Enable Other Scanners and
                                    //          hands the VIN to the house task; a TP2.0 module no longer reads another's
                                    //          channel; no stale part number on a module that won't identify; UDS
                                    //          answers over 256 bytes cut short, not lost. K-line: a lost KWP2000 VAG
                                    //          session reconnects (and a refused block is empty, not a lost session);
                                    //          menu > K-line with address 0 no longer spins the K-line task; KWP2000
                                    //          VAG keeps the module's identification; label lookup locked (K-line task
                                    //          and web server). Adapter: a failed diag setup puts the header back on
                                    //          the engine; PID lists 21-60 from the engine only; DHCP back after the
                                    //          WiFi adapter's fixed-IP try; nav link off lets the phone go; Bluetooth
                                    //          waits for a disconnect before reconnecting. Web: /api/ecu/library was
                                    //          shadowed by /api/ecu; uploads go through a .part file and report a full
                                    //          filesystem; Factory Reset takes the chain's shared settings; screen:
                                    //          the Oil (PQ) page's LEVEL line is back; a swipe / long press no longer
                                    //          also taps (or clears an alert); no blank screen after standby + a
                                    //          rebuild; partial-mode strips sized for RGB565 (77 KB, was 115 KB).
                                    //          Untested, not compiled.
                                    // 0.38.2 - No other company's scan tool named anywhere: Enable Other Scanners' help
                                    //          text says "another scan tool or code reader", and the code / tools / sim
                                    //          comments say "the capture" or "scan tools" instead. Wording only.
                                    // 0.38.1 - Fix: picking Settings on the screen's menu (seen with Gauge Control's Single
                                    //          Tap) crashed the gauge. Coredump: the LVGL task's 6 KB stack ran out
                                    //          drawing the whole Settings screen for the fade-in, from inside the tap
                                    //          (5.7 KB used); now 10 KB. Dashboard > Gauge Control is the first card on
                                    //          the Dashboard. Untested.
                                    // 0.38.0 - Simos 18 engines (EA888 gen 3, MQB), from the RokketGauge's UDS engine
                                    //          (RokketUds, contributed by Kile at Rokketek). Scan My Car asks a VW engine
                                    //          the 46 Simos 18 DIDs too; one that answers like a Simos 18 (36+ with their
                                    //          own length, MAP / AFR / turbo speed always) gets the Simos 18 names and
                                    //          scaling instead of OBDb's (whose VW list merges every engine: 0x1040 is
                                    //          turbo speed here, not DPF pressure). Picking one turns Turbo on in groups
                                    //          of 8 (was one DID every 50 ms). Turbo's flow control now asks for no gap
                                    //          between the answer's frames (was 1 ms), and 7F 22 22 to a grouped request
                                    //          (engine off) no longer halves the group size. Turbo steps back and forward:
                                    //          a long answer that loses frames with no gap goes back to 1 ms gaps for the
                                    //          session first (shown as "1 ms gap" on the Diagnostics tab); the group
                                    //          size, once halved, doubles again after 50 good answers in a row, up to the
                                    //          tab's size. The last, part-full group of a round now halves it too, and
                                    //          the adapter's one-frame limit holds after a group (was lost).
                                    //          K-line OBD-II on any make: Service > OBD-II (any make) reads stored /
                                    //          pending codes (modes 03 / 07, every message and ECU) and clears (04) at
                                    //          0x33; the K-line OBD session reads the VIN (mode 09, 5 messages); Scan My
                                    //          Car asks 0x33 when nothing else answered (klineObd job) and switches the
                                    //          K-line tab to KWP2000 from Off. Fault descriptions for ~9,500 SAE codes
                                    //          (OBDex, CC0: data/dtc/sae.json). Drag Timer on the menu (DragTimer.cpp):
                                    //          0-100 km/h / 0-60 mph, 5 s countdown or the car moving, the finish worked
                                    //          out between speed readings, best 10 of each kept in NVS "c2gdrag" (not in
                                    //          the settings blob), Log tab card. Web UI first load: app.js sent zipped
                                    //          (/app.js handler, 257 -> 79 kB), index.html's indentation taken out, and
                                    //          First Boot asked first (a cover shows at once). The progress ring glides to
                                    //          each new value (600 ms, was a jump), and First Boot sends more steps:
                                    //          10 -> 38 % through the checks, Apply 85 / 90 / 92 / 95 then SET UP (it
                                    //          went 0 -> 40 % and 40 -> 99 % in one step). Dashboard > Gauge Control is
                                    //          Advanced only and works the screen with a pretend finger through the touch
                                    //          input (lvglFakeTouch; the screens have no touch chip yet): Previous / Next
                                    //          Gauge are swipes (pages, menu items), Single Tap, Double Tap, Long Press
                                    //          (the menu). First Boot's menu suggestion had no Drag Timer (Apply hid it):
                                    //          on now. The Gearbox menu icon has its letters back (P R N D S, + -; Adam
                                    //          preferred them to 0.37.6's dots). Settings and the theme picker leave on a
                                    //          long press wherever it lands (picker -> Settings, Settings -> menu; their
                                    //          buttons take a short press only, so a long one let go doesn't click them -
                                    //          it could reboot); the picker's two colour dots are named Dial / Needle.
                                    //          First Boot's "Live values seen" leaves out the gauge's own dash-light
                                    //          input, output and output fault (always a reading, not the car's).
                                    //          Untested on a car.
                                    // 0.37.15 - Fix: switching Navigation on while the Bluetooth OBD adapter was connected
                                    //          crashed the gauge (coredump: NimBLE "assert failed: ble_svc_gap_init", a
                                    //          service can only be added while nothing is connected or scanning). With
                                    //          Bluetooth already running the nav link now waits for a restart (Restart Now
                                    //          on the Navigation tab); at boot it starts before the OBD task, as before.
                                    // 0.37.14 - First Boot runs Scan My Car by itself after the checks when there's a way in (OBD
                                    //          on the CAN, a connected dongle, the K-line), so the starter set gets the car's
                                    //          own values (oil temp, gear ...) with no second press; skipped when the saved scan
                                    //          is of the same VIN. The gauge list waits for it and is drawn once; the button
                                    //          is now Scan Again (the car has changed).
                                    // 0.37.13 - First Boot's gauges are a starter pack: only the ones this car can show, all
                                    //          ticked (first 8). A gauge is offered when its value was seen live, a found route
                                    //          always carries it (OBD-II, the K-line engine's blocks, usb2ip) or Scan My Car
                                    //          found it (oil temp, gear, throttle, EGT ... from other modules); Haldex only when
                                    //          an OpenHaldex was heard. Nothing found: three everyday gauges, unticked.
                                    // 0.37.12 - Throttle from the accelerator pedal when the engine has a pedal PID (0x5A relative
                                    //          pedal, else 0x49 pedal D stretched to its travel, rest and flat out learnt as
                                    //          driven); PID 0x11 is renamed "Throttle valve" (bench: X5 F15 30d, a diesel's
                                    //          intake flap read 85 % all the time). No pedal PID, or an ECU that won't list
                                    //          its PIDs: Throttle stays on 0x11.
                                    // 0.37.11 - Car value lists: no "- now 22" reading after each name (clutter); a finished scan
                                    //          (First Boot or the Scan My Car tab) draws every Car list on the Gauges tab again
                                    //          straight away (bench: the First Boot scan found the modules but the lists still
                                    //          showed only Live now until a second scan).
                                    // 0.37.10 - A TP2.0 diagnostics value from the module the gearbox reads over TP2.0 borrows
                                    //          the DSG's channel (a module allows only one) instead of opening a second: a block
                                    //          the DSG read moments ago is used as it is, any other block is read on its
                                    //          channel under a lock (dsgTp20Shares / dsgTp20Read). Picked from the settings,
                                    //          so it doesn't flip as the DSG's channel opens and closes.
                                    // 0.37.9 - Mixed sources: one asker at a time on the CAN. A "turn" (CANLine canDiagClaim / canDiagWait /
                                    //          canDiagRelease) taken for each question and its answer by the OBD polling (skips a go when busy; a
                                    //          waiting task goes first), the VAG diag task (UDS / OBD / TP2.0 values, scans, Service) and the DSG
                                    //          (ISO-TP and TP2.0), so an engine DID and the OBD polling no longer take each other's 7E8 answers.
                                    //          TP2.0: transact / keepAlive point the receive filter back at the channel (a UDS module read in
                                    //          between left it deaf), and the VAG diag client answers on 0x301 (the DSG keeps 0x300) so two
                                    //          channels can be open at once.
                                    // 0.37.8 - BMW models from the VIN (characters 4-5 of the type code): X5 E70 (FE), F15 (KR,
                                    //          KS - bench: WBAKS82... is an X5), G05 (CR); a BMW X model gets the SUV picture. More codes
                                    //          go in bmwModels (can2gauge_Vehicle.cpp) as real VINs confirm them.
                                    // 0.37.7 - Bench fixes. First Boot pairs the Bluetooth adapter its check finds and waits for the car
                                    //          (the WiFi adapter row is then "not needed", a dash, not a cross). The screen no longer sticks
                                    //          after saving the setup (a late SETTING UP update put the ring back: the firmware drops them once
                                    //          the setup is done, the web UI too). Car picker: the scanned modules first, Live now last with
                                    //          readings; the Scan My Car tab lists every value found by module. The adapter's K-line (Adapter
                                    //          Source 1, module, init) is set on the K-line tab now.
                                    // 0.37.6 - Scan My Car, smarter: a Bluetooth adapter it finds is paired there and then, and it waits until
                                    //          the adapter has found the car; the K-line isn't woken again when First Boot or the K-line already
                                    //          found nothing; the make's DID library only once the VIN says which (plain ISO / OBD before, never
                                    //          VW by default); OBD PID lists from every ECU (7E0-7E7, 29-bit 18DAxxF1), the non-engine ones read
                                    //          as a new diag value kind VDIAG_KIND_OBD (01 <pid> asked of that module, obdPidValueOf); BMW
                                    //          modules by name. Adapter: ISO 15765 (ATSP6) tried first on a new car, then the auto search; Fast
                                    //          OBD on by default, dropping back to standard timing after 3 lost answers. Icons: an EML icon
                                    //          (Distance / Time MIL on, codes cleared); no text in any menu icon (K-line, ADC, output, Ohm, info,
                                    //          gearbox, CAN scaling drawn as shapes).
                                    // 0.37.5 - Scan My Car has its own tab (fourth in the menu's Set up group; was the top of the
                                    //          Gauges tab) and shows on the screen as First Boot does: SCANNING with each step, then
                                    //          SCANNED and the count (or a red NO WAY IN), then the ring fades. /api/firstboot "screen"
                                    //          takes a title, hideMs and failed (carscan.js screenProgress, shared with First Boot).
                                    // 0.37.4 - The progress ring (First Boot's SETTING UP, updates) fades out over 0.5 s when it
                                    //          goes (PROGRESS_FADE_MS, an opacity animation), instead of vanishing.
                                    // 0.37.3 - The screen shows First Boot in progress: a SETTING UP ring (uiProgress) from the
                                    //          moment the phone opens the setup, with its stage and Scan My Car's progress (POST
                                    //          /api/firstboot {"screen": {"detail", "pct"}}), held until Apply (SET UP, 3 s) or Skip
                                    //          (gone). Was: the first page (Haldex lock) while the phone set the gauge up.
                                    // 0.37.2 - Scan My Car finds the way in before scanning: CAN (OBD 01 00), the OBD adapter,
                                    //          then the K-line by one wake-up of the engine (or the K-line already talking), not
                                    //          every module. Nothing answers = it stops with "Connect an ECU" (a busy CAN that answers
                                    //          nothing: "nothing to scan, see Live Now") and keeps the last scan. The board can't sense
                                    //          a loose K-line wire: R_KP (510R) holds K at battery voltage connected or not.
                                    // 0.37.1 - The OBD adapter stops being looked for when it isn't there: after 5 misses in a
                                    //          row (no adapter in a scan, a Bluetooth connect or a WiFi join that failed) the state is
                                    //          "not found - stopped" (was: retried every 30 s / 2 min forever, using the radio WiFi
                                    //          shares) until a settings change (pair, Forget), Scan, the OBD screen being opened, or
                                    //          the ignition going off and on.
                                    // 0.37.0 - Scan My Car: one button (Gauges tab, First Boot) finds every value the car can show
                                    //          and makes them the "Car" page source. The web UI decides what to ask (carscan.js: the way in,
                                    //          the VIN -> DID library group, OBD PID lists, UDS modules and their DIDs, VW TP2.0 modules and
                                    //          K-line modules with their blocks named from the label files); the gauge asks (/api/carscan,
                                    //          can2gauge_CarScan.cpp: uds / tp20Modules / tp20Blocks jobs in the VAG diag task, klineModules /
                                    //          klineBlocks in the K-line task) and only what answers is kept (/car/values.json). VAG diag:
                                    //          32 values (was 12), each from its own module (VdiagValue txId / rxId / ext / addr / protocol,
                                    //          0 = the tab's module), read module by module, and only the values a page / trigger uses
                                    //          (all while the tab is open). Settings v30. Basic shows Car / ADC / ECU / Specifics as page
                                    //          sources, Advanced all. OBD picker: the PIDs the car doesn't report in their own group. DID
                                    //          Library Add gives the value its own module. Enable Other Scanners (Settings > Device and the
                                    //          Diagnostics tab, not saved): no UDS / TP2.0 / K-line / gearbox / wired OBD requests, so another
                                    //          scan tool can connect. Fix: a diagnostics value never reached its gauge page (publish compared
                                    //          the OBD pid field, not diagValue).
                                    // 0.36.14 - Decimals everywhere (bench: lambda showed "1" on a page left at Decimals 0, while
                                    //          its Graph showed 1.00). A page's main value and Min / Max: the page's Decimals, but never
                                    //          fewer than the value has (GaugeUI pageDecimals). signalDecimals follows the unit on show
                                    //          (psi 1, kPa 0, AFR 1: unitsConvertedDecimals); DBC signals and Diagnostics UDS values take
                                    //          theirs from their scaling (0.01 = two places); unit names match in any case, and the Greek
                                    //          lambda (unitDecimals). The K-line screen keeps a unit's decimals on whole numbers (1.00, not
                                    //          1). Web UI: /api/status sends every value's decimals ("d", was ADC only), and the Dashboard
                                    //          tiles keep their zeros (13.0 V, not 13).
                                    // 0.36.13 - Update clock 14 -> 12 MHz (DISPLAY_UPDATE_PCLK_HZ): bench, a later chain update
                                    //          (web files as well as firmware) left slight lines on the gauge being updated.
                                    // 0.36.12 - Daisy Chain tab spacing: the gauge list drops a little under its "This gauge"
                                    //          line, and the Update Other Gauges list has a gap above its buttons.
                                    // 0.36.11 - Backlight off before every restart (after an update, Restart, a settings
                                    //          restore ...), so the screen isn't seen tearing as the chip resets: one ESP-IDF
                                    //          shutdown handler in Display_ST7701 (esp_register_shutdown_handler), so the shared
                                    //          OTA module's restart is covered too without touching it.
                                    // 0.36.10 - The gauge sending Update Other Gauges slows its screen clock too (bench: 0.36.9
                                    //          cleared the gauges being updated, but the sender had lines - reading its firmware and
                                    //          files back out of flash uses the same shared wires as the writes).
                                    // 0.36.9 - Lines on the screen of a gauge being updated (bench, Update Other Gauges):
                                    //          flash and PSRAM share the same wires, so the long run of flash writes starved the
                                    //          screen feed. While an update is written (from another gauge or the web page) the
                                    //          pixel clock drops to DISPLAY_UPDATE_PCLK_HZ (14 MHz, Display_ST7701.h) and goes
                                    //          back after; the screen only shows the progress ring then.
                                    // 0.36.8 - Diagnostics tab stops trying when it can't work: nothing is sent while the
                                    //          car's OBD (through the adapter) isn't CAN, and after 3 connects with no answer
                                    //          (1 when nothing else is heard on the CAN) it shows "stopped" until Restart Session,
                                    //          a settings save, Read It, the next ignition on or (quiet CAN) the car talking.
                                    //          Shorter Requests / Connection / Turbo options (they pushed the box wide). DID
                                    //          Library search: nothing on the picked module = every module searched; the module
                                    //          name and "0x2BE4" match; the matches Confirmed Only hides are counted. Phone menu:
                                    //          the page behind no longer scrolls when the menu is scrolled to its end.
                                    // 0.36.7 - Tabs renamed for what they are: the measuring values tab -> Diagnostics (measuring values from
                                    //          any module, any make), Diag -> System (the gauge's own health). The page
                                    //          source is "Diagnostics (UDS / TP2.0)". No tool names anywhere the user sees (web UI,
                                    //          README, sim README); data-page ids and settings keys (vdiag, diag) unchanged.
                                    // 0.36.6 - adc2can sensors are offered only for the boards heard (Gauges tab sensor
                                    //          picker, Triggers, Log, the ADC tab's rows), with one label everywhere:
                                    //          "Board 1 - Sensor 1 (V)" / "... - Oil press (bar)". Ready for more boards:
                                    //          ADC2CAN_BOARDS / ADC_SENSORS in can2gauge_defs.h are the one place it's set
                                    //          (settings.adc, the JSON loops, pageSourceOf, /api/adc/set's board check; a
                                    //          static_assert ties the adc signals to it), and the web UI takes the board
                                    //          count from /api/adc (the ADC tab's board lines are built from it).
                                    // 0.36.5 - An ADC page always draws with the adc2can's dial range (adc2can 1.4+, the
                                    //          sensor scaled there), in the units on show; its own Min / Max are kept but
                                    //          not used (bench: pages saved at 0.5-4.5 / 0.5-5 before 0.36.4 kept them).
                                    //          GaugeUI: PageWidgets.drawnMin / drawnMax from pageDrawnRange at build; a
                                    //          new range from the adc2can rebuilds the pages in uiTick (uiPageRangesChanged).
                                    //          Gauges tab: such a page's Min / Max show that range, greyed out, with a note.
                                    // 0.36.4 - Graph page: Max and Min sit just outside the graph, against its left edge
                                    //          (towards the screen's edge), not inside it on a box. ADC dial ranges come
                                    //          from the adc2can (1.4+): its Dial Min / Max or Auto range (plain volts
                                    //          0.00-5.50 V) arrives as config parts 30 / 31 (ADC2CAN_PARTS 32; a board's
                                    //          own part count from its info frame, so a 1.3 board with 30 still counts as
                                    //          complete). A page picking the sensor takes it as is; the ADC tab can set
                                    //          it (/api/adc/set dialMin / dialMax, null = Auto). The gauge's own range
                                    //          rule is gone; with an older adc2can a page takes the catalogue's adc1-16
                                    //          range, now 0-5.5 V (was 0-5).
                                    // 0.36.3 - Fix: saving settings could crash the gauge (bench: picking Quad on the ADC
                                    //          source, again on every save after). settingsIncoming copied the whole
                                    //          settings (now ~4.9 KB) onto the 8 KB async_tcp stack to compare old and
                                    //          new; with the reply on top the stack overflowed (coredump: async_tcp,
                                    //          settingsIncoming -> sendOk -> _assembleHead). The copy lives in PSRAM now.
                                    // 0.36.2 - My Car sets itself and can be the boot logo. The first OBD scan (the
                                    //          dongle, the CAN broadcast or OBD over the gauge's CAN) whose VIN names the make
                                    //          saves the car as My Car by itself (CarChoice.confirmed = CAR_FROM_VIN) when no car
                                    //          is saved yet; a saved car is never replaced that way, and the K-line's text block
                                    //          doesn't count. Boot logo 7 = My Car: /logos/car.bin, the car's picture in its
                                    //          paint, drawn by the web UI (data/car.js) and uploaded whenever it's missing; the
                                    //          gauge deletes it whenever My Car changes. With no picture yet the boot screen shows
                                    //          the make and model as text. /api/vehicle: car.label ("SEAT Ibiza IV"), carLogo.
                                    // 0.36.1 - Any car's VIN names its make. The WMI table (src/can2gauge_Vehicle.cpp)
                                    //          grew from about 70 codes to 222 (every common maker, its plants and whole-block
                                    //          fallbacks), each with a make key, plain make names ("Volkswagen", not "Volkswagen
                                    //          SUV"). Your Car's make list has all of them; a make key it doesn't know shows
                                    //          under the VIN's own name. Older VW-group model codes: Golf I 17, Golf II 19 / 1G,
                                    //          Corrado 50, Scirocco 53, T3 25, T4 70, Lupo 6X, A1 8X / GB, Ibiza 6K / KJ. OBD
                                    //          09 02 over CAN takes the last 17 bytes (an ECU that pads in front still reads).
                                    // 0.36.0 - Your Car. The gauge reads the VIN from wherever it can: the OBD adapter (09 02 /
                                    //          22 F190, as before), the CAN broadcast (MQB / MLB gateway VIN_01, 0x6B4, no
                                    //          requests), OBD over its own CAN (22 F190 / 09 02 to 7E0, 18DA01F1, 18DA10F1,
                                    //          once, when First Boot or Detect Again asks) and the K-line (ME7 block 081,
                                    //          VIN + immobiliser ID; First Boot shows it for a moment). The VIN decode
                                    //          (make, VW-group model, year) moved to src/can2gauge_Vehicle.cpp. First Boot
                                    //          step 4 and Settings > Device > Your Car (data/car.js): the car's picture
                                    //          (data/cars: one drawing per body type; per-model drawings to come),
                                    //          make / model / year / body / paint, This Is My Car. The confirmed car is
                                    //          saved in NVS key "car" (outside the settings blob: no settings reset) and
                                    //          decides Match car and the DID Library's car group. GET /api/vehicle,
                                    //          actions vehicleConfirm / vehicleForget / vehicleDetect.
                                    // 0.35.0 - Triggers on page values. The Triggers tab's Signal list is grouped (Engine
                                    //          and car, Gearbox, Page values, ADC, ECU, Haldex, This gauge) and offers only
                                    //          what's there: a page value ("Page 3: Boost (K-line)") only when a page has
                                    //          one set up (the unused User 1-40 are gone), ADC / usb2ip / OpenHaldex values
                                    //          once heard, ME7 1-8 never (nothing fills them since 0.17.6). A switched-on
                                    //          trigger keeps its value read when its page isn't showing or is off: it
                                    //          counts like a page in the rotation for K-line (same-module session), the
                                    //          ME7 logger's variable list, the OBD adapter / diagnostics connecting, and its OBD
                                    //          PID joins the polled list (userSigWanted, userSigTriggerPids). Default page 8
                                    //          is Oil temp (was ME7 1).
                                    // 0.34.2 - Fix: the Diagnostics tab's DID Library picked the car group from the make's name, so
                                    //          an Opel / Vauxhall (and a make with no group) came up as the VW group. It
                                    //          now goes by the VIN's make key (opel -> Stellantis), then the name.
                                    // 0.34.1 - Web UI: a Quad or Triple page is titled by its source (ADC, CAN, OBD,
                                    //          or the ECU's name: IGNITRON, HALTECH ...) when the style, source or
                                    //          first value is picked. It took the first value's name ("ADC8").
                                    // 0.34.0 - UDS through the OBD adapter and a DID library. The Diagnostics tab can read its
                                    //          module through the Bluetooth / WiFi adapter (Connection), to 11-bit or
                                    //          29-bit ids (17FC0076, 18DA01F1) and with BMW extended addressing, on the
                                    //          gauge's CAN as well; the adapter switches between 11 and 29-bit for it
                                    //          (MQB Evo: OBD on 29-bit, most modules on 11-bit) and carries on with its
                                    //          OBD values in between (elmDiagTransact). A value is any run of 1-32 bits
                                    //          (Bit / Bits / Byte Order). DID Library card: OBDb's values for 21 car
                                    //          groups (data/vdid, tools/vdid/obdb_to_c2g.py), Add fills a slot. Service:
                                    //          faults through the adapter (UDS), Scan Modules (part number, name, fault
                                    //          count of every known module, 11 and 29-bit). Bluetooth OBD Read / Clear
                                    //          DTCs now do UDS on a 29-bit car too (it was OBD codes only there).
                                    //          Settings v29 (vdiagLink / TxExt / RxExt, 32-bit ids, bit-level values).
                                    // 0.33.1 - Graph page: Max and Min have a small solid box behind them. They sit
                                    //          inside the graph, and the line scrolling under their writing made the
                                    //          top left flicker.
                                    // 0.33.0 - Settings tidy (nothing released yet, so no migration): the settings are one
                                    //          list in sections that follow the web UI (Display, Units, Boot logo,
                                    //          Screensaver, Power, Output and lights, Pages, Triggers, Menu, CAN, OBD,
                                    //          K-line, ME7, OBD adapter, Gearbox, VAG diag, Oil, adc2can, OpenHaldex,
                                    //          usb2ip, Nav, Logger, Daisy chain, WiFi, Learnt by the gauge). A page holds
                                    //          everything about itself (source, slots, sprite, its chosen-source values),
                                    //          a trigger its No Reading. Gone: the v4..v27 migration code, the unused
                                    //          fade style and chain key, the spare bytes. Stored the right way round:
                                    //          useCanRelay, tabsShown, navPopupM (metres), bootFadeMs (always real ms).
                                    //          A value's VAG diag number and the OBD adapter's Adapter Source
                                    //          (elmExtraSource, ELM_EXTRA_*) have their own fields. The settings JSON
                                    //          has matching groups: units, boot, saver, io, obd, me7, learnt (haldex ids
                                    //          under haldex). Web UI, First Boot and the sim follow. A gauge flashed with
                                    //          it starts from defaults once; backups from before it don't restore.
                                    // 0.32.2 - Fix: boot loop on a chained gauge (boot sync on) - the boot sync task drew
                                    //          the logo's first frame with only a 3 KB stack and overflowed it. Now 8 KB
                                    //          (the task ends a few seconds after power-on).
                                    // 0.32.1 - Triggers. No Reading (per trigger, Triggers tab): Ignore (as before) or
                                    //          Trigger - a sensor unplugged / shorted / faulted reads as "no value", so
                                    //          an "ADC 8 below 3.5 V" alarm never fired when it was shorted. Fix: tapping an
                                    //          alert (or Clear Alerts) only hid it - it came straight back; now the trigger
                                    //          stays quiet until its value is back past the threshold. Fix: a switched-off
                                    //          latched trigger kept its output on. Fix: an alert the busy screen refused
                                    //          was never retried. Settings v27 (settings.triggerNoReading).
                                    // 0.32.0 - Icons on Single, Dual, Triple and Quad pages (one per value, the Custom
                                    //          page's set), plus six generic ones: thermometer, pressure gauge,
                                    //          voltage, spark plug, injector, brake. Every value gets one: the gauge's
                                    //          own signals by what they are, a DBC signal, OBD PID, K-line field, ECU
                                    //          channel, ME7 variable or ADC sensor by its name and unit ("Oil temp" in
                                    //          C = the oil can). Decimals: one rule for every display (signalDecimals):
                                    //          an extra value (Quad / Triple 2-4, Dual / Haldex second, Custom rows)
                                    //          shows its own - an ADC sensor's two places, an ECU channel's - instead
                                    //          of the page's. Fix: LVGL's memory pool 96 KB -> 512 KB (PSRAM): Custom's
                                    //          big value wasn't drawn at all (photo), and running out can stop the
                                    //          screen task.
                                    // 0.31.0 - Two new page styles. Graph: the page signal over the last 30 s as a
                                    //          rolling line (Min at the bottom, Max at the top, the Redline across
                                    //          it), the value above; every Graph page keeps recording while another
                                    //          page is shown. Triple: three numbers, the page signal on top and two
                                    //          more below. Numeric is now called Single and Oil (OEM) Oil (PQ), and
                                    //          the web UI lists the styles as Needle, Arc, Bar, Graph, Single, Dual,
                                    //          Triple, Quad, Haldex, Gear, Oil (PQ), Custom. Saved pages keep their style.
                                    // 0.30.19 - Fix: changing a page's style in the web UI could crash the gauge
                                    //          (Custom -> Quad) or freeze its screen for good (Quad -> a ring
                                    //          style). The new style was saved before the page was rebuilt, and
                                    //          the screen refreshed the old page as the new style in between.
                                    //          Each page now remembers what it was built as and waits for the rebuild.
                                    // 0.30.18 - Web UI: moving a page to Source ADC now asks "Pick a Sensor"
                                    //          instead of quietly showing sensor 1 while the page kept its old
                                    //          label and range (OIL TEMP). The sensor list shows each unit, (V)
                                    //          included, for sensors still on their default "ADC n" name.
                                    // 0.30.17 - The nav card now fades in from the boot logo like the gauge pages.
                                    //          Before, a phone that linked during the logo put the card straight up
                                    //          over it. A turn that comes in while the logo is up is filled in but
                                    //          kept hidden, and shown as part of the logo's fade.
                                    // 0.30.16 - Fix: the move from Off to Auto termination didn't stick (the "done" mark
                                    //          was saved at once, the setting itself only later), so Adam's bench gauges
                                    //          stayed Off: no 120R anywhere, no CAN frame got across, and the boot sync
                                    //          heard nobody. Now saved together (new mark "termAuto2", so those gauges
                                    //          are moved this time).
                                    // 0.30.15 - Auto CAN termination (settings.canTermination 2, the new default; a
                                    //          gauge still on Off is moved to Auto once, NVS "termAuto"). At power-up
                                    //          the boot sync task listens 1.2 s (+ 40 ms x (id & 7)): any frame = the
                                    //          bus is terminated, leave it off; silence (the bench) = switch the 120R
                                    //          on and send TERM (boot sync type 3) so the others leave theirs off. Car
                                    //          frames later = off again (ioTick -> bootSyncTermTick). TCA9554 writes
                                    //          locked (a mutex): the boot sync task and setupDisplay both use it now.
                                    //          Status: can.terminationOn; web UI CAN tab has Auto.
                                    // 0.30.14 - Boot sync handshake (Adam: the gauges still came up 3, 2, 1, each a
                                    //          little apart). Before the display, the gauge keeps the glass dark
                                    //          (backlightTarget() 0) and, once its logo is built, says READY on CAN
                                    //          every 10 ms. When as many gauges as last time are READY, GO counts down
                                    //          150 ms; each sets its logo to start at that moment (first frame drawn
                                    //          120 ms ahead, uiBootLogoRestart) and the backlight comes on at it. A
                                    //          missing gauge: started after 1.5 s anyway. The gauge count is learnt each
                                    //          power-up (NVS "bootGauges"). The catch-up stays as a backup.
                                    // 0.30.13 - Fix: boot sync - the slow gauge still sat a steady ~100 ms behind (Adam,
                                    //          off serial too). The jump point was worked out before waiting for the
                                    //          LVGL lock (held while a frame is drawn), so every jump landed short by
                                    //          the wait. uiBootLogoCatchUp now takes when the other logo went up and
                                    //          works out the point once it has the lock.
                                    // 0.30.12 - Fix: boot sync - the slowest gauge to start (Adam: gauge 1) still lagged.
                                    //          Reading the logo's clock took the LVGL lock for at most 50 ms; on a
                                    //          gauge still busy starting up a missed lock read as "logo gone" and its
                                    //          sync stopped. The clock is now read without the lock, a jump waits up to
                                    //          200 ms (and is retried), and the others send for 3.5 s (was 2.5 s). The
                                    //          boot log says how many sync frames were heard.
                                    // 0.30.11 - Boot sync (src/can2gauge_BootSync.cpp): the row's boot logos sweep
                                    //          together. For the first 2.5 s of the logo each gauge sends its logo
                                    //          time on CAN (base + 0x21, every 20 ms, even with link frames on CAN
                                    //          off; back to that setting after), and a gauge behind jumps its sweep
                                    //          and fade forward to match (GaugeUI uiBootLogoJumpTo). CAN handlers 8 -> 10.
                                    // 0.30.10 - Fix: with a chain, giving one gauge the ignition only moved the gauges
                                    //          on switched power across; a gauge set to anything else (Adam: gauge 2
                                    //          left on the wire) kept it and stayed asleep, and still showed as
                                    //          following. WHICH gauge has the ignition is now a shared setting
                                    //          (SharedBlock layout 2, its WiFi MAC): the last gauge given the CAN or
                                    //          wire ignition wins and every other gauge switches to "Another gauge in
                                    //          the chain"; set that gauge back to switched power and they all go back.
                                    //          The old heartbeat-only follow is gone. The Gauge Numbers list shows the
                                    //          last 4 hex of each MAC, the same as the end of its WiFi name.
                                    // 0.30.9 - Fix: a needle dial's tick marks showed through the nav card (faint
                                    //          horizontal lines down the left of the ring, more each time the card
                                    //          came and went; photo 2026-10-30, not the glass: swapped displays, cold).
                                    //          The page under the card is now hidden while the card is up (and shown
                                    //          again when it goes: tap, route end, ignition off), so nothing under it
                                    //          is drawn. Safe if the page changes or is rebuilt meanwhile.
                                    // 0.30.8 - Nav ring clearer (Adam: black / white on the band, hard to read): a 3/4
                                    //          circle open at the bottom, in the theme's second accent on a dark
                                    //          outline, grey for the part still to go; it fills from a start dot
                                    //          (bottom left) over the top to a chequered flag (bottom right)
                                    //          as the journey goes on (Adam: the ring is the whole route, the flag
                                    //          the destination). The app works out "journey done" from Maps' bottom
                                    //          line (distance left, else time left, else the arrival time against the
                                    //          phone's clock) against the first reading of the route, and sends it in
                                    //          the STATE packet's new bytes 11-12 (thousandths, 0xFFFF = unknown; the
                                    //          text now starts at byte 13). Full on arrival. Arrival time line
                                    //          300 -> 260 px wide.
                                    // 0.30.7 - Fix: the nav card on the glass was a stray line with no text (first real
                                    //          drive): it was cut round with clip_corner, which makes LVGL draw it
                                    //          into a separate 480 x 480 layer first, and that failed on the gauge.
                                    //          Now a plain square card (the corners are off the round glass anyway).
                                    //          New: a distance ring round the edge, full at the pop-up distance and
                                    //          emptying to the turn (UiNavView hasDistance / distToTurnM / ringFullM).
                                    // 0.30.6 - Fix: boot loop with Bluetooth OBD and Navigation both on (since 0.30.0): the
                                    //          OBD task and setupNav started Bluetooth at the same time ("Setting up
                                    //          Bluetooth..." twice, then a double exception on core 0). bleInit now
                                    //          lets one task at a time start it (the other waits), and setupNav runs
                                    //          before setupELM.
                                    // 0.30.5 - Navigation has its own web UI tab, next to Log (always there; was a
                                    //          card on Bluetooth OBD). Fix: the nav card could stay on the glass
                                    //          for good (Show Test Turn "doesn't clear"): uiNavShow / uiNavHide
                                    //          gave up if the screen was busy for 200 ms but navTick counted it as
                                    //          done; now they return false and navTick tries again. navTick checks
                                    //          every 5 s that the gauge is advertising while no phone is connected
                                    //          and starts it again if not (the start was never checked); the web UI
                                    //          says whether the phone can see the gauge.
                                    // 0.30.4 - Shared settings (src/can2gauge_Shared.cpp): ignition / shutdown, screensaver,
                                    //          look (theme, brightness, feel, units) and boot logo are kept the
                                    //          same on every gauge in the chain, over ESP-NOW. Set them on any
                                    //          gauge: a revision number (NVS "sharedRev") one past any heard wins;
                                    //          same revision, different values = the master's win. The heartbeat
                                    //          carries ignKind, sharedRev and a CRC so a gauge that missed one is
                                    //          sent it again. A gauge on switched power switches itself to
                                    //          "Another gauge" when it hears one with its own ignition. Daisy
                                    //          Chain gauge table: each gauge's MAC and its part in the ignition.
                                    // 0.30.3 - Ignition from another gauge (ignSource 3, IGN_SRC_CHAIN): a gauge follows
                                    //          the chain - it sleeps and wakes with a gauge that has its own CAN /
                                    //          wire ignition. Heartbeat role byte bit 6 = "my own ignition is on"
                                    //          (never set on switched power or by a follower, so followers can't
                                    //          keep each other awake). Also watched in the CAN hook, so a follower
                                    //          woken from deep sleep by the chain's CAN frames can check it before
                                    //          the link runs. Chain on WiFi only: no deep sleep (can't wake on it).
                                    // 0.30.2 - the shutdown sprite defaults to Rev (was none): on switched power it
                                    //          never plays, on an ignition source it does, and Play It Now / the
                                    //          row wave work on a new gauge. A gauge already set up keeps its choice.
                                    //          Fix: a gauge with no shutdown sprite crashed at every ignition on
                                    //          (coredump: the io task's 3 KB stack ran out drawing the cross-fade
                                    //          back to the page). The wake now always finishes in uiTick, and
                                    //          showScreen only draws its own slides / fades in the lvgl, uiTick and
                                    //          web server tasks (others, e.g. the link task's "follow page", get
                                    //          LVGL's animation).
                                    //          Text kept inside the round glass: glassLabel() / fitOneLine() cut a
                                    //          line that changes (menu status, mode screen title / header / hint,
                                    //          page names and status lines, DSG lines, quad slots, row values) to
                                    //          the glass's width at its height, ending in "..." ("Bluetooth OBD:
                                    //          not connected, retry 30s" ran off the menu).
                                    // 0.30.1 - Nav card arrows kept on the gauge, like the boot logos (data/nav/*.bin,
                                    //          drawn by tools/nav_arrows.py, zipped by fs_pack.py, ~17 KB): straight
                                    //          on, left, right, slight left / right, U-turn, roundabout exits 1-5
                                    //          (clockwise, UK), arrived. The app picks one from Maps' words ("take
                                    //          the 2nd exit") and sends its number in the STATE packet (byte 2);
                                    //          wording it doesn't know still sends Maps' own arrow as dots. Show
                                    //          Test Turn uses the stored right arrow. Fix: lv_image_cache_drop is
                                    //          private in LVGL 9.6 (didn't compile).
                                    // 0.30.0 - Nav link: Google Maps turn-by-turn on the glass, Android Auto style
                                    //          (src/can2gauge_Nav.cpp, docs/nav-link/NAV_LINK.md). The new can2gauge
                                    //          Link phone app (android/) reads Maps' navigation notification and
                                    //          sends the next turn (arrow, distance, road, arrival time) over
                                    //          Bluetooth LE. The card comes up near a turn (pop-up distance, 300 m
                                    //          by default) or for the whole route; a tap hides it until the next
                                    //          turn. Web UI > Bluetooth OBD > Navigation: Nav Link, Pop-up
                                    //          Distance, Show Test Turn. NimBLE gets its peripheral + broadcaster
                                    //          roles back and 2 connections (OBD adapter + phone). Settings stay v26
                                    //          (the two spare26 bytes: navMode, navPopupDist).
                                    // 0.29.1 - Gauge Numbers fixes: the Role / Number pickers closed themselves (the
                                    //          table was redrawn every second: the browser rewrites innerHTML, so
                                    //          it never matched). Show Numbers leaves the screensaver / Rev Runner
                                    //          so the number shows, and the screensaver doesn't start over it.
                                    //          Switching the screensaver off now takes it away even when it was
                                    //          put up by hand (Show It Now), if the switch was on at the time.
                                    // 0.29.0 - Gauge Numbers (Daisy Chain tab): every gauge's number (its place in the
                                    //          row, 1 = leftmost) and role (Auto / Master / Follower) set from ANY
                                    //          gauge, over the link (CMD_SET_ROW / CMD_SET_ROLE). Only one gauge can be
                                    //          set to Master. Show Numbers puts each gauge's number big on its own
                                    //          glass for 15 s (CMD_SHOW_NUMBER); a gauge whose number or role is
                                    //          changed shows its new one for 6 s. Play It Now says which gauges join
                                    //          and which can't (heartbeat bit LINK_SRC_SHUTDOWN: no shutdown sprite).
                                    //          Fix: the command count started at 1 after every boot, so the other
                                    //          gauges could take a rebooted gauge's first command for a repeat and drop
                                    //          it - it starts at a random number now.
                                    // 0.28.3 - no command key: the chain's wireless commands and chain updates are
                                    //          signed with one key built into every can2gauge (CHAIN_KEY), so
                                    //          gauges in the same group just work. The key field is gone from the
                                    //          Daisy Chain tab; a key saved before is ignored.
                                    // 0.28.2 - ~45 KB of internal RAM handed back: the gauge's own fixed buffers
                                    //          (settings, the OBD terminal and bench-test text, the ME7 logger's
                                    //          tables, the theme picker list, the chain update's packets, the web
                                    //          replies' copies ...) live in PSRAM, marked EXT_RAM_BSS_ATTR
                                    //          (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY). They were ~75 KB of
                                    //          internal RAM all the time; WiFi, Bluetooth and task stacks get it.
                                    // 0.28.1 - Update Other Gauges no longer says "not enough free RAM": its
                                    //          task (8 KB internal RAM) is made once at boot and kept, rather
                                    //          than when the button is pressed (or a HELLO comes in), by when the
                                    //          RAM was too broken up to find 8 KB in one piece, even with
                                    //          Bluetooth off. The gauge being updated had the same silent failure.
                                    // 0.28.0 - DBCs feed the gauge (docs/CAN_DBC_plan.md, phase 1). A DBC signal with a
                                    //          GaugeKey sets that gauge signal straight from each frame, in the gauge's
                                    //          units (kPa -> mbar ...), with value labels for gear / selector / "no
                                    //          value" codes and GaugePriority (1 s hold). Up to three DBCs at once
                                    //          (car, gearbox, extra; an id clash leaves the later slot out), a
                                    //          relocatable one moved to its slot's base id. .c2d version 2 (keys,
                                    //          priority, V / K lines). The library gains the gauge's own ECU DBCs
                                    //          (EMU Black, MaxxECU, Link x2, Haltech) and overlays for vw_mqb /
                                    //          vw_mlb. New signal baro (boost against the ECU's baro). Settings v26.
                                    //          The hand-written PQ / MQB / MLB / MaxxECU decoders stay until each
                                    //          DBC is checked against them on a car.
                                    // 0.27.0 - a unit per quantity (src/can2gauge_Units.*): speed, boost, other
                                    //          pressures, temperature, mixture (lambda / AFR), distance; Metric /
                                    //          Imperial set them all, Custom picks each (web UI > Display). The dials
                                    //          follow: needle, arc, bar, redline and scale work on the shown value, and
                                    //          a change turns every page's Min / Max / Redline over (rounded; 280 km/h
                                    //          -> 175 mph), from the web UI or the glass's Units button. A gauge already
                                    //          on Imperial has its pages turned over once at the update. Logs,
                                    //          triggers and the API stay in the stored units. Settings v25.
                                    // 0.26.0 - ZF 8HP gearboxes (docs/8HP_research.md). Gearbox type (DSG / ZF 8HP
                                    //          Audi-VW / ZF 8HP BMW) fills in the diagnostic reads as presets; three
                                    //          more reads (temperature every 10th pass, converter lock-up, selector)
                                    //          and a "table" format for BMW's codes; BMW extended addressing (EGS
                                    //          0x18 in front of every frame) on the gauge's CAN and through the
                                    //          adapter (ATCEA / ATFCSM1, put back after each pass). New CAN profile
                                    //          VAG MLB (A6/A7 C7, A8 D4, Q5 8R). MQB adds shift state, lock-up and
                                    //          gearbox input speed. New signals shifting / lockup / inputRpm /
                                    //          convSlip. "DSG" is "Gearbox" on the menu and web UI. Settings v24.
                                    // 0.25.4 - internal RAM (Tiguan drive: two freezes, 2.4 KB at the lowest with
                                    //          Bluetooth OBD and a phone on). Every JSON reply is built in PSRAM
                                    //          (psramJsonAllocator, can2gauge_RAM.*): ArduinoJson's copies of each text
                                    //          value were small pieces of internal RAM, a few KB per /api/elm. The USB
                                    //          console's reply Strings sized up front. A memory request that fails is
                                    //          noted (size, kind, task), kept through a reset, printed at the next boot
                                    //          and shown on Diag ("RAM ran out", /api/status ramFail).
                                    // 0.25.3 - Bluetooth OBD on 29-bit CAN cars (2025 Tiguan, protocol 7): the
                                    //          engine header is 18 DA <engine> F1 (its address from the engine's
                                    //          01 00 answer, the one listing the most PIDs) instead of the 11-bit
                                    //          7E0 - every request came back NO DATA. Also put back after the
                                    //          gearbox read and DTC read / clear. The gearbox read through the
                                    //          adapter works there too: 18 DA <gearbox> F1, its address from the
                                    //          other ECU answering 01 00 (VW 0x02). Needle pages: the needle sweeps
                                    //          over the name, number and unit (they were drawn on top of it).
                                    //          Boot logo sweep 3 s (was 1.4), held 0.7 s, default fade 2000 ms
                                    //          (was 1500). Web UI: switching a page's Source to OBD no longer
                                    //          silently picks Engine load (PID 04).
                                    // 0.25.2 - the daisy chain is on from first boot (it was off, so a new gauge
                                    //          never heard the others and First Boot Setup's "Other gauges"
                                    //          check always said nothing heard). ESP-NOW only: frames on the
                                    //          car's CAN still wait for "allow link frames". A gauge set up
                                    //          before keeps its saved setting. The can2gauge boot logo's own
//          dial sweeps (up to full, back to 72 %, ticks lighting as it passes)
//          instead of a ring round the edge; the logo file has the dial empty and
//          a smaller wordmark (34 px, was 40: it ran to the edge of the glass).
                                    // 0.25.1 - screen rotation: Settings > Display > Screen Rotation turns the whole
                                    //          picture round the middle of the glass - Upside Down (180) and Rotation
                                    //          Trim (+-15 degrees in half-degree steps) for a gauge that sits crooked.
                                    //          The driver turns each finished frame into the glass's buffer, touch is
                                    //          turned back to match (lib/LVGL_Driver/src/LVGL_Rotate.*, SCREEN_ROTATION
                                    //          0 leaves it out). Settings v23.
                                    // 0.25.0 - chain update: Daisy Chain > Update Other Gauges sends this
                                    //          gauge's firmware and web files to the other gauges over ESP-NOW, one
                                    //          at a time. Firmware straight from the running slot (skipped when the
                                    //          same), web files only where they differ (each gauge keeps its own .ecu /
                                    //          logs; fs_manifest.txt lists the release's files). Signed with the chain
                                    //          key, parked only. The ESP-NOW heartbeat carries the full version.
                                    // 0.24.29 - the can2gauge boot logo sweeps a ring round the edge, as the OpenHaldex
                                    //          one does. No smudge before the logo: the backlight waits for its first
                                    //          frame (it lit up on the first gauge page drawn behind it).
                                    // 0.24.28 - screen on sooner: boot no longer waits 2 s for a PC on USB when
                                    //          none is plugged in. Boot logo -> page fade adjustable (Display >
                                    //          Boot fade, default 1500 ms, was 400). Boot timing in the log.
                                    // 0.24.27 - the large-number page (and the progress screen's %) grew its
                                    //          doubled value from the top-left corner: now from the middle, centred.
                                    // 0.24.26 - Fade style setting removed (cross-fade always; the backlight dip
                                    //          only in partial drawing mode). Page slide 800 ms by default (was
                                    //          250; the old default moves up). The menu content slide stays 250.
                                    // 0.24.25 - flicker (test alert) fixed: the frame-buffer swap is timed on the
                                    //          driver switching buffers, not the vsync pulse that comes just
                                    //          after it (a frame landing between the two was drawn over on glass).
                                    // 0.24.24 - fades are a true cross-fade (pixels mixed, as smooth as the
                                    //          slides) or the backlight dip: Settings > Display > Fade style
                                    //          (settings v22). Fixed the flick as a slide settled (LVGL's next
                                    //          frame could be drawn on the buffer on the glass).
                                    // 0.24.23 - smooth page changes: slides are played by the display driver
                                    //          (new screen drawn once, then moved in as a still picture -
                                    //          LVGL redrew both screens every frame, 4-5 frames a slide); fades
                                    //          dip the backlight (PWM) and swap the screen while it is dark.
                                    // 0.24.22 - Bluetooth OBD switched on after boot didn't start (RAM already
                                    //          taken by WiFi / web server) and just sat there: state "restart
                                    //          needed" and a Restart Now button on the OBD tab.
                                    // 0.24.21 - WiFi usable with Bluetooth OBD on: a Bluetooth connect attempt
                                    //          listened on the shared radio 100 % of the time (NimBLE default),
                                    //          now 25 %; with the adapter missing 3 times in a row, retries every
                                    //          30 s instead of 5 s. Web taps took seconds before.
                                    // 0.24.20 - bench: GET /api/tasks on the USB console - CPU share per task and
                                    //          core, stack spare, internal RAM split (plain malloc / DMA / PSRAM),
                                    //          to find what holds the web UI up with Bluetooth on.
                                    // 0.24.19 - internal RAM for the web UI with Bluetooth on: plain malloc() from
                                    //          1 KB (was 4 KB) goes to PSRAM, and 16 KB of internal RAM is kept for
                                    //          the WiFi / Bluetooth drivers (custom_sdkconfig). 4 h bench run had
                                    //          dipped to 3.5 KB free and the web page barely loaded.
                                    // 0.24.18 - Bluetooth + WiFi + the fast framework together. The rebuilt framework
                                    //          started PSRAM late (template lacked SPIRAM_BOOT_INIT), so the global
                                    //          psramCalloc tables fell back to internal RAM (~30 KB); fixed, RAM at start
                                    //          207 KB as the ready-made framework. Its generic template also had WiFi TX
                                    //          buffers static in internal RAM: dynamic now, as the ready-made one; WiFi
                                    //          buffers and three framework task stacks trimmed. Unused Arduino components
                                    //          (RainMaker, Zigbee, ...) left out of the framework build.
                                    // 0.24.17 - internal RAM for Bluetooth: WiFi / lwIP IRAM optimisations and WiFi CSI off
                                    //          (custom_sdkconfig). Boot log "RAM at start" (free / total internal).
                                    //          0.24.16's test (XIP off) boot-looped - the IRAM-safe RGB interrupt reads
                                    //          the PSRAM frame buffer while flash is busy, which needs XIP - and XIP
                                    //          wasn't the RAM cost; back to 0.24.15's framework settings.
                                    // 0.24.16 - TEST: program-from-PSRAM (CONFIG_SPIRAM_XIP_FROM_PSRAM) off again. 0.24.15
                                    //          was smooth (alert 15 fps, sprite decode 9.6 ms) but had ~33 KB less
                                    //          internal RAM from boot (24 KB after setup) and Bluetooth refused to start.
                                    // 0.24.15 - framework rebuilt for the glass (platformio.ini custom_sdkconfig, in
                                    //          can2gauge's own packages folder): 64-byte data cache lines, the RGB
                                    //          panel's interrupt in internal RAM, the program run from PSRAM (flash
                                    //          reads / writes no longer stall the glass's feed). Alert: only the box
                                    //          pulses (solid, dark <-> bright red); the full-screen glow stays steady -
                                    //          its pulse re-blended the whole glass every step (~177 ms: jumps).
                                    // 0.24.14 - LVGL compiled -O2 for speed (tools/lvgl_speed.py; the rest stays -Os, app
                                    //          slot ~92 % full). Direct drawing: the stale vsync is cleared after the
                                    //          buffer is handed over, not before - a vsync in between let LVGL draw the
                                    //          next frame into the buffer still on the glass (ghost / half-drawn lines).
                                    // 0.24.13 - Clear Alerts works while an alert is up: uiClearAlert waited 50 ms for the
                                    //          LVGL lock, but the alert's full-screen pulse held it ~165 ms a frame, so
                                    //          the clear nearly always gave up (bench: 6 tries). Now a clear that can't
                                    //          get the lock is done at uiTick's next pass. Measured: a still page with
                                    //          the test alert up redrew the whole glass 6x a second (98 % busy); with it
                                    //          cleared, 0 fps.
                                    // 0.24.12 - Bluetooth stops again after a scan with the OBD adapter off (First
                                    //          Boot's check started it and it stayed until a reboot: internal RAM fell to
                                    //          1 KB and WiFi stopped answering). The page dots are drawn circles (they
                                    //          were "●" / "·" text the built-in font lacks: empty boxes on the glass).
                                    //          Turning the screensaver off closes it at once (Show the Screensaver Now
                                    //          still works with it off).
                                    // 0.24.11 - K-line: with nothing answering since power-up, the background protocol
                                    //          and the rotation's page session stop after 3 wake-ups with no answer
                                    //          (they tried every ~3 s for ever - on the bench, "no 0x55" all evening).
                                    //          A K-line page on the glass, a K-line settings change, Reconnect, or an
                                    //          ignition off / on starts it again; once an ECU has answered it keeps
                                    //          trying. First Boot: a glass with no touch shows the login card, not
                                    //          the Yes / Not Now question it can't answer.
                                    // 0.24.10 - USB console stack back to 8 KB: 0.24.6's 6 KB overflowed on a POST
                                    //          /api/settings and rebooted the gauge. Sprite pacing only steps down after
                                    //          the smaller step has fitted for 3 s (0.24.9 flicked between every 2nd and
                                    //          3rd frame), decode time smoothed over 8. Layered decode: palette and row
                                    //          tables in internal RAM (~1.4 KB), shifts for the glow's divisions.
                                    //          Measured on 0.24.9: unzip 61 -> 3 ms with the clip cache; decode still
                                    //          70-120 ms, drawing 77-120 ms on the saver.
                                   // 0.24.9 - smoother sprites / screensaver. Clip cache: the playing clip's frames are kept
                                   //          unzipped in PSRAM as they are first decoded (Rev: 5 KB - 1.5 MB a clip,
                                   //          made only with 1 MB of PSRAM to spare), so every loop after the first
                                   //          skips the flash read and the unzip (~61 ms of a ~96 ms decode). Steady
                                   //          pacing: when decode or drawing can't keep the clip's speed (Rev: 15 fps),
                                   //          it shows every 2nd-4th frame at an even rate instead of whichever frame
                                   //          is due (jumpy). Boot log says when the step changes.
                                   // 0.24.8 - the RGB panel's interrupt back on core 1 (set up from setup() again). 0.24.7's
                                   //          move to core 0 drew a saver frame in 70-100 ms (was ~155) but decoding one
                                   //          took ~252 ms (was ~96, unzip 61 of it) - no net gain - and WiFi's start-up
                                   //          broke up the boot logo. The unzipper's working area back in PSRAM too: with
                                   //          the saver playing and a phone on the web UI, internal RAM fell to 4 KB
                                   //          (web page wouldn't load). 17 MHz and the unzip timing stay.
                                   // 0.24.7 - sprite / screensaver speed (Rev saver measured 6-8 fps: ~96 ms to decode a
                                   //          frame, ~155 ms to draw it). The unzipper's ~11 KB working area goes in
                                   //          internal RAM while a sprite plays, when 24 KB stays spare (its tables are
                                   //          read for every byte); the RGB panel is set up from core 0 so its
                                   //          bounce-buffer interrupt no longer takes LVGL's core; pixel clock 18 ->
                                   //          17 MHz (less PSRAM traffic). /api/status perf.unzipAvgUs and a Diag row
                                   //          show the unzip part of a sprite frame.
                                   // 0.24.6 - internal RAM with Bluetooth OBD on (17 KB left, largest piece 9 KB: phones
                                   //          struggled to join the AP; 55 KB with Bluetooth off). Bluetooth controller
                                   //          sized for what the gauge does (3 activities, was 6; 20 scan entries, was
                                   //          100 - include/can2gauge_nimble_cfg.h). Console and LVGL task stacks
                                   //          8 -> 6 KB each.
                                   // 0.24.5 - direct drawing back: LV_DRAW_BUF_ALIGN 64 -> 16 (lv_conf.h) so LVGL takes the RGB
                                   //          frame buffers. 0.24.4's strips in PSRAM tore (each strip copied into the
                                   //          buffer on the glass) and loaded PSRAM; direct swaps whole frames at vsync.
                                   //          Boot log shows both frame buffer addresses.
                                   // 0.24.4 - black glass found (bench 2026-10-26, two boards): the glass was fine (0x23 turned
                                   //          it white) but LVGL never drew a frame - 0 fps even in the benchmark. Direct
                                   //          mode (forced when internal RAM is short) hands LVGL the RGB frame buffers,
                                   //          and LVGL silently refuses buffers off its 64-byte boundary (log off), then
                                   //          skips every refresh. Now direct mode is only used when both are aligned,
                                   //          otherwise strips - in PSRAM when internal RAM is short. The boot log
                                   //          says which, with the frame buffer address.
                                   // 0.24.3 - glass bring-up bench tools (USB console / POST /api/action): "panelCommand"
                                   //          sends one ST7701 command to the glass (0x23 = every pixel on), and
                                   //          "panelReload" resets it and sends the register list again; either can
                                   //          clock the bits out by hand on SCK / SDA ("bitBang") instead of the SPI
                                   //          peripheral. 0.24.2's timing didn't bring the picture up.
                                   // 0.24.2 - YouriTech ET021CR01-Z glass blank on the first board (backlight on, black
                                   //          picture, two glasses the same). Its RGB timing was still the Waveshare
                                   //          glass's: 16 MHz pixel clock (its sheet says 17-27) and short porches. Now
                                   //          its own from the datasheet: 18 MHz, horizontal porches 60 / 60, vertical
                                   //          front porch 20 (Waveshare build unchanged). When the touch controller
                                   //          doesn't answer at 0x15 the boot log lists every address on the I2C bus.
                                   // 0.24.1 - Bluetooth OBD speed and drop-outs (bench 2026-10-24 night, ELM327 v2.3 BLE clone,
                                   //          8N0906018J on ISO 9141-2). Writes go without response when the adapter
                                   //          allows it (one radio round trip less per command), the connection asks
                                   //          for 7.5-15 ms (was 15) and a 4 s supervision timeout (was 1.5 s); the link
                                   //          info shows what the adapter agreed to. New "speed" bench test: 15 x 010C
                                   //          per adapter timing choice, round trip and answers. Found: the "one answer"
                                   //          digit is what counts (254 -> 184 ms a request on ISO 9141-2), so K-line /
                                   //          J1850 requests always carry it now, not only with Fast OBD (3.9 -> 5.4/s).
                                   //          After a dtc / clear / speed test on a K-line car the adapter is no longer
                                   //          reset: the reset made it wake the ECU while the ECU still held the last
                                   //          session (up to 5 s), got UNABLE TO CONNECT and dropped the Bluetooth link
                                   //          (reason 534) - every Read DTCs did it. A K-line protocol that doesn't answer
                                   //          straight after a reset is tried again after 5.5 s before the auto search.
                                   //          One-PID-per-request polling (K-line / J1850) gives the page on the glass
                                   //          3 requests in 4: its value ~4 times a second (was ~0.8). /api/elm pids carry
                                   //          each one's age; /api/status heapMin / heapBlock (internal RAM); bench-only
                                   //          K-line listener (POST /api/kline/listen) times the bytes on the line.
                                   // 0.24.0 - packed filesystem. The image was 97.5 % full (236 KB free): writing a bench
                                   //          test log on it was slow enough to trip the task watchdog (Bluetooth OBD
                                   //          Read DTCs reset the gauge, 2026-10-24). tools/fs_pack.py now builds the
                                   //          image from a packed copy of data/: web scripts / styles, make logos and
                                   //          the label index zipped (.gz), sprite packs as C2GS version 2 with
                                   //          every frame zipped (Rev 5.9 MB -> 2.8 MB). lib/FsUnzip reads them with
                                   //          the ROM's tinfl: logos and the label index unzip into PSRAM as they are
                                   //          read, a sprite frame just before it is decoded. ~47 % free now. A plain
                                   //          file is still read first, so an older image keeps working.
                                   // 0.23.1 - web UI tidy after a first look: Move Up / Move Down on each gauge (Gauges
                                   //          tab, saves at once); First Boot step 3 is "Gauges", steps 2 / 3 lose their
                                   //          explanation lines, the setup table keeps to the card's width (shorter
                                   //          choices, Menu and Tabs on a row of its own); "Menu and Tabs" capitalised; board temp
                                   //          and uptime are no longer "seen" values in the First Boot check.
                                   // 0.23.0 - web UI: Basic / Advanced. An "Advanced" switch (phone menu and Settings >
                                   //          Device, remembered in the browser) - Basic shows the everyday options;
                                   //          the fine-tuning sits under a "More Options" fold on each card, and the
                                   //          tool pages (Diagnostics, System) and the CAN / OBD / ME7 inspection cards only show
                                   //          in Advanced. The Gauges tab is one line per gauge (tap to open it, one at
                                   //          a time); Settings is split into sections (Display, Logo & Sprites, Power &
                                   //          Wiring, WiFi, Device) shown one at a time; the help text on a card sits
                                   //          behind an (i) button on its title. Web UI only - no settings changed.
                                   // 0.22.0 - Bluetooth OBD (ELM327) bench session, gauge as the BLE client. The USB
                                   //          bench console now drives the Bluetooth OBD tests too (GET /api/elm,
                                   //          POST /api/elm/action - scan / pair / cmd / test), so the whole adapter
                                   //          path can be run over the cable (tools/diag/kline_bench.py elm ...).
                                   //          Fixes: (1) a bench test now shows as running the instant it is asked for
                                   //          (elmStartTest) instead of leaving the previous test's result up until the
                                   //          ELM task catches up - the web UI's Run All showed the old result and kept
                                   //          its buttons live in that ~1 s gap; a dropped adapter now also cancels a
                                   //          pending / running test so it can't latch. (2) the K-line-via-adapter test
                                   //          quiets the line 5.2 s first, so an ISO 9141-2 OBD session the ECU still
                                   //          holds (no StopCommunication) can lapse before the first 5-baud init.
                                   //          (3) new "canmon" bench test: force the adapter to 11-bit 500k CAN and
                                   //          ATMA-monitor the broadcast frames, so a car's CAN can be read through the
                                   //          adapter even with no OBD-on-CAN responder (and a quick pin 6/14 check).
                                   //          (4) the K-line test also tries an auto-init (init on the first request,
                                   //          the way OBD connects) as well as the explicit ATSI/ATFI.
                                   //          Bench 8N0906018J (TT ME7.5, answers ISO 9141-2): BLE connect, live OBD
                                   //          PIDs and DTC read (03 / 07) all work through a BLE ELM v2.3 clone, and
                                   //          canmon reads the ECU's 0x280 broadcast frames (pins 6/14 right way round;
                                   //          the bench bus is under-terminated so only the one id came through). VAG
                                   //          measuring blocks through the adapter's own K-line do NOT work: the clone
                                   //          can't init a VAG module address (0x01 / 0x11) by ANY route - 5-baud, fast,
                                   //          ISO 9141 or auto-init all get BUS INIT ERROR, while it inits OBD 0x33
                                   //          fine. The gauge's own K-line reads the blocks; this needs a better
                                   //          adapter (genuine ELM / STN) for a wireless VAG path.
                                   //          (5) new "Adapter Source" (settings.elmKlMode 2): the adapter stays in
                                   //          CAN monitor (ATMA) and every frame it reads is injected into the gauge's
                                   //          CAN pipeline (canInject), so pages reading a DBC / profile signal get the
                                   //          car's CAN wirelessly - for cars that broadcast on CAN but answer no OBD.
                                   //          Re-arms ATMA if the stream stalls. Bench: 136 frames/s of 0x280 reach the
                                   //          gauge (air frames), decoded by the vw_pq profile. (elmKlMode is now 0 off /
                                   //          1 VAG K-line blocks / 2 CAN monitor - kept in that field, not a new one, to
                                   //          avoid a settings-blob version bump.)
                                   // 0.21.0 - Log tab: the data logger records the active gauges. A "Channels" choice,
                                   //          Active Gauges (default) / Chosen Below: Active Gauges logs the values the
                                   //          enabled gauge pages show, in gauge order (gauge 1 first), so an ME7 /
                                   //          K-line / OBD value a page shows (e.g. rl_w) is logged without hand-picking
                                   //          it, and the graphs come out in gauge order. Chosen Below is the old
                                   //          hand-picked list. Settings tab: Import Backup restores a Download Backup
                                   //          file (all settings) and reboots.
                                   // 0.20.0 - ME7 logger: pick the logged variables with checkboxes. The ME7 logger
                                   //          card lists the .cfg's variables ticked; Find adds any .ecu variable,
                                   //          untick drops one (up to 32). Saving writes the .cfg's [LogVariables]
                                   //          (POST /api/me7/cfg) and reconnects, and the logged variables then show
                                   //          in the gauge-page sources ("In your .cfg" first). No more hand-editing
                                   //          and re-uploading the .cfg.
                                   // 0.19.0 - ME7 fast logging (B7 RAM routine), WORKING on the bench 8N0906018J at
                                   //          ~50 samples/s (was 34 Normal). K-line tab "Fast log" = Normal / Test
                                   //          link / Fast (POST /api/me7/fast); Normal / Fast saved in
                                   //          settings.me7FastMode. Fast arms it; on the next logging connect the
                                   //          gauge writes our own clean-room C166 routine into ECU RAM at 0x387A00
                                   //          and repoints the ECU's measurement mapping-table base at it (the EXTP
                                   //          pointer at 0xE228/0xE22A, written {offset,page} - NOT a linear address,
                                   //          and NOT 0xE226 which is a computed index), then one 0xB7 per sample runs
                                   //          it and returns the whole block (1 request a sample instead of 3). The
                                   //          handler reads each variable byte-wise (C166 word reads must be
                                   //          even-aligned; byte vars sit at odd addresses). RAM only, un-hooks on
                                   //          stop / clears at ignition-off, falls back to normal 0x21/0x23 reads if
                                   //          B7 doesn't answer. v0 marker handler for the bench Test link (B7 ->
                                   //          "C2G!"), v1 for Fast. (docs/me7logger.md, docs/me7_ramroutine_re.md 9)
                                   //          Web UI: the K-line tab shows only the section for the chosen protocol
                                   //          (ME7 logger card with the ME7 logger, K-line mode card otherwise).
                                   //          Also K-line protocol "KWP2000 VAG": faster VAG measuring blocks via the
                                   //          double slow-init at 0x01 (NefMoto lead) - same groups/gauges as KWP1281,
                                   //          one 0x21 per group over KWP2000, falls back to KWP1281 if the module
                                   //          won't come up KWP2000. Persisted like any protocol choice, so it
                                   //          auto-connects KWP2000 and feeds the gauges. UNTESTED - bench first.
                                   // 0.18.0 - ME7 read: read a locked ECU (one that refuses SecurityAccess, 0x27 ->
                                   //          7F 27 10) with ReadMemoryByAddress (0x23) from the flash window at
                                   //          0x800000 - no 0x27 login needed. An REA of 16 ME7 images showed the
                                   //          flash gate is an integrity word at 0xE048, not a login, and a stock
                                   //          checksum-valid ECU already passes it, so it reads with NOTHING written
                                   //          to the ECU. Only a modified flash needs the fallback: forge 0xE048 (the
                                   //          per-ECU permit value, "Permit" box in the UI) + the 0xE074 grant bit in
                                   //          RAM, which clears at ignition-off. REA in docs/me7_ramroutine_re.md 6e
                                   // 0.17.6 - ME7 bench: USB-console probe (/api/me7/probe) sends one hand-made
                                   //          KWP2000 request inside the running logger session; session fallback
                                   //          10 86 -> 10 81 -> default (trySession); Samples/s and requests-per-sample
                                   //          shown for the logger
                                   // 0.17.5 - web UI, K-line mode "Live on this page": Find... (type part of a value's
                                   //          name, pick it and the gauge goes to its block - from the module's
                                   //          label file), the block number beside "Read Blocks Here" and the step
                                   //          buttons under it, the block shown as name / value lines with the
                                   //          formula, a / b and spec behind "Show Formula and Spec"; a gap
                                   //          between the Status tiles and the module's part number
                                   // 0.17.4 - K-line bench session 2: label files unzipped with the working area in
                                   //          PSRAM (it overflowed the 8 KB web server / K-line task stacks and reset
                                   //          the gauge - also the "gauge page with a K-line source" reset); label
                                   //          index read into PSRAM once (1.9 s -> 38 ms: K-line pages never got a
                                   //          block); KWP1281: no complement for KWP2000 key bytes + an immediate
                                   //          second wake-up (reconnect 16 s -> 6 s), blocks up to 255 bytes (the
                                   //          ME7.5's text blocks 80-82 dropped the session), K-line mode clears the
                                   //          old block's values on a goto / swipe, text blocks (formula 63 first,
                                   //          longer than 4 fields: ME7 81 = VIN) shown as text, formula 34 in
                                   //          degrees (was kW); OBD: 0x33 sent as the plain byte
                                   //          (was 0xB3), functional 0xC0 header, 5-baud fallback, ISO 9141-2 framing,
                                   //          10400 unless 9600 / 4800 chosen; ME7 flash read into PSRAM (the
                                   //          filesystem had no room for it) + /api/me7read/bin, NefMoto's login
                                   //          order, wake / talk addresses (0x11 / 0x10); KWP2000 wake-up address as
                                   //          the plain byte, header mode 0 (HM0), P4 = the ECU's P4min, frames in one
                                   //          write; ME7 logger: Communicate= used, 3 entries per 2C identifier (F0,
                                   //          F1 ...), grouped 23 reads, units without Latin-1 (3 -> 25 samples/s);
                                   //          ISO 9141 sessions released (5 s) before the next wake-up; settings
                                   //          saves one at a time (NVS INVALID_HANDLE); console: replies keep their
                                   //          @@C2G marker, FILEDEL; one KWP1281 session shared by engine blocks,
                                   //          K-line mode, pages and the oil page (no re-wake for the same module);
                                   //          K-line task on core 1 priority 5; filesystem used / total worked out
                                   //          on the house task (in /api/sprites it took >5 s: watchdog reset)
                                   // 0.17.3 - K-line bench fixes: listens for the ECU's 0x55 from the start of the
                                   //          stop bit (it was missed now and then) and skips stray bytes before it,
                                   //          1 s for the first byte of a block (slow blocks looked like a lost
                                   //          session, so K-line mode kept reconnecting), bytes picked up the moment
                                   //          they arrive (no 1 ms polling), samples/s for every K-line session (was
                                   //          0/s on KWP1281); web UI: Baud is a drop-down, the module's part number
                                   //          sits in the Status box; USB bench console (can2gauge_Console.cpp) for
                                   //          tools/diag/kline_bench.py
                                   // 0.17.2 - diagnostics fixes:
                                   //          K-line: a missing group (NAK) no longer drops the session, the module
                                   //          id keeps its first character, 5-baud address with odd parity, measuring
                                   //          formulas 4/27 signed (+ = BTDC), 33/44/45/50 fixed, 29/32/58/61/63 added;
                                   //          ME7 logger: timing set with 83 03 (was 83 02 = read), ISO 55 ms gap
                                   //          until then, 5 missed samples in a row before it gives up (was 1);
                                   //          ME7 flash read: 83 00 / 83 03 well-formed; TP2.0: a refused 10 89
                                   //          isn't fatal, a repeated ECU frame is re-ACKed and dropped, the DSG
                                   //          stands back while a Service tab CAN job runs
                                   // 0.17.1 - web UI status text in Title Case and as short as possible (Diag, Dashboard
                                   //          tiles, state lines), Output "● Output: On / Off / Fault" with a flashing
                                   //          orange dot on a fault, Log tab "● Recording: On"; ignition detail
                                   //          shortened ("Following Supply")
                                   // 0.17.0 - web UI flows phone <-> laptop (dropdowns get a line of their own, status as
                                   //          "Name: value" lines), Log tab shows the room before Start and redraws for
                                   //          new channels, ADC values with their own decimals, no "-0.00", old
                                   //          "can2gauge" .local name moved to c2g, Output says when standby holds it off
                                   // 0.16.0 - data logger (Log tab: up to 16 values at 1-50 Hz into PSRAM, graphs, CSV to
                                   //          the phone), Service tab: read / clear fault codes over the gauge's own
                                   //          K-line or CAN, throttle body adaptation (KWP1281 basic setting 060 / 098)
                                   // 0.15.0 - page sources ready before the swipe: one K-line module for the whole rotation
                                   //          stays connected, OBD adapter / VAG diag run while any page uses them,
                                   //          spinner on a page while its source connects
                                   // 0.14.0 - web UI tidy (shorter help, Dashboard status dots, Diagnostics / OpenHaldex /
                                   //          usb2ip tabs, Diag in blocks), Clear DTCs, web UI files can't be deleted
                                   // 0.13.0 - VAG diag: UDS / TP2.0 measuring values over CAN with a Turbo mode
                                   // 0.12.0 - gauges in a row wave to each other at ignition off
#define CAN2GAUGE_BOARD "can2gauge V3.4"
#define CAN2GAUGE_BUILD_DATE __DATE__ " " __TIME__
