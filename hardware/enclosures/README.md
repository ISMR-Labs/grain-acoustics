# Enclosures

All parts are OpenSCAD, designed on the lab's Forge service (OpenSCAD → STL → G-code for a
Creality Ender-3 V3 SE, 220 × 220 × 250 mm bed). Each `.scad` builds box and lid in one file;
set `part = "all" | "box" | "lid"` at the top.

| Folder | Part | Outer (mm) | Inner clear (mm) | Posts | Notes |
|---|---|---|---|---|---|
| `esp32-controller-box/` | Controller box + lid | 75.2 × 81.6 × 41.7 as laid out; box inner 20 × 70 × 40 | 20 × 70 × 40 | M3 | USB-C cutout on a short end, 4 × Ø4 mm cable holes on a long wall, 1.35 mm walls (3 × 0.45 lines) |
| `mic-box/` | Mic pod + lid | 30 × 30 × 8.7 | 27.3 × 27.3 × 7 | M2 | 4-hole sound grille in the lid, rim notch for the cable; a round module ≤ 25 mm clears the posts |
| `piezo-box/` | Piezo pad + lid | 57 × 57 × 5.5 | 54.3 × 54.3 × 3 | M2.5 | takes discs up to ~54 mm; rim notch for the lead |

Each folder has the `.scad` source, the rendered `.stl`, and `-iso/-top/-front.png` previews.
`gcode/` holds the sliced files actually printed (PLA+, 210 °C nozzle / 60 °C bed, 0.20 mm,
3 walls).

Printing notes

- Walls were set to exactly 3 × 0.45 mm so the slicer fills them with perimeters only and does
  not add gap-fill squiggles around the cable holes.
- Pads print lid-down for a flat contact face. Pods print box-down.
- The piezo pad floor (2 mm) is the contact surface; bond the brass side of the disc to it with a
  thin hard adhesive, not hot glue, for best high-frequency coupling.

Renders

| Controller box | Mic pod | Piezo pad |
|---|---|---|
| ![](esp32-controller-box/esp32-controller-box-iso.png) | ![](mic-box/mic-box-iso.png) | ![](piezo-box/piezo-box-iso.png) |
| ![](esp32-controller-box/esp32-controller-box-top.png) | ![](mic-box/mic-box-top.png) | ![](piezo-box/piezo-box-top.png) |

## Companion build: vibration-motor array (24 Sep 2026)

Two more Forge projects from the same lab, used for the 17-module vibration array shown in
`media/photos/2026-09-24-vibration-array`. They are kept here because the array is a usable
calibration stimulus for the piezo channels and shares the same enclosure style.

| Folder | Part | Outer (mm) | Inner clear (mm) | Posts | Notes |
|---|---|---|---|---|---|
| `vibration-motor-enclosure/` | Module pod + lid (v11) | 32.5 × 40.5 × 10.1 (laid out 117.5 × 40.5 with lid and base plate) | ~21.1 × 29.1 × 8.6 | M2.5 self-tap | Fits a module up to 21 × 29 mm; `gcode/vib_6baseplates.gcode` prints six base plates per bed |
| `controller-box/` | Push-button controller box + lid + base plate | 101.8 × 70.8 × 34.7 (laid out 329.4 × 70.8 with lid and plate) | 52 × 83 × 32 | M3 | No cable slot or LED holes; `gcode/controller_box.gcode`, `controller_lid.gcode`, `controller_lid_baseplate.gcode` |

| Vibration module pod | Controller box |
|---|---|
| ![](vibration-motor-enclosure/vibration-motor-enclosure-iso.png) | ![](controller-box/controller-box-iso.png) |
