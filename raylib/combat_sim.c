#include "combat_sim.h"
#include <math.h>
#include <string.h>

#ifndef PI
#define PI 3.14159265358979323846f
#endif

// Deterministic hash-based pseudo-random: produces 0.0–1.0 from game state
static float det_roll(int a, int b, float hp)
{
    unsigned int h;
    memcpy(&h, &hp, sizeof(h));          // bit-pattern of float HP
    h ^= (unsigned int)a * 2654435761u;  // Knuth multiplicative hash
    h ^= (unsigned int)b * 2246822519u;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return (float)(h & 0xFFFF) / 65535.0f;
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

int CombatTick(Unit units[], int unitCount,
               Modifier modifiers[],
               Projectile projectiles[],
               Fissure fissures[],
               float dt,
               CombatEvent events[], int *eventCount)
{
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
    }

    // === STEP 1b: Tick fissures ===
    if (fissures) UpdateFissures(fissures, dt);

    // === STEP 2: Update projectiles ===
    for (int p = 0; p < MAX_PROJECTILES; p++) {
        if (!projectiles[p].active) continue;
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
            // HIT — Hook: pull target to caster, damage by distance
            if (projectiles[p].type == PROJ_HOOK) {
                if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                    float hookDist = DistXZ(units[ti].position, units[projectiles[p].sourceIndex].position);
                    float hitDmg = hookDist * projectiles[p].damage;
                    if (units[ti].shieldHP > 0) {
                        if (hitDmg <= units[ti].shieldHP) { units[ti].shieldHP -= hitDmg; hitDmg = 0; }
                        else { hitDmg -= units[ti].shieldHP; units[ti].shieldHP = 0; }
                    }
                    units[ti].currentHealth -= hitDmg;
                    units[ti].position.x = units[projectiles[p].sourceIndex].position.x;
                    units[ti].position.z = units[projectiles[p].sourceIndex].position.z;
                    EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, ti, -1,
                              units[ti].position, 6.0f, 0.3f);
                    if (units[ti].currentHealth <= 0) units[ti].active = false;
                }
                projectiles[p].active = false;
            }
            // HIT — Maelstrom: bounce like chain frost
            else if (projectiles[p].type == PROJ_MAELSTROM) {
                if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                    float hitDmg = projectiles[p].damage;
                    if (units[ti].shieldHP > 0) {
                        if (hitDmg <= units[ti].shieldHP) { units[ti].shieldHP -= hitDmg; hitDmg = 0; }
                        else { hitDmg -= units[ti].shieldHP; units[ti].shieldHP = 0; }
                    }
                    units[ti].currentHealth -= hitDmg;
                    if (units[ti].currentHealth <= 0) units[ti].active = false;
                }
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
            // HIT — normal (Magic Missile / Chain Frost)
            else {
            if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                float hitDmg = projectiles[p].damage;
                if (projectiles[p].type == PROJ_MAGIC_MISSILE)
                    hitDmg *= UNIT_STATS[units[ti].typeIndex].health * units[ti].hpMultiplier;
                if (units[ti].shieldHP > 0) {
                    if (hitDmg <= units[ti].shieldHP) { units[ti].shieldHP -= hitDmg; hitDmg = 0; }
                    else { hitDmg -= units[ti].shieldHP; units[ti].shieldHP = 0; }
                }
                units[ti].currentHealth -= hitDmg;
                if (projectiles[p].stunDuration > 0) {
                    AddModifier(modifiers, ti, MOD_STUN, projectiles[p].stunDuration, 0);
                    EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, ti, -1,
                              units[ti].position, 5.0f, 0.25f);
                }
                if (units[ti].currentHealth <= 0) units[ti].active = false;
            }
            // Chain Frost bounce
            if (projectiles[p].type == PROJ_CHAIN_FROST && projectiles[p].bouncesRemaining > 0) {
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
            } // end else (normal projectile hit)
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

        // Tick ability cooldowns
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
            if (units[i].abilities[a].abilityId < 0) continue;
            if (units[i].abilities[a].cooldownRemaining > 0)
                units[i].abilities[a].cooldownRemaining -= dt;
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
                } else if (slot->abilityId == ABILITY_SUNDER) {
                    if (slot->triggered || slot->cooldownRemaining > 0) continue;
                    const AbilityDef *def = &ABILITY_DEFS[ABILITY_SUNDER];
                    float threshold = def->values[slot->level][AV_SU_HP_THRESH];
                    if (units[i].currentHealth > 0 && units[i].currentHealth <= unitMaxHP * threshold) {
                        int ally = FindHighestHPAlly(units, unitCount, i);
                        if (ally >= 0) {
                            float myHP = units[i].currentHealth;
                            float allyHP = units[ally].currentHealth;
                            units[i].currentHealth = allyHP;
                            units[ally].currentHealth = myHP;
                            float allyMax = UNIT_STATS[units[ally].typeIndex].health * units[ally].hpMultiplier;
                            if (units[i].currentHealth > unitMaxHP) units[i].currentHealth = unitMaxHP;
                            if (units[ally].currentHealth > allyMax) units[ally].currentHealth = allyMax;
                            slot->triggered = true;
                            slot->cooldownRemaining = def->cooldown[slot->level];
                            EmitEvent(events, eventCount, COMBAT_EVT_ABILITY_CAST, i,
                                      ABILITY_SUNDER, units[i].position, 0, 0);
                        }
                    }
                }
            }
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
                    def->values[slot->level][AV_CF_BOUNCE_RANGE]);
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
                    if (UnitHasModifier(modifiers, j, MOD_INVULNERABLE)) continue;
                    float d = DistXZ(units[i].position, units[j].position);
                    if (d <= radius) {
                        units[j].currentHealth -= damage;
                        if (units[j].currentHealth <= 0) units[j].active = false;
                    }
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
                    if (UnitHasModifier(modifiers, j, MOD_INVULNERABLE)) continue;
                    float ux = units[j].position.x - units[i].position.x;
                    float uz = units[j].position.z - units[i].position.z;
                    float proj = (ux * fdx + uz * fdz) * fnorm * fnorm;
                    if (proj < 0 || proj > length) continue;
                    float perpX = ux - fdx * fnorm * proj;
                    float perpZ = uz - fdz * fnorm * proj;
                    float perpDist = sqrtf(perpX * perpX + perpZ * perpZ);
                    if (perpDist <= width + 3.0f) {
                        units[j].currentHealth -= damage;
                        if (units[j].currentHealth <= 0) units[j].active = false;
                    }
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
                    hkDef->values[slot->level][AV_HK_DMG_PER_DIST], range);
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
            default: break;
            }
            if (castThisFrame) {
                EmitEvent(events, eventCount, COMBAT_EVT_ABILITY_CAST, i,
                          slot->abilityId, units[i].position, 0, 0);
                units[i].abilityCastDelay = 0.75f;
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
                    int chargeLvl = 0;
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        if (units[i].abilities[a].abilityId == ABILITY_PRIMAL_CHARGE) {
                            chargeLvl = units[i].abilities[a].level; break;
                        }
                    }
                    const AbilityDef *pcDef = &ABILITY_DEFS[ABILITY_PRIMAL_CHARGE];
                    float pcDmg = pcDef->values[chargeLvl][AV_PC_DAMAGE];
                    float pcKnock = pcDef->values[chargeLvl][AV_PC_KNOCKBACK];
                    float pcRadius = pcDef->values[chargeLvl][AV_PC_AOE_RADIUS];
                    for (int j = 0; j < unitCount; j++) {
                        if (!units[j].active || units[j].team == units[i].team) continue;
                        if (UnitHasModifier(modifiers, j, MOD_INVULNERABLE)) continue;
                        float dd = DistXZ(units[ct].position, units[j].position);
                        if (dd <= pcRadius) {
                            float dmgHit = pcDmg;
                            if (units[j].shieldHP > 0) {
                                if (dmgHit <= units[j].shieldHP) { units[j].shieldHP -= dmgHit; dmgHit = 0; }
                                else { dmgHit -= units[j].shieldHP; units[j].shieldHP = 0; }
                            }
                            units[j].currentHealth -= dmgHit;
                            if (units[j].currentHealth <= 0) units[j].active = false;
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

        float dist = DistXZ(units[i].position, units[target].position);
        if (dist > ATTACK_RANGE)
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
                if (!UnitHasModifier(modifiers, target, MOD_INVULNERABLE)) {
                    float dmg = stats->attackDamage * units[i].dmgMultiplier;
                    float armor = GetModifierValue(modifiers, target, MOD_ARMOR);
                    dmg -= armor;
                    if (dmg < 0) dmg = 0;
                    // Shield absorption
                    if (units[target].shieldHP > 0) {
                        if (dmg <= units[target].shieldHP) { units[target].shieldHP -= dmg; dmg = 0; }
                        else { dmg -= units[target].shieldHP; units[target].shieldHP = 0; }
                    }
                    units[target].currentHealth -= dmg;
                    // Lifesteal
                    float ls = GetModifierValue(modifiers, i, MOD_LIFESTEAL);
                    if (ls > 0) {
                        units[i].currentHealth += dmg * ls;
                        if (units[i].currentHealth > unitMaxHP)
                            units[i].currentHealth = unitMaxHP;
                    }
                    // Craggy Armor retaliation — chance to stun attacker
                    if (UnitHasModifier(modifiers, target, MOD_CRAGGY_ARMOR)) {
                        float stunChance = GetModifierValue(modifiers, target, MOD_CRAGGY_ARMOR);
                        float roll = det_roll(i, target, units[i].currentHealth);
                        if (roll < stunChance) {
                            float stunDur = 1.0f;
                            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                if (units[target].abilities[a].abilityId == ABILITY_CRAGGY_ARMOR) {
                                    int lvl = units[target].abilities[a].level;
                                    stunDur = ABILITY_DEFS[ABILITY_CRAGGY_ARMOR].values[lvl][AV_CA_STUN_DUR];
                                    break;
                                }
                            }
                            AddModifier(modifiers, i, MOD_STUN, stunDur, 0);
                            EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                                      units[i].position, 3.0f, 0.15f);
                        }
                    }
                    // Maelstrom on-hit proc (deterministic)
                    if (UnitHasModifier(modifiers, i, MOD_MAELSTROM)) {
                        float procChance = GetModifierValue(modifiers, i, MOD_MAELSTROM);
                        float roll = det_roll(i, target, units[target].currentHealth);
                        if (roll < procChance) {
                            int mlLvl = 0;
                            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                if (units[i].abilities[a].abilityId == ABILITY_MAELSTROM) {
                                    mlLvl = units[i].abilities[a].level; break;
                                }
                            }
                            const AbilityDef *mlDef = &ABILITY_DEFS[ABILITY_MAELSTROM];
                            SpawnMaelstromProjectile(projectiles,
                                units[target].position, target, i, units[i].team, mlLvl,
                                mlDef->values[mlLvl][AV_ML_SPEED],
                                mlDef->values[mlLvl][AV_ML_DAMAGE],
                                (int)mlDef->values[mlLvl][AV_ML_BOUNCES],
                                mlDef->values[mlLvl][AV_ML_BOUNCE_RANGE]);
                        }
                    }
                    if (units[target].currentHealth <= 0) units[target].active = false;
                }
                units[i].attackCooldown = stats->attackSpeed;
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
            float coneAngle = 45.0f;
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                if (units[g].abilities[a].abilityId == ABILITY_STONE_GAZE) {
                    int lvl = units[g].abilities[a].level;
                    coneAngle = ABILITY_DEFS[ABILITY_STONE_GAZE].values[lvl][AV_SG_CONE_ANGLE];
                    break;
                }
            }
            float coneThresh = cosf(coneAngle * (PI / 180.0f));
            if (dot >= coneThresh) {
                units[i].gazeAccum += dt;
                beingGazed = true;
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                    if (units[g].abilities[a].abilityId == ABILITY_STONE_GAZE) {
                        int lvl = units[g].abilities[a].level;
                        float thresh = ABILITY_DEFS[ABILITY_STONE_GAZE].values[lvl][AV_SG_GAZE_THRESH];
                        float stunDur = ABILITY_DEFS[ABILITY_STONE_GAZE].values[lvl][AV_SG_STUN_DUR];
                        if (units[i].gazeAccum >= thresh) {
                            AddModifier(modifiers, i, MOD_STUN, stunDur, 0);
                            units[i].gazeAccum = 0;
                            EmitEvent(events, eventCount, COMBAT_EVT_SHAKE, i, -1,
                                      units[i].position, 3.0f, 0.2f);
                            EmitEvent(events, eventCount, COMBAT_EVT_ABILITY_CAST, i,
                                      ABILITY_STONE_GAZE, units[i].position, 0, 0);
                        }
                        break;
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
    int ba, ra;
    CountTeams(units, unitCount, &ba, &ra);
    if (ba == 0 && ra == 0) return 3; // draw
    if (ra == 0) return 1;            // blue wins
    if (ba == 0) return 2;            // red wins
    return 0;                          // still fighting
}
