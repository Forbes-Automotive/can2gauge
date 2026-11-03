import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen import Car, OUT

# ---------------- front presets: (sx, f) on the front face, sx 0 centre .. 1 car's corner, f 0 bottom .. 1 top ----
DK = "#101317"
DK2 = "#1a1e24"
CHROME = "#b9c0c8"
F = {}
F["generic"] = dict(
    hl=[(0.42, 0.7), (0.97, 0.64), (0.99, 0.95), (0.44, 0.94)],
    grille=[([(0, 0.9), (0.4, 0.9), (0.4, 0.72), (0, 0.72)], DK), ([(0, 0.42), (0.58, 0.42), (0.52, 0.12), (0, 0.12)], DK)])
F["golf3"] = dict(
    hl=[(0.47, 0.62), (0.97, 0.62), (0.97, 0.95), (0.47, 0.95)],
    grille=[([(0, 0.93), (0.47, 0.93), (0.47, 0.64), (0, 0.64)], DK), ([(0, 0.36), (0.5, 0.36), (0.5, 0.2), (0, 0.2)], DK)])
F["golf4"] = dict(
    hl=[(0.42, 0.68), (0.95, 0.62), (0.99, 0.96), (0.45, 0.97)],
    grille=[([(0, 0.95), (0.42, 0.95), (0.42, 0.7), (0, 0.7)], DK), ([(0, 0.4), (0.5, 0.4), (0.46, 0.16), (0, 0.16)], DK)],
    frontbits=[([(0.66, 0.34), (0.86, 0.34), (0.86, 0.2), (0.66, 0.2)], DK)])
F["golf5"] = dict(
    hl=[(0.38, 0.62), (0.94, 0.54), (0.99, 0.97), (0.42, 0.95)],
    grille=[([(0, 0.96), (0.37, 0.95), (0.33, 0.6), (0, 0.58)], DK), ([(0, 0.45), (0.56, 0.45), (0.5, 0.14), (0, 0.14)], DK)],
    frontbits=[([(0.67, 0.38), (0.88, 0.38), (0.86, 0.18), (0.68, 0.2)], DK)])
F["golf6"] = dict(
    hl=[(0.36, 0.7), (0.96, 0.64), (0.99, 0.95), (0.38, 0.92)],
    grille=[([(0, 0.9), (0.36, 0.9), (0.36, 0.74), (0, 0.74)], DK), ([(0, 0.46), (0.62, 0.46), (0.54, 0.12), (0, 0.12)], DK)])
F["golf7"] = dict(
    hl=[(0.34, 0.75), (0.97, 0.66), (0.99, 0.93), (0.36, 0.9)],
    grille=[([(0, 0.88), (0.34, 0.88), (0.34, 0.79), (0, 0.79)], DK), ([(0, 0.5), (0.64, 0.5), (0.56, 0.1), (0, 0.1)], DK)],
    frontbits=[([(0.0, 0.84), (0.34, 0.84), (0.34, 0.82), (0, 0.82)], CHROME)])
F["golf8"] = dict(
    hl=[(0.3, 0.8), (0.97, 0.7), (0.99, 0.9), (0.32, 0.92)],
    grille=[([(0, 0.88), (0.3, 0.88), (0.3, 0.84), (0, 0.84)], "#cfd5dc"), ([(0, 0.6), (0.7, 0.6), (0.6, 0.08), (0, 0.08)], DK)])
F["polo3"] = dict(
    hl=[(0.44, 0.6), (0.96, 0.6), (0.98, 0.95), (0.46, 0.95)],
    grille=[([(0, 0.92), (0.44, 0.92), (0.44, 0.66), (0, 0.66)], DK), ([(0, 0.34), (0.5, 0.34), (0.5, 0.2), (0, 0.2)], DK)])
F["polo4"] = dict(  # twin round lamps
    hl=[(0.44, 0.62), (0.96, 0.58), (0.99, 0.96), (0.46, 0.97)],
    grille=[([(0, 0.95), (0.42, 0.95), (0.36, 0.66), (0, 0.64)], DK), ([(0, 0.4), (0.5, 0.4), (0.46, 0.16), (0, 0.16)], DK)],
    frontbits2=[([(0.56, 0.68), (0.7, 0.68), (0.7, 0.9), (0.56, 0.9)], "#eef1f4"), ([(0.77, 0.66), (0.92, 0.66), (0.92, 0.9), (0.77, 0.9)], "#eef1f4")])
F["beetle"] = dict(
    hl=[(0.64, 0.66), (0.78, 0.58), (0.93, 0.62), (0.99, 0.8), (0.94, 0.97), (0.78, 1.0), (0.66, 0.9)],
    grille=[([(0, 0.34), (0.42, 0.34), (0.4, 0.2), (0, 0.2)], DK)])
