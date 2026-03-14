#include "combat_sim.h"
#include <math.h>
#include <string.h>

#ifndef PI
#define PI 3.14159265358979323846f
#endif

// Seeded xorshift32 PRNG for deterministic combat
static uint32_t combatRngState = 1;

void combat_rng_seed(uint32_t seed) { combatRngState = seed ? seed : 1; }

float combat_rng_next(void) {
    combatRngState ^= combatRngState << 13;
    combatRngState ^= combatRngState >> 17;
    combatRngState ^= combatRngState << 5;
    return (float)(combatRngState & 0xFFFF) / 65535.0f;
}

// Damage pipeline flags
#define DMG_SINGLE_TARGET (1 << 0)  // apply Share Pain (melee + single-target projectiles)

// Forward-declare ApplySharePain (used by ApplyDamage)
static float ApplySharePain(Unit units[], int unitCount, Modifier modifiers[], int target, float dmg);

// Unified damage pipeline: invuln → armor → share pain → shield → HP reduce → mushroom → death
// Returns actual damage dealt after all reductions.
static float ApplyDamage(Unit units[], int *unitCount, Modifier modifiers[],
                         int target, float rawDmg, int flags)
{
    if (UnitHasModifier(modifiers, target, MOD_INVULNERABLE)) return 0;
    // Armor reduction
    float armor = GetModifierValue(modifiers, target, MOD_ARMOR);
    float dmg = rawDmg - armor;
    if (dmg < 0) dmg = 0;
    // Share Pain (single-target only)
    if (flags & DMG_SINGLE_TARGET)
        dmg = ApplySharePain(units, *unitCount, modifiers, target, dmg);
    // Shield absorption
    if (units[target].shieldHP > 0) {
        if (dmg <= units[target].shieldHP) { units[target].shieldHP -= dmg; dmg = 0; }
        else { dmg -= units[target].shieldHP; units[target].shieldHP = 0; }
    }
    // HP reduction
    units[target].currentHealth -= dmg;
    CheckMushroomSpawn(units, unitCount, target, dmg);
    // Death check
    if (units[target].currentHealth <= 0) units[target].active = false;
    return dmg;
}

// Share Pain: if target has MOD_SHARE_PAIN, redistribute a portion of damage to nearby allies.
// Only applies to single-target damage (melee, projectile hits), NOT AoE.
// Returns the damage the original target should take (reduced by shared amount).
static float ApplySharePain(Unit units[], int unitCount, Modifier modifiers[], int target, float dmg)
{
    static bool inSharePain = false; // recursion guard
    if (inSharePain) return dmg;
    if (!UnitHasModifier(modifiers, target, MOD_SHARE_PAIN)) return dmg;

    float sharePct = GetModifierValue(modifiers, target, MOD_SHARE_PAIN);
    // Look up radius from equipped ability level
    int spLvl = GetUnitAbilityLevel(units, target, ABILITY_SHARE_PAIN);
    float radius = (spLvl >= 0) ? ABILITY_DEFS[ABILITY_SHARE_PAIN].values[spLvl][AV_SPP_RADIUS] : 30.0f;

    // Find nearby allies
    int allies[MAX_UNITS];
    int allyCount = 0;
    for (int j = 0; j < unitCount; j++) {
        if (j == target || !units[j].active || units[j].team != units[target].team) continue;
        if (DistXZ(units[target].position, units[j].position) <= radius)
            allies[allyCount++] = j;
    }
    if (allyCount == 0) return dmg;

    float sharedDmg = dmg * sharePct;
    float selfDmg = dmg - sharedDmg;
    float perAlly = sharedDmg / (float)allyCount;

    inSharePain = true;
    for (int a = 0; a < allyCount; a++) {
        int ai = allies[a];
        if (units[ai].shieldHP > 0) {
            if (perAlly <= units[ai].shieldHP) { units[ai].shieldHP -= perAlly; }
            else { float rem = perAlly - units[ai].shieldHP; units[ai].shieldHP = 0; units[ai].currentHealth -= rem; }
        } else {
            units[ai].currentHealth -= perAlly;
        }
        if (units[ai].currentHealth <= 0) units[ai].active = false;
    }
    inSharePain = false;

    return selfDmg;
}

static void EmitEvent(CombatEvent events[], int *eventCount, CombatEventType type,
                      int unitIndex, int abilityId, Vector3 position, float v1, float v2)
{
    if (!events || !eventCount) return;
    if (*eventCount >= MAX_COMBAT_EVENTS) return;
    events[*eventCount] = (CombatEvent){
        .type = type, .unitIndex = unitIndex, .abilityId = abilityId,
        .position = position, .value1 = v1, .value2 = v2
    };
    (*eventCount)++;
}

