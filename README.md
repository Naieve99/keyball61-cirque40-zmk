# Keyball61 + Keyball44 Rev1 — nice!nano v2 / ZMK

Firmware for the Rev M2 Keyball61 → Cirque TM040040 adapter, plus a staged
Keyball44 Rev1 first article. The original Keyball61 targets are retained.

## Keyball44 status

The Keyball44 work adds the proper 44-key matrix and separate right-central
builds for either the M7U Cirque adapter or the stock PMW3360. Start with:

- [Keyball44 first-article plan](docs/keyball44-rev1-first-article.md)
- [M7U shared-connector verification and safe detection](docs/m7u-universal-detection.md)

Experimental universal targets now defer both buses and identify the installed
module by protocol: a read-only Cirque FirmwareId probe at I2C address `0x2A`,
followed only when absent by a PMW3360 SPI Product_ID probe. No identity straps
are required. P1.11 is held high during the I2C probe so a PMW3360 remains
deselected, and P0.10 remains an input until SPI probing begins.

This is convenient rather than electrically fail-safe: if a connected Cirque is
broken or miswired and does not acknowledge, the subsequent SPI probe will drive
P0.10 even though that net is the Cirque's HW_DR output. Use only with verified,
healthy modules, power off before swapping, and retain the fixed-pointer builds
for fault isolation.

Keyball44 also has standalone left and right targets. Those pair to the host as
`KB44 Left` and `KB44 Right`, operate without split transport, and retain
right-side access to pointer modes and mouse buttons. See the first-article plan
for the exact flash combinations and the independent-layer limitation.

Experimental peer targets are also available for both Keyball44 and Keyball61.
With those builds, each half remains an independent host-facing keyboard while
layer changes are mirrored over a private connectionless BLE broadcast. See the
[independent peer firmware guide](docs/independent-peer-firmware.md) for artifact
pairs, limitations, and the required hardware validation sequence.

## Repository layout

This repository is intentionally arranged as a normal ZMK user-config repo:

- `.github/workflows/build.yml` — GitHub Actions build
- `build.yaml` — firmware build matrix
- `config/west.yml` — pinned ZMK revision
- `config/keyball61.keymap` — editable keymap
- `config/boards/shields/keyball61/` — custom Keyball61 shield

The custom shield **must stay under `config/boards/shields/`** because the standard
ZMK user-config workflow builds with `ZMK_CONFIG=config`.

## Frozen Rev M2 electrical assumptions

The **right half is the BLE central** and owns the Cirque TM040040.

- Cirque: 3.3 V, I2C, address `0x2A`
- SCL: nice!nano P1.13 / Keyball J2-1 / Cirque J1-9
- SDA: nice!nano P0.09 / Keyball J2-2 / Cirque J1-10
- GND: Keyball J2-3/J2-5 / Cirque J1-11
- 3.3 V: Keyball J2-4 / Cirque J1-12
- DR: nice!nano P0.10 / Keyball J2-6 / Cirque J1-4
- Spare: nice!nano P1.11 / Keyball J2-7
- P0.09/P0.10 are NFC-capable nRF52840 pins and are released with
  `nfct-pins-as-gpios`.
- The Rev M2 adapter carries the two 4.7 kΩ I2C pull-ups.

Before first use, remove the Cirque module's populated **R1 470 kΩ** resistor to
select I2C mode. Do **not** apply the older 5 V R7/R8 modification.

## Physical trackpad orientation

The installed Cirque connector is at the **far-left / 9 o'clock** side of the
mount, at the end of the FFC cable slot. The TM040040 reference orientation has
the connector on the right, so the module is installed **180° rotated**.

The Cirque node therefore applies:

- `invert-x;`
- `invert-y;`
- no `swap-xy`

Zephyr's Pinnacle driver uses those invert bits in relative mode as well, despite
the binding text describing them as absolute-mode properties.

## Pointer behavior

Normal mode is the default:

- full Cirque surface = normal relative cursor movement
- primary tap = left click (`primary-tap-enable`)
- Cirque hardware sleep mode enabled

### Pointer modes

The universal keymaps reserve three synchronized pointer-mode layers:

