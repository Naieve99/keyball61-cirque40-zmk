# Keyball44 Rev1 first-article plan

This adds a Keyball44 Rev1 counterpart while leaving the existing Keyball61
firmware and build artifacts intact. It is a development package, not a claim
of hardware validation.

## What is implemented

- 44 populated key positions: three 12-key rows plus five left thumb keys and
  three right thumb keys.
- Official Rev1 4-row by 6-column matrix per half, with the right matrix offset
  into columns 6-11.
- nice!nano v2 split: left BLE peripheral, right BLE central.
- Separate right-half targets for the M7U Cirque adapter and stock PMW3360.
- ZMK Studio target for the Cirque build.
- Battery-level fetching/proxy support for the central half.
- Seven keymap layers, including three persistent pointer-mode layers and an
  independent full-pad scroll toggle.

The matrix is based on the official Keyball44 QMK files and cross-checked
against the Rev1 schematic. Its row/column pins are:

| Function | Pro Micro pin numbers | nice!nano GPIOs |
| --- | --- | --- |
| Rows | 21, 20, 19, 18 | P0.31, P0.29, P0.02, P1.15 |
| Columns | 4, 5, 6, 7, 8, 9 | P0.22, P0.24, P1.00, P0.11, P1.04, P1.06 |
| Diodes | COL2ROW | Active-high row reads with pull-downs |

## Build artifacts

| Artifact | Role | Status |
| --- | --- | --- |
| `keyball44_left` | Left BLE peripheral | First article |
| `keyball44_left_standalone` | Independent left keyboard | Host-facing standalone build |
| `keyball44_right_cirque` | Right central, Cirque I2C | First article |
| `keyball44_right_cirque_studio` | Right central, Cirque + Studio | Preferred Cirque development build |
| `keyball44_right_cirque_standalone` | Independent right keyboard, Cirque I2C | Host-facing standalone build |
| `keyball44_right_pmw3360_experimental` | Right central, stock PMW3360 SPI | Experimental; hardware untested |
| `keyball44_right_pmw3360_standalone_experimental` | Independent right keyboard, stock PMW3360 SPI | Experimental; hardware untested |

The PMW3360 build pins George Norton's external ZMK driver at commit
`0dfa2f0411b795613a35f222f993e45a1e4cc32a`. The stock connector does not
expose the sensor's MOTION signal, so the initial target polls at 250 Hz. The
driver itself warns that PMW3360 power consumption is relatively high for a
wireless build.

## Pointer modes

Hold the left `NAV` thumb key, then use these keys on the bottom alpha row:

| Shortcut | Mode | Current first-article behavior |
| --- | --- | --- |
| NAV + Z | Original Keyball | Conservative 1:1 processing; PMW axes follow the official Keyball swap |
| NAV + X | Expanded Keyball | 2:1 cursor scaling |
| NAV + C | Full Cirque/native | Cirque relative reporting and primary tap at 1:1 |
| NAV + V | Scroll toggle | Full surface maps X/Y motion to horizontal/vertical wheel events |

These stable mode selectors reserve the requested interface. Expanded
acceleration and gestures, and advanced native Cirque gestures, are **not yet
implemented** by this first article. They require measured hardware tuning and,
for native gestures, potentially a driver extension. On the PMW build the
`NATIVE` selector is retained for a consistent keymap but cannot add Cirque-only
features.

Tap the second left thumb key for Left Alt. Hold it to enter the `MOUSE` layer,
where H, J and K are left, middle and right click. This is required for the
PMW3360 target and is also available alongside Cirque tap-click.

The right half has matching standalone controls: tap its Enter thumb normally
or hold it for the `MOUSE` layer; tap its Delete thumb normally or hold it for
`NAV`. With right `NAV` held, N/M/Comma/Dot select Original/Expanded/Native or
toggle Scroll. The top right row provides Bluetooth profiles 0-4 and clear.

## Independent-half operation

For two completely independent Bluetooth keyboards, flash:

- `keyball44_left_standalone` to the left controller; and
- either `keyball44_right_cirque_standalone` or
  `keyball44_right_pmw3360_standalone_experimental` to the right controller.

Pair `KB44 Left` and `KB44 Right` separately. The operating system will see two
keyboard devices. Each half scans only its own switches, keeps its own Bluetooth
profiles, layer state and battery state, and continues working if the other is
powered off.

Layers do not cross between independent devices. Hold the left-side `NAV` key
for left settings and the right Delete/`NAV` key for right pointer settings.
Most desktop operating systems combine ordinary modifiers from multiple HID
keyboards, but that behavior must be verified on the intended host.

To return to normal single-keyboard split behavior, flash `keyball44_left` on
the left and the corresponding non-standalone right build. Changing between
standalone and split firmware may require `settings_reset` and re-pairing.

## First power-up sequence

1. Do the unpowered continuity and isolation checks in
   [m7u-universal-detection.md](m7u-universal-detection.md).
2. Do not connect a Cirque and flash the PMW build, or connect a PMW and flash
   the Cirque build, until the M7U routing has been verified.
3. Flash `settings_reset` to both controllers if they contain old split/BLE
   bonds.
4. Flash either the split pair or the standalone pair described above; do not
   mix modes unless the right half is intentionally being used alone.
5. Pair one split central or both standalone halves, as appropriate.
6. Verify all 44 switches before testing the pointer.
7. Verify right/up motion, tap-click (Cirque), and physical trackball axes.
8. Exercise Original, Expanded, Native, and Scroll independently.
9. Measure right-half idle and active current before enabling sleep changes,
   OLED, or RGB.

## Staged follow-up

| Stage | Work | Entry condition |
| --- | --- | --- |
| 1 | Matrix, split BLE, one selected pointer | Compile plus continuity checks |
| 2 | Universal runtime detection | Hardware ID/isolation design proven; see the M7U note |
| 3 | Pointer tuning | Measured directions, CPI, scaling, scroll and tap behavior |
| 4 | Low power + battery UI | Stable wake/sleep and measured current |
| 5 | OLED | Display choice, bus/pins, and enclosure clearance confirmed |
| 6 | RGB | 3.3 V signal behavior, current budget, and LED chain verified |
| Optional | Gen6 sensor work | Only if mechanical/electrical fit is confirmed; must not block stages 1-3 |

OLED, RGB and deep sleep remain disabled in `config/keyball44.conf` during
Stage 1. Battery reporting is enabled, but its displayed accuracy still depends
on the actual battery and controller calibration.

## Validation boundary

Successful compilation proves that the devicetree, Kconfig and keymap are
accepted by the pinned software stack. It does not prove connector numbering,
adapter routing, signal direction, pointer orientation, RF behavior, power
consumption, or safe hot-plugging. Those remain physical first-article tests.

## Source references

- [Official Yowkees Keyball repository](https://github.com/Yowkees/keyball)
- [Official Keyball build guide and schematics](https://github.com/Yowkees/keyball/blob/main/docs/rev1/buildguide_en.md)
- [Pinned ZMK PMW3360 driver](https://github.com/george-norton/zmk-driver-pmw3360/tree/0dfa2f0411b795613a35f222f993e45a1e4cc32a)
