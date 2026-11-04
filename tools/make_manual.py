#!/usr/bin/env python3
"""docs/manual_src.html (the sections) -> docs/user_manual.html (head and style of the previous manual, contents list from the
sections, header, footer). [[shot NAME|alt|caption]] becomes a figure with the size of docs/manual_img/NAME.webp.
Screenshots: tools/manual_shots.py. Usage: tools/make_manual.py"""
import os, re
from PIL import Image
R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
D = os.path.join(R, "docs")
prev = open(os.path.join(D, "user_manual_prev.html")).read()
head = prev[:prev.index('<div class="wrap">')]
body = open(os.path.join(D, "manual_src.html")).read()
body = re.sub(r"<!--.*?-->\s*", "", body, count=1, flags=re.S)

def shot(m):
    name, alt, cap = m.group(1), m.group(2), m.group(3)
    w, h = Image.open(os.path.join(D, "manual_img", name + ".webp")).size
    if name == "wide": w, h = w, h
    else: w, h = w // 2, h // 2             # 2x pictures, shown at their CSS size
    return (f'<figure class="shot"><img src="manual_img/{name}.webp" width="{w}" height="{h}" alt="{alt}" loading="lazy">'
            f'<figcaption>{cap}</figcaption></figure>')
body = re.sub(r"\[\[shot ([a-z0-9_]+)\|(.*?)\|(.*?)\]\]", shot, body)
secs = re.findall(r'<section class="s" id="([a-z0-9]+)">.*?<h2>(.*?)</h2>', body, flags=re.S)
toc = "\n".join(f'    <li><a href="#{i}">{t}</a></li>' for i, t in secs)
page = head + f'''<div class="wrap">
<nav class="toc" aria-label="Contents">
  <div class="eyebrow">Contents</div>
  <ol>
{toc}
  </ol>
</nav>

<main>
<header class="top">
  <div class="eyebrow">Elegoo Saturn 4 Ultra 16K · firmware V1.5.9 + mod</div>
  <h1>Mod user manual</h1>
  <p class="lead">What each function of the modded printer does, why it exists, and how to use it. The mod adds a force monitor, resin heating control, a dynamic wait and dynamic lift, a fixed two-stage tilt, slow layers, settings profiles, manual exposure and multi exposure, a Z monitor and flex plate compensation, all driven from a password-protected dashboard served by the printer itself.</p>
  <ul class="facts">
    <li>Build <b>45</b> · mod <b>8.40</b> (normal)</li>
    <li>As of <b>31 October 2026</b></li>
    <li>Screenshots: <b>live dashboard</b>, every card fully expanded (example data where marked)</li>
    <li>Each section: picture · why it exists · how to use it</li>
  </ul>
</header>

{body}
<footer>Saturn 4 Ultra 16K mod · build 45 (mod 8.40, normal line) on stock firmware V1.5.9 · 31 October 2026. The screenshots come from the live dashboard of the printer; the graphs of the Z monitor and Dynamic lift, the armed time of Manual Exposure and the two named profiles in the Settings picture are example data.</footer>
</main>
</div>

</body></html>
'''
open(os.path.join(D, "user_manual.html"), "w").write(page)
print("sections:", len(secs), "size:", len(page))
