#pragma once
#include "game.h"

//------------------------------------------------------------------------------------
// Plaza Sub-States
//------------------------------------------------------------------------------------
typedef enum {
    PLAZA_ROAMING,   // enemies wander freely
    PLAZA_SCARED,    // brief freeze + scared anim (0.5s)
    PLAZA_FLEEING,   // enemies run toward nearest edge
} PlazaSubState;

//------------------------------------------------------------------------------------
// Per-unit roaming data for plaza phase
//------------------------------------------------------------------------------------
typedef struct {
    Vector3 roamTarget;
    float   roamWaitTimer;
    bool    isScared;
    int     zoneIndex;
} PlazaUnitData;

//------------------------------------------------------------------------------------
// Plaza Functions
//------------------------------------------------------------------------------------
// Spawn a set of roaming red enemies for the plaza
void PlazaSpawnEnemies(Unit units[], int *unitCount, int unitTypeCount, PlazaUnitData plazaData[]);

// Update roaming AI (wander, pause, smooth rotation)
void PlazaUpdateRoaming(Unit units[], int unitCount, PlazaUnitData plazaData[], float dt);

// Trigger the scared reaction (freeze + ANIM_SCARED)
void PlazaTriggerScared(Unit units[], int unitCount, PlazaUnitData plazaData[],
                        PlazaSubState *plazaState, float *plazaTimer);

// Update flee behavior (run to edges, poof when off-board)
// Returns true when all red units are gone
bool PlazaUpdateFlee(Unit units[], int unitCount, PlazaUnitData plazaData[],
                     Particle particles[], float dt);

// Smoke poof a single unit
void PlazaPoofUnit(Unit *unit, Particle particles[]);

// Draw interactive 3D objects (door, trophy) with cel shading white tint — call inside BeginMode3D
void PlazaDrawObjects(Model doorModel, Model trophyModel, Vector3 doorPos, Vector3 trophyPos,
                      Camera camera, bool doorHover, bool trophyHover, float sparkleTimer);

// Check which 3D object the mouse is hovering/clicking
// Returns: 0=none, 1=trophy, 2=door
int PlazaCheckObjectHover(Camera camera, Vector3 trophyPos, Vector3 doorPos);
