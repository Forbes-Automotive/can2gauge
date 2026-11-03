# obdb_to_c2g.py - can2gauge 0.33.0. Builds the gauge's DID library (data/vdid/) from OBDb's vehicle profiles.
#
# OBDb (https://github.com/OBDb) is an open database of what each car answers on its diagnostic port: for every
# command the request / answer CAN ids, the DID, and how each value is packed in the answer (bit position, length,
# scaling, unit). Licence: Creative Commons Attribution-ShareAlike 4.0 (CC BY-SA 4.0). The files this script writes
# are made from that data, so they carry the same licence and the attribution in their "src" field.
#
# Only service 22 (ReadDataByIdentifier) commands are kept: those are the UDS measuring values the Diagnostics tab reads.
# Makes are grouped into one library per car group (VW group, Stellantis ...) because the group's makes share
# their modules; each value is marked "confirmed" when OBDb has it as a released (non-debug) command in at least one
# model profile, otherwise it is still being tried out there and may decode wrong.
#
# Use:
#   1. download the profiles:  python tools/vdid/obdb_to_c2g.py --download <folder>
#      (every OBDb repo's signalsets/v3/default.json -> <folder>/<repo>.json, about 65 MB)
#   2. build the library:      python tools/vdid/obdb_to_c2g.py <folder> data/vdid
#
# Output: data/vdid/index.json (the list of libraries) and one <key>.json per library. The filesystem image zips
# them (tools/fs_pack.py); the web UI's Diagnostics tab loads them when you open its DID library.

import json
import os
import sys
import urllib.request

# car groups: library key -> (name shown in the web UI, the OBDb make names in it). A make not listed here gets
# a library of its own.
FAMILIES = [
    ("vag", "VW group (VW, Audi, Skoda, SEAT, Cupra, Porsche, Bentley, Lamborghini)",
     ["Volkswagen", "Audi", "Skoda", "SEAT", "Seat", "Cupra", "Porsche", "Bentley", "Lamborghini"]),
    ("bmw", "BMW group (BMW, MINI, Rolls-Royce)", ["BMW", "MINI", "Rolls"]),
    ("mercedes", "Mercedes-Benz and smart", ["Mercedes", "smart"]),
    ("ford", "Ford and Lincoln", ["Ford", "Lincoln"]),
    ("gm", "GM (Chevrolet, Cadillac, GMC, Buick, Holden)", ["Chevrolet", "Cadillac", "GMC", "Buick", "Holden"]),
    ("stellantis", "Stellantis (Peugeot, Citroen, DS, Vauxhall / Opel, Fiat, Jeep, Chrysler, Dodge, Ram, Maserati, Alfa Romeo)",
     ["Peugeot", "Citroen", "DS", "VauxhallOpel", "Fiat", "FIAT", "Abarth", "Jeep", "Chrysler", "Dodge", "Ram", "Maserati", "Alfa"]),
    ("renault", "Renault, Dacia and Alpine", ["Renault", "Dacia", "Alpine"]),
    ("toyota", "Toyota, Lexus, Scion and Daihatsu", ["Toyota", "Lexus", "Scion", "Daihatsu"]),
    ("nissan", "Nissan, Infiniti and Datsun", ["Nissan", "INFINITI", "Datsun", "NissanInfiniti"]),
    ("honda", "Honda and Acura", ["Honda", "Acura"]),
    ("hyundai", "Hyundai, Kia and Genesis", ["Hyundai", "Kia", "Genesis"]),
    ("volvo", "Volvo, Polestar and Lotus", ["Volvo", "Polestar", "Lotus"]),
    ("jlr", "Jaguar Land Rover (and Tata)", ["Land", "Jaguar", "Tata"]),
    ("mazda", "Mazda", ["Mazda"]),
    ("subaru", "Subaru", ["Subaru"]),
]