F["beetle2"] = dict(
    hl=[(0.6, 0.64), (0.76, 0.56), (0.93, 0.6), (0.99, 0.8), (0.94, 0.97), (0.76, 1.0), (0.62, 0.9)],
    grille=[([(0, 0.82), (0.36, 0.82), (0.36, 0.76), (0, 0.76)], DK), ([(0, 0.44), (0.5, 0.44), (0.46, 0.16), (0, 0.16)], DK)])
F["audi_old"] = dict(
    hl=[(0.4, 0.66), (0.96, 0.62), (0.99, 0.95), (0.42, 0.95)],
    grille=[([(0, 0.95), (0.33, 0.94), (0.36, 0.7), (0.3, 0.64), (0, 0.63)], DK), ([(0, 0.42), (0.56, 0.42), (0.52, 0.16), (0, 0.16)], DK)])
F["audi_sf"] = dict(
    hl=[(0.42, 0.72), (0.96, 0.64), (0.99, 0.96), (0.4, 0.96)],
    grille=[([(0, 0.96), (0.31, 0.96), (0.38, 0.86), (0.38, 0.2), (0.3, 0.06), (0, 0.06)], DK)],
    frontbits=[([(0.62, 0.42), (0.9, 0.46), (0.9, 0.16), (0.66, 0.14)], DK)])
F["audi_sf2"] = dict(  # newer: wider, flat-topped single frame, slim lamps
    hl=[(0.44, 0.78), (0.97, 0.68), (0.99, 0.92), (0.42, 0.94)],
    grille=[([(0, 0.95), (0.4, 0.95), (0.44, 0.82), (0.42, 0.14), (0.32, 0.04), (0, 0.04)], DK)],
    frontbits=[([(0.62, 0.48), (0.92, 0.54), (0.92, 0.14), (0.68, 0.12)], DK)])
F["tt1"] = dict(
    hl=[(0.46, 0.62), (0.95, 0.58), (0.99, 0.95), (0.5, 0.97)],
    grille=[([(0, 0.92), (0.34, 0.9), (0.34, 0.6), (0, 0.58)], DK), ([(0, 0.4), (0.6, 0.4), (0.56, 0.14), (0, 0.14)], DK)])
F["seat1"] = dict(
    hl=[(0.36, 0.64), (0.95, 0.56), (0.99, 0.96), (0.4, 0.95)],
    grille=[([(0, 0.92), (0.33, 0.92), (0.24, 0.66), (0, 0.6)], DK), ([(0, 0.42), (0.56, 0.42), (0.5, 0.14), (0, 0.14)], DK)])
F["seat3"] = dict(
    hl=[(0.38, 0.72), (0.97, 0.62), (0.99, 0.92), (0.4, 0.92)],
    grille=[([(0, 0.9), (0.32, 0.9), (0.38, 0.56), (0, 0.56)], DK), ([(0, 0.4), (0.6, 0.4), (0.54, 0.1), (0, 0.1)], DK)])
F["seat4"] = dict(
    hl=[(0.38, 0.78), (0.97, 0.68), (0.99, 0.9), (0.4, 0.92)],
    grille=[([(0, 0.86), (0.38, 0.86), (0.38, 0.84), (0, 0.84)], "#cfd5dc"), ([(0, 0.7), (0.4, 0.7), (0.46, 0.2), (0.36, 0.08), (0, 0.08)], DK)])
F["skoda1"] = dict(
    hl=[(0.4, 0.62), (0.96, 0.6), (0.98, 0.95), (0.42, 0.95)],
    grille=[([(0, 0.96), (0.26, 0.96), (0.28, 0.66), (0.18, 0.56), (0, 0.54)], DK), ([(0, 0.36), (0.5, 0.36), (0.48, 0.18), (0, 0.18)], DK)])
F["skoda2"] = dict(
    hl=[(0.38, 0.62), (0.95, 0.56), (0.99, 0.96), (0.4, 0.95)],
    grille=[([(0, 0.96), (0.3, 0.96), (0.32, 0.64), (0.2, 0.52), (0, 0.5)], DK), ([(0, 0.4), (0.56, 0.4), (0.52, 0.14), (0, 0.14)], DK)])
F["skoda3"] = dict(
    hl=[(0.4, 0.7), (0.97, 0.64), (0.99, 0.94), (0.4, 0.94)],
    grille=[([(0, 0.94), (0.38, 0.94), (0.36, 0.62), (0, 0.6)], DK), ([(0, 0.4), (0.62, 0.4), (0.56, 0.1), (0, 0.1)], DK)])
F["van"] = dict(
    hl=[(0.5, 0.62), (0.97, 0.6), (0.99, 0.92), (0.5, 0.92)],
    grille=[([(0, 0.9), (0.5, 0.9), (0.5, 0.64), (0, 0.64)], DK), ([(0, 0.36), (0.6, 0.36), (0.6, 0.18), (0, 0.18)], DK)],
    frontbits2=[([(0, 0.8), (0.5, 0.8), (0.5, 0.76), (0, 0.76)], CHROME)])
