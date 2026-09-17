# GTA:SA — Auto Health Regeneration

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)

A delayed-heal mod for **GTA: San Andreas 2.10** on Android (Android Mod
Loader / FLA). Instead of an instant refill, your health regenerates
tick-by-tick after a configurable delay without damage, so police chases,
low-health escapes and hunger still matter. Built for players who want the
convenience of auto-regen without the arcade feel.

## Table of contents

- [Install](#install)
- [Configuration](#configuration)
- [How it works](#how-it-works)
- [Verify it loaded](#verify-it-loaded)
- [Building from source](#building-from-source)
- [Verified offsets](#verified-offsets)
- [License](#license)

## Install

1. Copy `libAML_PSDK_HealthRegen64.so` (arm64) — or
   `libAML_PSDK_HealthRegen.so` (armv7) — to the mods folder:

   ```bash
   /sdcard/Android/data/com.rockstargames.gtasa/mods/
   ```

2. Launch the game. The config file is created automatically on first run:

   ```bash
   /sdcard/Android/data/com.rockstargames.gtasa/configs/net.psdk.samod.regen.ini
   ```

> [!NOTE]
> Requires a game build with Android Mod Loader / FLA installed. Tested on
> GTA: SA 2.10 arm64.

## Configuration

All values live in the `[Regen]` section:

```ini
[Regen]
RegenDelay = 5.0        ; seconds without damage before healing starts
RegenRate = 2.0         ; HP per second (health and armour use the same rate)
RegenCap = 65.0         ; stop healing at 65% of max health
MinHealthToRegen = 15.0 ; never heal below 15% of max health
EnableWhileWanted = 0   ; 1 = also heal while chased by the police
EnableArmorRegen = 0    ; 1 = also regenerate armour (caps at 100)
```

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `RegenDelay` | float | `5.0` | Seconds without damage before healing starts. |
| `RegenRate` | float | `2.0` | Health (and armour) restored per second. |
| `RegenCap` | float | `65.0` | Stop healing at this % of max health. |
| `MinHealthToRegen` | float | `15.0` | Never start healing below this % of max health. |
| `EnableWhileWanted` | bool | `0` | Set to `1` to heal while pursued. |
| `EnableArmorRegen` | bool | `0` | Set to `1` to also regenerate armour. |

> [!TIP]
> For a stricter, more vanilla-feeling setup: `RegenDelay = 6.0` and
> `RegenCap = 50.0`.

## How it works

Hooks `CPlayerInfo::Process(int)`, called once per frame. Vanilla logic runs
first, then:

1. If you take damage (health or armour drops between frames), the timer resets to zero.
2. After `RegenDelay` seconds without damage, HP rises tick-by-tick.
3. Healing stops at `RegenCap` % of max health, and never starts below `MinHealthToRegen` %.

The rate is non-linear: faster right after the delay ends, easing off as you
approach the cap, so topping off is the slow part.

Regeneration is disabled while you are wasted or busted (death/arrest
animations), while a scripted cutscene is running (`CCutsceneMgr::ms_running`),
and while you have wanted stars (unless `EnableWhileWanted = 1`).

The mod does not touch hunger, Fat or Muscle stats, adds no HUD popups and no
sounds — the vanilla low-HP flash stays.

## Verify it loaded

```bash
logcat -d | grep -E "HealthRegen|AndroidModLoader"
```

Expected output:

```
I AndroidModLoader: Mod (GUID net.psdk.samod.regen) has been preprocessed.
I HealthRegen: Auto Health Regen loaded. Delay=5.0s Rate=2.0 HP/s Cap=65% Min=15% Wanted=OFF Armor=OFF
```

## Building from source

From this directory with the Android NDK (r29 used here):

```bash
ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=./Android.mk \
  NDK_APPLICATION_MK=./Application.mk
```

Outputs:

- `libs/arm64-v8a/libAML_PSDK_HealthRegen64.so`
- `libs/armeabi-v7a/libAML_PSDK_HealthRegen.so`

## Verified offsets

All symbols are resolved at runtime from `libGTASA.so` 2.10 (arm64) dynamic
symbols — no hard-coded addresses. Only the struct offsets below are
hard-coded, verified against the modded APK binary:

| Field | Offset |
| --- | --- |
| `CPed::m_fHealth` | `+0x6AC` |
| `CPed::m_fMaxHealth` | `+0x6B0` |
| `CPed::m_fArmour` | `+0x6B4` |
| `CWanted::m_nWantedLevel` | `+0x2C` |

## License

[MIT](./LICENSE). Based on the
[aml-psdk](https://github.com/AndroidModLoader/aml-psdk) template
(`examples/game_sa/health_regen`).