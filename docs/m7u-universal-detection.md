# M7U shared-connector verification and safe detection

## Strapless ordered protocol detection

The universal targets require no identity resistors. Both buses remain deferred
while firmware performs an ordered startup probe on raw GPIOs:

1. Hold P1.11 / PMW3360 NCS high so the SPI sensor is inactive.
2. Keep P0.10 / PMW3360 MOSI / Cirque HW_DR as an input.
3. Use open-drain-style GPIO transitions to read the Cirque FirmwareId at I2C
   address `0x2A`. Accept Cirque only when FirmwareId is `0x07`.
4. Only when that probe fails, initialize SPI and accept PMW3360 only when its
   Product_ID register is `0x42`.
5. If neither positive identity is returned, run keyboard-only.

This detects a healthy module without an adapter strap, but it is not fail-safe
against a broken or miswired Cirque. A failed Cirque probe is indistinguishable
from no Cirque, and the PMW3360 probe must then drive P0.10 as MOSI. The fixed
pointer builds remain the safer diagnostic option for questionable hardware.

## Shared signal map

| nice!nano GPIO | M7U Cirque role | Stock PMW3360 role | Reset-safe state |
| --- | --- | --- | --- |
| P1.13 | SCL, open-drain clock | SCLK, push-pull output | High impedance |
| P0.09 | SDA, bidirectional open-drain | MISO, input | High impedance |
| P0.10 | HW_DR, sensor output / MCU input | MOSI, MCU output | High impedance; never drive before identity is certain |
| P1.11 | Spare | NCS, active-low MCU output | Drive high before probing either protocol |
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
6. Verify P1.11 is unused on the Cirque adapter and reaches PMW NCS in the
   trackball configuration.
7. Confirm I2C pull-up values and that there is only one appropriate effective
   set on SCL/SDA.
8. Confirm the Cirque is configured for 3.3 V I2C at address 0x2A. For the
   TM040040 adapter plan this means removing populated R1 470 kOhm and not
   applying the older 5 V R7/R8 modification.
9. With sensors disconnected, verify there are no low-resistance cross-shorts
   between the four signal nets.

## Hardware contract for the experimental universal image

Do not install P1.11 identity straps. P1.11 must remain the normal PMW3360 NCS
connection and must be electrically unused by the Cirque adapter. The startup
shim runs before normal Zephyr I2C/SPI device initialization, holds NCS high,
and performs the positive device checks above.

Detection is deliberately based on the device's identity register, not merely
an address ACK. That avoids mistaking an unrelated device at `0x2A` for a
Pinnacle. It also avoids treating arbitrary SPI data as a PMW3360.

Detection runs independently on each half at every boot. Zero, one, or two
pointer modules therefore need no global mode and do not change peer enrollment.
The four universal artifacts remain named `experimental` until both positive
probe paths and the empty-connector path are tested on physical first articles.

Removing the current Cirque module's R1 selects its I2C configuration. Cirque
lists its Gen6s circle pads, including TM040040, as I2C-only. A Gen6s module can
therefore share the I2C electrical branch, but its exact connector wiring, I2C
address, and positive register identity must be added before firmware enables
the correct driver. See [Cirque's module interface table](https://www.cirque.com/glidepointspecification).

## Powered bring-up

Use a current-limited 3.3 V supply or a USB power monitor for the first article.
Power one sensor configuration at a time. Confirm rail voltage, idle current,
and the reset-state voltage on all four signal pins before flashing its matching
firmware. Stop immediately for excess current, a warm component, or an
unexpectedly driven P0.10.

Neither sensor connector should be hot-plugged unless the finished hardware is
specifically designed and tested for it.