F["suv"] = dict(
    hl=[(0.42, 0.72), (0.97, 0.64), (0.99, 0.94), (0.42, 0.92)],
    grille=[([(0, 0.92), (0.42, 0.92), (0.42, 0.72), (0, 0.72)], DK), ([(0, 0.5), (0.6, 0.5), (0.54, 0.16), (0, 0.16)], DK)],
    frontbits=[([(0, 0.12), (0.6, 0.12), (0.6, 0.06), (0, 0.06)], CHROME)])


def mk(body, L, W, H, wb, front="generic", doors5=True, **o):
    p = dict(L=L, W=W, H=H, wb=wb, body=body)
    fa = o.pop("fa", (L - wb) * 0.54)
    p["fa"] = fa
    ra = fa + wb
    if body in ("hatch", "estate", "saloon", "coupe", "convertible"):
        d = dict(wr=0.325, c=0.17, hn=0.7, hb=0.96, hbr=1.0)
    elif body == "suv":
        d = dict(wr=0.36, c=0.24, hn=0.86, hb=1.12, hbr=1.16)
        p["clad"] = 0.13
    elif body == "mpv":
        d = dict(wr=0.32, c=0.18, hn=0.78, hb=1.0, hbr=1.06)
    elif body == "van":
        d = dict(wr=0.34, c=0.25, hn=0.98, hb=1.13, hbr=1.13)
    else:  # pickup
        d = dict(wr=0.38, c=0.3, hn=1.0, hb=1.22, hbr=1.25)
    d.update({k: o.pop(k) for k in list(o) if k in d})
    p.update(d)
    wr, c, hn, hb, hbr = d["wr"], d["c"], d["hn"], d["hb"], d["hbr"]
    g = dict(o)
    def gv(k, v):
        return g.pop(k, v)
    if body in ("hatch", "estate", "suv", "mpv"):
        if body == "hatch":
            ws0 = gv("ws0", fa + 0.5); ws1 = gv("ws1", ws0 + 0.82); r1 = gv("r1", L - 0.62); ut = gv("ut", L - 0.18); zt = gv("zt", hbr + 0.03)
        elif body == "estate":
            ws0 = gv("ws0", fa + 0.55); ws1 = gv("ws1", ws0 + 0.84); r1 = gv("r1", L - 0.3); ut = gv("ut", L - 0.12); zt = gv("zt", hbr + 0.04)
        elif body == "suv":
            ws0 = gv("ws0", fa + 0.45); ws1 = gv("ws1", ws0 + 0.78); r1 = gv("r1", L - 0.4); ut = gv("ut", L - 0.13); zt = gv("zt", hbr + 0.04)
        else:
            ws0 = gv("ws0", fa + 0.28); ws1 = gv("ws1", ws0 + 1.0); r1 = gv("r1", L - 0.28); ut = gv("ut", L - 0.1); zt = gv("zt", hbr + 0.03)
        Hr = gv("Hr", H - 0.04)
        mid = gv("mid", None)
        prof = [(0, hn - 0.01, "n"), (gv("bu", 0.3), hn + 0.05, "b"), (ws0, hb + 0.01, "w"), (ws1, H, "r")]
        if mid:
            prof.append((mid[0], mid[1], "r"))
        else:
            prof.append(((ws1 + r1) / 2, max(H, Hr) + 0.012, "r"))
        prof += [(r1, Hr, "h"), (ut, zt, "t"), (L, zt - gv("tdrop", 0.06), "e")]
        if body == "hatch":
            cb = gv("cb", L - 0.95); ct = gv("ct", r1 - 0.28)
        elif body == "estate":
            cb = gv("cb", L - 0.42); ct = gv("ct", L - 0.5)
        elif body == "suv":
            cb = gv("cb", L - 0.6); ct = gv("ct", L - 0.68)
        else:
            cb = gv("cb", L - 0.42); ct = gv("ct", L - 0.48)
    elif body in ("saloon", "coupe"):
        if body == "saloon":
            ws0 = gv("ws0", fa + 0.55); ws1 = gv("ws1", ws0 + 0.85); r1 = gv("r1", L - 1.3); ud = gv("ud", L - 0.52); zdk = gv("zdk", hbr + 0.07)
        else:
            ws0 = gv("ws0", fa + 0.5); ws1 = gv("ws1", ws0 + 0.95); r1 = gv("r1", L - 1.25); ud = gv("ud", L - 0.4); zdk = gv("zdk", hbr + 0.04)
        Hr = gv("Hr", H - 0.05)
        mid = gv("mid", None)
        prof = [(0, hn - 0.01, "n"), (gv("bu", 0.3), hn + 0.05, "b"), (ws0, hb + 0.01, "w"), (ws1, H, "r")]
        if mid:
            prof.append((mid[0], mid[1], "r"))
        else:
            prof.append(((ws1 + r1) / 2, max(H, Hr) + 0.012, "r"))
        prof += [(r1, Hr, "h"), (ud, zdk, "d"), (L - 0.1, zdk - 0.02, "t"), (L, zdk - 0.09, "e")]
        cb = gv("cb", r1 + 0.32 if body == "saloon" else r1 + 0.2); ct = gv("ct", r1 - 0.12 if body == "saloon" else r1 - 0.25)
    elif body == "van":
        ws0 = gv("ws0", fa + 0.05); ws1 = gv("ws1", ws0 + 0.72); r1 = gv("r1", L - 0.06); ut = L - 0.02; zt = H - 0.08
        Hr = gv("Hr", H - 0.01)
        prof = [(0, hn - 0.01, "n"), (0.3, hn + 0.05, "b"), (ws0, hb + 0.01, "w"), (ws1, H, "r"), (r1, Hr, "h"), (ut, zt, "t"), (L, zt - 0.06, "e")]
        cb = gv("cb", ws1 + 0.62); ct = gv("ct", ws1 + 0.62)
    elif body == "pickup":
        ws0 = gv("ws0", fa + 0.42); ws1 = gv("ws1", ws0 + 0.8); r1 = gv("r1", ws1 + 1.05); uc = r1 + 0.1
        Hr = gv("Hr", H - 0.02)
        prof = [(0, hn - 0.01, "n"), (0.3, hn + 0.05, "b"), (ws0, hb + 0.01, "w"), (ws1, H, "r"), (r1, Hr, "h"), (uc, hbr + 0.03, "x"), (L - 0.03, hbr + 0.03, "t"), (L, hbr - 0.03, "e")]
        cb = gv("cb", r1 - 0.05); ct = gv("ct", r1 - 0.1)
        p["bed"] = (uc + 0.06,)
    elif body == "convertible":
        ws0 = gv("ws0", fa + 0.55)
        prof = [(0, hn - 0.01, "n"), (0.3, hn + 0.05, "b"), (ws0, hb + 0.01, "d"), (ws0 + 0.9, hb + 0.03, "d"), (L - 0.15, hbr + 0.02, "t"), (L, hbr - 0.04, "e")]
        cb = ct = 0
        p["conv"] = dict(ws0=ws0, ws1=gv("cws1", ws0 + 0.55), Hws=gv("Hws", H - 0.06), end=gv("cend", ws0 + 1.75),
                         seats=gv("seats", [(ws0 + 1.5, 0.16), (ws0 + 0.92, 0.24)]))
    p["prof"] = gv("prof", prof)
    p["cb"], p["ct"] = cb, ct
    # B pillar, doors, handles
    if body not in ("convertible",):
        gs = ws0
        if body == "van":
            bpu = gv("bpu", cb + 0.02)
            p["bp"] = gv("bp", [])
            p["doors"] = gv("doors", [(ws0 + 0.08, fa + wr + 0.1), (cb + 0.04, cb + 0.06), (cb + 1.05, cb + 1.05), (L - 0.12, L - 0.12)])
            p["handles"] = gv("handles", [cb - 0.25, cb + 0.85])
        elif doors5 and body not in ("coupe", "pickup"):
            bpu = gv("bpu", gs + (cb - gs) * gv("bpf", 0.5))
            p["bp"] = gv("bp", [(bpu, bpu - 0.06, 0.1)])
            p["doors"] = gv("doors", [(ws0 + 0.1, fa + wr + 0.12), (bpu + 0.0, bpu + 0.03), (min(cb + 0.06, ra - 0.1), ra - wr - 0.08)])
            p["handles"] = gv("handles", [bpu - 0.34, min(cb + 0.06, ra - 0.1) - 0.3])
        elif body == "pickup":
            bpu = gv("bpu", gs + (cb - gs) * 0.52)
            p["bp"] = gv("bp", [(bpu, bpu - 0.06, 0.1)])
            p["doors"] = gv("doors", [(ws0 + 0.1, fa + wr + 0.12), (bpu, bpu + 0.03), (cb + 0.08, cb + 0.1)])
            p["handles"] = gv("handles", [bpu - 0.34, cb - 0.25])
        else:
            dr = gv("dr", gs + (cb - gs) * 0.68)
            p["bp"] = gv("bp", [(dr + 0.02, dr - 0.04, 0.05)])
            p["doors"] = gv("doors", [(ws0 + 0.1, fa + wr + 0.12), (dr + 0.03, dr + 0.08)])
            p["handles"] = gv("handles", [dr - 0.3])
    else:
        p["doors"] = gv("doors", [(ws0 + 0.08, fa + wr + 0.12), (ws0 + 1.2, ws0 + 1.24)])
        p["handles"] = gv("handles", [ws0 + 0.9])
    # front, rear lamps
    fr = dict(F[front])
    p["hl"] = fr.get("hl")
    p["grille"] = fr.get("grille", [])
    p["frontbits"] = fr.get("frontbits", [])
    p["frontbits2"] = fr.get("frontbits2", [])
    p["wrap"] = gv("wrap", [(0.0, 0.64), (0.13, 0.72), (0.12, 0.97), (0.0, 0.97)])
    if body == "van":
        p["tl"] = gv("tl", [(0.02, -0.5), (0.07, -0.5), (0.07, -0.92), (0.02, -0.92)])  # tall lamps low on the rear corner
    else:
        p["tl"] = gv("tl", [(0.02, 0.01), (0.24, 0.01), (0.2, -0.11), (0.02, -0.13)])
    p.update(g)
    return p


