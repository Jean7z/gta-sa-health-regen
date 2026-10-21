#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <aml-psdk/game_sa/plugin.h>

#include <cstdint>
#include <cstdlib> /* atof */

MYMODCFG(net.psdk.samod.regen, SA Android Auto Health Regeneration, 1.3, Jean7z)

static ConfigEntry *cfgDelay, *cfgRate, *cfgCap, *cfgMinHealth, *cfgWhileWanted, *cfgArmorRegen;
static ConfigEntry *cfgArmorRegenRate, *cfgArmorRegenCap, *cfgArmorRegenDelay;

/* libGTASA.so offsets - verified by disassembly of the shipped binaries:
 *   2.10 (arm64): CPed ctor stores 100.0f at +0x6ac/+0x6b0, 0.0f at +0x6b4.
 *   2.00 (armv7): CPed ctor stores 100.0f at +0x544/+0x548, 0.0f at +0x54c.
 *   CWanted level read at +0x2c in CWanted::Update (both).
 * The ABI already selects the build: the 2.10 APK ships arm64-only and the
 * 2.00 APK ships armv7-only, so __aarch64__ <=> 2.10 / else <=> 2.00. */
#if defined(__aarch64__)
#define CPED_HEALTH     0x6ACu /* CPed::m_fHealth */
#define CPED_MAXHEALTH  0x6B0u /* CPed::m_fMaxHealth */
#define CPED_ARMOUR     0x6B4u /* CPed::m_fArmour */
#define GAME_VARIANT    "2.10 (arm64)"
#else
#define CPED_HEALTH     0x544u /* CPed::m_fHealth */
#define CPED_MAXHEALTH  0x548u /* CPed::m_fMaxHealth */
#define CPED_ARMOUR     0x54Cu /* CPed::m_fArmour */
#define GAME_VARIANT    "2.00 (armv7)"
#endif
#define CWANTED_LEVEL   0x2Cu  /* CWanted::m_nWantedLevel */

struct CPlayerInfo; /* opaque: we only forward the pointer */

typedef void* (*FindPlayerPedFn)(int);
typedef bool  (*IsAliveFn)(void*);
typedef bool  (*RestartFn)(CPlayerInfo*);
typedef void* (*FindPlayerWantedFn)(int);

static FindPlayerPedFn    pFindPlayerPed;
static IsAliveFn          pIsAlive;
static RestartFn          pRestartDeath;
static RestartFn          pRestartArrest;
static FindPlayerWantedFn pFindPlayerWanted;

static volatile uint32_t* gTimeMs;   /* CTimer::m_snTimeInMilliseconds */
static volatile bool*     gCutscene; /* CCutsceneMgr::ms_running */

static float    s_lastHealth = -1.0f;
static float    s_lastArmour = -1.0f;
static uint32_t s_lastDamageMs = 0;
static uint32_t s_lastTickMs = 0;

static void ResetRegenState(float health, float armour, uint32_t now)
{
    s_lastHealth  = health;
    s_lastArmour  = armour;
    s_lastDamageMs = now;
    s_lastTickMs   = now;
}

/* CPlayerInfo::Process(int): hooked in ON_MOD_LOAD. Runs vanilla logic first,
 * then applies the delayed heal. Symbols/offsets verified against both
 * libGTASA.so 2.10 (arm64) and 2.00 (armv7) in the modded APKs. */
