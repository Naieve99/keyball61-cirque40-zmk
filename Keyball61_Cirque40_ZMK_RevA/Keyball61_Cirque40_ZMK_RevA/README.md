# Keyball61 Cirque40 — nice!nano v2 / ZMK first-article firmware

This is the first firmware baseline for the Rev M2 Cirque40 adapter.

## Frozen electrical assumptions

Right half is the ZMK central and owns the Cirque TM040040.

- Cirque mode: I2C, 3.3 V, address `0x2A`
- SCL: nice!nano P1.13 / Pro Micro D15 / Keyball J2-1
- SDA: nice!nano P0.09 / Pro Micro D10 / Keyball J2-2
- DR: nice!nano P0.10 / Pro Micro D16 / Keyball J2-6
- Spare: nice!nano P1.11 / Pro Micro D14 / Keyball J2-7
- P0.09/P0.10 NFC pins are released using `nfct-pins-as-gpios`
- The Rev M2 adapter already carries the two 4.7 kOhm I2C pull-ups.

Before using this firmware, remove the Cirque module's populated **R1 470 kOhm**
resistor to select I2C mode. Do not do the old 5 V R7/R8 modification.

## Trackpad orientation

The installed Cirque's 12-pin connector is at the **far-left / 9 o'clock** side
of the mount, at the end of the FFC cable slot. The TM040040 reference orientation
has the connector on the right, so the installed module is rotated **180 degrees**.

The right overlay therefore sets:

- `invert-x;`
- `invert-y;`
- no `swap-xy`

This correction happens in the Cirque driver before ZMK's scroll mapping.

## Trackpad behavior

Default behavior is normal relative pointer movement. `primary-tap-enable` is on,
so a Cirque primary tap is sent as mouse button 1 / left-click by ZMK.

A persistent full-pad scroll mode is also configured:

- Layer 4 (`SCROLL`) maps Cirque X/Y into horizontal/vertical wheel events.
- Scroll deltas are scaled to 1/8 for a sane first-test speed.
- Default shortcut: **hold NAV and tap S** to toggle SCROLL on/off.
- You can remap that `&tog SCROLL` binding in ZMK Studio later.

### Circular/edge scroll note

The current upstream Zephyr `cirque,pinnacle` driver used by ZMK main explicitly
disables the Pinnacle hardware's built-in scroll output and decodes relative X/Y
plus primary tap only. Therefore this baseline does **not** claim native circular
scroll support yet. The toggle full-pad scroll mode is implemented entirely in ZMK
and does not require a custom driver fork.

## ZMK Studio

The right/central build enables ZMK Studio over USB UART RPC. Studio locking is
disabled during first-article development.

The physical Keyball61 layout and five editable layers are exposed to Studio.
The SCROLL layer must remain layer index 4 unless the right overlay is changed too.

## Deep sleep status

Deep sleep is intentionally disabled in this first build. As of 2026-08-30,
upstream ZMK issue #3195 is still open: on Zephyr 4.1, a Studio-enabled split
central can fail to wake from deep sleep. The Cirque's own hardware low-power mode
is enabled independently.

Once the keyboard, BLE split, Cirque, and Studio are validated, power policy can be
revisited (deep sleep without Studio, or after upstream fixes the wake issue).

## RGB

RGB is intentionally disabled for first bring-up. Validate matrix + BLE split +
Cirque first, then add the stock Keyball61 SK6812MINI-E chain and test it at 3.3 V.

## Build targets

`build.yaml` creates:

- `keyball61_left` — nice!nano left/peripheral
- `keyball61_right_studio` — nice!nano right/central + Cirque + ZMK Studio
- `settings_reset` — use if split/BLE bond state needs clearing

The config is pinned to ZMK commit `6e2ef41e022d555b10f116e395832913f71717b3`
(2026-08-18 main snapshot) because stable ZMK v0.3 predates the Zephyr 4.1
upstream Cirque driver.

## First hardware test order

1. Assemble Rev M2 and verify the continuity map before connecting the Cirque.
2. Remove Cirque R1 470 kOhm for I2C mode.
3. Flash `settings_reset` to both nice!nanos if they have prior ZMK pairings.
4. Flash left firmware to the left controller and right firmware to the right.
5. Pair the right/central half to the host.
6. Confirm all keyboard positions before testing the Cirque.
7. Confirm Cirque cursor direction: physical right/up must produce right/up.
8. Confirm tap-to-left-click.
9. Hold NAV + tap S and verify full-pad scroll; repeat to exit.
10. Only after those pass, tune cursor and scroll scalers.

## Tuning

Cursor speed is currently 1:1 in `keyball61_right.overlay`:

```dts
input-processors = <&zip_xy_scaler 1 1>;
```

Examples:

- faster: `&zip_xy_scaler 3 2`
- slower: `&zip_xy_scaler 2 3`

Scroll is currently:

```dts
&zip_scroll_scaler 1 8
```

Decrease the divisor for faster scrolling, increase it for slower scrolling.
ZMK's scaler parameters should stay at 16 or below.
