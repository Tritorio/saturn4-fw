# Changelog

Every release is a git tag (`v1.0`, `v1.1`, ...) and a GitHub Release with the same notes. The newest entry is first.
When a new revision is published, add its entry here (tools/github/templates/CHANGELOG.md) before re-running the export.

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
