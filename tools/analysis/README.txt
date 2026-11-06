Analysis scripts from the 2026-10-19/20 sessions (copied from the session scratchpad; paths inside
still point at that scratchpad: adjust S= / file paths before reuse). Python 3 stdlib (+ PIL none).
- waitrec.py LOG N      poll http://192.168.0.44:3030/tmp/sgweb/live.json every 0.5 s, append JSON lines,
                        stop after N finished waits (wait.last). waituntil.py LAYER TIMEOUT: wait for a layer.
- cap12.py OUT.json     decode every gauge reading (ts_us, raw) from a raw capture (sgcap cap_*.bin).
- ana12.py / ets12.py   bind readings to the app log (lift, heater on/off) + equivalent-time peel pulse
                        reconstruction (Piramidi 12x: pulse 0.27 s wide, logged ~82 % of the peak).
- cmp12.py, bind.py     settle time / peel per layer from the CSV, 3x vs 12x, area/perimeter fits.
- replayc.py            replay of the C settle rule (LSQ gradient on raw readings, 20 Hz) on a capture.
- rules.py, peakw.py    older rule comparison and peak-width / scatter checks.
RE helpers (ARM/Thumb xrefs for chitu, text vaddr 0x10000 = file 0): tools/re/ax.py, tools/re/tx.py.
- peel_report.py JOB.csv LOG2.txt [LOG1.txt] OUT.html [--txt JOB.txt]   (2026-10-22, dynamic lift analysis): peel peak per layer joined with
                        the tilt values the app used (app log 'sync_mode .. lift_angle .. lift_speed1/2', 'level_mode ..'), release duration,
                        exposure, vat temperature; HTML report (bar chart coloured by speed / angle / temperature, layer force traces,
                        table) + CSV. Pull the inputs read-only: the CSV + .txt from /media/mmcblk0p2/sglog/, and the app log filtered on
                        the printer: grep -a 'layer [0-9]*:\|sync_mode\|level_mode\|lifting = \|printer start exposure\|execute:' /media/mmcblk0p2/log2
                        (log1 / log2 alternate, 4 MB each). App tick ms = start_mono_s + t_s. Example output: docs/peel_FXtest.html / .csv.
                        2026-10-22: --ranges A-B,C-D (default = lowest / highest force 20 layer windows) adds peak angle per layer and a force-vs-angle
                        plot (angle = constant-speed model from the app log values + measured release time; resolution = the 0.5 s CSV row,
                        +-0.25 s x speed). For better resolution: raw capture (sgcap/ENABLE) + TILTTRACE during the layers of interest.
- settle_rules_cmp.py JOB.csv [JOB2.csv ..]   (2026-11-06) replays settle rules on the logged waits of job CSVs (0.5 s rows; wait = rows of phase 'layer' before the
                        first row with wait_end): slope rules A (|LSQ slope| < GRAD over WIN s, min 2 s) and an exponential-fit "remaining change < X g" rule B,
                        reports fire time (% of the actual wait) and the excess of the reading over the plateau (last 1.5 s of the logged wait). Needs numpy + scipy
                        (system python3). settle_rules_table.py = per-wait table; settle_rules_lib.py = the functions. Result 2026-11-06: B fires far too early
                        (decay is multi-exponential, +3..6 g median excess); A .3/2 reproduces the printer's waits (checked on layers of 'Variable height test').
