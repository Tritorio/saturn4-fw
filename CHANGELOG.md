# Changelog

Every release is a git tag (`v1.0`, `v1.1`, ...) and a GitHub Release with the same notes. The newest entry is first.
When a new revision is published, add its entry here (tools/github/templates/CHANGELOG.md) before re-running the export.

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
