#!/usr/bin/env python3
"""mascot_pack.py - packs the frames exported by mascot_export.py.

  python3 mascot_pack.py <export dir> <out dir>

Writes to <out dir>:
  manifest.json           per clip: frames, fps, loop, crop box, glow colour
  full/<clip>/NNNN.png    the 480x480 composited frames, untouched
  white/<clip>/NNNN.png   RGBA, cropped to the clip's box: body + face + effects (theme bits keyed magenta)
  glow/<clip>/NNNN.png    L (8-bit), cropped to the same box: the halo as an alpha mask
  preview/<clip>.gif      15 fps preview of the full frames
"""
import json, os, sys, shutil
from PIL import Image, ImageChops

src, out = sys.argv[1], sys.argv[2]
clips = json.load(open(os.path.join(src, 'clips.json')))
GLOW = {'oil_hot': '#FF4A2A', 'oil_cold': '#9FE4FF'}
GAIN = {'lock_full': 1.3, 'lock_slam': 1.3}
man = {'size': 480, 'decode': 'pixel with r>g and b>g is theme-tinted: accent=(min(r,b)-g)/255, white=g/255, rest black; any other pixel is literal', 'origin': 'top-left of the 480x480 frame; the figure is centred on (240,240)', 'key': '#FF00FF', 'clips': {}}
for d in ['full', 'white', 'glow', 'preview']:
    os.makedirs(os.path.join(out, d), exist_ok=True)

for name, c in clips.items():
    n = c['n']
    whites, glows = [], []
    for i in range(n):
        w = Image.open(os.path.join(src, 'white', name, f'{i:04d}.png')).convert('RGB')
        g = Image.open(os.path.join(src, 'glow', name, f'{i:04d}.png')).convert('L')
        # alpha = the body silhouette (mask export) plus anything drawn outside it (effects), by brightness
        m = Image.open(os.path.join(src, 'mask', name, f'{i:04d}.png')).convert('L')
        r_, g_, b_ = w.split()
        drawn = ImageChops.lighter(ImageChops.lighter(r_, g_), b_)
        alpha = ImageChops.lighter(m, drawn)
        rgba = w.copy(); rgba.putalpha(alpha)
        whites.append(rgba); glows.append(g)
    # one crop box per clip: union of the white alpha and the glow
    bbox = None
    for rgba, g in zip(whites, glows):
        for b in (rgba.getchannel('A').getbbox(), g.point(lambda v: 255 if v > 3 else 0).getbbox()):
            if b:
                bbox = b if bbox is None else (min(bbox[0], b[0]), min(bbox[1], b[1]), max(bbox[2], b[2]), max(bbox[3], b[3]))
    if bbox is None:
        bbox = (0, 0, 480, 480)
    x0, y0, x1, y1 = bbox
    x0, y0 = max(0, x0 - 2), max(0, y0 - 2); x1, y1 = min(480, x1 + 2), min(480, y1 + 2)
    for d in ['full', 'white', 'glow']:
        os.makedirs(os.path.join(out, d, name), exist_ok=True)
    frames = []
    for i in range(n):
        shutil.copy(os.path.join(src, 'full', name, f'{i:04d}.png'), os.path.join(out, 'full', name, f'{i:04d}.png'))
        whites[i].crop((x0, y0, x1, y1)).save(os.path.join(out, 'white', name, f'{i:04d}.png'), optimize=True)
        glows[i].crop((x0, y0, x1, y1)).save(os.path.join(out, 'glow', name, f'{i:04d}.png'), optimize=True)
        frames.append(Image.open(os.path.join(src, 'full', name, f'{i:04d}.png')).convert('RGB').resize((240, 240), Image.LANCZOS))
    # preview gif
    pal = [f.quantize(colors=64, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE) for f in frames]
    pal[0].save(os.path.join(out, 'preview', f'{name}.gif'), save_all=True, append_images=pal[1:], duration=int(round(1000 / c['fps'])), loop=0 if c['loop'] else 1, disposal=1)
    man['clips'][name] = {'frames': n, 'fps': c['fps'], 'loop': c['loop'], 'box': [x0, y0, x1 - x0, y1 - y0],
                          'glow': GLOW.get(name, 'accent'), 'glow_gain': GAIN.get(name, 1.0)}
    print(f'{name:14s} {n:4d} frames  box {x0},{y0} {x1 - x0}x{y1 - y0}')
json.dump(man, open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