- layer 4: Expanded (2x cursor scaling)
- layer 5: Native (1:1 device movement)
- layer 6: full-pad horizontal/vertical scroll

The NAV layer contains mode selection keys on both sides. The earlier fixed
Cirque build also uses layer 6 for full-pad scrolling.

Use the NAV-layer `SCROLL` toggle to enter or leave full-pad scroll mode.

Because the SCROLL layer is transparent for keys, the same shortcut can be used
to exit it.

Initial scroll scaling is 1/8. Cursor scaling starts at 1:1. Both are deliberately
conservative first-test values and can be tuned after hardware validation.

### Circular/edge scrolling

The upstream Zephyr Pinnacle driver used in this baseline focuses on pointer
movement and primary tap. Native circular/edge scrolling is therefore not relied
on for first bring-up. The optional full-pad scroll mode works entirely in ZMK.
A separate enhanced-driver branch can be evaluated later if native circular scroll
becomes desirable.

## ZMK Studio

The preferred right-half build enables ZMK Studio using the standard
`studio-rpc-usb-uart` snippet. Studio locking is disabled for development.

The Keyball61 physical layout and five keymap layers are defined in devicetree so
Studio can edit bindings at runtime. Studio cannot create arbitrary new firmware
behaviors, so the scroll toggle and mouse behaviors are pre-defined here.

## Build outputs

GitHub Actions retains the four Keyball61/support artifacts:

- `keyball61_left` — left BLE peripheral
- `keyball61_right` — right central, no Studio (fallback)
- `keyball61_right_studio` — preferred right central + Cirque + Studio
- `settings_reset` — clear stale BLE/split settings if needed

It also builds these Keyball44 artifacts:

- `keyball44_left`
- `keyball44_left_standalone`
- `keyball44_right_cirque`
- `keyball44_right_cirque_studio`
- `keyball44_right_cirque_standalone`
- `keyball44_right_pmw3360_experimental`
- `keyball44_right_pmw3360_standalone_experimental`

Experimental independent + peer-layer artifacts:

- `keyball61_left_independent_peer`
- `keyball61_right_cirque_independent_peer`
- `keyball44_left_independent_peer`
- `keyball44_right_cirque_independent_peer`
- `keyball44_right_pmw3360_independent_peer_experimental`

Experimental universal independent artifacts:

- `keyball61_left_universal_peer_experimental`
- `keyball61_right_universal_peer_experimental`
- `keyball44_left_universal_peer_experimental`
- `keyball44_right_universal_peer_experimental`

These four builds detect no module, Cirque I2C, or PMW3360 SPI independently on
each half at power-on. Side-specific files are still required for the physical
key matrix and explicit left/right enrollment identity; changing the removable
pointer module does not require reflashing or an identity strap.

The configuration is pinned to ZMK commit
`6e2ef41e022d555b10f116e395832913f71717b3` (2026-08-11), a Zephyr 4.1-era
main snapshot that includes the upstream Cirque Pinnacle driver.

## Known upstream caveat

As of this pinned ZMK era, upstream issue #3341 reports that holding an `&mkp`
mouse-button key while moving an integrated trackpad can release the held mouse
button unexpectedly. Basic pointer movement, tap-click, and scroll-mode bring-up
are still useful; click-drag via keyboard mouse-button bindings should be treated
as a separate validation item.

## First hardware test sequence

1. Assemble one Rev M2 adapter.
2. Verify continuity before connecting the Cirque, especially 3.3 V → J1-12 and
   GND → J1-11.
3. Remove Cirque R1 470 kΩ for I2C mode.
4. If the nice!nanos have old bonds/settings, flash `settings_reset` to both.
5. Flash `keyball61_left` to the left controller.
6. Flash `keyball61_right_studio` to the right controller.
7. Pair the right/central half to the host.
8. Validate all 61 key positions.
9. Validate pointer direction: physical right/up must produce right/up.
10. Validate primary tap.
11. Use the NAV-layer SCROLL toggle and validate full-pad scrolling; repeat to exit.
12. Tune pointer/scroll scaling only after the above passes.

## RGB

RGB is deliberately disabled for the first firmware bring-up. The stock Keyball61
SK6812MINI-E chain can be added after keyboard matrix + BLE split + Cirque are
known-good at 3.3 V.
