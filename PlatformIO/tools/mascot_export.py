import asyncio, sys, base64, os, json
from playwright.async_api import async_playwright
OUT=sys.argv[2]
async def main():
  async with async_playwright() as p:
    b=await p.chromium.launch(); pg=await b.new_page(viewport={'width':1280,'height':1000})
    await pg.route('**/fonts.googleapis.com/**', lambda r: r.abort())
    errs=[]; pg.on('pageerror', lambda e: errs.append(str(e)))
    await pg.goto('file://'+sys.argv[1]); await pg.wait_for_timeout(600)
    await pg.evaluate("window.__export.stop()")
    clips=await pg.evaluate("window.__export.clips")
    only=sys.argv[3].split(',') if len(sys.argv)>3 and sys.argv[3] else None
    if only: clips={k:v for k,v in clips.items() if k in only}  # clips.json lists only what was exported, for mascot_pack.py
    print(clips)
    json.dump(clips, open(os.path.join(OUT,'clips.json'),'w'), indent=1)
    modes=sys.argv[4].split(',') if len(sys.argv)>4 else ['full','white','glow','mask']
    for name,c in clips.items():
      if only and name not in only: continue
      for mode in modes:
        d=os.path.join(OUT,mode,name); os.makedirs(d,exist_ok=True)
        for i in range(c['n']):
          url=await pg.evaluate(f"window.__export.frame('{name}',{i},'{mode}')")
          open(os.path.join(d,f'{i:04d}.png'),'wb').write(base64.b64decode(url.split(',',1)[1]))
      print(name,'done',c['n'])
    print('errors',errs)
    await b.close()
asyncio.run(main())