# ---------------- body types (the fallback pictures) ----------------
BODIES = {
    "hatch": mk("hatch", 4.26, 1.79, 1.45, 2.63, "generic"),
    "saloon": mk("saloon", 4.72, 1.82, 1.44, 2.8, "generic"),
    "estate": mk("estate", 4.75, 1.82, 1.48, 2.79, "generic", bp=[(2.5, 2.44, 0.1), (3.75, 3.85, 0.12, "b")]),
    "suv": mk("suv", 4.5, 1.84, 1.67, 2.68, "suv", bp=[(2.45, 2.39, 0.1), (3.48, 3.6, 0.13, "b")]),
    "coupe": mk("coupe", 4.3, 1.81, 1.38, 2.6, "generic"),
    "convertible": mk("convertible", 4.4, 1.8, 1.4, 2.6, "generic"),
    "mpv": mk("mpv", 4.5, 1.8, 1.66, 2.75, "generic", bp=[(2.15, 2.12, 0.1), (3.5, 3.6, 0.12, "b")]),
    "van": mk("van", 4.9, 1.9, 1.99, 3.0, "van"),
    "pickup": mk("pickup", 5.25, 1.94, 1.83, 3.1, "suv"),
}

# ---------------- VW-group models ----------------
# (code, make, name, year, body, params or None, same-as code)
H = "hatch"; S = "saloon"; E = "estate"
M = []
def add(code, make, name, year, body, params=None, same=None):
    M.append(dict(code=code, make=make, name=name, year=year, body=body, params=params, same=same))

