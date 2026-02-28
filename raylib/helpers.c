#include "game.h"
#include "helpers.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

//------------------------------------------------------------------------------------
// Unit Utilities
//------------------------------------------------------------------------------------
// Count active units for a specific team
int CountTeamUnits(Unit units[], int unitCount, Team team)
{
    int count = 0;
    for (int i = 0; i < unitCount; i++)
        if (units[i].active && units[i].team == team) count++;
    return count;
}

// Returns true if spawn succeeded
bool SpawnUnit(Unit units[], int *unitCount, int typeIndex, Team team)
{
    if (*unitCount >= MAX_UNITS) return false;
    // Enforce blue team cap
    if (team == TEAM_BLUE && CountTeamUnits(units, *unitCount, TEAM_BLUE) >= BLUE_TEAM_MAX_SIZE)
        return false;
    const UnitStats *stats = &UNIT_STATS[typeIndex];
    units[*unitCount] = (Unit){
        .typeIndex      = typeIndex,
        .position       = (Vector3){ 0.0f, 0.0f, 0.0f },
        .team           = team,
        .currentHealth  = stats->health,
        .attackCooldown = 0.0f,
        .targetIndex    = -1,
        .active         = true,
        .selected       = false,
        .dragging       = false,
        .facingAngle    = (team == TEAM_BLUE) ? 180.0f : 0.0f,
        .currentAnim    = ANIM_IDLE,
        .animFrame      = GetRandomValue(0, 999),
        .scaleOverride  = 1.0f,
        .hpMultiplier   = 1.0f,
        .dmgMultiplier  = 1.0f,
    };
    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
        units[*unitCount].abilities[a] = (AbilitySlot){ .abilityId = -1, .level = 0,
            .cooldownRemaining = 0, .triggered = false };
    }
    units[*unitCount].nextAbilitySlot = 0;
    (*unitCount)++;
    return true;
}

BoundingBox GetUnitBounds(Unit *unit, UnitType *type)
{
    BoundingBox b = type->baseBounds;
    float s = type->scale;
    Vector3 p = unit->position;
    return (BoundingBox){
        (Vector3){ p.x + b.min.x * s, p.y + b.min.y * s, p.z + b.min.z * s },
        (Vector3){ p.x + b.max.x * s, p.y + b.max.y * s, p.z + b.max.z * s }
    };
}

Color GetTeamTint(Team team)
{
    if (team == TEAM_BLUE) return (Color){ 150, 180, 255, 255 };
    else                   return (Color){ 255, 150, 150, 255 };
}

// Distance on the XZ plane
float DistXZ(Vector3 a, Vector3 b)
{
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return sqrtf(dx * dx + dz * dz);
}

// Find index of closest active enemy (-1 if none)
int FindClosestEnemy(Unit units[], int unitCount, int selfIndex)
{
    Team myTeam = units[selfIndex].team;
    float bestDist = 1e30f;
    int bestIdx = -1;
    for (int j = 0; j < unitCount; j++)
    {
        if (j == selfIndex || !units[j].active) continue;
        if (units[j].team == myTeam) continue;
        float d = DistXZ(units[selfIndex].position, units[j].position);
        if (d < bestDist) { bestDist = d; bestIdx = j; }
    }
    return bestIdx;
}

// Count active units per team
void CountTeams(Unit units[], int unitCount, int *blueAlive, int *redAlive)
{
    *blueAlive = 0;
    *redAlive  = 0;
    for (int i = 0; i < unitCount; i++)
    {
        if (!units[i].active) continue;
        if (units[i].team == TEAM_BLUE) (*blueAlive)++;
        else (*redAlive)++;
    }
}

// Save unit layout for round-reset
void SaveSnapshot(Unit units[], int unitCount, UnitSnapshot snaps[], int *snapCount)
{
    *snapCount = unitCount;
    for (int i = 0; i < unitCount; i++)
    {
        snaps[i].typeIndex = units[i].typeIndex;
        snaps[i].position = units[i].position;
        snaps[i].team     = units[i].team;
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
            snaps[i].abilities[a] = units[i].abilities[a];
    }
}

