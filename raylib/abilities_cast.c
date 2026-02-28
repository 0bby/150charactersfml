#include "game.h"
#include "helpers.h"
#include <math.h>

//------------------------------------------------------------------------------------
// Ability-Specific Projectile Spawners
//------------------------------------------------------------------------------------
void SpawnChainFrostProjectile(Projectile projectiles[],
    Vector3 startPos, int targetIndex, int sourceIndex, Team sourceTeam, int level,
    float speed, float damage, int bounces, float bounceRange)
{
    for (int p = 0; p < MAX_PROJECTILES; p++) {
        if (!projectiles[p].active) {
            projectiles[p] = (Projectile){
                .type = PROJ_CHAIN_FROST,
                .position = (Vector3){ startPos.x, startPos.y + 3.0f, startPos.z },
                .targetIndex = targetIndex, .sourceIndex = sourceIndex, .sourceTeam = sourceTeam,
                .speed = speed, .damage = damage, .stunDuration = 0,
                .bouncesRemaining = bounces, .bounceRange = bounceRange, .lastHitUnit = -1,
                .level = level, .color = (Color){ 80, 140, 255, 255 }, .active = true,
            };
            return;
        }
    }
}

void SpawnHookProjectile(Projectile projectiles[], Vector3 startPos, int targetIndex,
    int sourceIndex, Team sourceTeam, int level, float speed, float dmgPerDist, float range)
{
    for (int p = 0; p < MAX_PROJECTILES; p++) {
        if (!projectiles[p].active) {
            projectiles[p] = (Projectile){
                .type = PROJ_HOOK,
                .position = (Vector3){ startPos.x, startPos.y + 3.0f, startPos.z },
                .targetIndex = targetIndex, .sourceIndex = sourceIndex, .sourceTeam = sourceTeam,
                .speed = speed, .damage = dmgPerDist, .stunDuration = 0,
                .bouncesRemaining = 0, .bounceRange = range, .lastHitUnit = -1,
                .level = level, .color = (Color){ 200, 60, 60, 255 }, .active = true,
            };
            return;
        }
    }
}

void SpawnMaelstromProjectile(Projectile projectiles[], Vector3 startPos, int targetIndex,
    int sourceIndex, Team sourceTeam, int level, float speed, float damage, int bounces, float bounceRange)
{
    for (int p = 0; p < MAX_PROJECTILES; p++) {
        if (!projectiles[p].active) {
            projectiles[p] = (Projectile){
                .type = PROJ_MAELSTROM,
                .position = (Vector3){ startPos.x, startPos.y + 3.0f, startPos.z },
                .targetIndex = targetIndex, .sourceIndex = sourceIndex, .sourceTeam = sourceTeam,
                .speed = speed, .damage = damage, .stunDuration = 0,
                .bouncesRemaining = bounces, .bounceRange = bounceRange, .lastHitUnit = -1,
                .level = level, .color = (Color){ 255, 230, 50, 255 }, .active = true,
            };
            return;
        }
    }
}

int FindChainFrostTarget(Unit units[], int unitCount, Vector3 fromPos,
    Team sourceTeam, int excludeIndex, float range)
{
    float bestDist = 1e30f;
    int bestIdx = -1;
    for (int j = 0; j < unitCount; j++) {
        if (!units[j].active || j == excludeIndex) continue;
        if (units[j].team == sourceTeam) continue;
        float d = sqrtf((units[j].position.x - fromPos.x) * (units[j].position.x - fromPos.x) +
                        (units[j].position.z - fromPos.z) * (units[j].position.z - fromPos.z));
        if (d <= range && d < bestDist) { bestDist = d; bestIdx = j; }
    }
    return bestIdx;
}