# 0.36.1: older VW-group codes, body-type pictures until each has a drawing of its own
add("17", "vw", "Golf I", 1980, H)
add("19", "vw", "Golf II", 1988, H)
add("1G", "vw", "Golf II / Jetta II", 1990, H)
add("50", "vw", "Corrado", 1991, "coupe")
add("53", "vw", "Scirocco", 1985, "coupe")
add("25", "vw", "Transporter T3", 1985, "van")
add("70", "vw", "Transporter T4", 1997, "van")
add("6X", "vw", "Lupo", 2001, H)
add("1H", "vw", "Golf III / Vento", 1995, H, mk(H, 4.02, 1.7, 1.42, 2.47, "golf3", hn=0.72, hb=0.95, hbr=0.98, ws1=2.0, r1=3.45, ut=3.86, zt=1.02, cb=3.12, ct=3.12, Hr=1.39))
add("1J", "vw", "Golf IV / Bora", 2001, H, mk(H, 4.15, 1.73, 1.44, 2.51, "golf4", r1=3.5, ut=3.98, cb=3.22, ct=3.08))
add("1K", "vw", "Golf V / Jetta", 2006, H, mk(H, 4.2, 1.76, 1.48, 2.58, "golf5", hn=0.72, r1=3.58, ut=4.03, cb=3.28, ct=3.1, Hr=1.44))
add("5K", "vw", "Golf VI", 2011, H, mk(H, 4.2, 1.78, 1.48, 2.58, "golf6", hn=0.72, r1=3.58, ut=4.03, cb=3.26, ct=3.1, Hr=1.44))
add("AJ", "vw", "Golf VI", 2011, H, same="5K")
add("AU", "vw", "Golf VII", 2016, H, mk(H, 4.26, 1.8, 1.45, 2.64, "golf7", hn=0.7, r1=3.62, ut=4.08, cb=3.3, ct=3.12))
add("5G", "vw", "Golf VII", 2016, H, same="AU")
add("CD", "vw", "Golf 8", 2022, H, mk(H, 4.28, 1.79, 1.46, 2.64, "golf8", hn=0.7, r1=3.62, ut=4.1, cb=3.3, ct=3.14))
import math
def dome(u0, u1, z0, top, peak, n, kinds):
    # points along a rounded roof from (u0, z0) over the top to (u1, z0): peak = where the top is (0..1)
    pts = []
    for i in range(n + 1):
        t = i / n
        if t <= peak:
            z = z0 + (top - z0) * math.sin(t / peak * math.pi / 2)
        else:
            z = z0 + (top - z0) * math.cos((t - peak) / (1 - peak) * math.pi / 2)
        pts.append((u0 + (u1 - u0) * t, z, kinds[i]))
    return pts
