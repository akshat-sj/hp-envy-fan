# HP Envy 14 Fan Control

Experimental Linux fan control for the HP ENVY 14-eb0xxx, board 8815, BIOS F.46.

Requires Python 3.11+, GCC, make, and matching kernel development headers.

```sh
make
./hp-fan status
./hp-fan cool
./hp-fan max
./hp-fan auto
```

`cool` targets 4,800 / 4,500 RPM; `max` targets 6,000 / 5,800 RPM.
Changes require sudo. Keep the command running; Ctrl+C restores automatic control.
Use `--seconds 30` for a timed session. Rebuild after kernel updates.

Automatic targets are restored at 80°C or after ten seconds without a heartbeat.
Sustained operation, sleep/resume, and GPU workloads remain incompletely verified.
The tool loads a local kernel module; no service is installed.

[GPL-2.0-only](LICENSE).
