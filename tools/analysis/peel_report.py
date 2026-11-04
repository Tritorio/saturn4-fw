#!/usr/bin/env python3
"""Peel force per layer joined with the tilt settings the app used for that layer -> one HTML report + CSV.
  peel_report.py JOB.csv APPLOG.txt [APPLOG2.txt ...] OUT.html [--txt JOB.txt] [--ranges A-B,C-D]
JOB.csv  = sglog job file (0.5 s rows: layer, phase, g, g_min, g_max, tank_c ...), JOB.txt next to it holds start_mono_s.
APPLOG   = lines of the app log (log1 / log2, any order) filtered with
           grep -a 'layer [0-9]*:\\|sync_mode\\|level_mode\\|lifting = \\|printer start exposure\\|execute:'
The app tick ([..][..][..][TICK]) is CLOCK_MONOTONIC ms = start_mono_s + t_s of the CSV.
Per layer: peel peak (min g_min between 'lifting = 1' and 'lifting = 2' + 0.5 s), push peak (max g_max in that window),
release / return angle and speeds, release duration, exposure, vat temperature. Python 3 stdlib only.
Tilt angle: the release goes 0 -> 140 deg at lift_speed1 up to lift_angle, then at lift_speed2 (constant speeds; the rest of the measured
release time 'lifting = 1' -> 'lifting = 2' is split between a start and an end ramp). A CSV row (0.5 s) is mapped to the angle interval
it covers: the angle at the peak is known to the row width (+- 0.25 s x speed), NOT better (the raw readings are not time stamped).
--ranges: two layer ranges for the force-vs-angle comparison (default: lowest- and highest-force 20 layer windows).
Peel peaks are under-sampled (5 Hz, a fast pulse is ~0.27 s wide: the log holds ~82 % of the true peak).
"""
import csv, json, re, sys, os

args = sys.argv[1:]
txt = None
if "--txt" in args:
    i = args.index("--txt"); txt = args[i + 1]; del args[i:i + 2]
rng = None
if "--ranges" in args:
    i = args.index("--ranges"); rng = args[i + 1]; del args[i:i + 2]
job, out, logs = args[0], args[-1], args[1:-1]
txt = txt or os.path.splitext(job)[0] + ".txt"
mono0 = None
for l in open(txt, errors="replace"):
    if l.startswith("start_mono_s="):
        mono0 = float(l.split("=")[1])
if mono0 is None:
    sys.exit("start_mono_s not found in " + txt)

num = r"(-?[0-9.]+)"
L = {}                                     # layer -> dict from the app log
cur = None
lines = []
for f in logs:
    lines += open(f, errors="replace").read().splitlines()
ev = []
for l in lines:
    m = re.match(r"\[[^\]]*\]\[[^\]]*\]\[[^\]]*\]\[(\d+)\]:(.*)", l)
    if m:
        ev.append((int(m.group(1)), m.group(2)))
ev.sort(key=lambda e: e[0])
for tick, s in ev:
    m = re.match(r"layer (\d+):", s)
    if m:
        cur = L.setdefault(int(m.group(1)), {"tick": tick}); continue
    if cur is None:
        continue
    m = re.search(r"exposure_time " + num + r".*rest_time_before_lift " + num, s)
    if s.startswith("execute:") and m:
        cur["exp_s"] = float(m.group(1)) / 1000; cur["rest_before_s"] = float(m.group(2)) / 1000
    if s.startswith("lifting = 1"):
        cur["lift0"] = tick
    elif s.startswith("lifting = 2"):
        cur["lift1"] = tick
    m = re.match(r"sync_mode \d lift_angle " + num + r" lift_speed1 " + num + r" lift_speed2 " + num, s)
    if m:
        cur["rel"] = [float(x) for x in m.groups()]
    m = re.match(r"level_mode \d drop_angle " + num + r" zero_position " + num + r" drop_speed1 " + num + r" drop_speed2 " + num, s)
    if m:
        cur["ret"] = [float(m.group(1)), float(m.group(3)), float(m.group(4))]