// Restore units from snapshot (full HP, ready to fight again)
void RestoreSnapshot(Unit units[], int *unitCount, UnitSnapshot snaps[], int snapCount)
{
    *unitCount = snapCount;
    for (int i = 0; i < snapCount; i++)
    {
        const UnitStats *stats = &UNIT_STATS[snaps[i].typeIndex];
        units[i] = (Unit){
            .typeIndex      = snaps[i].typeIndex,
            .position       = snaps[i].position,
            .team           = snaps[i].team,
            .currentHealth  = stats->health,
            .attackCooldown = 0.0f,
            .targetIndex    = -1,
            .active         = true,
            .selected       = false,
            .dragging       = false,
            .facingAngle    = (snaps[i].team == TEAM_BLUE) ? 180.0f : 0.0f,
            .currentAnim    = ANIM_IDLE,
            .animFrame      = GetRandomValue(0, 999),
            .scaleOverride  = 1.0f,
            .hpMultiplier   = 1.0f,
            .dmgMultiplier  = 1.0f,
        };
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
            units[i].abilities[a] = snaps[i].abilities[a];
    }
}

//------------------------------------------------------------------------------------
// Modifier Helpers
//------------------------------------------------------------------------------------
bool UnitHasModifier(Modifier modifiers[], int unitIndex, ModifierType type)
{
    for (int m = 0; m < MAX_MODIFIERS; m++)
        if (modifiers[m].active && modifiers[m].unitIndex == unitIndex && modifiers[m].type == type)
            return true;
    return false;
}

float GetModifierValue(Modifier modifiers[], int unitIndex, ModifierType type)
{
    float best = 0.0f;
    for (int m = 0; m < MAX_MODIFIERS; m++)
        if (modifiers[m].active && modifiers[m].unitIndex == unitIndex && modifiers[m].type == type)
            if (modifiers[m].value > best) best = modifiers[m].value;
    return best;
}

void AddModifier(Modifier modifiers[], int unitIndex, ModifierType type, float duration, float value)
{
    // Spell Protect blocks negative modifiers (stun)
    if (type == MOD_STUN && UnitHasModifier(modifiers, unitIndex, MOD_SPELL_PROTECT))
        return;

    // Dedup: if same (type, unitIndex) already active, refresh duration to max
    for (int m = 0; m < MAX_MODIFIERS; m++) {
        if (modifiers[m].active && modifiers[m].unitIndex == unitIndex && modifiers[m].type == type) {
            if (duration > modifiers[m].duration)
                modifiers[m].duration = duration;
            if (duration > modifiers[m].maxDuration)
                modifiers[m].maxDuration = duration;
            if (value > modifiers[m].value)
                modifiers[m].value = value;
            return;
        }
    }

    for (int m = 0; m < MAX_MODIFIERS; m++) {
        if (!modifiers[m].active) {
            modifiers[m] = (Modifier){ .type = type, .unitIndex = unitIndex,
                .duration = duration, .maxDuration = duration, .value = value, .active = true };
            return;
        }
    }
}

void ClearAllModifiers(Modifier modifiers[])
{
    for (int m = 0; m < MAX_MODIFIERS; m++) modifiers[m].active = false;
}

//------------------------------------------------------------------------------------
// Projectile Helpers
//------------------------------------------------------------------------------------
void SpawnProjectile(Projectile projectiles[], ProjectileType type,
    Vector3 startPos, int targetIndex, int sourceIndex, Team sourceTeam, int level,
    float speed, float damage, float stunDur, Color color)
{
    for (int p = 0; p < MAX_PROJECTILES; p++) {
        if (!projectiles[p].active) {
            projectiles[p] = (Projectile){
                .type = type, .position = (Vector3){ startPos.x, startPos.y + 3.0f, startPos.z },
                .targetIndex = targetIndex, .sourceIndex = sourceIndex, .sourceTeam = sourceTeam,
                .speed = speed, .damage = damage, .stunDuration = stunDur,
                .bouncesRemaining = 0, .bounceRange = 0, .lastHitUnit = -1,
                .level = level, .color = color, .active = true,
            };
            return;
        }
    }
}

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

void ClearAllProjectiles(Projectile projectiles[])
{
    for (int p = 0; p < MAX_PROJECTILES; p++) projectiles[p].active = false;
}

//------------------------------------------------------------------------------------
// Particle Helpers
//------------------------------------------------------------------------------------
void ClearAllParticles(Particle particles[])
{
    for (int i = 0; i < MAX_PARTICLES; i++) particles[i].active = false;
}

