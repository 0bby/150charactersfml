#include "plaza.h"
#include "helpers.h"
#include "raymath.h"
#include <math.h>

#define PLAZA_ROAM_SPEED_FACTOR 0.5f   // 50% of normal move speed
#define PLAZA_ZONE_HALF_RANGE   30.0f  // wander within +-30 of zone center
#define PLAZA_WAIT_MIN          1.0f
#define PLAZA_WAIT_MAX          4.0f
#define PLAZA_FLEE_SPEED_FACTOR 2.0f   // 2x speed when fleeing
#define PLAZA_EDGE_LIMIT        90.0f  // despawn when past this
#define PLAZA_ENEMY_COUNT       5      // number of roaming enemies
#define PLAZA_MOVE_SPEED        30.0f  // base move speed (units/sec)
#define PLAZA_TURN_SPEED        8.0f   // rotation lerp speed

//------------------------------------------------------------------------------------
// Spawn roaming enemies for the plaza
//------------------------------------------------------------------------------------
// Zone centers — 5 spread-out regions across the arena
static const float zoneCentersX[5] = { -50.0f, 50.0f, 0.0f, -50.0f, 50.0f };
static const float zoneCentersZ[5] = { -40.0f, -40.0f, 0.0f,  40.0f, 40.0f };

void PlazaSpawnEnemies(Unit units[], int *unitCount, int unitTypeCount, PlazaUnitData plazaData[])
{
    (void)unitTypeCount; // use VALID_UNIT_TYPES instead
    for (int i = 0; i < PLAZA_ENEMY_COUNT; i++) {
        int type = VALID_UNIT_TYPES[GetRandomValue(0, VALID_UNIT_TYPE_COUNT - 1)];
        if (SpawnUnit(units, unitCount, type, TEAM_RED)) {
            int idx = *unitCount - 1;
            Unit *u = &units[idx];
            u->position.x = zoneCentersX[i] + (float)GetRandomValue(-15, 15);
            u->position.z = zoneCentersZ[i] + (float)GetRandomValue(-15, 15);
            u->position.y = 0.0f;
            u->facingAngle = (float)GetRandomValue(0, 360);
            u->currentAnim = ANIM_IDLE;

            PlazaUnitData *pd = &plazaData[idx];
            pd->zoneIndex = i;
            pd->isScared = false;
            // Initial roam target within their zone
            pd->roamTarget.x = zoneCentersX[i] + (float)GetRandomValue((int)(-PLAZA_ZONE_HALF_RANGE), (int)(PLAZA_ZONE_HALF_RANGE));
            pd->roamTarget.y = 0.0f;
            pd->roamTarget.z = zoneCentersZ[i] + (float)GetRandomValue((int)(-PLAZA_ZONE_HALF_RANGE), (int)(PLAZA_ZONE_HALF_RANGE));
            // Brief idle before first walk so they don't all move frame 1
            pd->roamWaitTimer = PLAZA_WAIT_MIN +
                (float)GetRandomValue(0, (int)((PLAZA_WAIT_MAX - PLAZA_WAIT_MIN) * 10)) / 10.0f;
        }
    }
}

//------------------------------------------------------------------------------------
// Pick a new random roam target
//------------------------------------------------------------------------------------
static void PickRoamTarget(PlazaUnitData *pd, Unit units[], int unitCount,
                           PlazaUnitData plazaData[], int selfIndex)
{
    float cx = zoneCentersX[pd->zoneIndex];
    float cz = zoneCentersZ[pd->zoneIndex];
    float halfRange = PLAZA_ZONE_HALF_RANGE;

    for (int attempt = 0; attempt < 5; attempt++) {
        pd->roamTarget.x = cx + (float)GetRandomValue((int)(-halfRange), (int)(halfRange));
        pd->roamTarget.y = 0.0f;
        pd->roamTarget.z = cz + (float)GetRandomValue((int)(-halfRange), (int)(halfRange));

        bool tooClose = false;
        for (int j = 0; j < unitCount; j++) {
            if (j == selfIndex || !units[j].active || units[j].team != TEAM_RED) continue;
            // Check distance to other unit's current position
            float dx = pd->roamTarget.x - units[j].position.x;
            float dz = pd->roamTarget.z - units[j].position.z;
            if (sqrtf(dx * dx + dz * dz) < 20.0f) { tooClose = true; break; }
            // Check distance to other unit's roam target
            dx = pd->roamTarget.x - plazaData[j].roamTarget.x;
            dz = pd->roamTarget.z - plazaData[j].roamTarget.z;
            if (sqrtf(dx * dx + dz * dz) < 20.0f) { tooClose = true; break; }
        }
        if (!tooClose) break;
    }
    pd->roamWaitTimer = 0.0f;
}

