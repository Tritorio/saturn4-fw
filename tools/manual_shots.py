#!/usr/bin/env python3
"""Screenshots of the dashboard cards for docs/user_manual.html (docs/manual_img/*.webp).

  tools/manual_shots.py [NAME ...]     all shots, or only the named ones ("list" prints the names)
Needs TPW (telnet password) to upload a scratch copy of the page to the printer (/tmp/x/index.html, served by :3030),
headless Chrome from WSL (chrome.exe) and Pillow. Read-only: the scratch page never sends a command (cmd / helper are
stubbed), it only shows the live data of the printer. Sample profiles are faked in the page for the profile shots.
Each shot: the card(s) are moved into an empty black page, Chrome screenshots it at 2x, Pillow trims the black margin.
"""
import os, re, subprocess, sys
from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "mods/customer/mod/www/index.html")
OUT = os.path.join(ROOT, "docs/manual_img")
HOST = "192.168.0.44"
CHROME = "/mnt/c/Program Files/Google/Chrome/Application/chrome.exe"
SCRATCH = os.environ.get("MANUAL_SCRATCH", "/tmp/claude-1000/manual_shots")

# name -> (JS that runs after the data has loaded and returns the ids / selectors to show, window width, wait ms)
SHOTS = {
  "header":   ('''return ["header", ".chips"];'''),
  "status":   ('''return ["statuscard"];'''),
  "cpu":      ('''$("syscpu").click(); await wait(500); return ["statuscard"];'''),
  "storage":  ('''$("sysdisk").click(); await wait(500); return ["statuscard"];'''),
  "features": ('''if (!FEAT.open) $("fshow").click(); await wait(800); return ["statuscard"];'''),
  "heating":  ('''heatOpen(true); await wait(500); return ["heatcard"];'''),
  "force":    ('''return ["forcecard"];'''),
  "stages":   ('''$("stg").click(); await wait(500); return ["forcecard"];'''),
  "calibrate":('''$("calopen").click(); await wait(500); return ["calcard"];'''),
  "wait":     ('''$("dwopen").click(); await wait(500); document.querySelector('.qbtn[data-q="dwcard"]').click(); return ["dwcard"];'''),
  "gaugesync":('''$("gsopen").click(); await wait(500); return ["gscard"];'''),
  "dynlift":  ('''$("dlopen").click(); await wait(500); document.querySelector('.qbtn[data-q="dlcard"]').click(); return ["dlcard"];'''),
  "tilt":     ('''return ["tiltcard"];'''),
  "slowset":  ('''$("slsetopen").click(); await wait(500); return ["slsetcard"];'''),
  "slowlist": ('''slowOpen(stemOf(files[0].name)); await wait(800); return ["slowcard"];'''),
  "files":    ('''return ["filescard"];'''),
  "startprint": ('''fakeProfiles(); const b = document.querySelector('#flist button[data-act="print"]'); b.disabled = false; b.click(); await wait(1200);
                    $("pfsel").value = "Fast prints"; $("pfsel").dispatchEvent(new Event("change")); await wait(300); return ["#dlg"];'''),
  "settings": ('''fakeProfiles(); $("setopen").click(); await wait(600); return ["setcard"];'''),
  "machine":  ('''$("setopen").click(); await wait(300); $("machopen").click(); await wait(800); return ["machcard"];'''),
  "history":  ('''hOpen(true); await wait(1500); return ["histcard"];'''),
  "tiltmove": ('''xmOpen(true); await wait(600); return ["xmcard"];'''),
  "exposure": ('''exOpen(true); await wait(600); return ["excard"];'''),
  "flex":     ('''fxOpen(true); await wait(600); return ["fxcard"];'''),
  "multiexp": ('''mxOpen(true); await wait(600); document.querySelector('.qbtn[data-q="mxcard"]').click(); return ["mxcard"];'''),
  "zmonitor": ('''zmOpen(true); await wait(600); return ["zmcard"];'''),
  "developer": ('''return ["devcard"];'''),
  "wide":     ('''for (const f of [() => gsOpen(true), () => dlOpen(true), () => exOpen(true), () => zmOpen(true), () => xmOpen(true)]) { try { f(); } catch (e) {} } if (!FEAT.open) $("fshow").click(); await wait(1500); return null;'''),
}
WIDTH = {"wide": 1920}
HEIGHT = {"wide": 2600}