void SpawnParticle(Particle particles[], Vector3 pos, Vector3 vel, float life, float size, Color color)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (particles[i].active) continue;
        particles[i] = (Particle){
            .position = pos, .velocity = vel,
            .life = life, .maxLife = life,
            .color = color, .size = size, .active = true
        };
        return;
    }
}

void UpdateParticles(Particle particles[], float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!particles[i].active) continue;
        particles[i].life -= dt;
        if (particles[i].life <= 0) { particles[i].active = false; continue; }
        particles[i].position.x += particles[i].velocity.x * dt;
        particles[i].position.y += particles[i].velocity.y * dt;
        particles[i].position.z += particles[i].velocity.z * dt;
        // Gravity
        particles[i].velocity.y -= 15.0f * dt;
        // Fade out
        float alpha = particles[i].life / particles[i].maxLife;
        particles[i].color.a = (unsigned char)(255.0f * alpha);
    }
}

//------------------------------------------------------------------------------------
// Shop & Inventory Helpers
//------------------------------------------------------------------------------------
void RollShop(ShopSlot shopSlots[], int *gold, int cost)
{
    if (*gold < cost) return;
    *gold -= cost;
    for (int i = 0; i < MAX_SHOP_SLOTS; i++) {
        shopSlots[i].abilityId = GetRandomValue(0, ABILITY_COUNT - 1);
        shopSlots[i].level = 0;
    }
}

void BuyAbility(ShopSlot *slot, InventorySlot inventory[], Unit units[], int unitCount, int *gold)
{
    if (slot->abilityId < 0) return;
    int cost = ABILITY_DEFS[slot->abilityId].goldCost;
    if (*gold < cost) return;

    // Auto-combine: check if this ability already exists on a blue unit or in inventory
    // Upgrade existing instance instead of creating duplicate
    for (int i = 0; i < unitCount; i++) {
        if (!units[i].active || units[i].team != TEAM_BLUE) continue;
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
            if (units[i].abilities[a].abilityId == slot->abilityId &&
                units[i].abilities[a].level < ABILITY_MAX_LEVELS - 1) {
                units[i].abilities[a].level++;
                *gold -= cost;
                slot->abilityId = -1;
                return;
            }
        }
    }
    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) {
        if (inventory[i].abilityId == slot->abilityId &&
            inventory[i].level < ABILITY_MAX_LEVELS - 1) {
            inventory[i].level++;
            *gold -= cost;
            slot->abilityId = -1;
            return;
        }
    }

    // No existing copy — place in first empty inventory slot
    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) {
        if (inventory[i].abilityId < 0) {
            inventory[i].abilityId = slot->abilityId;
            inventory[i].level = slot->level;
            *gold -= cost;
            slot->abilityId = -1;
            return;
        }
    }
    // Inventory full — do nothing
}

void AssignRandomAbilities(Unit *unit, int numAbilities)
{
    for (int a = 0; a < numAbilities && a < MAX_ABILITIES_PER_UNIT; a++) {
        unit->abilities[a].abilityId = GetRandomValue(0, ABILITY_COUNT - 1);
        unit->abilities[a].level = GetRandomValue(0, 1);
        unit->abilities[a].cooldownRemaining = 0;
        unit->abilities[a].triggered = false;
    }
}

//------------------------------------------------------------------------------------
// Floating Text Helpers
//------------------------------------------------------------------------------------
void SpawnFloatingText(FloatingText texts[], Vector3 pos, const char *str, Color color, float life)
{
    for (int i = 0; i < MAX_FLOATING_TEXTS; i++) {
        if (!texts[i].active) {
            texts[i].position = pos;
            texts[i].position.y += 5.0f; // start slightly above unit
            strncpy(texts[i].text, str, 31);
            texts[i].text[31] = '\0';
            texts[i].color = color;
            texts[i].life = life;
            texts[i].maxLife = life;
            texts[i].active = true;
            return;
        }
    }
}

void UpdateFloatingTexts(FloatingText texts[], float dt)
{
    for (int i = 0; i < MAX_FLOATING_TEXTS; i++) {
        if (!texts[i].active) continue;
        texts[i].life -= dt;
        if (texts[i].life <= 0) { texts[i].active = false; continue; }
        texts[i].position.y += 15.0f * dt; // drift upward
    }
}

void ClearAllFloatingTexts(FloatingText texts[])
{
    for (int i = 0; i < MAX_FLOATING_TEXTS; i++) texts[i].active = false;
}