//------------------------------------------------------------------------------------
// Roaming AI update
//------------------------------------------------------------------------------------
void PlazaUpdateRoaming(Unit units[], int unitCount, PlazaUnitData plazaData[], float dt)
{
    for (int i = 0; i < unitCount; i++) {
        if (!units[i].active || units[i].team != TEAM_RED) continue;

        PlazaUnitData *pd = &plazaData[i];

        // If waiting at destination, count down
        if (pd->roamWaitTimer > 0.0f) {
            pd->roamWaitTimer -= dt;
            units[i].currentAnim = ANIM_IDLE;
            if (pd->roamWaitTimer <= 0.0f) {
                PickRoamTarget(pd, units, unitCount, plazaData, i);
            }
            continue;
        }

        // Move toward target
        float dx = pd->roamTarget.x - units[i].position.x;
        float dz = pd->roamTarget.z - units[i].position.z;
        float dist = sqrtf(dx * dx + dz * dz);

        if (dist < 2.0f) {
            // Arrived — pause
            pd->roamWaitTimer = PLAZA_WAIT_MIN +
                (float)GetRandomValue(0, (int)((PLAZA_WAIT_MAX - PLAZA_WAIT_MIN) * 10)) / 10.0f;
            units[i].currentAnim = ANIM_IDLE;
            continue;
        }

        // Normalize direction toward roam target
        float nx = dx / dist;
        float nz = dz / dist;

        // Proactive separation steering — repel from nearby red units
        float sepX = 0.0f, sepZ = 0.0f;
        float sepRadius = UNIT_COLLISION_RADIUS * 8.0f;
        for (int j = 0; j < unitCount; j++) {
            if (i == j || !units[j].active || units[j].team != TEAM_RED) continue;
            float sx = units[i].position.x - units[j].position.x;
            float sz = units[i].position.z - units[j].position.z;
            float sd = sqrtf(sx * sx + sz * sz);
            if (sd < sepRadius && sd > 0.001f) {
                // Inverse-proportional repulsion
                float strength = (sepRadius - sd) / sepRadius;
                sepX += (sx / sd) * strength;
                sepZ += (sz / sd) * strength;
            }
        }

        // Blend separation into movement direction (0.3 weight)
        float moveX = nx * 0.7f + sepX * 0.3f;
        float moveZ = nz * 0.7f + sepZ * 0.3f;
        float moveLen = sqrtf(moveX * moveX + moveZ * moveZ);
        if (moveLen > 0.001f) {
            moveX /= moveLen;
            moveZ /= moveLen;
        }

        float speed = PLAZA_MOVE_SPEED * PLAZA_ROAM_SPEED_FACTOR * dt;
        if (speed > dist) speed = dist;
        units[i].position.x += moveX * speed;
        units[i].position.z += moveZ * speed;
        units[i].currentAnim = ANIM_WALK;

        // Smooth rotation toward movement direction
        float targetAngle = atan2f(moveX, moveZ) * (180.0f / PI);
        float angleDiff = targetAngle - units[i].facingAngle;
        // Normalize to -180..180
        while (angleDiff > 180.0f) angleDiff -= 360.0f;
        while (angleDiff < -180.0f) angleDiff += 360.0f;
        units[i].facingAngle += angleDiff * PLAZA_TURN_SPEED * dt;

        // Hard-fallback reactive collision push (prevents actual overlap)
        for (int j = 0; j < unitCount; j++) {
            if (i == j || !units[j].active) continue;
            float colX = units[i].position.x - units[j].position.x;
            float colZ = units[i].position.z - units[j].position.z;
            float cd = sqrtf(colX * colX + colZ * colZ);
            float minDist = UNIT_COLLISION_RADIUS * 5.0f;
            if (cd < minDist && cd > 0.001f) {
                float push = (minDist - cd) * 0.5f;
                units[i].position.x += (colX / cd) * push;
                units[i].position.z += (colZ / cd) * push;
            }
        }
    }
}

//------------------------------------------------------------------------------------
// Trigger scared state
//------------------------------------------------------------------------------------
void PlazaTriggerScared(Unit units[], int unitCount, PlazaUnitData plazaData[],
                        PlazaSubState *plazaState, float *plazaTimer)
{
    *plazaState = PLAZA_SCARED;
    *plazaTimer = 0.5f;

    for (int i = 0; i < unitCount; i++) {
        if (!units[i].active || units[i].team != TEAM_RED) continue;
        plazaData[i].isScared = true;
        units[i].currentAnim = ANIM_SCARED;
    }
}

