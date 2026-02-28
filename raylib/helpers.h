#pragma once
#include "game.h"

// Unit utilities
int CountTeamUnits(Unit units[], int unitCount, Team team);
bool SpawnUnit(Unit units[], int *unitCount, int typeIndex, Team team);
BoundingBox GetUnitBounds(Unit *unit, UnitType *type);
Color GetTeamTint(Team team);
float DistXZ(Vector3 a, Vector3 b);
int FindClosestEnemy(Unit units[], int unitCount, int selfIndex);
void CountTeams(Unit units[], int unitCount, int *blueAlive, int *redAlive);
void SaveSnapshot(Unit units[], int unitCount, UnitSnapshot snaps[], int *snapCount);
void RestoreSnapshot(Unit units[], int *unitCount, UnitSnapshot snaps[], int snapCount);

// Modifier helpers
bool UnitHasModifier(Modifier modifiers[], int unitIndex, ModifierType type);
float GetModifierValue(Modifier modifiers[], int unitIndex, ModifierType type);
void AddModifier(Modifier modifiers[], int unitIndex, ModifierType type, float duration, float value);
void ClearAllModifiers(Modifier modifiers[]);

// Projectile helpers
void SpawnProjectile(Projectile projectiles[], ProjectileType type,
    Vector3 startPos, int targetIndex, int sourceIndex, Team sourceTeam, int level,
    float speed, float damage, float stunDur, Color color);
void SpawnChainFrostProjectile(Projectile projectiles[],
    Vector3 startPos, int targetIndex, int sourceIndex, Team sourceTeam, int level,
    float speed, float damage, int bounces, float bounceRange);
int FindChainFrostTarget(Unit units[], int unitCount, Vector3 fromPos,
    Team sourceTeam, int excludeIndex, float range);
void ClearAllProjectiles(Projectile projectiles[]);

// Particle helpers
void ClearAllParticles(Particle particles[]);
void SpawnParticle(Particle particles[], Vector3 pos, Vector3 vel, float life, float size, Color color);
void UpdateParticles(Particle particles[], float dt);

// Shop & inventory helpers
void RollShop(ShopSlot shopSlots[], int *gold, int cost);
void BuyAbility(ShopSlot *slot, InventorySlot inventory[], Unit units[], int unitCount, int *gold);
void AssignRandomAbilities(Unit *unit, int numAbilities);

// Floating text helpers
void SpawnFloatingText(FloatingText texts[], Vector3 pos, const char *str, Color color, float life);
void UpdateFloatingTexts(FloatingText texts[], float dt);
void ClearAllFloatingTexts(FloatingText texts[]);

// Screen shake helpers
void TriggerShake(ScreenShake *shake, float intensity, float duration);
void UpdateShake(ScreenShake *shake, float dt);