//------------------------------------------------------------------------------------
// Shared Combat Helpers
//------------------------------------------------------------------------------------
int FindHighestHPAlly(Unit units[], int unitCount, int selfIndex)
{
    Team myTeam = units[selfIndex].team;
    float bestHP = -1.0f;
    int bestIdx = -1;
    for (int j = 0; j < unitCount; j++) {
        if (j == selfIndex || !units[j].active) continue;
        if (units[j].team != myTeam) continue;
        if (units[j].currentHealth > bestHP) {
            bestHP = units[j].currentHealth;
            bestIdx = j;
        }
    }
    return bestIdx;
}

int FindFurthestEnemy(Unit units[], int unitCount, int selfIndex)
{
    Team myTeam = units[selfIndex].team;
    float bestDist = -1.0f;
    int bestIdx = -1;
    for (int j = 0; j < unitCount; j++) {
        if (j == selfIndex || !units[j].active) continue;
        if (units[j].team == myTeam) continue;
        float d = DistXZ(units[selfIndex].position, units[j].position);
        if (d > bestDist) {
            bestDist = d;
            bestIdx = j;
        }
    }
    return bestIdx;
}

int FindLowestHPAlly(Unit units[], int unitCount, int selfIndex)
{
    Team myTeam = units[selfIndex].team;
    float bestHP = 1e30f;
    int bestIdx = -1;
    for (int j = 0; j < unitCount; j++) {
        if (j == selfIndex || !units[j].active) continue;
        if (units[j].team != myTeam) continue;
        if (units[j].currentHealth < bestHP) {
            bestHP = units[j].currentHealth;
            bestIdx = j;
        }
    }
    return bestIdx;
}

