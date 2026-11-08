# Changelog

Every release is a git tag (`v1.0`, `v1.1`, ...) and a GitHub Release with the same notes. The newest entry is first.
When a new revision is published, add its entry here (tools/github/templates/CHANGELOG.md) before re-running the export.

## V1.1.4 (mod 8.48, build 54) - 2026-11-08
Source revision matching mod 8.48. The firmware image is not published; build it yourself (see `docs/BUILDING.md`). The library changed (loads at the next power cycle); the dashboard files are live files.
- **Wait Times Testing:**
  - The log starts at the first level, with new columns `t_wait_hms` (time from the first wait) and `heat_on`; the gradient columns are renamed `diff_g` / `diff_end_g` (g between the last two window averages).
  - The foreign matter guard is active only on the first layer; if it passes, the other layers ignore it.
  - Any layer height from 0.01 mm up (no upper limit), presets 0.5 / 1 / 2 mm.
  - Late "axis is idle" lines from the app log (it is block-buffered, up to 20 s late) no longer abort a run: moves finish by time and late lines are counted (`note-idle-late` rows).
  - Short settle windows need fewer readings.
- **Heating:** the pre-heat limit is 50 C; the heater thermostat (off at target - 0.5 C, on at target - 1.0 C) now also holds pre-heat (heater mode 3), which avoided overshoot and pre-heat cycling.
- **Job log CSV:** the columns raw, value, value_min, value_max, n and raw min / max are removed; new columns `t_wait_hms` and `heat_on`.
- **Dynamic wait:** the card shows the live measured g difference (last window against the one before); the Dynamic wait card and the Status box turn yellow with "Not enabled for Bottom Layers" when the function is on but "Also on bottom layers" is off.

## V1.1.3 (mod 8.47, build 53) - 2026-11-07
Source revision matching mod 8.47. The firmware image is not published; build it yourself (see `docs/BUILDING.md`). The library did not change since V1.1.2 builds of mod 8.46, so a printer that already runs it only needs the dashboard files; the new boot scripts run at the next power cycle.
- **Machine settings safety** (after a reset in which the app rewrote its machine settings file with factory defaults):
  - The boot guard now also restores a missing or empty `machine_param_ref.bin` and `machine_info.bin`, not only an invalid one.
  - Every changed, valid settings file is kept as a rolling backup (last 12 in `sglog/machine_backups/boot/`, last 3 plus `machine_info.bin` on the boot partition, because the files partition is not mounted yet when the guard runs).
  - A watcher (`machwatch.sh`, started by `hook.sh`) reads the app log for 150 s after start. If the app reports "can't find machine parameters", it puts the newest backup back, keeps the bad file as `suspect_N.bin`, and writes `sglog/MACHRESET`. A power cycle then loads the restored file. Afterwards it saves a changed file every minute.
  - Persistent log: `sglog/machguard.log` (`mg/boot.log` on the boot partition before the files partition exists). Kill switch: `/customer/mod/machguard_disable`.
- **Dynamic wait fix:** the dashboard server rejected the five-value `waitmin2` command that the card sends, so the second set (Force Variation 2 / Timespan 2), the Layer Switch and Min Wait after never reached the printer. Fixed.
- **Dynamic wait card:** Apply stays faded after pressing it until the printer reports the new values (it used to light up again for about a second).
- **Dynamic wait, two independent sets:** Force Variation and Timespan 1 and 2 with a Layer Switch between them, and Min Wait before / Transition Layer / Min Wait after with its own layer number; ceiling 15 minutes for "ignore the slicer wait". Wait Times Testing uses the average of two windows as its settle rule and tilts at 36 deg/s.
- **Charts:** the Manual Exposure chart and the Dynamic wait chart show a cursor with a per-layer tooltip when you hover or drag over them, like the Force card.
- **Manual Exposure** can be activated without a running job (the times apply from the first layer of the next job).
- **Pre-heat warning:** above 30 C, also use the Resin Mixing function.
- **Job log:** phase `wait` rows and the first-layer Z step are logged.

