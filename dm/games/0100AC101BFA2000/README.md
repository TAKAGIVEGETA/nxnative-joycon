# Fit Boxing 3 -Your Personal Trainer- (`0100AC101BFA2000`)

Update 1.3.0 (v524288) over base game `[本体v1.0.0][0100AC101BFA2000]`. SDK 17.5.6. Unity 2023.2.15f1 IL2CPP on NVN, CRIWARE + FMOD audio. Modules `main`, `subsdk0` (`NintendoSDK_movie`) and `sdk`. Dual Joy-Con motion controls (hands boxing) supported on macOS natively via dedicated Joy-Con HID driver (`joycon_mac.h`).

## What it needs

| Setting | Where | Why |
|---|---|---|
| Dual Joy-Con HID & HD Rumble driver | `dm/host/joycon_mac.h`, `dm/host/hle_hid.c`, `dm/host/haptics_apple.m` | Fit Boxing 3 requires dual Joy-Con motion input (`STYLE_JOYDUAL`), communicating with Left and Right Joy-Con 6-axis IMUs independently at 60 Hz and sending Report `0x10` HD Rumble |
| 7 GB guest layout: `mem_reserve 0x1C0000000`, `mem_alias 0x118000000`, `mem_stack 0x90000000`, `app_pool 0x40000000` | [`game.tsv`](game.tsv) | NPDM declares 16 MB system resource (`sys_resource 0x1000000`); `mem_stack` (2.25 GB) prevents `MapAliasStack` fragmentation across 400+ Unity worker threads; `app_pool` (1 GB) leaves +2 GB alias headroom so `StandardAllocator` does not starve Unity render target allocations |
| `notif:a` HIPC service | `dm/host/hle_sf.c` | In-game alarm reminder settings connect to `notif:a` via `nn::notification::ListAlarmSettings` |
| `shader_groups _unitybuiltinshaders_=engine /Resources/=engine /StreamingAssets/=assets /Data/=data` | `game.tsv` | Built-in shaders in engine, asset bundles in assets, serialized data in data |

## Found on this game

- [Game-linked HIPC clients of system services](../../../docs/reference/services.md#game-linked-hipc-clients-of-system-services): in-game settings open `notif:a` for workout alarm reminders.
- NVN texture & memory pool magic validation (`dm/gfx/nvn/hle_nvn.c`): guards `nvnTextureGetMemoryPool` and `nvnTextureInitialize` against uninitialized texture structs.

## Controls

- Dual Joy-Cons connected via Bluetooth to macOS:
  - Left Joy-Con: Left jab, body blows, D-pad, L, ZL, Minus.
  - Right Joy-Con: Right straight, hooks, uppercuts, ABXY, R, ZR, Plus.
  - Motion tracking: 6-axis IMU on both hands sampled independently at 200 Hz, converted into Switch coordinate frame and fed to `nn::hid` JoyDual sensors.