B9 = [(0, 0.6, "n"), (0.22, 0.7, "b"), (0.6, 0.8, "b"), (1.05, 0.9, "b")] +     dome(1.45, 4.0, 0.95, 1.5, 0.37, 8, "wwrrrhhte")[:-1] + [(3.9, 0.93, "t"), (4.08, 0.87, "e")]
B5 = [(0, 0.62, "n"), (0.25, 0.72, "b"), (0.7, 0.82, "b"), (1.12, 0.92, "b")] +     dome(1.45, 4.35, 0.97, 1.49, 0.35, 9, "wwrrrrhhte")[:-1] + [(4.12, 1.0, "t"), (4.28, 0.94, "e")]
B5[-3] = (B5[-3][0], max(B5[-3][1], 1.02), B5[-3][2])
add("9C", "vw", "New Beetle", 2003, H, mk(H, 4.08, 1.72, 1.5, 2.51, "beetle", hn=0.62, hb=0.94, hbr=0.98, ws0=1.45, prof=B9,
    cb=3.12, ct=2.62, doors5=False, dr=2.55, pf=0.3, pr=0.22, fa=0.82))
add("5C", "vw", "Beetle", 2014, H, mk(H, 4.28, 1.81, 1.49, 2.54, "beetle2", hn=0.64, hb=0.96, hbr=1.0, ws0=1.45, prof=B5,
    cb=3.35, ct=2.85, doors5=False, dr=2.7, pf=0.25, pr=0.18, fa=0.86))
add("3B", "vw", "Passat B5", 2001, S, mk(S, 4.7, 1.75, 1.46, 2.7, "golf4", hn=0.72))
add("3C", "vw", "Passat B6 / B7", 2008, S, mk(S, 4.77, 1.82, 1.47, 2.71, "golf6", hn=0.74, bp=[(2.4, 2.34, 0.1), (3.66, 3.62, 0.04, "b")]))
add("3G", "vw", "Passat B8", 2017, S, mk(S, 4.77, 1.83, 1.46, 2.79, "golf7", hn=0.74, bp=[(2.45, 2.39, 0.1), (3.7, 3.66, 0.04, "b")]))
add("35", "vw", "Passat CC", 2011, S, mk(S, 4.8, 1.86, 1.42, 2.71, "golf6", hn=0.72, ws1=2.5, r1=3.25, ud=4.32, zdk=1.07, ct=3.15, cb=3.75, Hr=1.36))
add("1T", "vw", "Touran", 2008, "mpv", mk("mpv", 4.39, 1.79, 1.63, 2.68, "golf5", bp=[(2.1, 2.06, 0.1), (3.48, 3.56, 0.1, "b")]))
add("5T", "vw", "Touran II", 2018, "mpv", mk("mpv", 4.53, 1.83, 1.66, 2.79, "golf7", ws0=1.2, bp=[(2.2, 2.16, 0.1), (3.62, 3.7, 0.12, "b")]))
add("5N", "vw", "Tiguan", 2012, "suv", mk("suv", 4.43, 1.81, 1.69, 2.6, "golf6", ws1=2.0, r1=4.0, bp=[(2.32, 2.28, 0.1), (3.36, 3.46, 0.12, "b")]))
add("AD", "vw", "Tiguan II", 2019, "suv", mk("suv", 4.49, 1.84, 1.67, 2.68, "golf7", Hr=1.6, bp=[(2.4, 2.35, 0.1), (3.48, 3.58, 0.13, "b")]))
add("6N", "vw", "Polo III", 1998, H, mk(H, 3.72, 1.65, 1.42, 2.41, "polo3", hn=0.72, hb=0.95, ws1=1.88, r1=3.2, ut=3.6, cb=2.86, ct=2.8))
add("9N", "vw", "Polo IV", 2005, H, mk(H, 3.92, 1.65, 1.47, 2.46, "polo4", hn=0.74, hb=0.98, r1=3.36, ut=3.8, cb=3.0, ct=2.88))
add("6R", "vw", "Polo V", 2013, H, mk(H, 3.97, 1.68, 1.46, 2.47, "golf6", r1=3.38, ut=3.82, cb=3.05, ct=2.92))
add("AW", "vw", "Polo VI", 2020, H, mk(H, 4.05, 1.75, 1.46, 2.55, "golf7", r1=3.42, ut=3.9, cb=3.1, ct=2.95))
add("13", "vw", "Scirocco III", 2011, "coupe", mk("coupe", 4.26, 1.81, 1.4, 2.58, "golf6", hn=0.68, hb=0.92, hbr=0.98, ws1=2.3, r1=3.45, ud=4.05, zdk=1.0, Hr=1.33, cb=3.28, ct=3.0))
add("7N", "vw", "Sharan II", 2014, "mpv", mk("mpv", 4.85, 1.9, 1.72, 2.92, "golf6", hn=0.8, hb=1.04, hbr=1.1, ws0=1.1, bp=[(2.3, 2.26, 0.1), (3.75, 3.85, 0.1, "b")],
    doors=[(1.25, 1.35), (2.3, 2.33), (3.7, 3.7)], handles=[1.96, 2.45]))