//------------------------------------------------------------------------------------
// Ability Casting Handlers
//------------------------------------------------------------------------------------
bool CastMagicMissile(CombatState *state, int caster, AbilitySlot *slot, int target)
{
    if (target < 0) return false;
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_MAGIC_MISSILE];
    float d = DistXZ(state->units[caster].position, state->units[target].position);
    if (d > def->range[slot->level]) return false;
    SpawnProjectile(state->projectiles, PROJ_MAGIC_MISSILE,
        state->units[caster].position, target, caster, state->units[caster].team, slot->level,
        def->values[slot->level][AV_MM_PROJ_SPEED],
        def->values[slot->level][AV_MM_DAMAGE],
        def->values[slot->level][AV_MM_STUN_DUR],
        def->color);
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastVacuum(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_VACUUM];
    float radius = def->values[slot->level][AV_VAC_RADIUS];
    float stunDur = def->values[slot->level][AV_VAC_STUN_DUR];
    bool hitAny = false;
    for (int j = 0; j < state->unitCount; j++) {
        if (!state->units[j].active || state->units[j].team == state->units[caster].team) continue;
        if (UnitHasModifier(state->modifiers, j, MOD_INVULNERABLE)) continue;
        float d = DistXZ(state->units[caster].position, state->units[j].position);
        if (d <= radius) {
            state->units[j].position.x = state->units[caster].position.x;
            state->units[j].position.z = state->units[caster].position.z;
            AddModifier(state->modifiers, j, MOD_STUN, stunDur, 0);
            TriggerShake(state->shake, 5.0f, 0.25f);
            hitAny = true;
        }
    }
    if (!hitAny) return false;
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastChainFrost(CombatState *state, int caster, AbilitySlot *slot, int target)
{
    if (target < 0) return false;
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_CHAIN_FROST];
    float d = DistXZ(state->units[caster].position, state->units[target].position);
    if (d > def->range[slot->level]) return false;
    SpawnChainFrostProjectile(state->projectiles,
        state->units[caster].position, target, caster, state->units[caster].team, slot->level,
        def->values[slot->level][AV_CF_PROJ_SPEED],
        def->values[slot->level][AV_CF_DAMAGE],
        (int)def->values[slot->level][AV_CF_BOUNCES],
        def->values[slot->level][AV_CF_BOUNCE_RANGE]);
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastBloodRage(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_BLOOD_RAGE];
    float dur = def->values[slot->level][AV_BR_DURATION];
    float ls = def->values[slot->level][AV_BR_LIFESTEAL];
    AddModifier(state->modifiers, caster, MOD_LIFESTEAL, dur, ls);
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastEarthquake(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_EARTHQUAKE];
    float radius = def->values[slot->level][AV_EQ_RADIUS];
    float damage = def->values[slot->level][AV_EQ_DAMAGE];
    for (int j = 0; j < state->unitCount; j++) {
        if (j == caster || !state->units[j].active) continue;
        if (UnitHasModifier(state->modifiers, j, MOD_INVULNERABLE)) continue;
        float d = DistXZ(state->units[caster].position, state->units[j].position);
        if (d <= radius) {
            state->units[j].currentHealth -= damage;
            if (state->units[j].currentHealth <= 0) state->units[j].active = false;
        }
    }
    TriggerShake(state->shake, 10.0f, 0.5f);
    // Spawn earth particles
    for (int p = 0; p < 20; p++) {
        float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
        float r = (float)GetRandomValue(0, (int)(radius * 10.0f)) / 10.0f;
        Vector3 pos = {
            state->units[caster].position.x + cosf(angle) * r,
            0.5f,
            state->units[caster].position.z + sinf(angle) * r
        };
        Vector3 vel = { cosf(angle) * 5.0f, (float)GetRandomValue(30, 80) / 10.0f, sinf(angle) * 5.0f };
        int shade = GetRandomValue(80, 160);
        Color brown = { (unsigned char)shade, (unsigned char)(shade * 0.7f), (unsigned char)(shade * 0.3f), 255 };
        SpawnParticle(state->particles, pos, vel, 0.6f, (float)GetRandomValue(4, 10) / 10.0f, brown);
    }
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastSpellProtect(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_SPELL_PROTECT];
    float dur = def->values[slot->level][AV_SP_DURATION];
    AddModifier(state->modifiers, caster, MOD_SPELL_PROTECT, dur, 0);
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastCraggyArmor(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_CRAGGY_ARMOR];
    float armor = def->values[slot->level][AV_CA_ARMOR];
    float stunChance = def->values[slot->level][AV_CA_STUN_CHANCE];
    float dur = def->values[slot->level][AV_CA_DURATION];
    AddModifier(state->modifiers, caster, MOD_ARMOR, dur, armor);
    AddModifier(state->modifiers, caster, MOD_CRAGGY_ARMOR, dur, stunChance);
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastStoneGaze(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_STONE_GAZE];
    float dur = def->values[slot->level][AV_SG_DURATION];
    float gazeThresh = def->values[slot->level][AV_SG_GAZE_THRESH];
    AddModifier(state->modifiers, caster, MOD_STONE_GAZE, dur, gazeThresh);
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastFissure(CombatState *state, int caster, AbilitySlot *slot, int target)
{
    if (target < 0) return false;
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_FISSURE];
    float d = DistXZ(state->units[caster].position, state->units[target].position);
    if (d > def->values[slot->level][AV_FI_RANGE]) return false;

    float length = def->values[slot->level][AV_FI_LENGTH];
    float width = def->values[slot->level][AV_FI_WIDTH];
    float duration = def->values[slot->level][AV_FI_DURATION];
    float damage = def->values[slot->level][AV_FI_DAMAGE];

    SpawnFissure(state->fissures, state->units[caster].position,
        state->units[target].position, length, width, duration,
        state->units[caster].team, caster);

    // Deal damage in area around fissure on spawn
    float dx = state->units[target].position.x - state->units[caster].position.x;
    float dz = state->units[target].position.z - state->units[caster].position.z;
    float dist = sqrtf(dx * dx + dz * dz);
    float norm = (dist > 0.001f) ? 1.0f / dist : 0.0f;
    for (int j = 0; j < state->unitCount; j++) {
        if (j == caster || !state->units[j].active) continue;
        if (UnitHasModifier(state->modifiers, j, MOD_INVULNERABLE)) continue;
        float ux = state->units[j].position.x - state->units[caster].position.x;
        float uz = state->units[j].position.z - state->units[caster].position.z;
        float proj = (ux * dx + uz * dz) * norm * norm;
        if (proj < 0 || proj > length) continue;
        float perpX = ux - dx * norm * proj;
        float perpZ = uz - dz * norm * proj;
        float perpDist = sqrtf(perpX * perpX + perpZ * perpZ);
        if (perpDist <= width + 3.0f) {
            state->units[j].currentHealth -= damage;
            if (state->units[j].currentHealth <= 0) state->units[j].active = false;
        }
    }
    TriggerShake(state->shake, 6.0f, 0.3f);
    slot->cooldownRemaining = def->cooldown[slot->level];
    return true;
}

