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

// Fissure helpers
void SpawnFissure(Fissure fissures[], Vector3 casterPos, Vector3 targetPos,
    float length, float width, float duration, Team team, int sourceIndex);
void UpdateFissures(Fissure fissures[], float dt);
void ClearAllFissures(Fissure fissures[]);
bool CheckFissureCollision(Fissure fissures[], Vector3 pos, float unitRadius);
Vector3 ResolveFissureCollision(Fissure fissures[], Vector3 pos, Vector3 oldPos, float unitRadius);

// Drawing helpers
void DrawArc3D(Vector3 center, float radius, float fraction, Color color);

// Ability casting handlers (return true if cast succeeded)
bool CastMagicMissile(CombatState *state, int caster, AbilitySlot *slot, int target);
bool CastVacuum(CombatState *state, int caster, AbilitySlot *slot);
bool CastChainFrost(CombatState *state, int caster, AbilitySlot *slot, int target);
bool CastBloodRage(CombatState *state, int caster, AbilitySlot *slot);
bool CastEarthquake(CombatState *state, int caster, AbilitySlot *slot);
bool CastSpellProtect(CombatState *state, int caster, AbilitySlot *slot);
bool CastCraggyArmor(CombatState *state, int caster, AbilitySlot *slot);
bool CastStoneGaze(CombatState *state, int caster, AbilitySlot *slot);
bool CastFissure(CombatState *state, int caster, AbilitySlot *slot, int target);

// Passive ability checks
void CheckPassiveSunder(CombatState *state, int unitIndex);

// On-hit checks
void CheckCraggyArmorRetaliation(CombatState *state, int attacker, int defender);

// Shared combat helpers
int FindHighestHPAlly(Unit units[], int unitCount, int selfIndex);

// Wave spawning helpers
Vector3 FindValidSpawnPos(Unit units[], int unitCount, float minDist);
void SpawnWave(Unit units[], int *unitCount, int round, int unitTypeCount);
void ClearRedUnits(Unit units[], int *unitCount);
