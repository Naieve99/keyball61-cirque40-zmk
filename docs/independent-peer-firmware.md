# Independent halves with peer layer synchronization

## What this firmware does

The peer targets turn each physical half into a complete host-facing ZMK
keyboard. The computer sees two Bluetooth keyboards, and each half keeps
working if the other half is powered off or out of range.

In parallel, both halves advertise their locally requested layer state using a
small, non-connectable Bluetooth LE packet. The peer listens for that packet and
combines the received layers with its own layers. This lets a layer key on one
half change what keys do on the other half without routing ordinary keypresses
or pointer reports through a central half.

The right half sends its trackball or trackpad reports directly to the host.
The left half sends its own keys directly to the host. If a pointing device or
encoder is later fitted to the left half, it can also report directly without a
change to the peer protocol.

## Firmware pairs

Keyball44:

- left: `keyball44_left_independent_peer`
- right with M7U Cirque: `keyball44_right_cirque_independent_peer`
- right with stock PMW3360: `keyball44_right_pmw3360_independent_peer_experimental`

Keyball61:

- left: `keyball61_left_independent_peer`
- right with Cirque: `keyball61_right_cirque_independent_peer`

Pair both devices to the host. They appear as `KB44 L Peer` / `KB44 R Peer` or
`KB61 L Peer` / `KB61 R Peer`.

The existing standard split and standalone artifacts are unchanged and remain
the rollback path.

## Safety and failure behavior

- Only the layer bitmask crosses the peer link. Keys, pointer movement, clicks,
  battery state, and host Bluetooth commands stay local.
- The peer link does not create a BLE connection and does not change the host
  connection owned by either half.
- If no valid peer packet arrives for 2.5 seconds, remotely contributed layers
  are removed. Locally held or toggled layers remain active.
- Keyball44 and Keyball61 use different family and group identifiers, so they do
  not react to each other.
- The packets have a protocol identifier and CRC, but they are not encrypted or
  authenticated. A nearby device running a deliberately matching custom build
  could inject layer state. Do not treat this channel as security-sensitive.

## First-version limitations

This implementation combines local and remote layer bits. It is suitable for
normal momentary, toggle, and pointer-mode use from one side at a time. One edge
case remains: if a layer is already active only because the peer holds it, a
second local press of that exact layer may not produce a new ZMK state-change
event. Releasing the peer first can therefore remove the layer even while the
local key is still held. Avoid simultaneously holding the same layer key on both
halves until the behavior-specific protocol is added.

Bluetooth radio scanning is intentionally moderate for the first article (a
20 ms window every 60 ms). Battery impact and real-world layer latency must be
measured on hardware before reducing the scan duty cycle.

## Validation checklist

1. Flash the matching left and right peer artifacts.
2. Clear old bonds if the host confuses the new names with older firmware.
3. Pair both halves to the host and verify typing from each half alone.
4. Power off each half in turn and verify the other continues typing.
5. Hold NAV, MOUSE, and SYM on each side and verify the peer follows.
6. Toggle the pointer scroll/mode layer and verify both sides agree.
7. Power off the transmitting half while it holds a layer; verify the other
   returns to its local layer state after about 2.5 seconds.
8. Validate the right-side Cirque or PMW3360 independently of layer sync.
9. Measure current draw and practical layer-change latency before daily use.

Compilation proves API and configuration compatibility only. BLE coexistence,
latency, battery life, layer edge cases, and pointer hardware still require the
above physical tests.