add("1F", "vw", "Eos", 2010, "convertible", mk("convertible", 4.41, 1.79, 1.44, 2.58, "golf6"))
add("7H", "vw", "Transporter T5", 2008, "van", mk("van", 4.89, 1.9, 1.99, 3.0, "van"))
# Audi
add("8X", "audi", "A1 8X", 2013, H)
add("GB", "audi", "A1 GB", 2020, H)
add("8L", "audi", "A3 8L", 2000, H, mk(H, 4.15, 1.74, 1.42, 2.51, "audi_old", doors5=False, r1=3.45, ut=3.98, cb=3.2, ct=3.0))
add("8P", "audi", "A3 8P", 2007, H, mk(H, 4.29, 1.77, 1.43, 2.58, "audi_sf", r1=3.66, ut=4.12, cb=3.4, ct=3.22))
add("8V", "audi", "A3 8V", 2016, H, mk(H, 4.31, 1.79, 1.42, 2.64, "audi_sf2", r1=3.66, ut=4.13, cb=3.36, ct=3.2))
add("8Y", "audi", "A3 8Y", 2022, H, mk(H, 4.34, 1.82, 1.43, 2.64, "audi_sf2", r1=3.66, ut=4.16, cb=3.36, ct=3.2, Hr=1.37))
T8N = [(0, 0.62, "n"), (0.25, 0.71, "b"), (0.7, 0.82, "b"), (1.05, 0.88, "b")] +     dome(1.4, 3.92, 0.93, 1.35, 0.42, 8, "wwrrrhhte")[:-1] + [(3.86, 0.96, "t"), (4.04, 0.9, "e")]
T8J = [(0, 0.64, "n"), (0.3, 0.73, "b"), (0.9, 0.85, "b")] +     dome(1.42, 4.02, 0.93, 1.35, 0.4, 8, "wwrrrhhde")[:-1] + [(4.08, 0.97, "t"), (4.18, 0.9, "e")]
add("8N", "audi", "TT 8N", 2002, "coupe", mk("coupe", 4.04, 1.76, 1.35, 2.42, "tt1", hn=0.62, hb=0.92, hbr=0.94, ws0=1.4, prof=T8N,
    cb=3.12, ct=2.62, pf=0.24, pr=0.2, dr=2.55, fa=0.8))
add("8J", "audi", "TT 8J", 2010, "coupe", mk("coupe", 4.18, 1.84, 1.35, 2.47, "audi_sf", hn=0.64, hb=0.92, hbr=0.95, ws0=1.42, prof=T8J,
    cb=3.25, ct=2.72, pf=0.2, pr=0.16, dr=2.65, fa=0.86))
add("FV", "audi", "TT 8S", 2017, "coupe", mk("coupe", 4.18, 1.83, 1.35, 2.5, "audi_sf2", hn=0.68, hb=0.92, hbr=0.95, ws0=1.42, ws1=2.12,
    mid=(2.6, 1.355), r1=3.12, Hr=1.25, ud=3.95, zdk=0.98, cb=3.12, ct=2.74, pf=0.18, pr=0.13, dr=2.7))
add("8D", "audi", "A4 B5", 1998, S, mk(S, 4.48, 1.73, 1.42, 2.62, "audi_old", hn=0.7))
add("8E", "audi", "A4 B6 / B7", 2004, S, mk(S, 4.55, 1.77, 1.43, 2.65, "audi_sf"))
add("8K", "audi", "A4 B8", 2011, S, mk(S, 4.7, 1.83, 1.43, 2.81, "audi_sf", hn=0.72))
add("8W", "audi", "A4 B9", 2018, S, mk(S, 4.73, 1.84, 1.43, 2.82, "audi_sf2", hn=0.72))
add("4B", "audi", "A6 C5", 2001, S, mk(S, 4.8, 1.81, 1.45, 2.76, "audi_old", hn=0.72, mid=(2.8, 1.465), r1=3.42, ud=4.3))
add("4F", "audi", "A6 C6", 2008, S, mk(S, 4.92, 1.86, 1.46, 2.84, "audi_sf", hn=0.74))
add("4G", "audi", "A6 C7", 2014, S, mk(S, 4.93, 1.87, 1.46, 2.91, "audi_sf2", hn=0.74))
# SEAT
add("6K", "seat", "Ibiza II", 1997, H)
add("KJ", "seat", "Ibiza V", 2019, H)
add("1M", "seat", "Leon I", 2002, H, mk(H, 4.18, 1.74, 1.44, 2.51, "seat1", r1=3.5, ut=4.0, cb=3.18, ct=3.02))
add("1P", "seat", "Leon II", 2009, H, mk(H, 4.32, 1.77, 1.46, 2.58, "seat1", ws0=1.35, ws1=2.3, r1=3.6, ut=4.12, cb=3.3, ct=3.2,
    handles=[2.05], doors=[(1.45, 1.3), (2.38, 2.41), (3.4, 3.2)]))