//------------------------------------------------------------------------------------
// Flee update — returns true when all red units are gone
//------------------------------------------------------------------------------------
bool PlazaUpdateFlee(Unit units[], int unitCount, PlazaUnitData plazaData[] __attribute__((unused)),
                     Particle particles[], float dt)
{
    int redAlive = 0;
    for (int i = 0; i < unitCount; i++) {
        if (!units[i].active || units[i].team != TEAM_RED) continue;
        redAlive++;

        // Find nearest edge direction
        float ax = fabsf(units[i].position.x);
        float az = fabsf(units[i].position.z);
        float fx, fz;
        if (ax > az) {
            fx = (units[i].position.x > 0) ? 1.0f : -1.0f;
            fz = 0.0f;
        } else {
            fx = 0.0f;
            fz = (units[i].position.z > 0) ? 1.0f : -1.0f;
        }

        float speed = PLAZA_MOVE_SPEED * PLAZA_FLEE_SPEED_FACTOR * dt;
        units[i].position.x += fx * speed;
        units[i].position.z += fz * speed;

        // Face flee direction
        float targetAngle = atan2f(fx, fz) * (180.0f / PI);
        float angleDiff = targetAngle - units[i].facingAngle;
        while (angleDiff > 180.0f) angleDiff -= 360.0f;
        while (angleDiff < -180.0f) angleDiff += 360.0f;
        units[i].facingAngle += angleDiff * PLAZA_TURN_SPEED * dt;

        units[i].currentAnim = ANIM_SCARED;

        // Check if off-board
        if (fabsf(units[i].position.x) > PLAZA_EDGE_LIMIT ||
            fabsf(units[i].position.z) > PLAZA_EDGE_LIMIT) {
            PlazaPoofUnit(&units[i], particles);
        }
    }
    return (redAlive == 0);
}

//------------------------------------------------------------------------------------
// Smoke poof a single unit
//------------------------------------------------------------------------------------
void PlazaPoofUnit(Unit *unit, Particle particles[])
{
    Vector3 pos = unit->position;
    pos.y += 3.0f;
    for (int i = 0; i < 20; i++) {
        float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
        float speed = (float)GetRandomValue(20, 60) / 10.0f;
        Vector3 vel = {
            cosf(angle) * speed,
            (float)GetRandomValue(10, 40) / 10.0f,
            sinf(angle) * speed
        };
        int shade = GetRandomValue(160, 230);
        Color smokeColor = { (unsigned char)shade, (unsigned char)shade, (unsigned char)shade, 255 };
        float sz = (float)GetRandomValue(5, 15) / 10.0f;
        SpawnParticle(particles, pos, vel, 0.6f + (float)GetRandomValue(0, 4) / 10.0f, sz, smokeColor);
    }
    unit->active = false;
}

//------------------------------------------------------------------------------------
// Draw interactive 3D objects
//------------------------------------------------------------------------------------
void PlazaDrawObjects(Model doorModel, Model trophyModel, Vector3 doorPos, Vector3 trophyPos,
                      Camera camera __attribute__((unused)), bool doorHover, bool trophyHover, float sparkleTimer)
{
    // Compute sparkle brightness pulse (sinusoidal shimmer)
    float sparkle = 0.5f + 0.5f * sinf(sparkleTimer * 3.0f);
    unsigned char sparkleVal = (unsigned char)(200 + (int)(55.0f * sparkle));

    // Door model — cel shaded white with sparkle
    Color doorTint = doorHover
        ? (Color){255, 255, (unsigned char)(180 + (int)(75.0f * sparkle)), 255}
        : (Color){sparkleVal, sparkleVal, sparkleVal, 255};
    DrawModel(doorModel, doorPos, 1.0f, doorTint);

    // Trophy model — cel shaded white with sparkle
    Color trophyTint = trophyHover
        ? (Color){255, 255, (unsigned char)(180 + (int)(75.0f * sparkle)), 255}
        : (Color){sparkleVal, sparkleVal, sparkleVal, 255};
    DrawModel(trophyModel, trophyPos, 1.0f, trophyTint);
}

//------------------------------------------------------------------------------------
// Check object hover via ray-cast
// Returns: 0=none, 1=trophy, 2=door
//------------------------------------------------------------------------------------
int PlazaCheckObjectHover(Camera camera, Vector3 trophyPos, Vector3 doorPos)
{
    Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);

    // Trophy bounding box
    BoundingBox trophyBox = {
        { trophyPos.x - 3.0f, trophyPos.y, trophyPos.z - 3.0f },
        { trophyPos.x + 3.0f, trophyPos.y + 10.0f, trophyPos.z + 3.0f }
    };
    RayCollision trophyHit = GetRayCollisionBox(ray, trophyBox);

    // Door bounding box (scaled to match model)
    BoundingBox doorBox = {
        { doorPos.x - 6.0f, doorPos.y, doorPos.z - 3.0f },
        { doorPos.x + 6.0f, doorPos.y + 15.0f, doorPos.z + 3.0f }
    };
    RayCollision doorHit = GetRayCollisionBox(ray, doorBox);

    // Return whichever is closer
    if (trophyHit.hit && doorHit.hit) {
        return (trophyHit.distance < doorHit.distance) ? 1 : 2;
    }
    if (trophyHit.hit) return 1;
    if (doorHit.hit) return 2;
    return 0;
}