DRIVER = r'''
// example data for the cards that are empty while the printer is idle (the captions say "example data"); never sent anywhere
window.SHOTFIX = j => {
  const name = location.hash.slice(1);
  const z = [[1, 0.099, 99.4, 1, 100], [2, 0.199, 100.0, 0, 100]]; let h = 0.199;
  for (let i = 3; i <= 14; i++) { h += 0.05; z.push([i, +h.toFixed(3), i === 9 ? 51.2 : 50.0, 0, 50]); }
  if (name === "exposure") { if (j.mx) j.mx.on = 0; if (j.expo) Object.assign(j.expo, {job: 0, bon: 0, non: 1, n: 2800, fb: -1, fn: -1, mode: 1}); }   // example: a time armed while no job runs
  if (["zmonitor", "wide"].includes(name)) j.zm = {now: j.zm ? j.zm.now : 6, n: 14, max: 1.2, h: z};
  if (["dynlift", "wide"].includes(name) && j.dl) j.dl.h = [[1, 1480, 72, 63.5, 0], [2, 1620, 72, 64.0, 0], [3, 1390, 72, 62.5, 0], [4, 1710, 72, 65.5, 0], [5, 1530, 72, 63.0, 0], [6, 1440, 72, 63.5, 0], [7, 1660, 72, 64.5, 0], [8, 1580, 72, 63.5, 0]];
};
;(async () => {
  const name = location.hash.slice(1); if (!name) return;
  const wait = ms => new Promise(r => setTimeout(r, ms));
  window.fakeProfiles = () => {
    const base = snapParse("1 1 0.50 1.00 1.00 1 1 0 1 33 40 1793020727 50 80 20 30 72 60 60 100 40 60 100 60 48 18 18 18 18 0 0 50 200 1 1 74 1 1 1 1 0 1 1 2000 1500 20 5 1");
    PF.list = [{name: "Fast prints", snap: {...base, min: 0.6, t: 1793040000, dl: [0, 2000, 1500, 20, 5]}},
               {name: "Gentle peel", snap: {...base, t: 1793043000, dl: [1, 1500, 1200, 30, 5], tcs: 0}}];
    PF.links = {}; PF.links[stemOf(files[0].name)] = "Fast prints"; setRender(); renderFiles();
  };
  await wait(7000);
  const ids = await (async () => { %s })();
  // every card fully expanded: the folded Settings groups and the "?" explanations are shown, nothing hides content.
  // cxApply (the fold logic, run every 0.5 s) is switched off first. Not for the overview shots (header, status, cpu, storage, wide, startprint).
  if (ids && !["header", "status", "cpu", "storage", "wide", "startprint"].includes(name)) {
    cxApply = () => {};
    for (const s of ids) {
      const e = s.startsWith(".") || s === "header" || s === "#dlg" ? document.querySelector(s) : $(s); if (!e) continue;
      e.querySelectorAll(".cxfold").forEach(x => x.classList.remove("cxfold"));
      e.querySelectorAll(".qnote").forEach(x => { x.hidden = false; });
      e.querySelectorAll(".qbtn").forEach(x => x.setAttribute("aria-expanded", "true"));
    }
    await wait(300);
  }
  document.querySelector(".screen").style.display = ids ? "none" : "";
  if (ids) {
    const root = document.createElement("div");
    root.id = "shotroot"; root.style.cssText = "position:absolute;left:0;top:0;width:600px;padding:12px;background:#000;display:flex;flex-direction:column;gap:12px";
    for (const s of ids) { if (s === "#dlg") continue; const e = s.startsWith(".") || s === "header" ? document.querySelector(s) : $(s); if (e) { e.hidden = false; root.appendChild(e); } }
    document.body.appendChild(root);
  }
  document.title = "READY";
})();
'''


def build_page():
    s = open(SRC).read()
    # never send anything: commands and helper calls are stubbed
    s = s.replace('const cmd = async line => waitCmd((await helper("/api/cmd", line)).id);', 'const cmd = async line => {};', 1)
    s = s.replace('async function helper(path, body) {', 'async function helper(path, body) {\n  return {ok: true, id: "x", in: true, left: 1700, ttl: 1800};', 1)
    s = s.replace('async function authPoll() {', 'async function authPoll() {\n  AUTH.in = true; AUTH.left = 1700; AUTH.ttl = 1800; authUi(); return;     // the shots show the logged-in dashboard', 1)
    s = s.replace('    fxClass(j.flex);\n    L = j;', '    fxClass(j.flex);\n    if (window.SHOTFIX) window.SHOTFIX(j);\n    L = j;', 1)
    i = s.rindex("</script>")
    js = DRIVER % ("switch (name) {\n" + "".join("case %r: {%s}\n" % (k, v) for k, v in SHOTS.items()) + "default: return null; }")
    return s[:i] + js + s[i:]


def upload(page):
    os.makedirs(SCRATCH, exist_ok=True)
    p = os.path.join(SCRATCH, "shot_index.html")
    open(p, "w").write(page)
    env = dict(os.environ, TPUT=p + "=/tmp/x/index.html", TTIMEOUT="200")
    subprocess.run([os.path.join(ROOT, "tools/tsh.py"), HOST, "mkdir -p /tmp/x"], env=env, capture_output=True)
    r = subprocess.run([os.path.join(ROOT, "tools/tsh.py"), HOST, "md5sum /tmp/x/index.html"], env=env, capture_output=True, text=True)
    print("uploaded:", r.stdout.strip()[-60:])


def shoot(name):
    w = WIDTH.get(name, 640)
    h = HEIGHT.get(name, 3400)
    png = os.path.join(SCRATCH, name + ".png")
    wp = subprocess.run(["wslpath", "-w", png], capture_output=True, text=True).stdout.strip()
    subprocess.run([CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=2", "--virtual-time-budget=30000",
                    f"--window-size={w},{h}", f"--screenshot={wp}", f"http://{HOST}:3030/tmp/x/index.html#{name}"],
                   capture_output=True, timeout=180)
    im = Image.open(png).convert("RGB")
    bg = Image.new("RGB", im.size, (0, 0, 0))
    box = ImageChops.difference(im, bg).getbbox()
    if box and name != "wide":
        pad = 16
        box = (max(0, box[0] - pad), max(0, box[1] - pad), min(im.width, box[2] + pad), min(im.height, box[3] + pad))
        im = im.crop(box)
    elif name == "wide":
        im = im.crop((0, 0, im.width, min(im.height, box[3] + 16 if box else im.height)))
        im = im.resize((im.width // 2, im.height // 2))      # the 1920 px page at 1x is plenty
    out = os.path.join(OUT, name + ".webp")
    im.save(out, "WEBP", quality=80, method=6)
    print(f"{name}: {im.width}x{im.height} -> {out} ({os.path.getsize(out) // 1024} KB)")


if __name__ == "__main__":
    names = sys.argv[1:]
    if names == ["list"]:
        print(" ".join(SHOTS)); sys.exit(0)
    names = names or list(SHOTS)
    upload(build_page())
    for n in names:
        shoot(n)