bool CastVladAura(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_VLAD_AURA];
    int lvl = slot->level;
    float ls = def->values[lvl][AV_VA_LIFESTEAL];
    float dur = def->values[lvl][AV_VA_DURATION];
    for (int j = 0; j < state->unitCount; j++) {
        if (!state->units[j].active) continue;
        if (state->units[j].team != state->units[caster].team) continue;
        AddModifier(state->modifiers, j, MOD_LIFESTEAL, dur, ls);
    }
    AddModifier(state->modifiers, caster, MOD_VLAD_AURA, dur, ls);
    slot->cooldownRemaining = def->cooldown[lvl];
    return true;
}

bool CastMaelstrom(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_MAELSTROM];
    int lvl = slot->level;
    float procChance = def->values[lvl][AV_ML_PROC_CHANCE];
    float dur = def->values[lvl][AV_ML_DURATION];
    AddModifier(state->modifiers, caster, MOD_MAELSTROM, dur, procChance);
    slot->cooldownRemaining = def->cooldown[lvl];
    return true;
}

bool CastSwap(CombatState *state, int caster, AbilitySlot *slot)
{
    int target = FindFurthestEnemy(state->units, state->unitCount, caster);
    if (target < 0) return false;
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_SWAP];
    int lvl = slot->level;
    float tmpX = state->units[caster].position.x;
    float tmpZ = state->units[caster].position.z;
    state->units[caster].position.x = state->units[target].position.x;
    state->units[caster].position.z = state->units[target].position.z;
    state->units[target].position.x = tmpX;
    state->units[target].position.z = tmpZ;
    float shieldHP = def->values[lvl][AV_SW_SHIELD];
    float shieldDur = def->values[lvl][AV_SW_SHIELD_DUR];
    state->units[caster].shieldHP = shieldHP;
    AddModifier(state->modifiers, caster, MOD_SHIELD, shieldDur, shieldHP);
    TriggerShake(state->shake, 4.0f, 0.2f);
    slot->cooldownRemaining = def->cooldown[lvl];
    return true;
}

bool CastAphoticShield(CombatState *state, int caster, AbilitySlot *slot)
{
    int ally = FindLowestHPAlly(state->units, state->unitCount, caster);
    if (ally < 0) ally = caster;
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_APHOTIC_SHIELD];
    int lvl = slot->level;
    for (int m = 0; m < MAX_MODIFIERS; m++) {
        if (!state->modifiers[m].active || state->modifiers[m].unitIndex != ally) continue;
        if (state->modifiers[m].type == MOD_STUN || state->modifiers[m].type == MOD_STONE_GAZE) {
            state->modifiers[m].active = false;
        }
    }
    float shieldHP = def->values[lvl][AV_AS_SHIELD];
    float dur = def->values[lvl][AV_AS_DURATION];
    state->units[ally].shieldHP = shieldHP;
    AddModifier(state->modifiers, ally, MOD_SHIELD, dur, shieldHP);
    slot->cooldownRemaining = def->cooldown[lvl];
    return true;
}