//------------------------------------------------------------------------------------
// Screen Shake Helpers
//------------------------------------------------------------------------------------
void TriggerShake(ScreenShake *shake, float intensity, float duration)
{
    // Only override if new shake is stronger than remaining shake
    float remaining = shake->intensity * (shake->duration > 0 ? shake->timer / shake->duration : 0);
    if (intensity > remaining) {
        shake->intensity = intensity;
        shake->duration = duration;
        shake->timer = duration;
    }
}

void UpdateShake(ScreenShake *shake, float dt)
{
    if (shake->timer <= 0) {
        shake->offset = (Vector3){ 0, 0, 0 };
        return;
    }
    shake->timer -= dt;
    if (shake->timer <= 0) {
        shake->timer = 0;
        shake->offset = (Vector3){ 0, 0, 0 };
        return;
    }
    float factor = shake->intensity * (shake->timer / shake->duration);
    shake->offset.x = ((GetRandomValue(0, 200) - 100) / 100.0f) * factor;
    shake->offset.y = ((GetRandomValue(0, 200) - 100) / 100.0f) * factor;
    shake->offset.z = 0;
}

//------------------------------------------------------------------------------------
// Drawing Helpers
//------------------------------------------------------------------------------------
void DrawArc3D(Vector3 center, float radius, float fraction, Color color)
{
    if (fraction <= 0.0f) return;
    if (fraction > 1.0f) fraction = 1.0f;
    float maxAngle = fraction * 2.0f * PI;
    float step = 0.1f;
    for (float angle = 0.0f; angle < maxAngle; angle += step) {
        float nextAngle = angle + step;
        if (nextAngle > maxAngle) nextAngle = maxAngle;
        Vector3 a = { center.x + cosf(angle) * radius, center.y, center.z + sinf(angle) * radius };
        Vector3 b = { center.x + cosf(nextAngle) * radius, center.y, center.z + sinf(nextAngle) * radius };
        DrawLine3D(a, b, color);
    }
}

//------------------------------------------------------------------------------------
// Fissure Helpers
//------------------------------------------------------------------------------------
void SpawnFissure(Fissure fissures[], Vector3 casterPos, Vector3 targetPos,
    float length, float width, float duration, Team team, int sourceIndex)
{
    float dx = targetPos.x - casterPos.x;
    float dz = targetPos.z - casterPos.z;
    float angle = atan2f(dx, dz) * (180.0f / PI);
    float dist = sqrtf(dx * dx + dz * dz);
    // Place fissure center halfway along the direction
    float halfLen = length / 2.0f;
    float norm = (dist > 0.001f) ? 1.0f / dist : 0.0f;
    Vector3 center = {
        casterPos.x + dx * norm * halfLen,
        0.0f,
        casterPos.z + dz * norm * halfLen,
    };
    for (int i = 0; i < MAX_FISSURES; i++) {
        if (!fissures[i].active) {
            fissures[i] = (Fissure){
                .position = center, .rotation = angle,
                .length = length, .width = width,
                .duration = duration, .active = true,
                .sourceTeam = team, .sourceIndex = sourceIndex,
            };
            return;
        }
    }
}

void UpdateFissures(Fissure fissures[], float dt)
{
    for (int i = 0; i < MAX_FISSURES; i++) {
        if (!fissures[i].active) continue;
        fissures[i].duration -= dt;
        if (fissures[i].duration <= 0) fissures[i].active = false;
    }
}

void ClearAllFissures(Fissure fissures[])
{
    for (int i = 0; i < MAX_FISSURES; i++) fissures[i].active = false;
}

// Check if a point collides with any fissure (for movement blocking)
bool CheckFissureCollision(Fissure fissures[], Vector3 pos, float unitRadius)
{
    for (int i = 0; i < MAX_FISSURES; i++) {
        if (!fissures[i].active) continue;
        // Transform pos into fissure-local space
        float dx = pos.x - fissures[i].position.x;
        float dz = pos.z - fissures[i].position.z;
        float rad = fissures[i].rotation * (PI / 180.0f);
        float cosA = cosf(-rad), sinA = sinf(-rad);
        float localX = dx * cosA - dz * sinA;
        float localZ = dx * sinA + dz * cosA;
        float halfL = fissures[i].length / 2.0f + unitRadius;
        float halfW = fissures[i].width / 2.0f + unitRadius;
        if (fabsf(localX) < halfW && fabsf(localZ) < halfL)
            return true;
    }
    return false;
}

