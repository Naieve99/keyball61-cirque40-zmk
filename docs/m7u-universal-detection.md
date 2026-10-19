# M7U shared-connector verification and safe detection

## Do not enable blind protocol fallback

The four signal pins can represent either Cirque I2C or stock PMW3360 SPI, but
they do not all have compatible directions. In particular, P0.10 is an MCU
input for Cirque data-ready and an MCU output for PMW3360 MOSI. If a connected
Cirque fails to acknowledge I2C, blindly changing P0.10 to a push-pull SPI
output can contend with the Cirque's HW_DR output.

For that reason blind fallback remains forbidden. The branch now contains
experimental universal targets with deferred I2C/SPI initialization, but they
must not be flashed until the adapter supplies and electrically verifies the
three-state identity described below. The fixed-pointer builds remain the safe
bring-up fallback.

## Shared signal map

| nice!nano GPIO | M7U Cirque role | Stock PMW3360 role | Reset-safe state |
| --- | --- | --- | --- |
| P1.13 | SCL, open-drain clock | SCLK, push-pull output | High impedance |
| P0.09 | SDA, bidirectional open-drain | MISO, input | High impedance |
| P0.10 | HW_DR, sensor output / MCU input | MOSI, MCU output | High impedance; never drive before identity is certain |
| P1.11 | Spare | NCS, active-low MCU output | High impedance; for PMW, assert inactive high before enabling SPI |
| GND | Ground | Ground | Must be common |
| 3.3 V | Sensor supply | Sensor supply | Never apply 5 V |

The provisional mapping matches the official Rev1 signal allocation at the
Pro Micro pins: SCLK=B1/P1.13, MISO=B2/P0.09, MOSI=B3/P0.10, and
NCS=B6/P1.11.

However, the official Rev1 schematic draws the `BALL_PCB` connection as **eight
electrical contacts**: VCC, MISO, GND, MOSI, GND, NCS, GND, SCLK. The M7U notes
describe a seven-position J2 connection. Treat names and position numbers as
unverified until a meter confirms the actual PCB, cable and adapter end to end.

## Required unpowered checks

Record these results for the exact keyboard PCB revision and adapter revision:

1. Photograph and label both connector orientations before inserting a cable.
2. Verify every signal from nice!nano pad to adapter pad and sensor pad.
3. Verify every intended ground contact is ground.
4. Verify 3.3 V has no continuity to ground or any signal.
5. Verify P0.10 reaches only Cirque HW_DR in Cirque configuration and only
   PMW3360 MOSI in PMW configuration.
6. Verify P1.11 reaches only the intended spare/identity circuit or PMW NCS.
7. Confirm I2C pull-up values and that there is only one appropriate effective
   set on SCL/SDA.
8. Confirm the Cirque is configured for 3.3 V I2C at address 0x2A. For the
   TM040040 adapter plan this means removing populated R1 470 kOhm and not
   applying the older 5 V R7/R8 modification.
9. With sensors disconnected, verify there are no low-resistance cross-shorts
   between the four signal nets.

## Hardware contract for the experimental universal image

The implemented detector reads P1.11 twice while all shared protocol pins are
inputs: once with the nRF internal pull-up and once with its internal pull-down.
The removable module or adapter, not the keyboard base PCB, must provide this
identity:

| Pull-up reading | Pull-down reading | Result |
| --- | --- | --- |
| low | low | I2C class; initialize only Cirque today |
| high | high | SPI class; initialize only PMW3360 |
| high | low | floating/empty; keyboard-only mode |
| low | high | invalid; keep shared pins inert |

The external low/high straps must be strong enough to override both internal
pull directions while respecting sensor limits. A 2.2-4.7 kOhm starting range
is reasonable for bench evaluation, but the final value, current, leakage, VIH,
VIL, and PMW3360 NCS startup behavior must be measured before production. The
PMW high strap also keeps NCS inactive. A dedicated ID contact or isolation
device remains more robust.

Firmware also needs an early initialization shim so normal Zephyr I2C and SPI
device initialization cannot race ahead and configure both protocols. The safe
state machine is:

1. Start with P1.13, P0.09, P0.10 and P1.11 as high-impedance inputs.
2. Read the hardware-defined low/high/floating identity. Do not infer protocol
   from a failed bus transaction.
3. If identity is Cirque, keep P0.10 as an input and initialize only I2C.
4. If identity is PMW3360, establish NCS inactive high first, then configure
   SCLK/MOSI/MISO and initialize only SPI.
5. For a floating identity, remain in keyboard-only mode. For a conflicting or
   invalid identity, leave every signal safe and expose an error.

Detection runs independently on each half at every boot. Zero, one, or two
pointer modules therefore need no global mode and do not change peer enrollment.
The four universal artifacts are intentionally named `experimental` until this
contract is proven with a meter and physical first articles.

Removing the Cirque module's R1 selects its I2C configuration; that is separate
from the new P1.11 identity-low strap. A future Gen6 I2C module can share the
I2C-class branch, but its exact connector wiring, I2C address, and positive
register identity must be added before firmware can distinguish it from Cirque.

## Powered bring-up

Use a current-limited 3.3 V supply or a USB power monitor for the first article.
Power one sensor configuration at a time. Confirm rail voltage, idle current,
and the reset-state voltage on all four signal pins before flashing its matching
firmware. Stop immediately for excess current, a warm component, or an
unexpectedly driven P0.10.

Neither sensor connector should be hot-plugged unless the finished hardware is
specifically designed and tested for it.
