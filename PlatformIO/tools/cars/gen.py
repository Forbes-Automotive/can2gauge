# Generates the can2gauge car pictures (data/cars/*.svg + index.json) from a simple 3D "loft" model of each car:
# a list of cross-sections along the car, projected in a fixed 3/4 front-left view, faces shaded by overlays.
import math, json, os, sys

import os
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "data", "cars")  # <project>/data/cars
K = 64.0            # pixels per metre (same for every car, so a van is bigger than a Polo)
OX, OY = 200.0, 183.0  # screen point of the centre of the car's footprint
PHI = math.radians(32)
ALPHA = math.radians(21)
LD = (math.cos(PHI), -math.sin(PHI) * math.sin(ALPHA))      # +u (towards the rear)
WD = (-math.sin(PHI), -math.cos(PHI) * math.sin(ALPHA))     # +v (towards the car's right side)
ZD = (0.0, -math.cos(ALPHA))                                # +z (up)
# direction towards the viewer (null vector of the projection)
_v = (WD[0] * ZD[1] - 0, 0 - LD[0] * ZD[1], LD[0] * WD[1] - WD[0] * LD[1])
if (-_v[0] - _v[1] + _v[2]) < 0:
    _v = (-_v[0], -_v[1], -_v[2])
_n = math.sqrt(sum(c * c for c in _v))
VIEW = tuple(c / _n for c in _v)
_l = (-0.35, -0.5, 0.8)
_n = math.sqrt(sum(c * c for c in _l))
LIGHT = tuple(c / _n for c in _l)
I_SIDE = -LIGHT[1]

TYRE, TYRE2, RIM, SPOKE, HUB = "#1c1f23", "#0f1113", "#c3c9d0", "#5d656e", "#8f979f"
DARK = "#101317"
LAMP, LAMP_IN = "#d5dae0", "#98a2ad"
RED = "#c3161d"


def fmt(x):
    s = "%.1f" % x
    if s.endswith(".0"):
        s = s[:-2]
    if s == "-0":
        s = "0"
    return s