rows = []
for r in csv.DictReader(open(job)):
    if r["layer"] == "" or r["g_min"] == "":
        continue
    rows.append((float(r["t_s"]) + mono0, int(r["layer"]), r["phase"], float(r["g"]), float(r["g_min"]), float(r["g_max"]),
                 float(r["tank_c"]) if r["tank_c"] else None))

res = []
for n in sorted(L):
    d = L[n]
    if "lift0" not in d or "lift1" not in d:
        continue
    t0, t1 = d["lift0"] / 1000, d["lift1"] / 1000
    win = [x for x in rows if t0 - 0.3 <= x[0] <= t1 + 0.5 and x[1] == n]
    if not win:
        continue
    pk = min(win, key=lambda x: x[4])
    rel = d.get("rel", [None] * 3)
    def ang(t):                                   # tilt angle (deg) t seconds after the release start
        if rel[0] is None:
            return None
        F, v1, v2 = rel; v1 = v1 or 1; v2 = v2 or v1
        if F <= 0:
            v2 = v1 if v2 == 0 else v2
            mt = 140 / v2; ex = max(0.0, (t1 - t0) - mt); t = max(0.0, t - ex / 2)
            return min(140.0, v2 * t)
        mt = F / v1 + (140 - F) / v2; ex = max(0.0, (t1 - t0) - mt); t = max(0.0, t - ex / 2)
        return min(140.0, v1 * t) if t <= F / v1 else min(140.0, F + v2 * (t - F / v1))
    tr = [[round(x[0] - t0, 2), x[3], x[4], x[5], round(ang(x[0] - t0 - 0.5) or 0, 1), round(ang(x[0] - t0) or 0, 1)]
          for x in rows if x[1] == n and t0 - 1.5 <= x[0] <= t1 + 3.5]
    tank = [x[6] for x in win if x[6] is not None]
    ret = d.get("ret", [None] * 3)
    pkrow = [x for x in tr if x[2] == pk[4]]
    pk_ang = round((pkrow[0][4] + pkrow[0][5]) / 2, 1) if pkrow else None
    res.append({"layer": n, "peel": pk[4], "peel_t": round(pk[0] - t0, 2), "push": max(x[5] for x in win),
                "rel_angle": rel[0], "rel_v1": rel[1], "rel_v2": rel[2], "ret_angle": ret[0], "ret_v1": ret[1], "ret_v2": ret[2],
                "rel_s": round(t1 - t0, 2), "peel_ang": pk_ang, "exp_s": d.get("exp_s"), "tank": round(sum(tank) / len(tank), 1) if tank else None,
                "trace": tr})

cols = ["layer", "peel", "peel_t", "peel_ang", "push", "rel_angle", "rel_v1", "rel_v2", "ret_angle", "ret_v1", "ret_v2", "rel_s", "exp_s", "tank"]
with open(os.path.splitext(out)[0] + ".csv", "w", newline="") as f:
    w = csv.writer(f); w.writerow(cols)
    for r in res:
        w.writerow([r[c] for c in cols])

def window(lo=True):
    best = None
    for i in range(0, len(res) - 19):
        if res[i]["layer"] < 3:
            continue
        m = sum(r["peel"] for r in res[i:i + 20]) / 20
        if best is None or (m > best[0] if lo else m < best[0]):
            best = (m, res[i]["layer"], res[i + 19]["layer"])
    return best
if rng:
    RG = [tuple(int(v) for v in x.split("-")) for x in rng.split(",")]
else:
    a_, b_ = window(True), window(False)
    RG = [(a_[1], a_[2]), (b_[1], b_[2])] if a_ and b_ else []