# OBDb unit names -> the short unit the gauge shows (8 characters at most, the Diagnostics tab's unit field)
UNITS = {
    "celsius": "C", "fahrenheit": "F", "percent": "%", "volts": "V", "millivolts": "mV", "amps": "A",
    "milliamps": "mA", "ampereHours": "Ah", "kilometers": "km", "miles": "mi", "meters": "m", "centimeters": "cm",
    "millimeters": "mm", "kilometersPerHour": "km/h", "metersPerSecond": "m/s", "metersPerSecondSquared": "m/s2",
    "gravity": "g", "rpm": "rpm", "bars": "bar", "kilopascal": "kPa", "psi": "psi", "seconds": "s",
    "milliseconds": "ms", "minutes": "min", "hours": "h", "liters": "l", "gallons": "gal", "litersPerHour": "l/h",
    "kilowatts": "kW", "watts": "W", "kilowattHours": "kWh", "newtonMeters": "Nm", "degrees": "deg",
    "radians": "rad", "ohms": "ohm", "kiloohms": "kohm", "milliohms": "mohm", "hertz": "Hz",
    "gramsPerSecond": "g/s", "gramsPerLiter": "g/l", "milligramsPerStroke": "mg/st",
    "kilowattHoursPer100Kilometers": "kWh/100k", "kilowattHoursPer100Miles": "kWh/100m",
    "scalar": "", "unknown": "", "hex": "", "offon": "", "noyes": "", "yesno": "", "": "",
}

# VW's diagnostic addresses for the 11-bit ids (the same table as src/can2gauge_VagDiag.cpp), and names for the
# modules OBDb uses that have no confirmed address yet (named from what they carry)
VW_MODULES = {
    "7E0": (0x01, "Engine"), "7E1": (0x02, "Gearbox"), "713": (0x03, "Brakes"), "746": (0x08, "Climate"),
    "70E": (0x09, "Central electrics"), "715": (0x15, "Airbag"), "714": (0x17, "Instruments"),
    "710": (0x19, "Gateway"), "70F": (0x22, "AWD"), "712": (0x44, "Steering assist"),
    "773": (0x5F, "Infotainment"), "767": (0x75, "Emergency call"), "752": (0x53, "Parking brake"),
    "7E5": (0x8C, "Hybrid battery"), "7E6": (0x51, "Electric drive"), "744": (0xC6, "Charger"),
    "70B": (0x00, "Tyre pressure"), "71D": (0x00, "Drivetrain 71D"), "71E": (0x00, "Drivetrain 71E"),
    "742": (0x00, "Heater 742"), "74F": (0x00, "Driver assistance 74F"),
}
VW_29BIT_MODULES = {0x76: "Engine (29-bit)", 0x77: "Gearbox (29-bit)", 0x7B: "HV battery (29-bit)",
                    0x7C: "Electric drive (29-bit)", 0xB9: "DC-DC converter (29-bit)"}


def download(folder):
    """every OBDb repo's default signalset into folder/<repo>.json"""
    os.makedirs(folder, exist_ok=True)
    page = 1
    names = []
    while True:
        url = "https://api.github.com/orgs/OBDb/repos?per_page=100&page=%d" % page
        repos = json.load(urllib.request.urlopen(url))
        if not repos:
            break
        for repo in repos:
            if not repo["name"].startswith("."):
                names.append(repo["name"])
        page += 1
    for name in names:
        url = "https://raw.githubusercontent.com/OBDb/%s/main/signalsets/v3/default.json" % name
        try:
            data = urllib.request.urlopen(url).read()
        except Exception:
            continue  # a repo with no signalset (tools, the website ...)
        open(os.path.join(folder, name + ".json"), "wb").write(data)
        print("got", name)


def ids_for(command):
    """the request and answer CAN ids of an OBDb command, as numbers, and whether they are 29-bit.
    11-bit: hdr is the request id, rax the answer id (missing: request + 8, the OBD-II rule; VW's 0x7xx modules
    answer on request + 0x6A). 29-bit (hdr has 4 digits): request = priority (pri, default 18) + hdr + tester address
    (tst, default F1); OBDb's own decoder builds the answer as: rax of 2 digits = 18 DA F1 <rax>, 6 digits = the
    priority + rax, 8 digits = the whole id. VW's 17 FC 00 xx ids carry no tst on some entries: there the last byte
    is taken from the answer (17 FE 00 76 -> 17 FC 00 76), which is how the entries that do have one pair up."""
    header = command["hdr"]
    rax = command.get("rax")
    if len(header) == 3:
        request = int(header, 16)
        if rax:
            answer = int(rax, 16)
        elif 0x700 <= request < 0x7E0:
            answer = request + 0x6A
        else:
            answer = request + 8
        return request, answer, False
    priority = int(command.get("pri", "18"), 16)
    if rax and len(rax) == 8:
        answer = int(rax, 16) & 0x1FFFFFFF
    elif rax and len(rax) > 2:
        answer = (priority << 24) | (int(rax, 16) & 0xFFFFFF)
    elif rax:
        answer = (priority << 24) | 0x00DAF100 | int(rax, 16)
    else:
        answer = (priority << 24) | 0x00DA0000 | (0xF1 << 8) | (int(header, 16) & 0xFF)  # 18 DA <target> F1 -> 18 DA F1 <target>
    if "tst" in command:
        tester = int(command["tst"], 16)
    elif header.upper().startswith("FC") and rax:
        tester = answer & 0xFF
    else:
        tester = 0xF1
    request = (priority << 24) | (int(header, 16) << 8) | tester
    return request, answer, True


