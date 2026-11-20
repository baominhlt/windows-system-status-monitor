# Changelog

## 1.0.0

First release.

- Two-row status strip on the Windows taskbar: CPU, RAM, GPU 0/1, disk active time and read/write speed,
  partition usage, network down/up, GPU temperature and system (ACPI) temperature.
- Values change colour at 60 % and 85 % (temperatures: GPU 70/85 °C, system 50/65 °C).
- Icons with automatic fallback to text labels; optional translucent rounded background with column dividers.
- Hover tooltip with full details; right-click menu to show/hide every metric, change the sample interval,
  switch the network unit (bytes/bits), toggle NVIDIA polling and the background, and enable *Start with Windows*.
- Placement on the left or right of the taskbar (`align`), measured around the Start/Search/app buttons;
  columns are dropped automatically from the right when space runs out.
- Tray-icon fallback (`--tray`, vertical taskbars, or when the taskbar cannot be found).
- No administrator rights, no installer, single self-contained executable (static C runtime).
- Release automation: pushing a `v*` tag builds the executable and publishes a GitHub Release
  with the zip and its SHA-256 checksum.
