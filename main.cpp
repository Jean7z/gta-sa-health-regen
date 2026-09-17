#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

#include <aml-psdk/game_sa/plugin.h>

#include <cstdint>

MYMODCFG(net.psdk.samod.regen, SA Android Auto Health Regeneration, 1.1, Jean7z)

static ConfigEntry *cfgDelay, *cfgRate, *cfgCap, *cfgMinHealth, *cfgWhileWanted, *cfgArmorRegen;

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

    /* Regeneration window (% of max health). */
    const float minH = maxHealth * (cfgMinHealth->GetFloat() / 100.0f);
    const float capH = maxHealth * (cfgCap->GetFloat() / 100.0f);
    if(health <= minH || health >= capH) return;

    /* Delay since last damage. */
    const uint32_t delayMs = (uint32_t)(cfgDelay->GetFloat() * 1000.0f);
    if(now - s_lastDamageMs < delayMs) return;

    /* Tick: rate (HP/s) * dt, eased near the cap. */
    uint32_t dt = now - s_lastTickMs;
    if(dt > 500u) dt = 500u; /* clamp pauses / hitches */
    s_lastTickMs = now;

    float progress = (health - minH) / (capH - minH);
    if(progress < 0.0f) progress = 0.0f;
    if(progress > 1.0f) progress = 1.0f;
    const float mult = 1.0f - 0.8f * progress; /* 1.0 near minH -> 0.2 near cap */

    const float dtSec = (float)dt / 1000.0f;
    float heal = cfgRate->GetFloat() * mult * dtSec;
    if(heal > 0.0f)
    {
        health += heal;
        if(health > capH) health = capH;
        *(float*)((uintptr_t)ped + CPED_HEALTH) = health;
        s_lastHealth = health;
    }

    if(cfgArmorRegen->GetBool())
    {
        float aHeal = cfgRate->GetFloat() * mult * dtSec;
        if(aHeal > 0.0f)
        {
            armour += aHeal;
            if(armour > 100.0f) armour = 100.0f; /* armor maxes at 100 in SA */
            *(float*)((uintptr_t)ped + CPED_ARMOUR) = armour;
            s_lastArmour = armour;
        }
    }
}

ON_MOD_LOAD()
{
    logger->SetTag("HealthRegen");

    cfgDelay      = cfg->Bind("RegenDelay", 5.0f, "Regen");
    cfgRate       = cfg->Bind("RegenRate", 2.0f, "Regen");
    cfgCap        = cfg->Bind("RegenCap", 65.0f, "Regen");
    cfgMinHealth  = cfg->Bind("MinHealthToRegen", 15.0f, "Regen");
    cfgWhileWanted = cfg->Bind("EnableWhileWanted", false, "Regen");
    cfgArmorRegen = cfg->Bind("EnableArmorRegen", false, "Regen");

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

    logger->Info("Auto Health Regen loaded [%s]. Delay=%.1fs Rate=%.1f HP/s Cap=%d%% Min=%d%% Wanted=%s Armor=%s",
        GAME_VARIANT, cfgDelay->GetFloat(), cfgRate->GetFloat(), (int)cfgCap->GetFloat(),
        (int)cfgMinHealth->GetFloat(),
        cfgWhileWanted->GetBool() ? "ON" : "OFF",
        cfgArmorRegen->GetBool() ? "ON" : "OFF");
}