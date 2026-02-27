#include "game.h"
#include "helpers.h"
#include <math.h>
#include <string.h>

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
    };
    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
        units[*unitCount].abilities[a] = (AbilitySlot){ .abilityId = -1, .level = 0,
            .cooldownRemaining = 0, .triggered = false };
    }
    units[*unitCount].nextAbilitySlot = 0;
    // Red team AI: assign random abilities
    if (team == TEAM_RED) AssignRandomAbilities(&units[*unitCount], GetRandomValue(1, 2));
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
    for (int m = 0; m < MAX_MODIFIERS; m++) {
        if (!modifiers[m].active) {
            modifiers[m] = (Modifier){ .type = type, .unitIndex = unitIndex,
                .duration = duration, .value = value, .active = true };
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