def year_text(command):
    """'2016-2020', '2016 on', 'to 2015' from an OBDb year filter ('' = every year)"""
    year_filter = command.get("filter") or {}
    start = year_filter.get("from")
    end = year_filter.get("to")
    if start and end:
        return "%d-%d" % (start, end)
    if start:
        return "%d on" % start
    if end:
        return "to %d" % end
    return ""


def module_name(family, request, answer, is29Bit, ext, ecu_types):
    if family == "vag" and not is29Bit and ("%03X" % request) in VW_MODULES:
        return VW_MODULES["%03X" % request]
    if family == "vag" and is29Bit and (request >> 8) & 0xFFFF == 0xFC00:
        return (0, VW_29BIT_MODULES.get(answer & 0xFF, "Module %08X" % request))
    if is29Bit:
        name = "Module %08X" % request
    else:
        name = "Module %03X" % request
    kind = ecu_types.get(request)
    if kind:
        name = kind[0].upper() + kind[1:] + " " + name[7:]
    if ext:
        name += " ext %02X" % ext
    return (0, name)


def number(value):
    """a float written short: 0.1 not 0.1000000001, 5 not 5.0"""
    text = "%.7g" % value
    if "e" in text:
        return float(text)
    if "." in text:
        return float(text)
    return int(text)