int CombatTick(Unit units[], int *unitCountPtr,
               Modifier modifiers[],
               Projectile projectiles[],
               Fissure fissures[],
               float dt,
               CombatEvent events[], int *eventCount)
{
    int unitCount = *unitCountPtr;
    if (eventCount) *eventCount = 0;

    // === STEP 1: Tick modifiers ===
    for (int m = 0; m < MAX_MODIFIERS; m++) {
        if (!modifiers[m].active) continue;
        int ui = modifiers[m].unitIndex;
        if (ui < 0 || ui >= unitCount || !units[ui].active) {
            modifiers[m].active = false; continue;
        }
        if (modifiers[m].duration > 0) {
            modifiers[m].duration -= dt;
            if (modifiers[m].duration <= 0) {
                if (modifiers[m].type == MOD_SHIELD) units[ui].shieldHP = 0;
                modifiers[m].active = false; continue;
            }
        }
        // Per-tick effects
        if (modifiers[m].type == MOD_DIG_HEAL) {
            float maxHP = UNIT_STATS[units[ui].typeIndex].health * units[ui].hpMultiplier;
            units[ui].currentHealth += modifiers[m].value * dt;
            if (units[ui].currentHealth > maxHP) units[ui].currentHealth = maxHP;
        }
        // Rejuvenate HOT
        if (modifiers[m].type == MOD_REJUVENATE) {
            float maxHP = UNIT_STATS[units[ui].typeIndex].health * units[ui].hpMultiplier;
            units[ui].currentHealth += modifiers[m].value * dt;
            if (units[ui].currentHealth > maxHP) units[ui].currentHealth = maxHP;
        }
        // Poison DOT — bypasses armor/shield
        if (modifiers[m].type == MOD_POISON && units[ui].active) {
            float poisonDmg = modifiers[m].value * dt;
            units[ui].currentHealth -= poisonDmg;
            if (units[ui].currentHealth <= 0) units[ui].active = false;
        }
    }

    // === STEP 1b: Tick fissures ===
    if (fissures) UpdateFissures(fissures, dt);

    // === STEP 2: Update projectiles ===
    for (int p = 0; p < MAX_PROJECTILES; p++) {
        if (!projectiles[p].active) continue;
        // Charge-up phase: stay in place and grow
        if (projectiles[p].chargeTimer > 0) {
            projectiles[p].chargeTimer -= dt;
            if (projectiles[p].chargeTimer > 0) continue;
        }
        int ti = projectiles[p].targetIndex;
        // Target gone?
        if (ti < 0 || ti >= unitCount || !units[ti].active) {
            if ((projectiles[p].type == PROJ_CHAIN_FROST || projectiles[p].type == PROJ_MAELSTROM) && projectiles[p].bouncesRemaining > 0) {
                int next = FindChainFrostTarget(units, unitCount, projectiles[p].position,
                    projectiles[p].sourceTeam, projectiles[p].lastHitUnit, projectiles[p].bounceRange);
                if (next >= 0) { projectiles[p].targetIndex = next; continue; }
            }
            projectiles[p].active = false; continue;
        }
        // Move toward target
        Vector3 tgt = { units[ti].position.x, units[ti].position.y + 3.0f, units[ti].position.z };
        float pdx = tgt.x - projectiles[p].position.x;
        float pdy = tgt.y - projectiles[p].position.y;
        float pdz = tgt.z - projectiles[p].position.z;
        float pdist = sqrtf(pdx*pdx + pdy*pdy + pdz*pdz);
        float pstep = projectiles[p].speed * dt;

        if (pdist <= pstep) {
            // HIT — Hook: damage by distance, then pull target to caster
            if (projectiles[p].type == PROJ_HOOK) {
                float hookDist = DistXZ(units[ti].position, units[projectiles[p].sourceIndex].position);
                float rawDmg = projectiles[p].baseDmg + hookDist * projectiles[p].damage;
                float hitDmg = ApplyDamage(units, &unitCount, modifiers, ti, rawDmg, DMG_SINGLE_TARGET);
                if (hitDmg > 0 && units[ti].active) {
                    // Start pulling target to caster
                    units[ti].hookPullDest = units[projectiles[p].sourceIndex].position;
                    units[ti].hookPullSpeed = projectiles[p].speed;
                    AddModifier(modifiers, ti, MOD_STUN, 10.0f, 0); // stun during pull
                }
                EmitEvent(events, eventCount, COMBAT_EVT_PROJECTILE_HIT, ti,
                          projectiles[p].type, projectiles[p].position, 0, 0);
                if (hitDmg > 0 || rawDmg > 0)
                    EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, ti, -1,
                              units[ti].position, 6.0f, 0.3f);
                projectiles[p].active = false;
            }
            // HIT — Maelstrom: bounce like chain frost (now with Share Pain)
            else if (projectiles[p].type == PROJ_MAELSTROM) {
                ApplyDamage(units, &unitCount, modifiers, ti, projectiles[p].damage, DMG_SINGLE_TARGET);
                EmitEvent(events, eventCount, COMBAT_EVT_PROJECTILE_HIT, ti,
                          projectiles[p].type, projectiles[p].position, 0, 0);
                if (projectiles[p].bouncesRemaining > 0) {
                    projectiles[p].bouncesRemaining--;
                    projectiles[p].lastHitUnit = ti;
                    projectiles[p].position = units[ti].position;
                    projectiles[p].position.y += 3.0f;
                    int next = FindChainFrostTarget(units, unitCount, units[ti].position,
                        projectiles[p].sourceTeam, ti, projectiles[p].bounceRange);
                    if (next >= 0) projectiles[p].targetIndex = next;
                    else projectiles[p].active = false;
                } else {
                    projectiles[p].active = false;
                }
            }
            // HIT — Devil Bolt: flat damage ranged auto-attack
            else if (projectiles[p].type == PROJ_DEVIL_BOLT) {
                int si = projectiles[p].sourceIndex;
                float hitDmg = ApplyDamage(units, &unitCount, modifiers, ti, projectiles[p].damage, DMG_SINGLE_TARGET);
                // Lifesteal from devil bolt
                if (hitDmg > 0 && si >= 0 && si < unitCount && units[si].active) {
                    float ls = GetModifierValue(modifiers, si, MOD_LIFESTEAL);
                    if (ls > 0) {
                        float maxHP = UNIT_STATS[units[si].typeIndex].health * units[si].hpMultiplier;
                        units[si].currentHealth += hitDmg * ls;
                        if (units[si].currentHealth > maxHP) units[si].currentHealth = maxHP;
                    }
                }
                // Venom Strike on-hit: apply/refresh poison from devil bolt
                if (hitDmg > 0 && si >= 0 && si < unitCount && units[si].active) {
                    int vsLvl = GetUnitAbilityLevel(units, si, ABILITY_VENOM_STRIKE);
                    if (vsLvl >= 0) {
                        float poisonDPS = ABILITY_DEFS[ABILITY_VENOM_STRIKE].values[vsLvl][AV_VS_POISON_DPS];
                        float poisonDur = ABILITY_DEFS[ABILITY_VENOM_STRIKE].values[vsLvl][AV_VS_DURATION];
                        AddModifier(modifiers, ti, MOD_POISON, poisonDur, poisonDPS);
                    }
                }
                EmitEvent(events, eventCount, COMBAT_EVT_PROJECTILE_HIT, ti,
                          projectiles[p].type, projectiles[p].position, 0, 0);
                projectiles[p].active = false;
            }
            // HIT — normal (Magic Missile / Chain Frost)
            else {
                float rawDmg = projectiles[p].damage;
                if (projectiles[p].type == PROJ_MAGIC_MISSILE)
                    rawDmg *= UNIT_STATS[units[ti].typeIndex].health * units[ti].hpMultiplier;
                ApplyDamage(units, &unitCount, modifiers, ti, rawDmg, DMG_SINGLE_TARGET);
                EmitEvent(events, eventCount, COMBAT_EVT_PROJECTILE_HIT, ti,
                          projectiles[p].type, projectiles[p].position, 0, 0);
                if (projectiles[p].stunDuration > 0) {
                    AddModifier(modifiers, ti, MOD_STUN, projectiles[p].stunDuration, 0);
                    EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, ti, -1,
                              units[ti].position, 5.0f, 0.25f);
                }
                // Chain Frost bounce
                if (projectiles[p].type == PROJ_CHAIN_FROST && projectiles[p].bouncesRemaining > 0) {
                    projectiles[p].bouncesRemaining--;
                    projectiles[p].damage += projectiles[p].damageIncrease;
                    projectiles[p].lastHitUnit = ti;
                    projectiles[p].position = units[ti].position;
                    projectiles[p].position.y += 3.0f;
                    int next = FindChainFrostTarget(units, unitCount, units[ti].position,
                        projectiles[p].sourceTeam, ti, projectiles[p].bounceRange);
                    if (next >= 0) projectiles[p].targetIndex = next;
                    else projectiles[p].active = false;
                } else {
                    projectiles[p].active = false;
                }
            }
        } else {
            projectiles[p].position.x += (pdx / pdist) * pstep;
            projectiles[p].position.y += (pdy / pdist) * pstep;
            projectiles[p].position.z += (pdz / pdist) * pstep;
        }
    }

    // === STEP 3: Process each unit ===
    for (int i = 0; i < unitCount; i++)
    {
        if (!units[i].active) continue;
        const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];
        float unitMaxHP = stats->health * units[i].hpMultiplier;
        bool stunned = UnitHasModifier(modifiers, i, MOD_STUN);

        // Tick ability cooldowns (CDR makes cooldowns tick faster)
        {
            float cdr = GetModifierValue(modifiers, i, MOD_COOLDOWN_REDUCTION);
            float effectiveDt = dt * (1.0f + cdr);
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                if (units[i].abilities[a].abilityId < 0) continue;
                if (units[i].abilities[a].cooldownRemaining > 0)
                    units[i].abilities[a].cooldownRemaining -= effectiveDt;
            }
        }

        // Passive triggers (Dig, Sunder) — blocked by stun
        if (!stunned) {
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                AbilitySlot *slot = &units[i].abilities[a];
                if (slot->abilityId == ABILITY_DIG) {
                    if (slot->triggered || slot->cooldownRemaining > 0) continue;
                    const AbilityDef *def = &ABILITY_DEFS[ABILITY_DIG];
                    float threshold = def->values[slot->level][AV_DIG_HP_THRESH];
                    if (units[i].currentHealth > 0 && units[i].currentHealth <= unitMaxHP * threshold) {
                        slot->triggered = true;
                        slot->cooldownRemaining = def->cooldown[slot->level];
                        float healDur = def->values[slot->level][AV_DIG_HEAL_DUR];
                        float healPerSec = unitMaxHP / healDur;
                        AddModifier(modifiers, i, MOD_INVULNERABLE, healDur, 0);
                        AddModifier(modifiers, i, MOD_DIG_HEAL, healDur, healPerSec);
                    }
                } else if (slot->abilityId == ABILITY_FERVOR) {
                    // Apply fervor modifier once (long duration, starts at 0 stacks)
                    if (!slot->triggered) {
                        slot->triggered = true;
                        AddModifier(modifiers, i, MOD_FERVOR, 99999.0f, 0.0f);
                    }
                } else if (slot->abilityId == ABILITY_SUNDER) {
                    if (slot->triggered || slot->cooldownRemaining > 0) continue;
                    const AbilityDef *def = &ABILITY_DEFS[ABILITY_SUNDER];
                    float threshold = def->values[slot->level][AV_SU_HP_THRESH];
                    if (units[i].currentHealth > 0 && units[i].currentHealth <= unitMaxHP * threshold) {
                        int enemy = FindHighestHPEnemy(units, unitCount, i);
                        if (enemy >= 0 && units[enemy].currentHealth > units[i].currentHealth) {
                            float myHP = units[i].currentHealth;
                            float enemyHP = units[enemy].currentHealth;
                            units[i].currentHealth = enemyHP;
                            units[enemy].currentHealth = myHP;
                            float enemyMax = UNIT_STATS[units[enemy].typeIndex].health * units[enemy].hpMultiplier;
                            if (units[i].currentHealth > unitMaxHP) units[i].currentHealth = unitMaxHP;
                            if (units[enemy].currentHealth > enemyMax) units[enemy].currentHealth = enemyMax;
                            slot->triggered = true;
                            slot->cooldownRemaining = def->cooldown[slot->level];
                            EmitEvent(events, eventCount, COMBAT_EVT_ABILITY_CAST, i,
                                      ABILITY_SUNDER, units[i].position, 0, 0);
                        }
                    }
                }
            }
        }

        // Hook pull movement — drag unit toward hook destination
        if (units[i].hookPullSpeed > 0) {
            float hdx = units[i].hookPullDest.x - units[i].position.x;
            float hdz = units[i].hookPullDest.z - units[i].position.z;
            float hlen = sqrtf(hdx*hdx + hdz*hdz);
            float hstep = units[i].hookPullSpeed * dt;
            if (hlen <= hstep) {
                // Arrived at destination
                units[i].position.x = units[i].hookPullDest.x;
                units[i].position.z = units[i].hookPullDest.z;
                units[i].hookPullSpeed = 0;
                EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                          units[i].position, 6.0f, 0.3f);
                // Remove the pull stun
                for (int m = 0; m < MAX_MODIFIERS; m++) {
                    if (modifiers[m].active && modifiers[m].unitIndex == i && modifiers[m].type == MOD_STUN)
                        modifiers[m].active = false;
                }
            } else {
                units[i].position.x += (hdx/hlen) * hstep;
                units[i].position.z += (hdz/hlen) * hstep;
            }
            continue; // skip normal movement while being pulled
        }

        bool digging = UnitHasModifier(modifiers, i, MOD_DIG_HEAL);
        if (stunned || digging) continue;

        // Find target
        int target = FindClosestEnemy(units, unitCount, i);
        units[i].targetIndex = target;

        // Smooth rotation towards target
        if (target >= 0 && units[target].active) {
            float dx = units[target].position.x - units[i].position.x;
            float dz = units[target].position.z - units[i].position.z;
            float goalAngle = atan2f(dx, dz) * (180.0f / PI);
            float diff = goalAngle - units[i].facingAngle;
            while (diff > 180.0f) diff -= 360.0f;
            while (diff < -180.0f) diff += 360.0f;
            float turnSpeed = 360.0f;
            if (fabsf(diff) < turnSpeed * dt)
                units[i].facingAngle = goalAngle;
            else
                units[i].facingAngle += (diff > 0 ? 1.0f : -1.0f) * turnSpeed * dt;
        }

        // Tick ability cast delay
        if (units[i].abilityCastDelay > 0)
            units[i].abilityCastDelay -= dt;

        // Active ability casting — one per frame, clockwise rotation
        bool castThisFrame = false;
        if (units[i].abilityCastDelay <= 0)
        for (int attempt = 0; attempt < MAX_ABILITIES_PER_UNIT && !castThisFrame; attempt++) {
            int slotIdx = ACTIVATION_ORDER[units[i].nextAbilitySlot];
            units[i].nextAbilitySlot = (units[i].nextAbilitySlot + 1) % MAX_ABILITIES_PER_UNIT;

            AbilitySlot *slot = &units[i].abilities[slotIdx];
            if (slot->abilityId < 0 || slot->cooldownRemaining > 0) continue;

            const AbilityDef *def = &ABILITY_DEFS[slot->abilityId];
            if (def->isPassive) continue; // skip passives (Dig, Sunder)

            // Range gate for targeted abilities
            float castRange = def->range[slot->level];
            if (castRange > 0 && target >= 0) {
                float d = DistXZ(units[i].position, units[target].position);
                if (d > castRange) continue;
            } else if (castRange > 0 && target < 0) {
                continue;
            }

            switch (slot->abilityId) {
            case ABILITY_MAGIC_MISSILE: {
                if (target < 0) break;
                SpawnProjectile(projectiles, PROJ_MAGIC_MISSILE,
                    units[i].position, target, i, units[i].team, slot->level,
                    def->values[slot->level][AV_MM_PROJ_SPEED],
                    def->values[slot->level][AV_MM_DAMAGE],
                    def->values[slot->level][AV_MM_STUN_DUR],
                    (Color){120, 80, 255, 255});
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_VACUUM: {
                float radius = def->values[slot->level][AV_VAC_RADIUS];
                float stunDur = def->values[slot->level][AV_VAC_STUN_DUR];
                bool hitAny = false;
                for (int j = 0; j < unitCount; j++) {
                    if (!units[j].active || units[j].team == units[i].team) continue;
                    if (UnitHasModifier(modifiers, j, MOD_INVULNERABLE)) continue;
                    float d = DistXZ(units[i].position, units[j].position);
                    if (d <= radius) {
                        units[j].position.x = units[i].position.x;
                        units[j].position.z = units[i].position.z;
                        AddModifier(modifiers, j, MOD_STUN, stunDur, 0);
                        EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, j, -1,
                                  units[j].position, 5.0f, 0.25f);
                        hitAny = true;
                    }
                }
                if (!hitAny) break;
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_CHAIN_FROST: {
                if (target < 0) break;
                SpawnChainFrostProjectile(projectiles,
                    units[i].position, target, i, units[i].team, slot->level,
                    def->values[slot->level][AV_CF_PROJ_SPEED],
                    def->values[slot->level][AV_CF_DAMAGE],
                    (int)def->values[slot->level][AV_CF_BOUNCES],
                    def->values[slot->level][AV_CF_BOUNCE_RANGE],
                    def->values[slot->level][AV_CF_DMG_INCREASE]);
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_BLOOD_RAGE: {
                float dur = def->values[slot->level][AV_BR_DURATION];
                float ls = def->values[slot->level][AV_BR_LIFESTEAL];
                AddModifier(modifiers, i, MOD_LIFESTEAL, dur, ls);
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_EARTHQUAKE: {
                float radius = def->values[slot->level][AV_EQ_RADIUS];
                float damage = def->values[slot->level][AV_EQ_DAMAGE];
                for (int j = 0; j < unitCount; j++) {
                    if (j == i || !units[j].active) continue;
                    float d = DistXZ(units[i].position, units[j].position);
                    if (d <= radius)
                        ApplyDamage(units, &unitCount, modifiers, j, damage, 0);
                }
                EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                          units[i].position, 10.0f, 0.5f);
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_SPELL_PROTECT: {
                float dur = def->values[slot->level][AV_SP_DURATION];
                AddModifier(modifiers, i, MOD_SPELL_PROTECT, dur, 0);
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_CRAGGY_ARMOR: {
                float armor = def->values[slot->level][AV_CA_ARMOR];
                float stunChance = def->values[slot->level][AV_CA_STUN_CHANCE];
                float dur = def->values[slot->level][AV_CA_DURATION];
                AddModifier(modifiers, i, MOD_ARMOR, dur, armor);
                AddModifier(modifiers, i, MOD_CRAGGY_ARMOR, dur, stunChance);
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_STONE_GAZE: {
                float dur = def->values[slot->level][AV_SG_DURATION];
                float gazeThresh = def->values[slot->level][AV_SG_GAZE_THRESH];
                AddModifier(modifiers, i, MOD_STONE_GAZE, dur, gazeThresh);
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_FISSURE: {
                if (target < 0 || !fissures) break;
                float length = def->values[slot->level][AV_FI_LENGTH];
                float width = def->values[slot->level][AV_FI_WIDTH];
                float duration = def->values[slot->level][AV_FI_DURATION];
                float damage = def->values[slot->level][AV_FI_DAMAGE];

                SpawnFissure(fissures, units[i].position,
                    units[target].position, length, width, duration,
                    units[i].team, i);

                // Deal damage in area along fissure line
                float fdx = units[target].position.x - units[i].position.x;
                float fdz = units[target].position.z - units[i].position.z;
                float fdist = sqrtf(fdx * fdx + fdz * fdz);
                float fnorm = (fdist > 0.001f) ? 1.0f / fdist : 0.0f;
                for (int j = 0; j < unitCount; j++) {
                    if (j == i || !units[j].active) continue;
                    float ux = units[j].position.x - units[i].position.x;
                    float uz = units[j].position.z - units[i].position.z;
                    float proj = (ux * fdx + uz * fdz) * fnorm * fnorm;
                    if (proj < 0 || proj > length) continue;
                    float perpX = ux - fdx * fnorm * proj;
                    float perpZ = uz - fdz * fnorm * proj;
                    float perpDist = sqrtf(perpX * perpX + perpZ * perpZ);
                    if (perpDist <= width + 3.0f)
                        ApplyDamage(units, &unitCount, modifiers, j, damage, 0);
                }
                EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                          units[i].position, 6.0f, 0.3f);
                slot->cooldownRemaining = def->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_VLAD_AURA: {
                const AbilityDef *vaDef = &ABILITY_DEFS[ABILITY_VLAD_AURA];
                float ls = vaDef->values[slot->level][AV_VA_LIFESTEAL];
                float dur = vaDef->values[slot->level][AV_VA_DURATION];
                for (int j = 0; j < unitCount; j++) {
                    if (!units[j].active || units[j].team != units[i].team) continue;
                    AddModifier(modifiers, j, MOD_LIFESTEAL, dur, ls);
                }
                AddModifier(modifiers, i, MOD_VLAD_AURA, dur, ls);
                slot->cooldownRemaining = vaDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_MAELSTROM: {
                const AbilityDef *mlDef = &ABILITY_DEFS[ABILITY_MAELSTROM];
                float procChance = mlDef->values[slot->level][AV_ML_PROC_CHANCE];
                float dur = mlDef->values[slot->level][AV_ML_DURATION];
                AddModifier(modifiers, i, MOD_MAELSTROM, dur, procChance);
                slot->cooldownRemaining = mlDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_SWAP: {
                int swTarget = FindFurthestEnemy(units, unitCount, i);
                if (swTarget < 0) break;
                const AbilityDef *swDef = &ABILITY_DEFS[ABILITY_SWAP];
                float tmpX = units[i].position.x, tmpZ = units[i].position.z;
                units[i].position.x = units[swTarget].position.x;
                units[i].position.z = units[swTarget].position.z;
                units[swTarget].position.x = tmpX;
                units[swTarget].position.z = tmpZ;
                float shieldHP = swDef->values[slot->level][AV_SW_SHIELD];
                float shieldDur = swDef->values[slot->level][AV_SW_SHIELD_DUR];
                units[i].shieldHP = shieldHP;
                AddModifier(modifiers, i, MOD_SHIELD, shieldDur, shieldHP);
                EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1, units[i].position, 4.0f, 0.2f);
                slot->cooldownRemaining = swDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_APHOTIC_SHIELD: {
                int asAlly = FindLowestHPAlly(units, unitCount, i);
                if (asAlly < 0) asAlly = i;
                const AbilityDef *asDef = &ABILITY_DEFS[ABILITY_APHOTIC_SHIELD];
                for (int m = 0; m < MAX_MODIFIERS; m++) {
                    if (!modifiers[m].active || modifiers[m].unitIndex != asAlly) continue;
                    if (modifiers[m].type == MOD_STUN || modifiers[m].type == MOD_STONE_GAZE)
                        modifiers[m].active = false;
                }
                float asShield = asDef->values[slot->level][AV_AS_SHIELD];
                float asDur = asDef->values[slot->level][AV_AS_DURATION];
                units[asAlly].shieldHP = asShield;
                AddModifier(modifiers, asAlly, MOD_SHIELD, asDur, asShield);
                slot->cooldownRemaining = asDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_HOOK: {
                const AbilityDef *hkDef = &ABILITY_DEFS[ABILITY_HOOK];
                float range = hkDef->values[slot->level][AV_HK_RANGE];
                int hkTarget = FindFurthestEnemy(units, unitCount, i);
                if (hkTarget < 0) break;
                float hkd = DistXZ(units[i].position, units[hkTarget].position);
                if (hkd > range) {
                    hkTarget = FindClosestEnemy(units, unitCount, i);
                    if (hkTarget < 0) break;
                    hkd = DistXZ(units[i].position, units[hkTarget].position);
                    if (hkd > range) break;
                }
                SpawnHookProjectile(projectiles, units[i].position,
                    hkTarget, i, units[i].team, slot->level,
                    hkDef->values[slot->level][AV_HK_SPEED],
                    hkDef->values[slot->level][AV_HK_DMG_PER_DIST], range,
                    hkDef->values[slot->level][AV_HK_BASE_DMG]);
                slot->cooldownRemaining = hkDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_PRIMAL_CHARGE: {
                int pcTarget = FindFurthestEnemy(units, unitCount, i);
                if (pcTarget < 0) break;
                const AbilityDef *pcDef = &ABILITY_DEFS[ABILITY_PRIMAL_CHARGE];
                float chargeSpeed = pcDef->values[slot->level][AV_PC_CHARGE_SPEED];
                units[i].chargeTarget = pcTarget;
                AddModifier(modifiers, i, MOD_CHARGING, 10.0f, chargeSpeed);
                slot->cooldownRemaining = pcDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_MULTICAST: {
                const AbilityDef *mcDef = &ABILITY_DEFS[ABILITY_MULTICAST];
                float dur = mcDef->values[slot->level][AV_MC_DURATION];
                float chance2x = mcDef->values[slot->level][AV_MC_CHANCE_2X];
                AddModifier(modifiers, i, MOD_MULTICAST, dur, chance2x);
                slot->cooldownRemaining = mcDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_SHARE_PAIN: {
                const AbilityDef *spDef = &ABILITY_DEFS[ABILITY_SHARE_PAIN];
                float dur = spDef->values[slot->level][AV_SPP_DURATION];
                float pct = spDef->values[slot->level][AV_SPP_SHARE_PCT];
                AddModifier(modifiers, i, MOD_SHARE_PAIN, dur, pct);
                slot->cooldownRemaining = spDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_TOXIC_CLOUD: {
                const AbilityDef *tcDef = &ABILITY_DEFS[ABILITY_TOXIC_CLOUD];
                float radius = tcDef->values[slot->level][AV_TC_RADIUS];
                float poisonDPS = tcDef->values[slot->level][AV_TC_POISON_DPS];
                float poisonDur = tcDef->values[slot->level][AV_TC_DURATION];
                bool hitAny = false;
                for (int j = 0; j < unitCount; j++) {
                    if (!units[j].active || units[j].team == units[i].team) continue;
                    if (DistXZ(units[i].position, units[j].position) <= radius) {
                        AddModifier(modifiers, j, MOD_POISON, poisonDur, poisonDPS);
                        hitAny = true;
                    }
                }
                if (!hitAny) break;
                slot->cooldownRemaining = tcDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            case ABILITY_MEND: {
                int ally = FindLowestHPAlly(units, unitCount, i);
                if (ally < 0) ally = i;
                const AbilityDef *mnDef = &ABILITY_DEFS[ABILITY_MEND];
                float range = mnDef->range[slot->level];
                if (DistXZ(units[i].position, units[ally].position) > range) break;
                float maxHP = UNIT_STATS[units[ally].typeIndex].health * units[ally].hpMultiplier;
                float heal = mnDef->values[slot->level][AV_MN_FLAT_HEAL]
                           + maxHP * mnDef->values[slot->level][AV_MN_PCT_HEAL];
                units[ally].currentHealth += heal;
                if (units[ally].currentHealth > maxHP)
                    units[ally].currentHealth = maxHP;
                slot->cooldownRemaining = mnDef->cooldown[slot->level];
                EmitEvent(events, eventCount, COMBAT_EVT_HEAL, ally, ABILITY_MEND, units[ally].position, 0, 0);
                castThisFrame = true;
            } break;
            case ABILITY_REJUVENATE: {
                int ally = FindLowestHPAlly(units, unitCount, i);
                if (ally < 0) ally = i;
                const AbilityDef *rjDef = &ABILITY_DEFS[ABILITY_REJUVENATE];
                float range = rjDef->range[slot->level];
                if (DistXZ(units[i].position, units[ally].position) > range) break;
                float maxHP = UNIT_STATS[units[ally].typeIndex].health * units[ally].hpMultiplier;
                float flatHPS = rjDef->values[slot->level][AV_RJ_FLAT_HPS];
                float pctHPS = rjDef->values[slot->level][AV_RJ_PCT_HPS];
                float totalHPS = flatHPS + maxHP * pctHPS;
                float dur = rjDef->values[slot->level][AV_RJ_DURATION];
                AddModifier(modifiers, ally, MOD_REJUVENATE, dur, totalHPS);
                slot->cooldownRemaining = rjDef->cooldown[slot->level];
                castThisFrame = true;
            } break;
            default: break;
            }
            if (castThisFrame) {
                EmitEvent(events, eventCount, COMBAT_EVT_ABILITY_CAST, i,
                          slot->abilityId, units[i].position, 0, 0);
                units[i].abilityCastDelay = 0.75f;
                // Multicast: chance to re-execute the same ability (not Multicast itself)
                if (slot->abilityId != ABILITY_MULTICAST && slot->abilityId != ABILITY_SHARE_PAIN
                    && UnitHasModifier(modifiers, i, MOD_MULTICAST)) {
                    float chance2x = GetModifierValue(modifiers, i, MOD_MULTICAST);
                    float chance3x = 0.0f;
                    int mcLvl = GetUnitAbilityLevel(units, i, ABILITY_MULTICAST);
                    if (mcLvl >= 0) chance3x = ABILITY_DEFS[ABILITY_MULTICAST].values[mcLvl][AV_MC_CHANCE_3X];
                    float roll2x = combat_rng_next();
                    int extraCasts = 0;
                    if (roll2x < chance2x) {
                        extraCasts = 1;
                        float roll3x = combat_rng_next();
                        if (roll3x < chance3x) extraCasts = 2;
                    }
                    if (extraCasts > 0) {
                        EmitEvent(events, eventCount, COMBAT_EVT_MULTICAST, i,
                                  slot->abilityId, units[i].position, (float)extraCasts, 0);
                    }
                    // Re-execute the cast logic (reset cooldown was already set, just re-trigger effects)
                    for (int mc = 0; mc < extraCasts; mc++) {
                        EmitEvent(events, eventCount, COMBAT_EVT_ABILITY_CAST, i,
                                  slot->abilityId, units[i].position, 0, 0);
                        // Re-trigger the same ability's effect (projectiles/modifiers)
                        switch (slot->abilityId) {
                        case ABILITY_MAGIC_MISSILE:
                            if (target >= 0)
                                SpawnProjectile(projectiles, PROJ_MAGIC_MISSILE,
                                    units[i].position, target, i, units[i].team, slot->level,
                                    def->values[slot->level][AV_MM_PROJ_SPEED],
                                    def->values[slot->level][AV_MM_DAMAGE],
                                    def->values[slot->level][AV_MM_STUN_DUR],
                                    (Color){120, 80, 255, 255});
                            break;
                        case ABILITY_CHAIN_FROST:
                            if (target >= 0)
                                SpawnChainFrostProjectile(projectiles,
                                    units[i].position, target, i, units[i].team, slot->level,
                                    def->values[slot->level][AV_CF_PROJ_SPEED],
                                    def->values[slot->level][AV_CF_DAMAGE],
                                    (int)def->values[slot->level][AV_CF_BOUNCES],
                                    def->values[slot->level][AV_CF_BOUNCE_RANGE],
                                    def->values[slot->level][AV_CF_DMG_INCREASE]);
                            break;
                        case ABILITY_HOOK: {
                            const AbilityDef *hkDef2 = &ABILITY_DEFS[ABILITY_HOOK];
                            int hkT2 = FindFurthestEnemy(units, unitCount, i);
                            if (hkT2 >= 0)
                                SpawnHookProjectile(projectiles, units[i].position,
                                    hkT2, i, units[i].team, slot->level,
                                    hkDef2->values[slot->level][AV_HK_SPEED],
                                    hkDef2->values[slot->level][AV_HK_DMG_PER_DIST],
                                    hkDef2->values[slot->level][AV_HK_RANGE],
                                    hkDef2->values[slot->level][AV_HK_BASE_DMG]);
                        } break;
                        case ABILITY_BLOOD_RAGE:
                            AddModifier(modifiers, i, MOD_LIFESTEAL,
                                def->values[slot->level][AV_BR_DURATION],
                                def->values[slot->level][AV_BR_LIFESTEAL]);
                            break;
                        case ABILITY_EARTHQUAKE: {
                            float eqR = def->values[slot->level][AV_EQ_RADIUS];
                            float eqD = def->values[slot->level][AV_EQ_DAMAGE];
                            for (int j = 0; j < unitCount; j++) {
                                if (j == i || !units[j].active) continue;
                                if (DistXZ(units[i].position, units[j].position) <= eqR)
                                    ApplyDamage(units, &unitCount, modifiers, j, eqD, 0);
                            }
                        } break;
                        default: break; // non-repeatable abilities just get extra event
                        }
                    }
                }
            }
        }

        // Primal Charge movement — overrides normal movement
        if (units[i].chargeTarget >= 0) {
            int ct = units[i].chargeTarget;
            if (ct >= unitCount || !units[ct].active) {
                units[i].chargeTarget = -1;
            } else {
                float chargeDist = DistXZ(units[i].position, units[ct].position);
                float chargeSpeed = GetModifierValue(modifiers, i, MOD_CHARGING);
                if (chargeSpeed <= 0) chargeSpeed = 80.0f;
                if (chargeDist <= ATTACK_RANGE) {
                    // IMPACT — AoE damage + knockback
                    int chargeLvl = GetUnitAbilityLevel(units, i, ABILITY_PRIMAL_CHARGE);
                    if (chargeLvl < 0) chargeLvl = 0;
                    const AbilityDef *pcDef = &ABILITY_DEFS[ABILITY_PRIMAL_CHARGE];
                    float pcDmg = pcDef->values[chargeLvl][AV_PC_DAMAGE];
                    float pcKnock = pcDef->values[chargeLvl][AV_PC_KNOCKBACK];
                    float pcRadius = pcDef->values[chargeLvl][AV_PC_AOE_RADIUS];
                    for (int j = 0; j < unitCount; j++) {
                        if (!units[j].active || units[j].team == units[i].team) continue;
                        float dd = DistXZ(units[ct].position, units[j].position);
                        if (dd <= pcRadius) {
                            ApplyDamage(units, &unitCount, modifiers, j, pcDmg, 0);
                            // Knockback
                            float kx = units[j].position.x - units[ct].position.x;
                            float kz = units[j].position.z - units[ct].position.z;
                            float klen = sqrtf(kx*kx + kz*kz);
                            if (klen > 0.001f) {
                                units[j].position.x += (kx/klen) * pcKnock;
                                units[j].position.z += (kz/klen) * pcKnock;
                            }
                        }
                    }
                    EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                              units[i].position, 8.0f, 0.4f);
                    units[i].chargeTarget = -1;
                    for (int m = 0; m < MAX_MODIFIERS; m++) {
                        if (modifiers[m].active && modifiers[m].unitIndex == i && modifiers[m].type == MOD_CHARGING)
                            modifiers[m].active = false;
                    }
                } else {
                    float cdx = units[ct].position.x - units[i].position.x;
                    float cdz = units[ct].position.z - units[i].position.z;
                    float clen = sqrtf(cdx*cdx + cdz*cdz);
                    units[i].position.x += (cdx/clen) * chargeSpeed * dt;
                    units[i].position.z += (cdz/clen) * chargeSpeed * dt;
                }
                continue; // skip normal movement while charging
            }
        }

        // Movement + basic attack
        if (target < 0) continue;
        float moveSpeed = stats->movementSpeed * units[i].speedMultiplier;
        float speedMult = GetModifierValue(modifiers, i, MOD_SPEED_MULT);
        if (speedMult > 0) moveSpeed *= speedMult;

        bool isDevil = (units[i].typeIndex == DEVIL_TYPE_INDEX);
        float attackRange = isDevil ? DEVIL_RANGED_RANGE : ATTACK_RANGE;

        float dist = DistXZ(units[i].position, units[target].position);
        if (dist > attackRange)
        {
            Vector3 oldPos = units[i].position;
            float dx = units[target].position.x - units[i].position.x;
            float dz = units[target].position.z - units[i].position.z;
            float len = sqrtf(dx*dx + dz*dz);
            if (len > 0.001f) {
                units[i].position.x += (dx/len) * moveSpeed * dt;
                units[i].position.z += (dz/len) * moveSpeed * dt;
            }
            // Fissure collision — slide along impassable terrain
            if (fissures) {
                float unitRadius = 2.0f;
                units[i].position = ResolveFissureCollision(fissures, units[i].position, oldPos, unitRadius);
            }

            // Unit-unit collision — push overlapping units apart on XZ plane
            for (int j = 0; j < unitCount; j++) {
                if (j == i || !units[j].active) continue;
                float cdist = DistXZ(units[i].position, units[j].position);
                float minDist = UNIT_COLLISION_RADIUS * 2.0f;
                if (cdist < minDist && cdist > 0.001f) {
                    float overlap = minDist - cdist;
                    float pushX = (units[i].position.x - units[j].position.x) / cdist;
                    float pushZ = (units[i].position.z - units[j].position.z) / cdist;
                    units[i].position.x += pushX * overlap * 0.5f;
                    units[i].position.z += pushZ * overlap * 0.5f;
                    units[j].position.x -= pushX * overlap * 0.5f;
                    units[j].position.z -= pushZ * overlap * 0.5f;
                }
            }
        }
        else
        {
            units[i].attackCooldown -= dt;
            if (units[i].attackCooldown <= 0.0f)
            {
                if (isDevil) {
                    // Devil ranged attack — spawn a bolt projectile
                    float dmg = stats->attackDamage * units[i].dmgMultiplier;
                    SpawnProjectile(projectiles, PROJ_DEVIL_BOLT,
                        units[i].position, target, i, units[i].team, 0,
                        50.0f, dmg, 0,
                        (Color){200, 50, 50, 255});
                    units[i].attackCooldown = stats->attackSpeed;
                } else {
                    float rawDmg = stats->attackDamage * units[i].dmgMultiplier;
                    float dmg = ApplyDamage(units, &unitCount, modifiers, target, rawDmg, DMG_SINGLE_TARGET);
                    EmitEvent(events, eventCount, COMBAT_EVT_MELEE_HIT, target, -1,
                              units[target].position, rawDmg, 0);
                    // Lifesteal
                    if (dmg > 0) {
                        float ls = GetModifierValue(modifiers, i, MOD_LIFESTEAL);
                        if (ls > 0) {
                            units[i].currentHealth += dmg * ls;
                            if (units[i].currentHealth > unitMaxHP)
                                units[i].currentHealth = unitMaxHP;
                        }
                    }
                    // Craggy Armor retaliation — chance to stun attacker
                    if (dmg > 0 && UnitHasModifier(modifiers, target, MOD_CRAGGY_ARMOR)) {
                        float stunChance = GetModifierValue(modifiers, target, MOD_CRAGGY_ARMOR);
                        float roll = combat_rng_next();
                        if (roll < stunChance) {
                            int caLvl = GetUnitAbilityLevel(units, target, ABILITY_CRAGGY_ARMOR);
                            float stunDur = (caLvl >= 0) ? ABILITY_DEFS[ABILITY_CRAGGY_ARMOR].values[caLvl][AV_CA_STUN_DUR] : 1.0f;
                            AddModifier(modifiers, i, MOD_STUN, stunDur, 0);
                            EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                                      units[i].position, 3.0f, 0.15f);
                        }
                    }
                    // Maelstrom on-hit proc (deterministic)
                    if (dmg > 0 && UnitHasModifier(modifiers, i, MOD_MAELSTROM)) {
                        float procChance = GetModifierValue(modifiers, i, MOD_MAELSTROM);
                        float roll = combat_rng_next();
                        if (roll < procChance) {
                            int mlLvl = GetUnitAbilityLevel(units, i, ABILITY_MAELSTROM);
                            if (mlLvl < 0) mlLvl = 0;
                            const AbilityDef *mlDef = &ABILITY_DEFS[ABILITY_MAELSTROM];
                            SpawnMaelstromProjectile(projectiles,
                                units[target].position, target, i, units[i].team, mlLvl,
                                mlDef->values[mlLvl][AV_ML_SPEED],
                                mlDef->values[mlLvl][AV_ML_DAMAGE],
                                (int)mlDef->values[mlLvl][AV_ML_BOUNCES],
                                mlDef->values[mlLvl][AV_ML_BOUNCE_RANGE]);
                        }
                    }
                    // Venom Strike on-hit: apply/refresh poison on target
                    {
                        int vsLvl = GetUnitAbilityLevel(units, i, ABILITY_VENOM_STRIKE);
                        if (vsLvl >= 0 && dmg > 0) {
                            float poisonDPS = ABILITY_DEFS[ABILITY_VENOM_STRIKE].values[vsLvl][AV_VS_POISON_DPS];
                            float poisonDur = ABILITY_DEFS[ABILITY_VENOM_STRIKE].values[vsLvl][AV_VS_DURATION];
                            AddModifier(modifiers, target, MOD_POISON, poisonDur, poisonDPS);
                        }
                    }
                    units[i].attackCooldown = stats->attackSpeed;
                }
                // Fervor: track same-target attacks for attack speed bonus (melee + ranged)
                {
                    int fvLvl = GetUnitAbilityLevel(units, i, ABILITY_FERVOR);
                    if (fvLvl >= 0) {
                        int maxStacks = (int)ABILITY_DEFS[ABILITY_FERVOR].values[fvLvl][AV_FV_MAX_STACKS];
                        if (units[i].lastAttackTarget == target) {
                            for (int m = 0; m < MAX_MODIFIERS; m++) {
                                if (modifiers[m].active && modifiers[m].unitIndex == i && modifiers[m].type == MOD_FERVOR) {
                                    float stacks = modifiers[m].value + 1.0f;
                                    if (stacks > (float)maxStacks) stacks = (float)maxStacks;
                                    modifiers[m].value = stacks;
                                    break;
                                }
                            }
                        } else {
                            for (int m = 0; m < MAX_MODIFIERS; m++) {
                                if (modifiers[m].active && modifiers[m].unitIndex == i && modifiers[m].type == MOD_FERVOR) {
                                    modifiers[m].value = 1.0f;
                                    break;
                                }
                            }
                        }
                        units[i].lastAttackTarget = target;
                    }
                }
                // Fervor: modify attack cooldown based on stacks
                {
                    float fvStacks = GetModifierValue(modifiers, i, MOD_FERVOR);
                    if (fvStacks > 0) {
                        int fvLvl = GetUnitAbilityLevel(units, i, ABILITY_FERVOR);
                        if (fvLvl >= 0) {
                            float redPerStack = ABILITY_DEFS[ABILITY_FERVOR].values[fvLvl][AV_FV_SPEED_RED];
                            float speedMlt = 1.0f - fvStacks * redPerStack;
                            if (speedMlt < 0.3f) speedMlt = 0.3f;
                            units[i].attackCooldown = stats->attackSpeed * speedMlt;
                        }
                    }
                }
            }
        }
    }

    // === STEP 4: Stone Gaze accumulation ===
    for (int i = 0; i < unitCount; i++) {
        if (!units[i].active) continue;
        bool beingGazed = false;
        for (int g = 0; g < unitCount; g++) {
            if (!units[g].active || units[g].team == units[i].team) continue;
            if (!UnitHasModifier(modifiers, g, MOD_STONE_GAZE)) continue;
            float dx = units[g].position.x - units[i].position.x;
            float dz = units[g].position.z - units[i].position.z;
            float distToGazer = sqrtf(dx*dx + dz*dz);
            if (distToGazer < 0.1f) continue;
            // Check if unit i is facing toward gazer g
            float facingRad = units[i].facingAngle * (PI / 180.0f);
            float faceDirX = sinf(facingRad);
            float faceDirZ = cosf(facingRad);
            float dot = (dx/distToGazer) * faceDirX + (dz/distToGazer) * faceDirZ;
            int sgLvl = GetUnitAbilityLevel(units, g, ABILITY_STONE_GAZE);
            float coneAngle = (sgLvl >= 0) ? ABILITY_DEFS[ABILITY_STONE_GAZE].values[sgLvl][AV_SG_CONE_ANGLE] : 45.0f;
            float coneThresh = cosf(coneAngle * (PI / 180.0f));
            if (dot >= coneThresh) {
                units[i].gazeAccum += dt;
                beingGazed = true;
                if (sgLvl >= 0) {
                    float thresh = ABILITY_DEFS[ABILITY_STONE_GAZE].values[sgLvl][AV_SG_GAZE_THRESH];
                    float stunDur = ABILITY_DEFS[ABILITY_STONE_GAZE].values[sgLvl][AV_SG_STUN_DUR];
                    if (units[i].gazeAccum >= thresh) {
                        AddModifier(modifiers, i, MOD_STUN, stunDur, 0);
                        units[i].gazeAccum = 0;
                        EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                                  units[i].position, 3.0f, 0.2f);
                        EmitEvent(events, eventCount, COMBAT_EVT_ABILITY_CAST, i,
                                  ABILITY_STONE_GAZE, units[i].position, 0, 0);
                    }
                }
                break; // only accumulate from one gazer at a time
            }
        }
        if (!beingGazed && units[i].gazeAccum > 0) {
            units[i].gazeAccum -= dt * 2.0f;
            if (units[i].gazeAccum < 0) units[i].gazeAccum = 0;
        }
    }

    // === STEP 5: Check round end ===
    *unitCountPtr = unitCount;
    int ba, ra;
    CountTeams(units, unitCount, &ba, &ra);
    if (ba == 0 && ra == 0) return 3; // draw
    if (ra == 0) return 1;            // blue wins
    if (ba == 0) return 2;            // red wins
    return 0;                          // still fighting
}