// Resolve collision: return the closest valid position outside fissures
Vector3 ResolveFissureCollision(Fissure fissures[], Vector3 pos, Vector3 oldPos, float unitRadius)
{
    (void)oldPos; // reserved for future fallback logic
    for (int i = 0; i < MAX_FISSURES; i++) {
        if (!fissures[i].active) continue;
        float dx = pos.x - fissures[i].position.x;
        float dz = pos.z - fissures[i].position.z;
        float rad = fissures[i].rotation * (PI / 180.0f);
        float cosA = cosf(-rad), sinA = sinf(-rad);
        float localX = dx * cosA - dz * sinA;
        float localZ = dx * sinA + dz * cosA;
        float halfL = fissures[i].length / 2.0f + unitRadius;
        float halfW = fissures[i].width / 2.0f + unitRadius;
        if (fabsf(localX) < halfW && fabsf(localZ) < halfL) {
            // Push out along the shortest axis
            float overlapX = halfW - fabsf(localX);
            float overlapZ = halfL - fabsf(localZ);
            if (overlapX < overlapZ)
                localX += (localX >= 0 ? overlapX : -overlapX);
            else
                localZ += (localZ >= 0 ? overlapZ : -overlapZ);
            // Transform back to world space
            float cosB = cosf(rad), sinB = sinf(rad);
            pos.x = fissures[i].position.x + localX * cosB - localZ * sinB;
            pos.z = fissures[i].position.z + localX * sinB + localZ * cosB;
        }
    }
    return pos;
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
    // Check all units along the fissure line
    for (int j = 0; j < state->unitCount; j++) {
        if (j == caster || !state->units[j].active) continue;
        if (UnitHasModifier(state->modifiers, j, MOD_INVULNERABLE)) continue;
        // Distance from unit to fissure line (approximate with distance to center line)
        float ux = state->units[j].position.x - state->units[caster].position.x;
        float uz = state->units[j].position.z - state->units[caster].position.z;
        // Project onto fissure direction
        float proj = (ux * dx + uz * dz) * norm * norm;
        if (proj < 0 || proj > length) continue;
        // Perpendicular distance
        float perpX = ux - dx * norm * proj;
        float perpZ = uz - dz * norm * proj;
        float perpDist = sqrtf(perpX * perpX + perpZ * perpZ);
        if (perpDist <= width + 3.0f) { // slightly wider than fissure
            state->units[j].currentHealth -= damage;
            if (state->units[j].currentHealth <= 0) state->units[j].active = false;
        }
    }
    TriggerShake(state->shake, 6.0f, 0.3f);
    slot->cooldownRemaining = def->cooldown[slot->level];
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
                // Clamp to max HP
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
        // Find the craggy armor ability on defender to get stun duration
        float stunDur = 1.0f; // default
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

//------------------------------------------------------------------------------------
// Wave Spawning System
//------------------------------------------------------------------------------------

// Assign N random non-duplicate abilities at a specific level
static void AssignAbilitiesAtLevel(Unit *unit, int numAbilities, int level)
{
    int used[ABILITY_COUNT] = {0};
    for (int a = 0; a < numAbilities && a < MAX_ABILITIES_PER_UNIT; a++) {
        int id;
        int attempts = 0;
        do {
            id = GetRandomValue(0, ABILITY_COUNT - 1);
            attempts++;
        } while (used[id] && attempts < 50);
        used[id] = 1;
        unit->abilities[a].abilityId = id;
        unit->abilities[a].level = level;
        unit->abilities[a].cooldownRemaining = 0;
        unit->abilities[a].triggered = false;
    }
}

// Find a valid spawn position on the red half (Z < 0), not overlapping others
Vector3 FindValidSpawnPos(Unit units[], int unitCount, float minDist)
{
    for (int attempt = 0; attempt < 30; attempt++) {
        float x = (float)GetRandomValue(-80, 80);
        float z = (float)GetRandomValue(-90, -20);
        bool valid = true;
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active) continue;
            float dx = units[i].position.x - x;
            float dz = units[i].position.z - z;
            if (sqrtf(dx*dx + dz*dz) < minDist) { valid = false; break; }
        }
        if (valid) return (Vector3){ x, 0.0f, z };
    }
    // Fallback: just pick a random spot
    return (Vector3){ (float)GetRandomValue(-80, 80), 0.0f, (float)GetRandomValue(-90, -20) };
}