add("5F", "seat", "Leon III", 2015, H, mk(H, 4.26, 1.82, 1.46, 2.64, "seat3", r1=3.6, ut=4.08, cb=3.3, ct=3.12))
add("KL", "seat", "Leon IV", 2022, H, mk(H, 4.37, 1.8, 1.46, 2.69, "seat4", r1=3.7, ut=4.18, cb=3.38, ct=3.2))
add("6L", "seat", "Ibiza III", 2005, H, mk(H, 3.95, 1.7, 1.44, 2.46, "seat1", r1=3.36, ut=3.82, cb=3.05, ct=2.9))
add("6J", "seat", "Ibiza IV", 2012, H, mk(H, 4.05, 1.69, 1.45, 2.47, "seat3", r1=3.4, ut=3.9, cb=3.1, ct=2.95))
# Skoda
add("1U", "skoda", "Octavia I", 2001, S, mk(S, 4.51, 1.73, 1.43, 2.51, "skoda1", hn=0.72, r1=3.3, ud=4.08, zdk=1.08, cb=3.55, ct=3.15,
    bp=[(2.25, 2.2, 0.1), (3.48, 3.42, 0.04, "b")]))
add("1Z", "skoda", "Octavia II", 2008, S, mk(S, 4.57, 1.77, 1.46, 2.58, "skoda2", hn=0.73, r1=3.35, ud=4.12, zdk=1.09, cb=3.62, ct=3.2,
    bp=[(2.3, 2.25, 0.1), (3.52, 3.46, 0.04, "b")]))
add("5E", "skoda", "Octavia III", 2016, S, mk(S, 4.66, 1.81, 1.46, 2.69, "skoda3", hn=0.73, r1=3.42, ud=4.18, zdk=1.09, cb=3.7, ct=3.28,
    bp=[(2.36, 2.31, 0.1), (3.6, 3.54, 0.04, "b")]))
add("NX", "skoda", "Octavia IV", 2022, S, mk(S, 4.69, 1.83, 1.47, 2.69, "skoda3", hn=0.73, r1=3.45, ud=4.2, zdk=1.09, cb=3.72, ct=3.3,
    bp=[(2.38, 2.33, 0.1), (3.62, 3.56, 0.04, "b")]))
add("6Y", "skoda", "Fabia I", 2003, H, mk(H, 3.96, 1.65, 1.45, 2.46, "skoda1", r1=3.4, ut=3.84, cb=3.06, ct=2.95))
add("5J", "skoda", "Fabia II", 2010, H, mk(H, 4.0, 1.64, 1.5, 2.45, "skoda2", ws1=1.95, Hr=1.49, r1=3.55, ut=3.9, cb=3.2, ct=3.15, tdrop=0.04))
add("3U", "skoda", "Superb I", 2005, S, mk(S, 4.8, 1.76, 1.47, 2.8, "skoda1", hn=0.74, bp=[(2.48, 2.43, 0.1), (3.66, 3.62, 0.04, "b")]))
add("3T", "skoda", "Superb II", 2011, S, mk(S, 4.84, 1.82, 1.46, 2.76, "skoda2", hn=0.74, r1=3.55, ud=4.36, zdk=1.09, cb=3.95, ct=3.45,
    bp=[(2.48, 2.43, 0.1), (3.75, 3.7, 0.05, "b")]))
add("3V", "skoda", "Superb III", 2018, S, mk(S, 4.86, 1.86, 1.47, 2.84, "skoda3", hn=0.74, r1=3.58, ud=4.4, zdk=1.1, cb=3.98, ct=3.48,
    bp=[(2.5, 2.45, 0.1), (3.78, 3.72, 0.05, "b")]))


def main():
    os.makedirs(OUT, exist_ok=True)
    files = []
    for b, p in BODIES.items():
        fn = "body-%s.svg" % b
        open(os.path.join(OUT, fn), "w", newline="\n").write(Car(p).svg())
        files.append(fn)
    models = []
    for m in M:
        fn = ""
        if m["same"]:
            fn = m["same"] + ".svg"
        elif m["params"]:
            fn = m["code"] + ".svg"
            open(os.path.join(OUT, fn), "w", newline="\n").write(Car(m["params"]).svg())
            files.append(fn)
        models.append(dict(code=m["code"], make=m["make"], name=m["name"], year=m["year"], body=m["body"], file=fn))
    idx = dict(v=1, bodies=list(BODIES.keys()), models=models)
    js = json.dumps(idx, separators=(",", ":"))
    js = js.replace("},{", "},\n{").replace('"models":[', '"models":[\n')
    open(os.path.join(OUT, "index.json"), "w", newline="\n").write(js + "\n")
    tot = 0
    for f in os.listdir(OUT):
        tot += os.path.getsize(os.path.join(OUT, f))
    sizes = sorted((os.path.getsize(os.path.join(OUT, f)), f) for f in files)
    print("files", len(files), "total", tot, "min", sizes[0], "max", sizes[-1])
    return files


if __name__ == "__main__":
    main()
