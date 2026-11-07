# Ring Fit Adventure (`01002FF008C24000`)

Update 1.2.0 (`v327680`) over base game `v0`. SDK 9.3.0. Nintendo EPD engine (`sead` / `agl` / `gsys`, NintendoWare) on NVN. Modules `main`, `subsdk0` (`multimedia`) and `sdk`. Played on macOS with physical Right Joy-Con mounted on the Ring-Con (`npad 0`) and Left Joy-Con in the Leg Strap (`npad 1`).

## What it needs

| Setting | Where | Why |
|---|---|---|
| Split Joy-Con & Ring-Con (`nn::hidbus`) support | `dm/host/joycon_mac.h`, `dm/host/hle_hid.c`, `dm/host/hle_offline.c` | Ring Fit Adventure splits a Joy-Con pair across two controller slots (`npad 0` = Right Joy-Con with Ring-Con external MCU polling on `hidbus`, `npad 1` = Left Joy-Con in the Leg Strap) |
| SPI IMU calibration & upright orientation priming | `dm/host/joycon_mac.h`, `dm/host/motion_fusion.h` | Reads factory (`0x6020`) and user (`0x8026`/`0x8028`) IMU calibration from Joy-Con SPI flash, removes resting gyroscope bias, and uses shortest-arc gravity priming so the vertically worn Leg Strap has no heading flip or tilt offset |
| Joy-Con stick SPI calibration & deadzone | `dm/host/joycon_mac.h`, `dm/host/hle_hid.c` | Reads factory (`0x603D`/`0x6046`) and user (`0x8010`/`0x801B`) 12-bit stick calibration and parameters (`0x6086`/`0x6098`) with radial deadzone to eliminate phantom character movement from resting analog stick drift |
| Per-Joy-Con paced Bluetooth HD Rumble | `dm/host/joycon_mac.h`, `dm/host/haptics_apple.m` | Dedicated rumble worker threads per Joy-Con pace Bluetooth HID `0x10` output reports at ~28 Hz with window-averaged amplitude so macOS `IOBluetoothHIDDriver` never enters a multi-second transaction stall |
| Valid JPEG icon in `nn::account::LoadProfileImage` | `dm/host/hle_account.c` | The save system decodes the player's profile icon via `LoadProfileImage` when formatting or writing `saveMount0:` and aborts save creation if the returned buffer is not a valid baseline JPEG |

## Found on this game

- Split Joy-Con + Ring-Con `nn::hidbus` protocol (`dm/host/joycon_mac.h`, `dm/host/hle_offline.c`): probes the Right Joy-Con MCU (`0x22`, `0x21`, `0x59`, `0x5C`, `0x5A`) for external device `0x2000` (RingController) and maps strain-gauge force readings from `Report 0x30` into the `hidbus` shared ring buffer.
- Upright six-axis orientation priming (`dm/host/motion_fusion.h`): shortest-arc rotation from `(0, 0, 1)` to `-acc / |acc|` avoids the vertical heading singularity when the Left Joy-Con starts upright in the Leg Strap.
- Joy-Con stick SPI calibration and radial deadzone (`dm/host/joycon_mac.h`, `dm/host/hle_hid.c`): unpacks packed 12-bit factory and user stick calibration from SPI flash and applies radial deadzone with continuous scaling so physical Joy-Con resting drift does not trigger auto-forward locomotion on world maps and courses.
- Profile JPEG generation (`dm/host/hle_account.c`): `nn::account::LoadProfileImage` synthesizes a valid 256x256 baseline JFIF JPEG so games that embed or decode the user avatar during save initialization succeed.

## Controls

- Bluetooth Joy-Con pair on macOS:
  - Right Joy-Con (in Ring-Con): Ring-Con squeeze/pull strain sensor, 6-axis IMU, ABXY, R, ZR, Plus, Right Stick.
  - Left Joy-Con (in Leg Strap): 6-axis IMU posture and exercise tracking, D-pad, L, ZL, Minus, Left Stick.