## V1.1.2 (source update after V1.1.1) - 2026-11-06
Source-only revision: no new firmware image yet. The library changes load at the next power cycle of a printer that has them installed.
- **Wait Times Testing (new, Developer card):** a dry-run print simulation (no UV) to measure how long the resin takes to settle after each retract, with no maximum wait. Input: layer height and number of layers. It runs its own sequence from the levelling (calibrate in air, resin probe, auto-levelling with the machine's own settings), then the first wait right after levelling, then per layer: tilt release (the Tilt speed card values), plate rise by one layer height while the tilt returns, wait until settled. Wait & heat follows its dashboard setting. Every run writes its own log (`sglog/waittest/waittest_<height>mm_<date>_<time>.csv`: force, vat temperature, gradient, plate height above the levelled contact, settle time per layer) that can be downloaded and deleted from the card. First results: `docs/wait_test_results_20261106.txt`.
- **Dynamic wait rule changed:** the wait now ends when the force changes by less than N **grams over the window** (the slope times the window), not a g/s slope. The window ("Over") can be up to 600 s (was 3 s); the change can be set from 0.05 g (typed, 0.01 resolution). The gauge history buffer grew to about 13 minutes. A 3 s window is too noisy (about +-0.3 g) for settling times: a longer window and a limit of about twice the window's noise are more reliable (table in the results file).
- **Heated waits have a thermostat:** Wait & heat holds the "Heating during print" target (heater off at the target, on again 0.5 C below). Before, the heater mode used in waits had no temperature control, so long waits heated the vat past the target.
- **Resin mixing:** the first descent to the low position is slow (2.5 mm/s) with the foreign matter guard active, then the guard limit is raised for the strokes; lift height 30 mm; fixed the +/- buttons of the minutes (they set 0).
- **Foreign matter guard trip detection fixed:** only the app's real trip lines count (before, its per-check lines were mistaken for a trip).
- **Force card:** per-layer maximum and minimum (stats and chart marks); the history survives a page reload (kept in the browser); the Status card shows a yellow dot next to the resin tank temperature while the heater is on.
- **Dashboard:** "Resin profile" card opens with the green highlight like the others; the Pre-heat warning mentions Resin Mixing; "time-lapse" wording; Dynamic wait values can be typed with two decimals.
- **Camera relay** runs at low priority (nice 19).
- **Test switch:** a marker file `sglog/WAITNOMAX` removes the 10 minute ceiling of "ignore the slicer wait" (for tests).

## V1.1.1 (source update after mod 8.45 / build 51) - 2026-11-06
Source-only revision: no new firmware image yet (the library change loads at the next power cycle of a printer that has it installed).
- **Resin mixing, cold resin:** the first descent to Z low now runs slowly (2.5 mm/s, like the resin probe) with the foreign matter guard active, so a real obstacle is still detected. After a clean arrival the guard limit is raised for the rest of the mixing (restored on stop, end, pre-heat off or a print start) and the strokes run at 5 mm/s. Before, viscous cold resin pushed the gauge over the limit on the downstroke and the guard stopped the mixing after a few strokes.
- **Resin mixing lift height** reduced by 5 mm (Z low + 30 mm).
- **Fix:** the + / - buttons of the mixing minutes in the Pre-heat card set the value to 0.
- **Job history:** "Delete timelapse" button deletes only the time-lapse of a job; the delete pop-up shows the time-lapse sizes.

## V1.1 (mod 8.45, build 51) - 2026-11-05
- **Resin profiles:** named resin profiles for the print settings, tested on a variable-layer print.
- **Resin mixing:** in the Pre-heat part of the Resin heating card the plate moves in the resin for the set minutes, and rises at the end or on stop.
- **Timelapse** (Camera card): one camera picture per layer after the retract, taken from the live view without delaying the print; H.264 file per job.
  - Sync fix: the trigger is sent early and anchored to the Z step / exposure end.
  - Files go to a hidden folder on the files partition, capped at 2 GiB (oldest deleted first), with a warning in the Camera card.
- **Live Z position** in the Status card, measured from the levelling zero.
- **Datalogger:** line 2 of the CSV describes each column (analysis scripts must skip it).
- Brighter dark-theme Tilt speed panels.
- Known: the timelapse sync fix is untested on a full print; the library loads at the next power cycle.

## V1.0 (mod 8.43) - 2026-11-04
First public source release: strain-gauge logger, dynamic wait, dynamic lift, Multi Exposure, Z monitor, Flex Plate, resin heating,
camera live view, dashboard served by the printer, login for writes, low-resin force-print fix, return-to-stock button.
See README.md and docs/user_manual.html for the feature list.
