# Make logo sources

**The basic set is already here.** `python tools/fetch_logos.py` downloaded all 33 full-colour logos:
- 30 from Wikimedia Commons;
- Alfa Romeo, Jaguar and Porsche from English Wikipedia.

The logos drawn for a white page were lightened, keeping their hue, so they show on the black glass: Alfa, Aston,
Hyundai, Infiniti, Lexus, Mercedes text, Mini, Opel, Peugeot, Saab, Seat, Subaru and Vauxhall text. The built files
are in `data/logos/`, and `tools/logos_preview/_sheet.png` shows them all. To replace one, drop your own file here
under the same key, or change its entry in `fetch_logos.py` and run `python tools/fetch_logos.py <key> --force`.

Drop one source image per make in this folder, named by its key, then run `python tools/logo_convert.py`. It writes
`data/logos/<key>.bin` (the C2GL format the gauge reads), and `--preview` also writes round-masked PNGs to
`tools/logos_preview/` to check the fit.

**Best source:** SVG (vector). It scales cleanly to the 480 px glass.
**Otherwise:** a transparent-background PNG, at least 1000 px on the longest side.

Logos are drawn on black, so use the version that reads well on a dark background: the chrome / white / colour
badge, not a dark-on-white print version. Trim the canvas close to the logo. The converter trims transparent margins
and fits the logo into 80 % of the glass by default (`--scale`).

| Key | Make | | Key | Make |
|---|---|---|---|---|
| `alfa` | Alfa Romeo | | `mazda` | Mazda |
| `aston` | Aston Martin | | `mercedes` | Mercedes-Benz |
| `audi` | Audi | | `mini` | Mini |
| `bmw` | BMW | | `mitsubishi` | Mitsubishi |
| `chevrolet` | Chevrolet | | `nissan` | Nissan |
| `citroen` | Citroen | | `opel` | Opel |
| `dodge` | Dodge | | `peugeot` | Peugeot |
| `fiat` | Fiat | | `porsche` | Porsche |
| `ford` | Ford | | `renault` | Renault |
| `honda` | Honda | | `saab` | Saab |
| `hyundai` | Hyundai | | `seat` | Seat |
| `infiniti` | Infiniti | | `skoda` | Skoda |
| `jaguar` | Jaguar | | `subaru` | Subaru |
| `kia` | Kia | | `toyota` | Toyota |
| `landrover` | Land Rover | | `vauxhall` | Vauxhall |
| `lexus` | Lexus | | `volvo` | Volvo |
| | | | `vw` | Volkswagen |

Manufacturer logos are trademarks of their owners. They're used here only to identify the car the gauge is fitted
to. Check the terms before shipping them in a product image, as opposed to letting users upload their own.

`forbes.svg` is the Forbes Automotive logo for boot logo option 1, not a make. Its master is
`J:\My Drive\Forbes Automotive\BRANDING\svg's\FORBES AUTOMOTIVE.svg` (black); this copy has the fills changed to
white for the black glass. If the master changes, recolour it again and re-run the converter.