DECL_HOOK(void, CPlayerInfo__Process, CPlayerInfo* _this, int pad)
{
    CPlayerInfo__Process(_this, pad);

    if(!gTimeMs || !gCutscene || *gCutscene) return; /* scripted cutscenes: vanilla */
    if(pRestartDeath(_this) || pRestartArrest(_this)) /* wasted/busted: no regen */
    { s_lastHealth = -1.0f; return; }

    void* ped = pFindPlayerPed(-1);
    if(!ped || !pIsAlive(ped)) { s_lastHealth = -1.0f; return; }

    const uint32_t now = *gTimeMs;
    float health   = *(float*)((uintptr_t)ped + CPED_HEALTH);
    float maxHealth = *(float*)((uintptr_t)ped + CPED_MAXHEALTH);
    float armour   = *(float*)((uintptr_t)ped + CPED_ARMOUR);
    if(maxHealth <= 0.0f) return;

    /* First frame / post-respawn: seed the tracker only. */
    if(s_lastHealth < 0.0f) { ResetRegenState(health, armour, now); return; }

    /* Damage detection by comparing frames (any health/armour drop resets the delay). */
    if(health < s_lastHealth - 0.01f || armour < s_lastArmour - 0.01f)
        s_lastDamageMs = now;
    s_lastHealth = health;
    s_lastArmour = armour;

    /* Wanted stars gate: disable while pursued, force a clean delay after they drop. */
    if(!cfgWhileWanted->GetBool())
    {
        void* wanted = pFindPlayerWanted(-1);
        if(wanted && *(int*)((uintptr_t)wanted + CWANTED_LEVEL) > 0)
        { s_lastDamageMs = now; return; }
    }

    /* Regeneration windows (% of max health). Health and armour are fully
     * decoupled: armour uses its own window/delay/rate and does NOT require
     * the health to be inside [minH, capH]. */
    const float minH = maxHealth * (cfgMinHealth->GetFloat() / 100.0f);
    const float capH = maxHealth * (cfgCap->GetFloat() / 100.0f);

    /* Tick: computed BEFORE the gates so armour can advance independently of
     * the health window. Rate (HP/s) * dt, eased near each cap. */
    uint32_t dt = now - s_lastTickMs;
    if(dt > 500u) dt = 500u; /* clamp pauses / hitches */
    s_lastTickMs = now;
    const float dtSec = (float)dt / 1000.0f;

    /* --- Health: unchanged window/behaviour [minH, capH] on max health. --- */
    if(health > minH && health < capH)
    {
        const uint32_t delayMs = (uint32_t)(cfgDelay->GetFloat() * 1000.0f);
        if(now - s_lastDamageMs >= delayMs)
        {
            float progress = (health - minH) / (capH - minH);
            if(progress < 0.0f) progress = 0.0f;
            if(progress > 1.0f) progress = 1.0f;
            const float mult = 1.0f - 0.8f * progress; /* 1.0 near minH -> 0.2 near cap */

            float heal = cfgRate->GetFloat() * mult * dtSec;
            if(heal > 0.0f)
            {
                health += heal;
                if(health > capH) health = capH;
                *(float*)((uintptr_t)ped + CPED_HEALTH) = health;
                s_lastHealth = health;
            }
        }
    }

    /* --- Armour: independent window/delay/rate. --- */
    if(cfgArmorRegen->GetBool())
    {
        /* Own window: armour regenerates whenever it is below its cap
         * (armourMin = 0, so it never depends on the health window).
         * Armour maxes at 100 in SA, so clamp the cap to 100. */
        const float armourMin = 0.0f;
        float armourCap = 100.0f * (cfgArmorRegenCap->GetFloat() / 100.0f);
        if(armourCap > 100.0f) armourCap = 100.0f;

        if(armour > armourMin && armour < armourCap)
        {
            const uint32_t aDelayMs = (uint32_t)(cfgArmorRegenDelay->GetFloat() * 1000.0f);
            if(now - s_lastDamageMs >= aDelayMs)
            {
                /* Own easing: slow down as armour approaches its cap. */
                float armourProgress = armour / armourCap;
                if(armourProgress < 0.0f) armourProgress = 0.0f;
                if(armourProgress > 1.0f) armourProgress = 1.0f;
                const float multArmor = 1.0f - 0.8f * armourProgress;

                float aHeal = cfgArmorRegenRate->GetFloat() * multArmor * dtSec;
                if(aHeal > 0.0f)
                {
                    armour += aHeal;
                    if(armour > armourCap) armour = armourCap;
                    if(armour > 100.0f) armour = 100.0f; /* armor maxes at 100 in SA */
                    *(float*)((uintptr_t)ped + CPED_ARMOUR) = armour;
                    s_lastArmour = armour;
                }
            }
        }
    }
}

