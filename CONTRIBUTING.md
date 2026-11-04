# Contributing

Thank you for helping. A few rules keep users' printers safe:

* **No Elegoo files** in pull requests (firmware images, extracted rootfs/kernel, UI images, manuals) and **no
  passwords or personal data** (Wi-Fi settings, printer serial numbers, IP addresses of your network).
* Read the *Hard constraints* in `CLAUDE.md`. A change that touches anything but the `customer` volume, or that
  weakens a signature / kill-switch check, will not be merged.
* Say what you tested and how: host test, scratch page with mocked data, or on a real printer (which firmware,
  which image number, power-cycled or not). Untested printer behaviour must be marked as such.
* New library patch points need a code-word signature, a kill switch and a note in `docs/ARCHITECTURE.md`.
* Findings (log excerpts, measurements, RE notes) belong in `docs/` as plain text.
* Issues for other board / firmware revisions: attach the output of `board-report/` (read-only).

Working with Claude Code or another agent is welcome; it will follow `CLAUDE.md`. Review its changes as you
would any other contribution.
