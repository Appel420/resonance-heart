# resonance-heart

ESP32 controller sketch for a CDI/MED hybrid water system.

## Files

- `SovereigntyOne.ino` — main controller sketch
- `LICENSE` — MIT license

## What it does

- switches between CDI and MED modes from PV voltage and TDS thresholds
- pauses on dry-run, over-pressure, and temperature faults
- schedules MED polarity reversal to reduce scaling
- emits JSON telemetry over Serial at 1 Hz

## Default thresholds

- CDI: 13.0 V PV and 300 ppm TDS
- MED: 14.0 V PV and 1200 ppm TDS

## Notes

- Pin assignments and thresholds are defined at the top of `SovereigntyOne.ino`
- The sketch targets the Arduino/ESP32 toolchain