ON_MOD_LOAD()
{
    logger->SetTag("HealthRegen");

    if(!cfg || !cfg->IsInitialized()) { logger->Error("HealthRegen: config not initialised, mod disabled"); return; }

    /* The keys now carry their unit in the name so the .ini is unambiguous
     * (% of max health vs HP/s vs seconds). Old (<=1.2) names are still read
     * as aliases and migrate automatically, see below. */
    cfgDelay      = cfg->Bind("HealthRegenDelay", 5.0f, "Regen");  /* seconds */
    cfgRate       = cfg->Bind("HealthRegenRate", 2.0f, "Regen");   /* HP/s */
    cfgCap        = cfg->Bind("HealthRegenCapPct", 65.0f, "Regen");   /* % of max health */
    cfgMinHealth  = cfg->Bind("HealthRegenMinPct", 15.0f, "Regen");   /* % of max health */
    cfgWhileWanted = cfg->Bind("EnableWhileWanted", false, "Regen");
    cfgArmorRegen = cfg->Bind("EnableArmorRegen", false, "Regen");
    cfgArmorRegenRate  = cfg->Bind("ArmorRegenRate", 2.0f, "Regen");   /* HP/s */
    cfgArmorRegenCap   = cfg->Bind("ArmorRegenCapPct", 100.0f, "Regen"); /* % of armour */
    cfgArmorRegenDelay = cfg->Bind("ArmorRegenDelay", 5.0f, "Regen");    /* seconds */

    /* Legacy key migration (1.2 and older). If the .ini still contains an old
     * key name and the user did NOT set the new key, copy the old value into
     * the new entry (SetFloat marks it changed, so cfg->Save() persists it).
     *
     * Detection uses cfg->TryGetRaw(), a PURE READ (no side effects, no key
     * creation, no Save). We deliberately do NOT use cfg->HasKey() (AML 1.4.0+
     * vtable slot, UB on older AML) nor cfg->GetString() (which writes the key
     * back on a miss and would dirty the .ini). TryGetRaw only uses
     * m_pICFG->GetValueFrom(), part of ICFG v1.
     *
     * ICFG v1 has no key-deletion API, so the legacy key is left in the .ini;
     * it is harmless and is not migrated again because once propagated the new
     * entry's LoadedUndefault() becomes true, skipping this block. */
    {
        struct AliasFix { const char* oldKey; const char* newKey; ConfigEntry* entry; };
        const AliasFix aliases[] = {
            { "RegenDelay",       "HealthRegenDelay",   cfgDelay },
            { "RegenRate",        "HealthRegenRate",    cfgRate },
            { "RegenCap",         "HealthRegenCapPct",  cfgCap },
            { "MinHealthToRegen", "HealthRegenMinPct",  cfgMinHealth },
            { "ArmorRegenCap",    "ArmorRegenCapPct",   cfgArmorRegenCap },
        };
        for(const AliasFix& a : aliases)
        {
            if(!a.entry || a.entry->LoadedUndefault()) continue;

            const char* v = cfg->TryGetRaw(a.oldKey, "Regen");
            if(!v) continue; /* legacy key absent: nothing to migrate */

            char* end = NULL;
            const double parsed = strtod(v, &end);
            if(end == v || (end && *end != '\0'))
            {
                logger->Info("Ignoring legacy '%s' (unparsable value '%s')", a.oldKey, v);
                continue;
            }

            a.entry->SetFloat((float)parsed);
            logger->Info("Migrated legacy '%s' -> '%s' (=%.1f)", a.oldKey, a.newKey, (float)parsed);
        }
    }
    cfg->Save();

    pFindPlayerPed    = (FindPlayerPedFn)GetMainLibrarySymbol("_Z13FindPlayerPedi");
    pIsAlive          = (IsAliveFn)GetMainLibrarySymbol("_ZNK4CPed7IsAliveEv");
    pRestartDeath     = (RestartFn)GetMainLibrarySymbol("_ZN11CPlayerInfo22IsRestartingAfterDeathEv");
    pRestartArrest    = (RestartFn)GetMainLibrarySymbol("_ZN11CPlayerInfo23IsRestartingAfterArrestEv");
    pFindPlayerWanted = (FindPlayerWantedFn)GetMainLibrarySymbol("_Z16FindPlayerWantedi");
    gTimeMs   = (volatile uint32_t*)GetMainLibrarySymbol("_ZN6CTimer22m_snTimeInMillisecondsE");
    gCutscene = (volatile bool*)GetMainLibrarySymbol("_ZN12CCutsceneMgr10ms_runningE");

    if(!pFindPlayerPed || !pIsAlive || !pRestartDeath || !pRestartArrest ||
       !pFindPlayerWanted || !gTimeMs || !gCutscene)
    {
        logger->Error("HealthRegen: required symbols not found, mod disabled");
        return;
    }

    HOOK(CPlayerInfo__Process, GetMainLibrarySymbol("_ZN11CPlayerInfo7ProcessEi"));

    logger->Info("Auto Health Regen loaded [%s]. HealthDelay=%.1fs HealthRate=%.1f HP/s HealthCapPct=%.1f%% HealthMinPct=%.1f%% Wanted=%s Armor=%s",
        GAME_VARIANT, cfgDelay->GetFloat(), cfgRate->GetFloat(), cfgCap->GetFloat(),
        cfgMinHealth->GetFloat(),
        cfgWhileWanted->GetBool() ? "ON" : "OFF",
        cfgArmorRegen->GetBool() ? "ON" : "OFF");
    logger->Info("Armor regen: ArmorRate=%.1f HP/s ArmorCapPct=%.1f%% ArmorDelay=%.1fs",
        cfgArmorRegenRate->GetFloat(), cfgArmorRegenCap->GetFloat(), cfgArmorRegenDelay->GetFloat());
}