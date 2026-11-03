import os, sys, json, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import models
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = models.OUT
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"


def page(names, paints, cell, cols, title):
    h = ['<!DOCTYPE html><html><head><meta charset="utf-8"><title>%s</title><style>'
         'body{background:#0b0e13;color:#cdd5df;font:13px sans-serif;margin:16px}'
         '.g{display:grid;grid-template-columns:repeat(%d,%dpx);gap:10px}'
         '.c{background:#141a22;border-radius:10px;padding:6px}.c svg{width:100%%;display:block}'
         '.n{padding:2px 4px}</style></head><body><div class="g">' % (title, cols, cell)]
    idx = json.load(open(os.path.join(OUT, "index.json")))
    label = {}
    for m in idx["models"]:
        if m["file"]:
            label.setdefault(m["file"], m["code"] + " " + m["name"])
    for n in names:
        svg = open(os.path.join(OUT, n)).read()
        for pc in paints:
            h.append('<div class="c" style="--paint:%s">%s<div class="n">%s %s</div></div>' % (pc, svg, label.get(n, n), pc))
    h.append("</div></body></html>")
    return "".join(h)


def shot(html_name, png, w, hgt):
    path = os.path.join(HERE, html_name)
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--screenshot=" + os.path.join(HERE, png),
                    "--window-size=%d,%d" % (w, hgt), "file:///" + path.replace("\\", "/")], capture_output=True, timeout=120)


if __name__ == "__main__":
    files = models.main()
    allp = ["#c8102e", "#1f3a93"]
    cols = 6
    cell = 220
    open(os.path.join(HERE, "preview.html"), "w").write(page(files, allp, cell, cols, "cars"))
    rows = (len(files) * 2 + cols - 1) // cols
    shot("preview.html", "preview.png", cols * (cell + 10) + 40, rows * 175 + 40)
    # detail pages: 4 paints, bigger
    sel = sys.argv[1:] or []
    if sel:
        names = [f for f in files if any(f.startswith(s) for s in sel)]
        open(os.path.join(HERE, "detail.html"), "w").write(page(names, ["#c8102e", "#1f3a93", "#f2f2f2", "#151515"], 330, 4, "d"))
        r = len(names)
        shot("detail.html", "detail.png", 4 * 340 + 40, r * 245 + 40)