def stats(lo, hi):
    sel = [r for r in res if lo <= r["layer"] <= hi and r["peel_ang"] is not None]
    if not sel:
        return None
    angs = sorted(r["peel_ang"] for r in sel); pk = [r["peel"] for r in sel]
    mp = sum(pk) / len(pk); ma = sum(r["peel_ang"] for r in sel) / len(sel)
    cov = sum((r["peel_ang"] - ma) * (r["peel"] - mp) for r in sel)
    va = sum((r["peel_ang"] - ma) ** 2 for r in sel); vp = sum((x - mp) ** 2 for x in pk)
    corr = cov / (va * vp) ** 0.5 if va > 0 and vp > 0 else float("nan")
    return {"range": [lo, hi], "n": len(sel), "peel_mean": round(mp), "peel_worst": round(min(pk)),
            "ang_median": angs[len(angs) // 2], "ang_min": angs[0], "ang_max": angs[-1], "corr_angle_peel": round(corr, 2)}
ST = [stats(*r) for r in RG]
for r, st in zip(RG, ST):
    print("range", r, st)

HTML = r"""<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Peel force per layer</title>
<style>
:root{--bg:#000;--card:#1a1a1a;--card2:#232323;--line:#2c2c2c;--ink:#fff;--ink2:#b4b4b4;--muted:#8f8f8f;--green:#79d853;--warn:#f0b429;--bad:#ff5c5c;--blue:#799fff}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 Roboto,"Segoe UI",system-ui,sans-serif;padding:18px max(16px,env(safe-area-inset-left))}
main{max-width:1100px;margin:0 auto;display:flex;flex-direction:column;gap:14px}h1{font-size:22px;margin:0}h2{font-size:12px;text-transform:uppercase;letter-spacing:.6px;color:var(--muted);margin:0 0 8px;font-weight:500}
section{background:var(--card);border:1px solid #222;border-radius:12px;padding:14px;min-width:0}.sub{color:var(--muted);font-size:13px}
canvas{width:100%;height:320px;display:block}.row{display:flex;gap:12px;flex-wrap:wrap;align-items:center}
input,select,button{font:inherit;background:var(--card2);color:var(--ink);border:1px solid var(--line);border-radius:8px;padding:6px 10px}button{cursor:pointer}
table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums;font-size:13px}th,td{padding:4px 8px;text-align:right;border-bottom:1px solid var(--line);white-space:nowrap}th{color:var(--muted);font-weight:500;position:sticky;top:0;background:var(--card)}
.scroll{max-height:380px;overflow:auto}tr:hover td{background:#222}.lg{display:flex;gap:12px;flex-wrap:wrap;font-size:12px;color:var(--ink2)}.lg i{display:inline-block;width:10px;height:10px;border-radius:2px;margin-right:4px}
</style></head><body><main>
<h1>Peel force per layer <span class="sub" id="meta"></span></h1>
<section><h2>Peel peak per layer (most negative force during the release)</h2>
<div class="row"><label>Colour by <select id="by"><option value="rel_v1">release speed, stage 1</option><option value="rel_angle">release stage angle</option><option value="tank">vat temperature</option></select></label>
<label>Smoothing <select id="sm"><option value="1">none</option><option value="3">max of last 3</option><option value="5">max of last 5</option></select></label><span class="lg" id="lg"></span></div>
<canvas id="c1" height="320"></canvas><div class="sub" id="hover">Hover a bar for the layer's numbers; click to open its trace below. Peaks are the logged minimum (5 Hz sampling: the real peak is higher).</div></section>
<section><h2>Force trace of chosen layers, time from the start of the release</h2>
<div class="row"><label>Layers <input id="ls" size="28" value=""></label><button id="go">Show</button><span class="sub">comma separated, e.g. 30,118,119,120</span></div>
<canvas id="c2" height="320"></canvas><div class="lg" id="lg2"></div></section>
<section><h2>Peel force against estimated tilt angle: two layer ranges</h2>
<div class="row"><label>Range A <input id="ra" size="9"></label><label>Range B <input id="rb" size="9"></label><label><input type="checkbox" id="zoomA" style="width:auto"> scale to range A</label><button id="rgo">Show</button><span class="sub">each 0.5 s row = a vertical bar (lowest to highest force) at the middle of the angle interval it covers; line = median of the lowest force per 10 deg bin. Angle resolution is the row width: +-0.25 s x speed (about +-12 deg at 50 deg/s).</span></div>
<canvas id="c3" height="340"></canvas><div class="lg" id="lg3"></div><div class="sub" id="st3"></div></section>
<section><h2>Per layer</h2><div class="scroll"><table id="tb"></table></div></section>
</main><script>
const D = __DATA__, META = __META__, RG = __RG__;
document.getElementById("meta").textContent = META;
const $ = id => document.getElementById(id);
const COLS = ["layer","peel","peel_t","peel_ang","push","rel_angle","rel_v1","rel_v2","ret_angle","ret_v1","ret_v2","rel_s","exp_s","tank"];
const HEAD = {layer:"layer",peel:"peel peak g",peel_t:"peak at s",peel_ang:"peak at deg",push:"push g",rel_angle:"rel angle",rel_v1:"rel v1",rel_v2:"rel v2",ret_angle:"ret angle",ret_v1:"ret v1",ret_v2:"ret v2",rel_s:"release s",exp_s:"exposure s",tank:"vat °C"};
$("tb").innerHTML = "<tr>" + COLS.map(c => `<th>${HEAD[c]}</th>`).join("") + "</tr>" + D.map(r => "<tr>" + COLS.map(c => `<td>${r[c] == null ? "" : r[c]}</td>`).join("") + "</tr>").join("");
const PAL = ["#799fff","#79d853","#f0b429","#ff8a5c","#c79bff","#5ce1e6","#ff5c9a","#b4b4b4"];
function colorFor(by, vals, v){ const u = [...new Set(vals)].sort((a,b)=>a-b); const i = u.indexOf(v); return v == null ? "#555" : by === "tank" ? `hsl(${220 - (v - Math.min(...vals)) / (Math.max(...vals) - Math.min(...vals) + .01) * 200},70%,60%)` : PAL[i % PAL.length]; }
let hit = [];
function draw1(){
  const c = $("c1"), W = c.clientWidth, H = 320; c.width = W * devicePixelRatio; c.height = H * devicePixelRatio; const x = c.getContext("2d"); x.scale(devicePixelRatio, devicePixelRatio);
  const by = $("by").value, sm = +$("sm").value, vals = D.map(r => r[by]).filter(v => v != null);
  const pk = D.map((r, i) => -Math.min(...D.slice(Math.max(0, i - sm + 1), i + 1).map(q => q.peel)));
  const mx = Math.max(500, ...pk) * 1.08, L = 52, B = H - 24, T = 10, bw = (W - L - 8) / D.length;
  x.fillStyle = "#8f8f8f"; x.font = "11px sans-serif"; x.strokeStyle = "#2c2c2c";
  for (let g = 0; g <= mx; g += mx > 2000 ? 500 : 250){ const y = B - g / mx * (B - T); x.beginPath(); x.moveTo(L, y); x.lineTo(W - 8, y); x.stroke(); x.fillText(-g, 4, y + 4); }
  hit = [];
  D.forEach((r, i) => { const h = pk[i] / mx * (B - T), xx = L + i * bw; x.fillStyle = colorFor(by, vals, r[by]); x.fillRect(xx + .5, B - h, Math.max(1, bw - 1), h); hit.push([xx, xx + bw, r]); });
  x.fillStyle = "#8f8f8f"; const st = Math.ceil(D.length / 12); D.forEach((r, i) => { if (i % st === 0) x.fillText(r.layer, L + i * bw, H - 8); });
  const u = [...new Set(vals)].sort((a, b) => a - b);
  $("lg").innerHTML = by === "tank" ? "" : u.map(v => `<span><i style="background:${colorFor(by, vals, v)}"></i>${v}</span>`).join("");
}
$("c1").addEventListener("mousemove", e => { const r = e.target.getBoundingClientRect(), px = e.clientX - r.left; const h = hit.find(h => px >= h[0] && px < h[1]); if (h){ const d = h[2]; $("hover").textContent = `Layer ${d.layer}: peel ${d.peel} g at ${d.peel_t} s · push ${d.push} g · release ${d.rel_angle}° at ${d.rel_v1}/${d.rel_v2} deg/s, ${d.rel_s} s · return ${d.ret_angle}° at ${d.ret_v1}/${d.ret_v2} · exposure ${d.exp_s} s · vat ${d.tank} °C`; } });
$("c1").addEventListener("click", e => { const r = e.target.getBoundingClientRect(), px = e.clientX - r.left; const h = hit.find(h => px >= h[0] && px < h[1]); if (h){ const v = $("ls").value.split(",").map(s => s.trim()).filter(Boolean); if (!v.includes(String(h[2].layer))) v.push(h[2].layer); $("ls").value = v.slice(-6).join(","); draw2(); } });
function draw2(){
  const c = $("c2"), W = c.clientWidth, H = 320; c.width = W * devicePixelRatio; c.height = H * devicePixelRatio; const x = c.getContext("2d"); x.scale(devicePixelRatio, devicePixelRatio);
  const ls = $("ls").value.split(",").map(s => +s.trim()).filter(n => !isNaN(n) && D.find(r => r.layer === n)); const sel = ls.map(n => D.find(r => r.layer === n));
  const all = sel.flatMap(r => r.trace); if (!all.length){ x.fillStyle = "#8f8f8f"; x.fillText("No layer chosen", 20, 30); return; }
  const t0 = Math.min(...all.map(p => p[0])), t1 = Math.max(...all.map(p => p[0])), g0 = Math.min(...all.map(p => p[2]), 0), g1 = Math.max(...all.map(p => p[3]), 1);
  const L = 56, B = H - 24, T = 10, X = t => L + (t - t0) / (t1 - t0 || 1) * (W - L - 12), Y = g => B - (g - g0) / (g1 - g0 || 1) * (B - T);
  x.strokeStyle = "#2c2c2c"; x.fillStyle = "#8f8f8f"; x.font = "11px sans-serif";
  const gs = Math.max(100, Math.round((g1 - g0) / 6 / 100) * 100); for (let g = Math.ceil(g0 / gs) * gs; g <= g1; g += gs){ x.beginPath(); x.moveTo(L, Y(g)); x.lineTo(W - 12, Y(g)); x.stroke(); x.fillText(g, 4, Y(g) + 4); }
  for (let t = Math.ceil(t0); t <= t1; t++){ x.fillText(t + " s", X(t) - 8, H - 8); }
  x.strokeStyle = "#ffffff55"; x.beginPath(); x.moveTo(X(0), T); x.lineTo(X(0), B); x.stroke();
  $("lg2").innerHTML = ""; sel.forEach((r, i) => { const col = PAL[i % PAL.length]; x.strokeStyle = col; x.lineWidth = 2; x.beginPath(); r.trace.forEach((p, k) => k ? x.lineTo(X(p[0]), Y(p[1])) : x.moveTo(X(p[0]), Y(p[1]))); x.stroke(); x.lineWidth = 1;
    x.fillStyle = col + "55"; r.trace.forEach(p => x.fillRect(X(p[0]) - 2, Y(p[3]), 4, Math.max(1, Y(p[2]) - Y(p[3]))));
    $("lg2").innerHTML += `<span><i style="background:${col}"></i>layer ${r.layer}: peel ${r.peel} g, ${r.rel_angle}° at ${r.rel_v1}/${r.rel_v2} deg/s</span>`; });
}
function draw3(){
  const c = $("c3"), W = c.clientWidth, H = 340; c.width = W * devicePixelRatio; c.height = H * devicePixelRatio; const x = c.getContext("2d"); x.scale(devicePixelRatio, devicePixelRatio);
  const rs = [$("ra").value, $("rb").value].map(v => v.split("-").map(Number)).filter(r => r.length === 2 && !r.some(isNaN));
  const cols = ["#799fff", "#f0b429"], L = 56, B = H - 26, T = 10; let g0 = 0, any = [];
  const sel = rs.map(r => D.filter(d => d.layer >= r[0] && d.layer <= r[1])); sel.forEach(s => s.forEach(d => d.trace.forEach(p => { g0 = Math.min(g0, p[2]); any.push(p); })));
  if ($("zoomA").checked && sel[0]){ g0 = 0; sel[0].forEach(d => d.trace.forEach(p => { g0 = Math.min(g0, p[2]); })); any = sel[0].flatMap(d => d.trace); }
  const g1 = Math.max(100, ...any.map(p => p[3])), X = a => L + a / 140 * (W - L - 12), Y = g => B - (g - g0) / (g1 - g0 || 1) * (B - T);
  x.strokeStyle = "#2c2c2c"; x.fillStyle = "#8f8f8f"; x.font = "11px sans-serif";
  const gs = Math.max(100, Math.round((g1 - g0) / 6 / 100) * 100); for (let g = Math.ceil(g0 / gs) * gs; g <= g1; g += gs){ x.beginPath(); x.moveTo(L, Y(g)); x.lineTo(W - 12, Y(g)); x.stroke(); x.fillText(g, 4, Y(g) + 4); }
  for (let a = 0; a <= 140; a += 20){ x.fillText(a + "°", X(a) - 8, H - 8); }
  sel.forEach((s, k) => { x.fillStyle = cols[k] + "66"; x.strokeStyle = cols[k]; const bins = {};
    s.forEach(d => d.trace.forEach(p => { if (p[5] <= 0 && p[4] <= 0) return; const am = (p[4] + p[5]) / 2; x.fillRect(X(am) - 1.5, Y(p[3]), 3, Math.max(2, Y(p[2]) - Y(p[3]))); (bins[Math.floor(am / 10)] = bins[Math.floor(am / 10)] || []).push(p[2]); }));
    x.lineWidth = 2.5; x.beginPath(); Object.keys(bins).map(Number).sort((a, b) => a - b).forEach((b, i) => { const v = bins[b].sort((p, q) => p - q), m = v[v.length >> 1]; i ? x.lineTo(X(b * 10 + 5), Y(m)) : x.moveTo(X(b * 10 + 5), Y(m)); }); x.stroke(); x.lineWidth = 1; });
  $("lg3").innerHTML = rs.map((r, k) => `<span><i style="background:${cols[k]}"></i>layers ${r[0]}–${r[1]} (${sel[k].length})</span>`).join("");
  $("st3").innerHTML = rs.map((r, k) => { const s = sel[k].filter(d => d.peel_ang != null); if (!s.length) return ""; const a = s.map(d => d.peel_ang).sort((p, q) => p - q); const pk = s.map(d => d.peel);
    return `Layers ${r[0]}–${r[1]}: mean peak ${Math.round(pk.reduce((p, q) => p + q, 0) / pk.length)} g, worst ${Math.round(Math.min(...pk))} g, peak at about ${a[a.length >> 1]}° (median; range ${a[0]}–${a[a.length - 1]}°)`; }).join("<br>");
}
$("ra").value = RG[0] ? RG[0].join("-") : ""; $("rb").value = RG[1] ? RG[1].join("-") : ""; $("rgo").onclick = draw3; $("zoomA").onchange = draw3;
$("by").onchange = $("sm").onchange = draw1; $("go").onclick = draw2; addEventListener("resize", () => { draw1(); draw2(); draw3(); });
const worst = [...D].sort((a, b) => a.peel - b.peel).slice(0, 3).map(r => r.layer); $("ls").value = worst.join(",");
draw1(); draw2(); draw3();
</script></body></html>"""
meta = f"· {os.path.basename(job)} · layers {res[0]['layer']}–{res[-1]['layer']}" if res else ""
open(out, "w").write(HTML.replace("__DATA__", json.dumps(res, separators=(",", ":"))).replace("__META__", json.dumps(meta)).replace("__RG__", json.dumps(RG)))
print(f"{len(res)} layers -> {out} (+ .csv)")
