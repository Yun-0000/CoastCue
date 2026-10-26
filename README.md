<h1 align="center">CoastCue · CN 433 MHz</h1>

<p align="center">
  This branch is the CN 433 MHz version of CoastCue, used in the Zhoushan field test.<br>
  The US 915 MHz version and the project overview are on the <a href="https://github.com/Yun-0000/CoastCue/tree/us"><code>us</code> branch</a>.
</p>

CoastCue is a nearshore safety prototype for one boat, one shore station and
one iPhone. The boat unit sends Help or SOS with its GPS position over LoRa.
The shore station sounds an alarm and passes the alert to the paired iPhone,
and the boat sees when the alert is acknowledged and resolved.

```text
Boat unit  --LoRa-->  Shore station  --BLE-->  iPhone app
           <--state--  display, buzzer, journal
```

After firmware and app installation, field use needs no laptop, Wi-Fi,
internet, cloud account or web dashboard. The shore display and buzzer remain
the fallback when the phone is unavailable.

## Firmware

The default `boat` build is the T-Beam with a wired SSD1306 that shows Receipt,
Acknowledged and Resolved; `shore` is the ESP32 DevKit with RA-02 and SH1106.
Headless, GPS diagnostic, storage-fault, US and AXP192 builds are engineering
variants only.

## Documents

- [Validation and early impact brief](docs/VALIDATION_AND_EARLY_IMPACT_BRIEF.md)
- [Protocol](docs/PROTOCOL.md)

## Development

```bash
make setup               # private config + pinned PlatformIO
make project-check       # firmware, host and Python tests, iOS
make project-full-check  # + engineering firmware variants
make help                # build and flash targets
```

`make project-check` runs iOS tests when a Simulator is installed and only
compiles them otherwise; CI always runs them. Run PlatformIO only through the
Makefile.

- Flash with `make board-loop-boat PORT=...` or `make board-loop-shore PORT=...`;
  `make devices` lists the ports, and the target checks the port before writing
  and captures a short serial log after.
- The shore never formats its storage by itself. Run
  `make init-shore-storage PORT=... CONFIRM_STORAGE_FORMAT=ERASE` only on a new
  or deliberately reset shore.
- The boat ID and LoRa key live only in the untracked
  `ESP-32-CoastCue/pio/include/coastcue_config.h` that `make setup` creates.
  Fill in both before use: the build rejects the placeholder key, and the
  upload check rejects the placeholder boat ID. Never commit it, serial-port
  names, `.pio`, `.tooling` or other build output.

The iPhone app uses a free Xcode Personal Team, whose signing expires after
seven days: reinstall before a field session.

## License

[MIT](LICENSE)