class Car:
    def __init__(s, p):
        s.p = p
        s.L = p["L"]
        s.W = p["W"]
        s.cu = s.L / 2
        s.cv = s.W / 2
        s.out = []

    # ---------- projection ----------
    def P(s, pt):
        u, v, z = pt
        u -= s.cu
        v -= s.cv
        x = OX + K * (u * LD[0] + v * WD[0])
        y = OY + K * (u * LD[1] + v * WD[1] + z * ZD[1])
        return (x, y)

    def path(s, pts3, close=True):
        pts = [s.P(q) for q in pts3]
        return s.path2(pts, close)

    def path2(s, pts, close=True):
        d = "M" + " ".join(fmt(x) + " " + fmt(y) for x, y in pts)
        d = d.replace(" -", "-")
        if close:
            d += "Z"
        return d

    # ---------- the cross sections ----------
    def build(s):
        p = s.p
        prof = p["prof"]
        L, W = s.L, s.W
        c = p["c"]
        kinds = [k for (_, _, k) in prof]
        s.gs = None
        s.ge = None
        for i, (u, z, k) in enumerate(prof):
            if k == "w" and s.gs is None:
                s.gs = u
            if k in "rh":
                s.ge = prof[i + 1][0]
        hb, hbr = p["hb"], p["hbr"]
        pf, pr = p.get("pf", 0.15), p.get("pr", 0.09)
        th = p.get("th", 0.15)
        s.S = []
        s.info = []
        n = len(prof)
        for i, (u, z, k) in enumerate(prof):
            plan = pf * max(0, 1 - u / 0.5) ** 2 + pr * max(0, 1 - (L - u) / 0.4) ** 2
            glass = s.gs is not None and s.gs - 1e-6 <= u <= s.ge + 1e-6
            if glass:
                bz = s.belt(u)
                zc = min(bz, z - 0.02)
                tumble = max(th * min(1, max(0, (z - zc) / 0.35)), 0.03)
                dIn = plan + 0.03 + tumble
            else:
                zc = z - p.get("crown", 0.035)
                dIn = plan + 0.09
            zb = c + (0.07 if (i == 0 or i == n - 1) else 0)
            zb = p.get("zbf", zb) if i == 0 else zb
            zs = max(zc - p.get("sh", 0.17), zb + 0.08)
            du = [0, 0, 0, 0]
            if i == 0:
                du = p.get("fdu", [0.07, 0, 0.04, 0.06])
            if i == n - 1:
                du = p.get("rdu", [-0.07, 0, -0.03, -0.07])
            ins = [plan + 0.05, plan, plan + 0.03, dIn]
            zz = [zb, zs, zc, z]
            left = [(u + du[j], ins[j], zz[j]) for j in range(4)]
            right = [(u + du[j], W - ins[j], zz[j]) for j in range(4)]
            s.S.append(left + right[::-1])
            s.info.append(dict(u=u, z=z, k=k))

    def belt(s, u):
        p = s.p
        if s.gs is None:
            return p["hb"]
        t = (u - s.gs) / (s.ge - s.gs)
        return p["hb"] + (p["hbr"] - p["hb"]) * t

    def station_at(s, u):
        # interpolated section (8 points) at u
        S = s.S
        us = [inf["u"] for inf in s.info]
        if u <= us[0]:
            return S[0]
        if u >= us[-1]:
            return S[-1]
        for i in range(len(us) - 1):
            if us[i] <= u <= us[i + 1]:
                t = (u - us[i]) / max(1e-9, us[i + 1] - us[i])
                return [tuple(S[i][j][m] + (S[i + 1][j][m] - S[i][j][m]) * t for m in range(3)) for j in range(8)]

    def ztop(s, u):
        return s.station_at(u)[3][2]

    def surf(s, u, z, out=0.0):
        # point on the car's left side at length u, height z
        sec = s.station_at(u)[:4]
        if z <= sec[0][2]:
            a, b, t = sec[0], sec[1], 0.0
            q = sec[0]
            return (u, q[1] - out, z)
        for j in range(3):
            a, b = sec[j], sec[j + 1]
            if a[2] <= z <= b[2] or j == 2:
                t = (z - a[2]) / max(1e-9, b[2] - a[2])
                t = min(1, max(0, t))
                return (u, a[1] + (b[1] - a[1]) * t - out, z)

    def top(s, u, t):
        sec = s.station_at(u)
        a, b = sec[3], sec[4]
        return (a[0], a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)

    def cap(s, sx, f):
        # front face: sx = -1..1 across (1 = car's left edge), f = 0 (bottom) .. 1 (top of the face, below the crown)
        sec = s.S[0]
        A, C = sec[0], sec[2]
        z = A[2] + (C[2] - A[2]) * f
        # left edge at this height
        e = s.edge_at(sec[:4], z)
        half = s.W / 2 - e[1]
        return (e[0], s.W / 2 - sx * half, z)

    def edge_at(s, pts, z):
        for j in range(3):
            a, b = pts[j], pts[j + 1]
            if a[2] <= z <= b[2] or j == 2:
                t = (z - a[2]) / max(1e-9, b[2] - a[2])
                t = min(1, max(0, t))
                return tuple(a[m] + (b[m] - a[m]) * t for m in range(3))

    # ---------- faces ----------
    def faces(s):
        S = s.S
        out = []
        n = len(S)
        for i in range(n - 1):
            k = s.info[i]["k"]
            for j in range(8):
                j2 = (j + 1) % 8
                q = [S[i][j], S[i][j2], S[i + 1][j2], S[i + 1][j]]
                mat = "p"
                if j == 3 and k in "wh":
                    mat = "g"
                out.append((q, mat))
        out.append((S[0], "p"))
        out.append((S[-1][::-1], "p"))
        return out

    def normal(s, q):
        nx = ny = nz = 0.0
        for i in range(len(q)):
            a, b = q[i], q[(i + 1) % len(q)]
            nx += (a[1] - b[1]) * (a[2] + b[2])
            ny += (a[2] - b[2]) * (a[0] + b[0])
            nz += (a[0] - b[0]) * (a[1] + b[1])
        m = math.sqrt(nx * nx + ny * ny + nz * nz)
        if m < 1e-9:
            return None
        return (nx / m, ny / m, nz / m)

    def shade_of(s, nrm):
        I = sum(nrm[m] * LIGHT[m] for m in range(3))
        a = (I - I_SIDE) * 0.8
        a = max(-0.42, min(0.26, a))
        a = round(a / 0.04) * 0.04
        return a, I

    def emit_overlay(s, d, a):
        if abs(a) < 0.015:
            return
        col = "#fff" if a > 0 else "#000"
        s.out.append('<path fill="%s" fill-opacity="%s" d="%s"/>' % (col, ("%.2f" % abs(a)).lstrip("0"), d))

    def glass_col(s, I):
        g = min(1, max(0, (I + 0.3) / 1.3))
        a, b = (0x12, 0x18, 0x21), (0x48, 0x5a, 0x6e)
        return "#%02x%02x%02x" % tuple(int(a[m] + (b[m] - a[m]) * g) for m in range(3))

    def body(s):
        items = []
        for q, mat in s.faces():
            nrm = s.normal(q)
            if nrm is None:
                continue
            cen = tuple(sum(pt[m] for pt in q) / len(q) for m in range(3))
            ref = (min(max(cen[0], 0.3 * s.L), 0.7 * s.L), s.W / 2, 0.45 * s.p["H"])
            dv = tuple(cen[m] - ref[m] for m in range(3))
            if sum(nrm[m] * dv[m] for m in range(3)) < 0:
                nrm = tuple(-x for x in nrm)
            if sum(nrm[m] * VIEW[m] for m in range(3)) <= 0.01:
                continue
            depth = sum(cen[m] * VIEW[m] for m in range(3))
            a, I = s.shade_of(nrm)
            items.append((depth, q, mat, a, I))
        items.sort(key=lambda x: x[0])
        # all the paint in one path (same colour, order doesn't matter), then glass, then the shading overlays
        # grouped by strength (visible faces of this shape don't overlap, so grouping is safe)
        paint = []
        shades = {}
        for depth, q, mat, a, I in items:
            d = s.path(q)
            if mat == "p":
                paint.append(d)
            else:
                paint.append(d)
        s.out.append('<path class="paint" d="%s"/>' % "".join(paint))
        for depth, q, mat, a, I in items:
            if mat == "g":
                s.out.append('<path fill="%s" d="%s"/>' % (s.glass_col(I), s.path(q)))
            elif abs(a) >= 0.015:
                shades.setdefault(a, []).append(s.path(q))
        for a in sorted(shades):
            s.emit_overlay("".join(shades[a]), a)

    # ---------- details ----------
    def ellipse(s, cen3, r, fill, extra=""):
        cx, cy = s.P(cen3)
        a = (LD[0] * r * K, LD[1] * r * K)
        b = (0.0, ZD[1] * r * K)
        S00 = a[0] ** 2 + b[0] ** 2
        S11 = a[1] ** 2 + b[1] ** 2
        S01 = a[0] * a[1] + b[0] * b[1]
        tr = S00 + S11
        det = S00 * S11 - S01 * S01
        l1 = tr / 2 + math.sqrt(max(0, tr * tr / 4 - det))
        l2 = tr / 2 - math.sqrt(max(0, tr * tr / 4 - det))
        ang = 0.5 * math.degrees(math.atan2(2 * S01, S00 - S11))
        return '<ellipse cx="%s" cy="%s" rx="%s" ry="%s" transform="rotate(%s %s %s)"%s%s/>' % (
            fmt(cx), fmt(cy), fmt(math.sqrt(l1)), fmt(math.sqrt(l2)), fmt(ang), fmt(cx), fmt(cy),
            (' fill="%s"' % fill) if fill else "", extra)

    def shadow(s):
        L, W = s.L, s.W
        cx, cy = s.P((L / 2, W / 2, 0))
        corners = [s.P(q) for q in [(0, 0, 0), (L, 0, 0), (L, W, 0), (0, W, 0)]]
        w = max(x for x, y in corners) - min(x for x, y in corners)
        h = max(y for x, y in corners) - min(y for x, y in corners)
        for f, o in ((0.58, ".2"), (0.51, ".2"), (0.44, ".25")):
            s.out.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="#000" fill-opacity="%s"/>' % (
                fmt(cx), fmt(cy + 3), fmt(w * f), fmt(h * f * 0.95), o))

    def wheel_far(s, ua):
        p = s.p
        r = p["wr"]
        s.out.append(s.ellipse((ua, s.W - 0.08, r), r, TYRE2))

    def clad(s):
        # SUV: dark plastic sill strip along the bottom of the side
        if not s.p.get("clad"):
            return
        c = s.p["c"]
        h = s.p["clad"]
        pts = [(u, c) for u in frange(0.12, s.L - 0.08, 0.6)] + [(u, c + h) for u in frange(s.L - 0.08, 0.12, -0.6)]
        s.out.append('<path fill="#1e2227" d="%s"/>' % s.side_poly(pts, 0.004))

    def arch(s, ua, extra=0.0, col="#0a0c0f"):
        p = s.p
        r = p["wr"]
        ra = r + p.get("arch", 0.05) + extra
        c = p["c"]
        a0 = math.degrees(math.asin(max(-1, min(1, (r - c) / ra))))
        pts = []
        steps = 10
        for i in range(steps + 1):
            th = math.radians(-a0 + (180 + 2 * a0) * i / steps)
            u = ua + ra * math.cos(th)
            z = r + ra * math.sin(th)
            pts.append(s.surf(u, max(z, c)))
        pts = pts[::-1]
        s.out.append('<path fill="%s" d="%s"/>' % (col, s.path(pts)))

    def wheel(s, ua):
        p = s.p
        r = p["wr"]
        v0 = s.surf(ua, r)[1] + 0.02
        s.out.append(s.ellipse((ua, v0 + 0.16, r), r, TYRE2))
        s.out.append(s.ellipse((ua, v0, r), r, TYRE))
        s.out.append(s.ellipse((ua, v0 - 0.005, r), r * 0.68, RIM))
        sw = r * 0.26 * K
        circ = 2 * math.pi * r * 0.42 * K * 0.85
        n = p.get("spokes", 5)
        dash = circ / n
        s.out.append(s.ellipse((ua, v0 - 0.01, r), r * 0.42, "none",
                               ' stroke="%s" stroke-width="%s" stroke-dasharray="%s %s"' % (
                                   SPOKE, fmt(sw), fmt(dash * 0.55), fmt(dash * 0.45))))
        s.out.append(s.ellipse((ua, v0 - 0.012, r), r * 0.13, HUB))

    def dlo(s):
        # side window opening in (u, z), clipped from the glasshouse band
        p = s.p
        if s.gs is None:
            return None
        rail = p.get("rail", 0.055)
        gap = p.get("gap", 0.025)
        us = [inf["u"] for inf in s.info if s.gs - 1e-6 <= inf["u"] <= s.ge + 1e-6]
        poly = [(u, s.ztop(u) - rail) for u in us]
        poly += [(us[-1], -5), (us[0], -5)]
        poly = clip(poly, lambda q: q[1] - (s.belt(q[0]) + gap))
        cb, ct = p["cb"], p["ct"]  # rear edge of the side glass: u at the belt, u at roof height
        zb0, zt0 = s.belt(cb), p["H"]
        # inside if on the front side of the line (cb,zb0)-(ct,zt0)
        def rear(q):
            return -((q[0] - cb) * (zt0 - zb0) - (q[1] - zb0) * (ct - cb)) * (1 if zt0 > zb0 else -1)
        poly = clip(poly, rear)
        fe = p.get("fe")  # optional front edge (quarter-light gone etc)
        if fe:
            fb, ft = fe
            def front(q):
                return ((q[0] - fb) * (zt0 - zb0) - (q[1] - zb0) * (ft - fb))
            poly = clip(poly, front)
        return poly

    def side_poly(s, poly, out=0.004):
        # map a (u,z) polygon onto the side, splitting long edges
        pts = []
        n = len(poly)
        for i in range(n):
            a, b = poly[i], poly[(i + 1) % n]
            seg = max(1, int(abs(b[0] - a[0]) / 0.25))
            for k in range(seg):
                t = k / seg
                pts.append(s.surf(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, out))
        return s.path(pts)

    def side_band_overlay(s, u):
        sec = s.station_at(u)
        a, b = sec[2], sec[3]
        q = [a, b, (b[0] + 0.1, b[1], b[2]), (a[0] + 0.1, a[1], a[2])]
        nrm = s.normal(q)
        if nrm is None:
            return 0
        if nrm[1] > 0:
            nrm = tuple(-x for x in nrm)
        return s.shade_of(nrm)[0]

    def windows(s):
        p = s.p
        poly = s.dlo()
        if not poly or len(poly) < 3:
            return
        gcol = p.get("gcol", "#1d2631")
        s.out.append('<path fill="%s" d="%s"/>' % (gcol, s.side_poly(poly)))
        # a soft reflection stripe
        u0 = min(q[0] for q in poly)
        u1 = max(q[0] for q in poly)
        span = u1 - u0
        for (fa, fb, op) in ((0.18, 0.42, ".09"), (0.6, 0.68, ".07")):
            a = u0 + span * fa
            b = u0 + span * fb
            st = clip(poly, lambda q, a=a: q[0] - a - (q[1] - 1.0) * 0.6)
            st = clip(st, lambda q, b=b: -(q[0] - b - (q[1] - 1.0) * 0.6))
            if st and len(st) >= 3:
                s.out.append('<path fill="#fff" fill-opacity="%s" d="%s"/>' % (op, s.side_poly(st, 0.006)))
        rail = p.get("rail", 0.055)
        gap = p.get("gap", 0.025)
        for spec in p.get("bp", []):
            ub, ut, w = spec[0], spec[1], spec[2]
            col = spec[3] if len(spec) > 3 else "b"
            zt = s.ztop(ut) - rail + 0.01
            zb = s.belt(ub) + gap - 0.01
            q = [(ub - w / 2, zb), (ub + w / 2, zb), (ut + w / 2, zt), (ut - w / 2, zt)]
            q = clip(q, lambda r: -((r[0] - p["cb"]) * (p["H"] - s.belt(p["cb"])) - (r[1] - s.belt(p["cb"])) * (p["ct"] - p["cb"])))
            if not q or len(q) < 3:
                continue
            d = s.side_poly(q, 0.008)
            if col == "b":
                s.out.append('<path fill="#14181d" d="%s"/>' % d)
            else:
                s.out.append('<path class="paint" d="%s"/>' % d)
                s.emit_overlay(d, s.side_band_overlay(ub))

    def doors(s):
        p = s.p
        c = p["c"]
        lines = []
        for (ut, ub) in p.get("doors", []):
            zt = s.belt(ut) - 0.012 if s.gs is not None else s.S[1][2][2] - 0.02
            pts = [s.surf(ut, zt, 0.003), s.surf((ut + ub) / 2, (zt + c) / 2 + 0.05, 0.003), s.surf(ub, c + 0.05, 0.003)]
            lines.append(s.path(pts, False))
        if lines:
            s.out.append('<path fill="none" stroke="#000" stroke-opacity=".38" stroke-width="1" d="%s"/>' % "".join(lines))
        hs = []
        for u in p.get("handles", []):
            z = s.belt(u) - 0.08 if s.gs is not None else p["hb"] - 0.1
            hs.append(s.path([s.surf(u, z, 0.006), s.surf(u + 0.13, z, 0.006)], False))
        if hs:
            s.out.append('<path fill="none" stroke="#000" stroke-opacity=".45" stroke-width="2" stroke-linecap="round" d="%s"/>' % "".join(hs))
        # sill shadow line
        cl = p.get("crease")
        if cl:
            pts = [s.surf(u, cl[1] + (cl[3] - cl[1]) * (u - cl[0]) / (cl[2] - cl[0]), 0.002)
                   for u in frange(cl[0], cl[2], 0.3)]
            s.out.append('<path fill="none" stroke="#fff" stroke-opacity=".22" stroke-width="1" d="%s"/>' % s.path(pts, False))

    def front(s):
        p = s.p
        # grille and intakes
        for poly, col in p.get("grille", []):
            for side in (1,):
                pts = [s.cap(x, f) for (x, f) in poly]
                pts = pts + [s.cap(-x, f) for (x, f) in poly[::-1]]
                s.out.append('<path fill="%s" d="%s"/>' % (col, s.path(pts)))
        for poly, col in p.get("frontbits", []):
            for side in (1, -1):
                pts = [s.cap(x * side, f) for (x, f) in poly]
                s.out.append('<path fill="%s" d="%s"/>' % (col, s.path(pts)))
        hl = p.get("hl")
        if hl:
            for side in (1, -1):
                pts = [s.cap(x * side, f) for (x, f) in hl]
                s.out.append('<path fill="%s" d="%s"/>' % (LAMP, s.path(pts)))
                inner = shrink(hl, 0.55)
                pts = [s.cap(x * side, f) for (x, f) in inner]
                s.out.append('<path fill="%s" d="%s"/>' % (LAMP_IN, s.path(pts)))
            wrap = p.get("wrap")
            if wrap:
                A, C = s.S[0][0][2], s.S[0][2][2]
                pts = [(u, A + (C - A) * f) for (u, f) in wrap]
                s.out.append('<path fill="%s" d="%s"/>' % (LAMP, s.side_poly(pts, 0.003)))
        for poly, col in p.get("frontbits2", []):
            for side in (1, -1):
                pts = [s.cap(x * side, f) for (x, f) in poly]
                s.out.append('<path fill="%s" d="%s"/>' % (col, s.path(pts)))

    def rear(s):
        p = s.p
        tl = p.get("tl")
        if tl:
            zc = s.S[-1][2][2]
            pts = [(s.L - du, zc + dz) for (du, dz) in tl]
            s.out.append('<path fill="%s" d="%s"/>' % (RED, s.side_poly(pts, 0.004)))

    def mirror(s):
        p = s.p
        if s.gs is None and not p.get("conv"):
            return
        um = p.get("mu", (s.gs if s.gs is not None else p["conv"]["ws0"]) + 0.14)
        zb = (s.belt(um) if s.gs is not None else p["hb"]) + 0.02
        b = s.surf(um, zb)[1]
        o = b - 0.2
        fr = [(um, b + 0.02, zb), (um, o, zb + 0.03), (um + 0.03, o - 0.01, zb + 0.13), (um + 0.02, b + 0.02, zb + 0.13)]
        sd = [(um, o, zb + 0.03), (um + 0.14, o + 0.04, zb + 0.04), (um + 0.15, o + 0.04, zb + 0.12), (um + 0.03, o - 0.01, zb + 0.13)]
        d1 = s.path(sd)
        s.out.append('<path class="paint" d="%s"/>' % d1)
        s.out.append('<path fill="#000" fill-opacity=".12" d="%s"/>' % d1)
        d2 = s.path(fr)
        s.out.append('<path class="paint" d="%s"/>' % d2)
        s.out.append('<path fill="#000" fill-opacity=".28" d="%s"/>' % d2)

    def windscreen_hi(s):
        if s.gs is None:
            return
        # highlight on the windscreen
        u0 = s.gs
        u1 = None
        for i, inf in enumerate(s.info):
            if inf["k"] == "w":
                u1 = s.info[i + 1]["u"]
        if u1 is None:
            return
        def wp(a, t):
            return s.top(u0 + (u1 - u0) * a, t)
        pts = [wp(0.1, 0.2), wp(0.2, 0.06), wp(0.9, 0.3), wp(0.85, 0.5)]
        s.out.append('<path fill="#fff" fill-opacity=".12" d="%s"/>' % s.path(pts))

    def convertible(s):
        p = s.p
        cv = p.get("conv")
        if not cv:
            return
        W = s.W
        ws0, ws1, Hws, ce = cv["ws0"], cv["ws1"], cv["Hws"], cv["end"]
        # cabin opening
        pts = []
        for u in frange(ws0 + 0.05, ce, 0.3):
            pts.append(s.top(u, 0.13))
        for u in frange(ce, ws0 + 0.05, -0.3):
            pts.append(s.top(u, 0.87))
        s.out.append('<path fill="#171a1f" d="%s"/>' % s.path(pts))
        # seats (headrests), back row first
        for (us, hh) in cv.get("seats", []):
            for t in (0.72, 0.3):
                b = s.top(us, t)
                z0 = b[2]
                q = [(us, b[1] - 0.13, z0), (us, b[1] + 0.13, z0), (us + 0.05, b[1] + 0.11, z0 + hh), (us + 0.05, b[1] - 0.11, z0 + hh)]
                s.out.append('<path fill="#2b3037" d="%s"/>' % s.path(q))
        # windscreen frame + glass
        b0, b1 = s.top(ws0, 0.04), s.top(ws0, 0.96)
        z0 = b0[2]
        fr = [b0, b1, (ws1, W - 0.2, Hws), (ws1, 0.2, Hws)]
        d = s.path(fr)
        s.out.append('<path class="paint" d="%s"/>' % d)
        s.out.append('<path fill="#fff" fill-opacity=".08" d="%s"/>' % d)
        def lerp(a, b, t):
            return tuple(a[m] + (b[m] - a[m]) * t for m in range(3))
        g = [lerp(lerp(fr[0], fr[3], 0.06), lerp(fr[1], fr[2], 0.06), 0.03),
             lerp(lerp(fr[0], fr[3], 0.06), lerp(fr[1], fr[2], 0.06), 0.97),
             lerp(lerp(fr[0], fr[3], 0.9), lerp(fr[1], fr[2], 0.9), 0.97),
             lerp(lerp(fr[0], fr[3], 0.9), lerp(fr[1], fr[2], 0.9), 0.03)]
        s.out.append('<path fill="#2c3846" d="%s"/>' % s.path(g))
        h = [lerp(lerp(g[0], g[3], 0.2), lerp(g[1], g[2], 0.2), 0.15), lerp(lerp(g[0], g[3], 0.1), lerp(g[1], g[2], 0.1), 0.3),
             lerp(lerp(g[0], g[3], 0.8), lerp(g[1], g[2], 0.8), 0.55), lerp(lerp(g[0], g[3], 0.9), lerp(g[1], g[2], 0.9), 0.4)]
        s.out.append('<path fill="#fff" fill-opacity=".14" d="%s"/>' % s.path(h))

    def bed(s):
        p = s.p
        bd = p.get("bed")
        if not bd:
            return
        pts = []
        for u in frange(bd[0], s.L - 0.1, 0.4):
            pts.append(s.top(u, 0.06))
        for u in frange(s.L - 0.1, bd[0], -0.4):
            pts.append(s.top(u, 0.94))
        s.out.append('<path fill="#15181c" d="%s"/>' % s.path(pts))

    def svg(s):
        p = s.p
        s.build()
        s.out = []
        s.shadow()
        fa = p["fa"]
        ra = fa + p["wb"]
        s.wheel_far(ra)
        s.wheel_far(fa)
        s.body()
        s.front()
        s.rear()
        s.bed()
        s.windscreen_hi()
        s.windows()
        s.doors()
        s.clad()
        if p.get("clad"):
            s.arch(ra, 0.06, "#1e2227")
            s.arch(fa, 0.06, "#1e2227")
        s.arch(ra)
        s.arch(fa)
        s.wheel(ra)
        s.wheel(fa)
        s.convertible()
        s.mirror()
        return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 240">'
                '<style>.paint{fill:var(--paint,#8a96a3)}</style>' + "".join(s.out) + "</svg>\n")


def frange(a, b, step):
    out = []
    n = max(1, int(math.ceil(abs(b - a) / abs(step))))
    for i in range(n + 1):
        out.append(a + (b - a) * i / n)
    return out


def shrink(poly, f):
    cx = sum(q[0] for q in poly) / len(poly)
    cy = sum(q[1] for q in poly) / len(poly)
    return [(cx + (x - cx) * f, cy + (y - cy) * f) for x, y in poly]


def clip(poly, fn):
    # keep the part of poly where fn(point) >= 0
    out = []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        fa, fb = fn(a), fn(b)
        if fa >= 0:
            out.append(a)
        if (fa >= 0) != (fb >= 0):
            t = fa / (fa - fb)
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
    return out