bool CastHook(CombatState *state, int caster, AbilitySlot *slot)
{
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_HOOK];
    int lvl = slot->level;
    float range = def->values[lvl][AV_HK_RANGE];
    int target = FindFurthestEnemy(state->units, state->unitCount, caster);
    if (target < 0) return false;
    float d = DistXZ(state->units[caster].position, state->units[target].position);
    if (d > range) {
        target = FindClosestEnemy(state->units, state->unitCount, caster);
        if (target < 0) return false;
        d = DistXZ(state->units[caster].position, state->units[target].position);
        if (d > range) return false;
    }
    float speed = def->values[lvl][AV_HK_SPEED];
    float dmgPerDist = def->values[lvl][AV_HK_DMG_PER_DIST];
    SpawnHookProjectile(state->projectiles, state->units[caster].position,
        target, caster, state->units[caster].team, lvl, speed, dmgPerDist, range);
    slot->cooldownRemaining = def->cooldown[lvl];
    return true;
}

bool CastPrimalCharge(CombatState *state, int caster, AbilitySlot *slot)
{
    int target = FindFurthestEnemy(state->units, state->unitCount, caster);
    if (target < 0) return false;
    const AbilityDef *def = &ABILITY_DEFS[ABILITY_PRIMAL_CHARGE];
    int lvl = slot->level;
    float chargeSpeed = def->values[lvl][AV_PC_CHARGE_SPEED];
    state->units[caster].chargeTarget = target;
    AddModifier(state->modifiers, caster, MOD_CHARGING, 10.0f, chargeSpeed);
    slot->cooldownRemaining = def->cooldown[lvl];
    return true;
}

//------------------------------------------------------------------------------------
// Passive Ability Checks
//------------------------------------------------------------------------------------
void CheckPassiveSunder(CombatState *state, int unitIndex)
{
    Unit *unit = &state->units[unitIndex];
    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
        AbilitySlot *slot = &unit->abilities[a];
        if (slot->abilityId != ABILITY_SUNDER) continue;
        if (slot->triggered || slot->cooldownRemaining > 0) continue;
        const AbilityDef *def = &ABILITY_DEFS[ABILITY_SUNDER];
        float threshold = def->values[slot->level][AV_SU_HP_THRESH];
        float maxHP = UNIT_STATS[unit->typeIndex].health;
        if (unit->currentHealth > 0 && unit->currentHealth <= maxHP * threshold) {
            int ally = FindHighestHPAlly(state->units, state->unitCount, unitIndex);
            if (ally >= 0) {
                float myHP = unit->currentHealth;
                float allyHP = state->units[ally].currentHealth;
                unit->currentHealth = allyHP;
                state->units[ally].currentHealth = myHP;
                float allyMax = UNIT_STATS[state->units[ally].typeIndex].health;
                if (unit->currentHealth > maxHP) unit->currentHealth = maxHP;
                if (state->units[ally].currentHealth > allyMax) state->units[ally].currentHealth = allyMax;
                slot->triggered = true;
                slot->cooldownRemaining = def->cooldown[slot->level];
                SpawnFloatingText(state->floatingTexts, unit->position,
                    def->name, def->color, 1.0f);
            }
        }
    }
}

//------------------------------------------------------------------------------------
// On-Hit Checks
//------------------------------------------------------------------------------------
void CheckCraggyArmorRetaliation(CombatState *state, int attacker, int defender)
{
    if (!UnitHasModifier(state->modifiers, defender, MOD_CRAGGY_ARMOR)) return;
    float stunChance = GetModifierValue(state->modifiers, defender, MOD_CRAGGY_ARMOR);
    float roll = (float)GetRandomValue(0, 100) / 100.0f;
    if (roll < stunChance) {
        float stunDur = 1.0f;
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
            if (state->units[defender].abilities[a].abilityId == ABILITY_CRAGGY_ARMOR) {
                int lvl = state->units[defender].abilities[a].level;
                stunDur = ABILITY_DEFS[ABILITY_CRAGGY_ARMOR].values[lvl][AV_CA_STUN_DUR];
                break;
            }
        }
        AddModifier(state->modifiers, attacker, MOD_STUN, stunDur, 0);
        TriggerShake(state->shake, 3.0f, 0.15f);
    }
}
