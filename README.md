# GTA:SA — Auto Health Regeneration

[![license](https://img.shields.io/badge/license-MIT-blue.svg)](./LICENSE)

A delayed-heal mod for GTA: San Andreas on Android (Android Mod Loader / FLA),
supporting 2.10 (arm64) and 2.00 (armv7). Instead of an instant refill, your
health regenerates tick-by-tick after a configurable delay without damage.
Police chases and low health stay dangerous, and hunger is untouched.

## Table of contents

- [Install](#install)
- [Configuration](#configuration)
  - [Upgrading from 1.2 or older](#upgrading-from-12-or-older)
- [How it works](#how-it-works)
- [Verify it loaded](#verify-it-loaded)
- [Building from source](#building-from-source)
- [Verified offsets](#verified-offsets)
- [License](#license)

## Install

Pick the `.so` for your game version:

| Game version | Architecture | File |
| --- | --- | --- |
| 2.10 | arm64-v8a | `libAML_PSDK_HealthRegen64.so` |
| 2.00 | armeabi-v7a | `libAML_PSDK_HealthRegen.so` |

1. Copy the matching `.so` to the mods folder:

   ```bash
   /sdcard/Android/data/com.rockstargames.gtasa/mods/
   ```

2. Launch the game. The config file is created automatically on first run:

   ```bash
   /sdcard/Android/data/com.rockstargames.gtasa/configs/net.psdk.samod.regen.ini
   ```

> [!NOTE]
> Requires a game build with Android Mod Loader / FLA installed. The game
> version is detected from the process ABI: the 2.10 APK ships arm64-only
> and the 2.00 APK ships armv7-only, so each build carries its own verified
> offsets (`Verify it loaded` shows which one is active).

## Configuration

Every option name carries its **unit**, so there is no doubt what a value
means. All values live in the `[Regen]` section:

```ini
[Regen]
HealthRegenDelay = 5.0      ; seconds without damage before health healing starts
HealthRegenRate = 2.0       ; health HP per second
HealthRegenCapPct = 65.0    ; stop health healing at 65% of max health
HealthRegenMinPct = 15.0    ; never heal health below 15% of max health
EnableWhileWanted = 0       ; 1 = also heal while chased by the police
EnableArmorRegen = 0        ; 1 = master switch for armour regen
ArmorRegenRate = 2.0        ; armour HP per second
ArmorRegenCapPct = 100.0    ; stop armour healing at 100% of armour (armour maxes at 100)
ArmorRegenDelay = 5.0       ; seconds without damage before armour healing starts
```

| Option | Unit | Type | Default | Description |
| --- | --- | --- | --- | --- |
| `HealthRegenDelay` | seconds | float | `5.0` | Seconds without damage before health healing starts. |
| `HealthRegenRate` | HP/s | float | `2.0` | Health restored per second. |
| `HealthRegenCapPct` | % of max health | float | `65.0` | Stop health healing at this % of max health. |
| `HealthRegenMinPct` | % of max health | float | `15.0` | Never start health healing below this % of max health. |
| `EnableWhileWanted` | — | bool | `0` | Set to `1` to heal while pursued. |
| `EnableArmorRegen` | — | bool | `0` | Master switch for armour regen. When `1`, armour regenerates using its own independent settings below. |
| `ArmorRegenRate` | HP/s | float | `2.0` | Armour restored per second. Independent of `HealthRegenRate`. |
| `ArmorRegenCapPct` | % of armour | float | `100.0` | Stop armour healing at this % (clamped to 100, since armour maxes at 100 in SA). |
| `ArmorRegenDelay` | seconds | float | `5.0` | Seconds without damage before armour healing starts. Independent of `HealthRegenDelay`. |

> [!NOTE]
> `HealthRegenCapPct = 100` means "heal all the way back to full health". You
> do **not** need to put absurd values like `1000` — that was only a workaround
> for an old bug where the cap was misread. The value is a plain percentage of
> max health; anything above `100` is simply equivalent to `100`.

> [!IMPORTANT]
> Health and armour regen are **fully decoupled**. Armour has its own window,
> delay and rate, and no longer depends on the health being inside
> `[HealthRegenMinPct, HealthRegenCapPct]`. With the default
> `HealthRegenCapPct = 65.0`, health stops healing at 65%, but armour keeps
> regenerating up to `ArmorRegenCapPct` (100% by default) as long as
> `EnableArmorRegen = 1`.


> [!TIP]
> For a stricter, more vanilla-feeling setup: `HealthRegenDelay = 6.0` and
> `HealthRegenCapPct = 50.0`.

### Upgrading from 1.2 or older

The old option names still work as **aliases** and are migrated automatically
on the first launch. If your `[Regen]` section uses:

| Old name (≤ 1.2) | New name |
| --- | --- |
| `RegenDelay` | `HealthRegenDelay` |
| `RegenRate` | `HealthRegenRate` |
| `RegenCap` | `HealthRegenCapPct` |
| `MinHealthToRegen` | `HealthRegenMinPct` |
| `ArmorRegenCap` | `ArmorRegenCapPct` |

…the value is copied into the new key (the ini is rewritten with the new
names) the next time the game starts, and a line is logged, e.g.:

```
I HealthRegen: Migrated legacy 'RegenCap' -> 'HealthRegenCapPct' (=65.0)
```

The alias is only used when the new key was **not** already set by you, so the
new names always win if both are present. `EnableWhileWanted`,
`EnableArmorRegen`, `ArmorRegenRate` and `ArmorRegenDelay` keep their names.

## How it works

Hooks `CPlayerInfo::Process(int)`, called once per frame. Vanilla logic runs
first, then:

1. If you take damage (health or armour drops between frames), the timer resets to zero.
2. After `HealthRegenDelay` seconds without damage, HP rises tick-by-tick.
3. Healing stops at `HealthRegenCapPct` % of max health, and never starts below `HealthRegenMinPct` %.

The rate is non-linear: faster right after the delay ends, easing off as you
approach the cap, so topping off is the slow part.

Armour regen (when `EnableArmorRegen = 1`) runs on its own independent track:
its own window (any armour below `ArmorRegenCapPct`), its own `ArmorRegenDelay`
against the same "last damage" timestamp, and its own `ArmorRegenRate`. It does
**not** require the health to be inside `[HealthRegenMinPct, HealthRegenCapPct]`,
so armour keeps filling up to 100% even after health has stopped at its cap.

Regeneration is disabled while you are wasted or busted (death/arrest
animations), while a scripted cutscene is running (`CCutsceneMgr::ms_running`),
and while you have wanted stars (unless `EnableWhileWanted = 1`).

The mod does not touch hunger, Fat, or Muscle stats, and it adds no HUD popups
or sounds. The vanilla low-HP flash stays.

## Verify it loaded

```bash
logcat -d | grep -E "HealthRegen|AndroidModLoader"
```

Expected output:

```
I AndroidModLoader: Mod (GUID net.psdk.samod.regen) has been preprocessed.
I HealthRegen: Auto Health Regen loaded [2.10 (arm64)]. HealthDelay=5.0s HealthRate=2.0 HP/s HealthCapPct=65.0% HealthMinPct=15.0% Wanted=OFF Armor=OFF
I HealthRegen: Armor regen: ArmorRate=2.0 HP/s ArmorCapPct=100.0% ArmorDelay=5.0s
```

On a 2.00 (armv7) game the bracketed variant reads `[2.00 (armv7)]`.

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

All symbols are resolved at runtime from the game's dynamic symbols — no
hard-coded addresses. Only the struct offsets below are hard-coded, verified
by disassembling the shipped binaries (the `CPed` constructor stores
`100.0/100.0/0.0` at the health/maxHealth/armour fields, the cheat
`MoneyArmourHealthCheat` writes armour, and `CWanted::Update` reads the wanted
level):

| Field | 2.10 (arm64) | 2.00 (armv7) |
| --- | --- | --- |
| `CPed::m_fHealth` | `+0x6AC` | `+0x544` |
| `CPed::m_fMaxHealth` | `+0x6B0` | `+0x548` |
| `CPed::m_fArmour` | `+0x6B4` | `+0x54C` |
| `CWanted::m_nWantedLevel` | `+0x2C` | `+0x2C` |

## License

[MIT](./LICENSE). Based on the
[aml-psdk](https://github.com/AndroidModLoader/aml-psdk) template
(`examples/game_sa/health_regen`).