// Remove all red (enemy) units
void ClearRedUnits(Unit units[], int *unitCount)
{
    // Compact: move active blues to front, drop reds
    int write = 0;
    for (int read = 0; read < *unitCount; read++) {
        if (units[read].team == TEAM_RED) continue;
        if (write != read) units[write] = units[read];
        write++;
    }
    *unitCount = write;
}

// Remove inactive blue units and compact the array
void CompactBlueUnits(Unit units[], int *unitCount)
{
    int write = 0;
    for (int read = 0; read < *unitCount; read++) {
        if (units[read].team == TEAM_BLUE && !units[read].active) continue;
        if (write != read) units[write] = units[read];
        write++;
    }
    *unitCount = write;
}

// Static wave definitions for rounds 1-5
static const WaveDef WAVE_DEFS[TOTAL_ROUNDS] = {
    // Round 1: "Skirmish" — no abilities
    { .count = 3, .entries = {
        { .unitType = 0, .numAbilities = 0, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 0, .numAbilities = 0, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 0, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
    }},
    // Round 2: "Scouts" — 1 ability each (level 0)
    { .count = 4, .entries = {
        { .unitType = 0, .numAbilities = 1, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 0, .numAbilities = 1, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 1, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 1, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
    }},
    // Round 3: "Veterans" — 2 abilities level 0 each
    { .count = 5, .entries = {
        { .unitType = 0, .numAbilities = 2, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 0, .numAbilities = 2, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 0, .numAbilities = 1, .abilityLevel = 1, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 2, .abilityLevel = 0, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 1, .abilityLevel = 1, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
    }},
    // Round 4: "Elite Squad" — 2 abilities level 1 each
    { .count = 5, .entries = {
        { .unitType = 0, .numAbilities = 2, .abilityLevel = 1, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 0, .numAbilities = 2, .abilityLevel = 1, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 2, .abilityLevel = 1, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 2, .abilityLevel = 1, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
        { .unitType = 1, .numAbilities = 2, .abilityLevel = 1, .hpMult = 1.0f, .dmgMult = 1.0f, .scaleMult = 1.0f },
    }},
    // Round 5: "BOSS" — single massive unit, 4 abilities all level 2
    { .count = 1, .entries = {
        { .unitType = -1, .numAbilities = 4, .abilityLevel = 2, .hpMult = 8.0f, .dmgMult = 3.0f, .scaleMult = 2.5f },
    }},
};

// Spawn a wave of enemies for the given round (0-indexed)
void SpawnWave(Unit units[], int *unitCount, int round, int unitTypeCount)
{
    if (round < TOTAL_ROUNDS) {
        // Scripted wave (rounds 0-4)
        const WaveDef *wave = &WAVE_DEFS[round];
        for (int e = 0; e < wave->count; e++) {
            int type = wave->entries[e].unitType;
            if (type < 0) type = GetRandomValue(0, unitTypeCount - 1);
            if (SpawnUnit(units, unitCount, type, TEAM_RED)) {
                Unit *u = &units[*unitCount - 1];
                u->position = FindValidSpawnPos(units, *unitCount, 10.0f);
                u->scaleOverride = wave->entries[e].scaleMult;
                u->hpMultiplier = wave->entries[e].hpMult;
                u->dmgMultiplier = wave->entries[e].dmgMult;
                u->currentHealth = UNIT_STATS[type].health * wave->entries[e].hpMult;
                if (wave->entries[e].numAbilities > 0) {
                    AssignAbilitiesAtLevel(u, wave->entries[e].numAbilities, wave->entries[e].abilityLevel);
                }
            }
        }
    } else {
        // Infinite scaling (round 5+)
        int extraRounds = round - TOTAL_ROUNDS;  // 0 for round 6, 1 for round 7, etc.
        int enemyCount = 3 + (extraRounds + 1);
        if (enemyCount > MAX_WAVE_ENEMIES) enemyCount = MAX_WAVE_ENEMIES;
        float hpScale = powf(1.25f, (float)(extraRounds + 1));
        float dmgScale = powf(1.15f, (float)(extraRounds + 1));
        for (int e = 0; e < enemyCount; e++) {
            int type = GetRandomValue(0, unitTypeCount - 1);
            if (SpawnUnit(units, unitCount, type, TEAM_RED)) {
                Unit *u = &units[*unitCount - 1];
                u->position = FindValidSpawnPos(units, *unitCount, 10.0f);
                u->scaleOverride = 1.0f;
                u->hpMultiplier = hpScale;
                u->dmgMultiplier = dmgScale;
                u->currentHealth = UNIT_STATS[type].health * hpScale;
                int numAb = GetRandomValue(2, 4);
                int abLevel = GetRandomValue(1, 2);
                AssignAbilitiesAtLevel(u, numAb, abLevel);
            }
        }
    }
}

//------------------------------------------------------------------------------------
// NFC Unit Code Parse / Format
//------------------------------------------------------------------------------------

// Look up ability ID by 2-char abbreviation, returns -1 if not found
static int LookupAbilityAbbrev(const char *abbrev)
{
    for (int i = 0; i < ABILITY_COUNT; i++) {
        if (ABILITY_DEFS[i].abbrev[0] == abbrev[0] && ABILITY_DEFS[i].abbrev[1] == abbrev[1])
            return i;
    }
    return -1;
}

// Parse a unit code string into type index and abilities.
// Format: {type_digit}{slot1}{slot2}{slot3}{slot4}
// Each slot is 3 chars (abbrev + level 1-3) or 2 chars "XX" (empty).
// Single-digit input = legacy format (type with no abilities).
// Returns true on success.
bool ParseUnitCode(const char *code, int *outTypeIndex, AbilitySlot outAbilities[MAX_ABILITIES_PER_UNIT])
{
    if (!code || !code[0]) {
        fprintf(stderr, "[ParseUnitCode] Empty input\n");
        return false;
    }

    // Initialize abilities to empty
    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
        outAbilities[a] = (AbilitySlot){ .abilityId = -1, .level = 0,
            .cooldownRemaining = 0, .triggered = false };
    }

    // Type digit
    if (code[0] < '0' || code[0] > '5') {
        fprintf(stderr, "[ParseUnitCode] Invalid type digit '%c'\n", code[0]);
        return false;
    }
    *outTypeIndex = code[0] - '0';

    // Legacy format: single digit = type with no abilities
    if (code[1] == '\0') return true;

    // Parse 4 ability slots
    const char *p = code + 1;
    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
        if (*p == '\0') break;  // fewer than 4 slots is OK

        // Check for empty slot "XX"
        if (p[0] == 'X' && p[1] == 'X') {
            p += 2;
            continue;
        }

        // Need at least 3 chars: 2-char abbrev + 1-digit level
        if (!p[0] || !p[1] || !p[2]) {
            fprintf(stderr, "[ParseUnitCode] Truncated ability at slot %d\n", a);
            return false;
        }

        char abbrev[3] = { p[0], p[1], '\0' };
        int abilityId = LookupAbilityAbbrev(abbrev);
        if (abilityId < 0) {
            fprintf(stderr, "[ParseUnitCode] Unknown ability '%s' at slot %d\n", abbrev, a);
            return false;
        }

        int level = p[2] - '0';
        if (level < 1 || level > 3) {
            fprintf(stderr, "[ParseUnitCode] Invalid level '%c' at slot %d\n", p[2], a);
            return false;
        }

        outAbilities[a].abilityId = abilityId;
        outAbilities[a].level = level - 1;  // displayed 1-3, stored 0-2
        p += 3;
    }

    return true;
}

// Format a unit's type and abilities into a unit code string.
// Returns the number of characters written (excluding null terminator).
// buf must be at least 14 bytes.
int FormatUnitCode(int typeIndex, const AbilitySlot abilities[MAX_ABILITIES_PER_UNIT], char *buf, int bufSize)
{
    if (bufSize < 14) { buf[0] = '\0'; return 0; }

    int pos = 0;
    buf[pos++] = '0' + (typeIndex % 6);

    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
        if (abilities[a].abilityId >= 0 && abilities[a].abilityId < ABILITY_COUNT) {
            const char *abbrev = ABILITY_DEFS[abilities[a].abilityId].abbrev;
            buf[pos++] = abbrev[0];
            buf[pos++] = abbrev[1];
            buf[pos++] = '1' + abilities[a].level;  // stored 0-2, displayed 1-3
        } else {
            buf[pos++] = 'X';
            buf[pos++] = 'X';
        }
    }
    buf[pos] = '\0';
    return pos;
}