def build(folder, out_dir):
    files = sorted(f for f in os.listdir(folder) if f.endswith(".json"))
    family_of_make = {}
    for key, _, makes in FAMILIES:
        for make in makes:
            family_of_make[make] = key
    names = {key: name for key, name, _ in FAMILIES}
    libraries = {}  # key -> {"makes": set, "modules": {...}, "dids": {...}}
    for file_name in files:
        repo = file_name[:-5]
        make = repo.split("-")[0]
        key = family_of_make.get(make, make.lower())
        names.setdefault(key, make)
        try:
            profile = json.load(open(os.path.join(folder, file_name), encoding="utf-8"))
        except Exception:
            continue
        library = libraries.setdefault(key, {"makes": set(), "models": set(), "modules": {}, "dids": {}})
        library["makes"].add(make)
        if "-" in repo:
            library["models"].add(repo)
        ecu_types = {}
        for ecu in profile.get("ecu", []):
            try:
                ecu_types[int(ecu["hdr"], 16)] = ecu.get("type", "")
            except Exception:
                pass
        for command in profile.get("commands", []):
            if "22" not in command.get("cmd", {}) or "hdr" not in command:
                continue
            did = command["cmd"]["22"].upper()
            if len(did) != 4:
                continue  # a 22 with more than one DID: not a single measuring value
            request, answer, is29Bit = ids_for(command)
            ext = int(command["eax"], 16) if "eax" in command else 0
            module_key = (request, answer, ext)
            if module_key not in library["modules"]:
                address, name = module_name(key, request, answer, is29Bit, ext, ecu_types)
                library["modules"][module_key] = {"tx": request, "rx": answer, "w29": is29Bit, "ext": ext,
                                                  "addr": address, "name": name}
            confirmed = not command.get("dbg", False)
            years = year_text(command)
            for signal in command.get("signals", []):
                fmt = signal.get("fmt", {})
                if "len" not in fmt:
                    continue
                bits = fmt["len"]
                if bits < 1 or bits > 32:
                    continue  # text or long arrays: not one number
                flags = 0
                if fmt.get("sign"):
                    flags |= 1   # signed (two's complement)
                if fmt.get("blsb"):
                    flags |= 2   # bytes least significant first (Intel)
                if "map" in fmt:
                    flags |= 4   # a state with names (shown as its number)
                scale = fmt.get("mul", 1) / fmt.get("div", 1)
                offset = fmt.get("add", 0)
                unit = UNITS.get(fmt.get("unit", ""), "")
                # the same value listed again for other model years is one entry, with the years joined
                signal_key = (module_key, did, fmt.get("bix", 0), bits, flags, round(scale, 9), round(offset, 6),
                              signal.get("name", ""), unit)
                entry = library["dids"].get(signal_key)
                if entry is None:
                    entry = {"confirmed": confirmed, "years": set()}
                    library["dids"][signal_key] = entry
                elif confirmed:
                    entry["confirmed"] = True
                entry["years"].add(years)

    os.makedirs(out_dir, exist_ok=True)
    index = []
    for key, library in sorted(libraries.items()):
        if not library["dids"]:
            continue
        module_list = sorted(library["modules"].values(), key=lambda m: (m["w29"], m["tx"], m["ext"]))
        module_index = {}
        modules_out = []
        for module in module_list:
            module_key = (module["tx"], module["rx"], module["ext"])
            module_index[module_key] = len(modules_out)
            if module["w29"]:
                tx_text = "%08X" % module["tx"]
                rx_text = "%08X" % module["rx"]
            else:
                tx_text = "%03X" % module["tx"]
                rx_text = "%03X" % module["rx"]
            modules_out.append([tx_text, rx_text, module["ext"], module["addr"], module["name"]])
        # one row per DID: [module, "DID", [signal, ...]]; signal = [name, bit, bits, flags, scale, offset, unit,
        # confirmed 0/1, years]
        rows = {}
        confirmed_count = 0
        for signal_key, entry in library["dids"].items():
            module_key, did, bit, bits, flags, scale, offset, name, unit = signal_key
            years = ""  # every year (or the years aren't known), else "2016-2020, 2021 on"
            if "" not in entry["years"]:
                years = ", ".join(sorted(entry["years"]))
            row = rows.setdefault((module_index[module_key], did), [])
            row.append([name[:64], bit, bits, flags, number(scale), number(offset), unit,
                        1 if entry["confirmed"] else 0, years])
            if entry["confirmed"]:
                confirmed_count += 1
        dids_out = []
        for (module_number, did), signals in sorted(rows.items()):
            signals.sort(key=lambda s: (s[1], s[0]))
            dids_out.append([module_number, did, signals])
        document = {
            "v": 1,
            "key": key,
            "name": names.get(key, key),
            "makes": sorted(library["makes"]),
            "models": len(library["models"]),
            "src": "OBDb (github.com/OBDb), CC BY-SA 4.0 - built by tools/vdid/obdb_to_c2g.py",
            "mods": modules_out,
            "dids": dids_out,
        }
        path = os.path.join(out_dir, key + ".json")
        with open(path, "w", encoding="utf-8", newline="\n") as out:
            json.dump(document, out, separators=(",", ":"), ensure_ascii=False)
        signal_count = len(library["dids"])
        index.append({"key": key, "name": document["name"], "makes": document["makes"], "dids": len(dids_out),
                      "values": signal_count, "confirmed": confirmed_count, "bytes": os.path.getsize(path)})
        print("%-11s %5d DIDs %6d values (%5d confirmed) %8d bytes" % (key, len(dids_out), signal_count,
                                                                       confirmed_count, os.path.getsize(path)))
    # the web UI lists the groups in this order: the VW group first (what most of these gauges go in), then A to Z
    vw_first = []
    others = []
    for entry in index:
        if entry["key"] == "vag":
            vw_first.append(entry)
        else:
            others.append(entry)
    index = vw_first + sorted(others, key=lambda entry: entry["name"].lower())
    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8", newline="\n") as out:
        json.dump({"v": 1, "src": "OBDb (github.com/OBDb), CC BY-SA 4.0", "libs": index}, out, indent=1)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--download":
        download(sys.argv[2])
    elif len(sys.argv) == 3:
        build(sys.argv[1], sys.argv[2])
    else:
        print("use: obdb_to_c2g.py --download <folder>   or   obdb_to_c2g.py <folder> <out dir>")
