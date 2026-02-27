/*******************************************************************************************
*
*   Unit Spawning System - raylib (Autochess)
*
*   Two-team autochess with round-based combat.
*   Prep phase: place units.  Combat phase: units fight automatically.
*   Best-of-5 rounds.
*
********************************************************************************************/

#include "raylib.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

//------------------------------------------------------------------------------------
// Data Structures & Constants
//------------------------------------------------------------------------------------
#define MAX_UNIT_TYPES 8
#define MAX_UNITS 64
#define TOTAL_ROUNDS 5
#define ATTACK_RANGE 8.0f       // how close a unit needs to be to attack
#define BLUE_TEAM_MAX_SIZE 4   // player team cap (change this to rebalance)

//------------------------------------------------------------------------------------
// Unit Stats — "Master Library" for balancing.
// Change numbers here; every spawned unit picks them up automatically.
//------------------------------------------------------------------------------------
typedef struct {
    float health;
    float movementSpeed;        // world-units per second
    float attackDamage;
    float attackSpeed;          // seconds between attacks
} UnitStats;

static const UnitStats UNIT_STATS[] = {
    /* 0  Mushroom */ { .health = 15.0f, .movementSpeed = 12.0f, .attackDamage = 3.0f, .attackSpeed = 1.2f },
    /* 1  Goblin   */ { .health =  5.0f, .movementSpeed = 20.0f, .attackDamage = 2.0f, .attackSpeed = 0.5f },
};

//------------------------------------------------------------------------------------
// Team
//------------------------------------------------------------------------------------
typedef enum { TEAM_BLUE = 0, TEAM_RED = 1 } Team;

//------------------------------------------------------------------------------------
// Game phases
//------------------------------------------------------------------------------------
typedef enum {
    PHASE_PREP,       // place / arrange units
    PHASE_COMBAT,     // units fight automatically
    PHASE_ROUND_OVER, // brief pause showing round result
    PHASE_GAME_OVER,  // all rounds finished
} GamePhase;

//------------------------------------------------------------------------------------
// Ability System (placeholder for future)
//------------------------------------------------------------------------------------
#define MAX_ABILITIES_PER_UNIT 4
#define MAX_SHOP_SLOTS 3

typedef struct {
    int abilityId;        // -1 = empty, >= 0 = ability index (future)
} AbilitySlot;

//------------------------------------------------------------------------------------
// HUD Layout Constants
//------------------------------------------------------------------------------------
#define HUD_UNIT_BAR_HEIGHT 130
#define HUD_SHOP_HEIGHT 50
#define HUD_TOTAL_HEIGHT (HUD_UNIT_BAR_HEIGHT + HUD_SHOP_HEIGHT)
#define HUD_CARD_WIDTH 180
#define HUD_CARD_HEIGHT 120
#define HUD_CARD_SPACING 10
#define HUD_PORTRAIT_SIZE 80
#define HUD_ABILITY_SLOT_SIZE 32
#define HUD_ABILITY_SLOT_GAP 4

//------------------------------------------------------------------------------------
// Unit type (visual info — model, scale, name)
//------------------------------------------------------------------------------------
typedef struct {
    const char *name;
    const char *modelPath;
    Model model;
    BoundingBox baseBounds;
    float scale;
    bool loaded;
} UnitType;

//------------------------------------------------------------------------------------
// Runtime unit instance
//------------------------------------------------------------------------------------
typedef struct {
    int typeIndex;
    Vector3 position;
    Team team;
    float currentHealth;
    float attackCooldown;     // counts down each frame
    int targetIndex;          // index of current target (-1 = none)
    bool active;
    bool selected;
    bool dragging;
    AbilitySlot abilities[MAX_ABILITIES_PER_UNIT];
} Unit;

//------------------------------------------------------------------------------------
// Snapshot of a unit for round-reset
//------------------------------------------------------------------------------------
typedef struct {
    int typeIndex;
    Vector3 position;
    Team team;
    AbilitySlot abilities[MAX_ABILITIES_PER_UNIT];
} UnitSnapshot;

//------------------------------------------------------------------------------------
// Functions
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
    };
    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
        units[*unitCount].abilities[a].abilityId = -1;
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
        };
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
            units[i].abilities[a] = snaps[i].abilities[a];
    }
}

//------------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------------
int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "Autochess — Best of 5");
    SetWindowMinSize(640, 360);

    // Camera
    float camHeight = 102.0f;
    float camDistance = 104.0f;
    float camFOV = 52.0f;
    Camera camera = { 0 };
    camera.position = (Vector3){ 0.0f, camHeight, camDistance };
    camera.target   = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up       = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy     = camFOV;
    camera.projection = CAMERA_PERSPECTIVE;

    // Unit types
    int unitTypeCount = 2;
    UnitType unitTypes[MAX_UNIT_TYPES] = { 0 };
    unitTypes[0].name = "Mushroom";
    unitTypes[0].modelPath = "MUSHROOMmixamotest.obj";
    unitTypes[0].scale = 0.1f;
    unitTypes[1].name = "Goblin";
    unitTypes[1].modelPath = "goblin.obj";
    unitTypes[1].scale = 0.1f;

    for (int i = 0; i < unitTypeCount; i++)
    {
        unitTypes[i].model = LoadModel(unitTypes[i].modelPath);
        if (unitTypes[i].model.meshCount > 0)
        {
            unitTypes[i].baseBounds = GetMeshBoundingBox(unitTypes[i].model.meshes[0]);
            unitTypes[i].loaded = true;
        }
        else unitTypes[i].loaded = false;
    }

    // Portrait render textures for HUD (one per max blue unit)
    RenderTexture2D portraits[BLUE_TEAM_MAX_SIZE];
    for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++)
        portraits[i] = LoadRenderTexture(HUD_PORTRAIT_SIZE, HUD_PORTRAIT_SIZE);

    // Dedicated camera for portrait rendering
    Camera portraitCam = { 0 };
    portraitCam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    portraitCam.fovy = 30.0f;
    portraitCam.projection = CAMERA_PERSPECTIVE;

    // Units
    Unit units[MAX_UNITS] = { 0 };
    int unitCount = 0;

    // Snapshot for round-reset
    UnitSnapshot snapshots[MAX_UNITS] = { 0 };
    int snapshotCount = 0;

    // Round / score state
    GamePhase phase = PHASE_PREP;
    int currentRound = 0;          // 0-indexed, displayed as 1-indexed
    int blueWins = 0;
    int redWins  = 0;
    float roundOverTimer = 0.0f;   // brief pause after a round ends
    const char *roundResultText = "";

    // UI button sizes (positions computed each frame for resize support)
    const int btnWidth = 150;
    const int btnHeight = 30;
    const int btnMargin = 10;
    const int playBtnW = 120;
    const int playBtnH = 40;

    SetTargetFPS(60);

    // --- NFC Bridge Subprocess ---
    FILE *nfcPipe = popen("../nfc/build/bridge", "r");
    if (nfcPipe) {
        int nfcFd = fileno(nfcPipe);
        int flags = fcntl(nfcFd, F_GETFL, 0);
        fcntl(nfcFd, F_SETFL, flags | O_NONBLOCK);
        printf("[NFC] Bridge launched\n");
    } else {
        printf("[NFC] Failed to launch bridge\n");
    }

    //==================================================================================
    // MAIN LOOP
    //==================================================================================
    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();

        // Update camera
        camera.position.y = camHeight;
        camera.position.z = camDistance;
        camera.fovy = camFOV;

        // Poll NFC bridge for tag scans (only spawn during prep)
        if (nfcPipe && phase == PHASE_PREP) {
            char nfcBuf[64];
            if (fgets(nfcBuf, sizeof(nfcBuf), nfcPipe)) {
                nfcBuf[strcspn(nfcBuf, "\r\n")] = '\0';
                if (strcmp(nfcBuf, "0") == 0) {
                    if (SpawnUnit(units, &unitCount, 1, TEAM_BLUE))
                        printf("[NFC] Tag '0' -> Spawning Goblin (Blue)\n");
                    else
                        printf("[NFC] Tag '0' -> Blue team full (%d/%d)\n", BLUE_TEAM_MAX_SIZE, BLUE_TEAM_MAX_SIZE);
                } else if (strcmp(nfcBuf, "1") == 0) {
                    if (SpawnUnit(units, &unitCount, 0, TEAM_BLUE))
                        printf("[NFC] Tag '1' -> Spawning Mushroom (Blue)\n");
                    else
                        printf("[NFC] Tag '1' -> Blue team full (%d/%d)\n", BLUE_TEAM_MAX_SIZE, BLUE_TEAM_MAX_SIZE);
                } else {
                    printf("[NFC] Unknown payload: '%s'\n", nfcBuf);
                }
            }
        }

        //------------------------------------------------------------------------------
        // PHASE: PREP — place units, click Play to start
        //------------------------------------------------------------------------------
        if (phase == PHASE_PREP)
        {
            // Smooth Y lift
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                float targetY = units[i].dragging ? 5.0f : 0.0f;
                units[i].position.y += (targetY - units[i].position.y) * 0.1f;
            }

            // Dragging
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active || !units[i].dragging) continue;
                Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
                RayCollision groundHit = GetRayCollisionQuad(ray,
                    (Vector3){ -500, 0, -500 }, (Vector3){ -500, 0, 500 },
                    (Vector3){  500, 0,  500 }, (Vector3){  500, 0, -500 });
                if (groundHit.hit)
                {
                    units[i].position.x = groundHit.point.x;
                    units[i].position.z = groundHit.point.z;
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) units[i].dragging = false;
            }

            // Clicks
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();
                int hudTop = sh - HUD_TOTAL_HEIGHT;
                int btnXBlue = btnMargin;
                int btnXRed  = sw - btnWidth - btnMargin;
                int btnYStart = hudTop - (unitTypeCount * (btnHeight + btnMargin)) - btnMargin;
                Rectangle playBtn = { (float)(sw/2 - playBtnW/2), (float)(hudTop - playBtnH - btnMargin), (float)playBtnW, (float)playBtnH };
                bool clickedButton = false;

                // Play button
                if (CheckCollisionPointRec(mouse, playBtn) && unitCount > 0)
                {
                    // Check both teams have units
                    int ba, ra;
                    CountTeams(units, unitCount, &ba, &ra);
                    if (ba > 0 && ra > 0)
                    {
                        SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
                        phase = PHASE_COMBAT;
                        // Deselect everything
                        for (int j = 0; j < unitCount; j++) { units[j].selected = false; units[j].dragging = false; }
                        clickedButton = true;
                    }
                }

                // Blue spawn buttons
                if (!clickedButton)
                {
                    for (int i = 0; i < unitTypeCount; i++)
                    {
                        Rectangle r = { (float)btnXBlue, (float)(btnYStart + i*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                        if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded)
                        { SpawnUnit(units, &unitCount, i, TEAM_BLUE); clickedButton = true; break; }
                    }
                }
                // Red spawn buttons
                if (!clickedButton)
                {
                    for (int i = 0; i < unitTypeCount; i++)
                    {
                        Rectangle r = { (float)btnXRed, (float)(btnYStart + i*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                        if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded)
                        { SpawnUnit(units, &unitCount, i, TEAM_RED); clickedButton = true; break; }
                    }
                }
                // Unit selection (skip if clicking inside HUD area)
                if (!clickedButton && mouse.y < hudTop)
                {
                    bool hitAny = false;
                    for (int i = unitCount - 1; i >= 0; i--)
                    {
                        if (!units[i].active) continue;
                        BoundingBox sb = GetUnitBounds(&units[i], &unitTypes[units[i].typeIndex]);
                        if (GetRayCollisionBox(GetScreenToWorldRay(mouse, camera), sb).hit)
                        {
                            units[i].selected = true;
                            units[i].dragging = true;
                            hitAny = true;
                            for (int j = 0; j < unitCount; j++) if (j != i) units[j].selected = false;
                            break;
                        }
                    }
                    if (!hitAny) for (int j = 0; j < unitCount; j++) units[j].selected = false;
                }
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: COMBAT — units seek + attack automatically
        //------------------------------------------------------------------------------
        else if (phase == PHASE_COMBAT)
        {
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];

                // Find target
                int target = FindClosestEnemy(units, unitCount, i);
                units[i].targetIndex = target;
                if (target < 0) continue; // no enemies left

                float dist = DistXZ(units[i].position, units[target].position);

                if (dist > ATTACK_RANGE)
                {
                    // Move toward target
                    float dx = units[target].position.x - units[i].position.x;
                    float dz = units[target].position.z - units[i].position.z;
                    float len = sqrtf(dx*dx + dz*dz);
                    if (len > 0.001f)
                    {
                        dx /= len;
                        dz /= len;
                        units[i].position.x += dx * stats->movementSpeed * dt;
                        units[i].position.z += dz * stats->movementSpeed * dt;
                    }
                }
                else
                {
                    // In range — attack on cooldown
                    units[i].attackCooldown -= dt;
                    if (units[i].attackCooldown <= 0.0f)
                    {
                        units[target].currentHealth -= stats->attackDamage;
                        units[i].attackCooldown = stats->attackSpeed;

                        // Kill check
                        if (units[target].currentHealth <= 0.0f)
                        {
                            units[target].active = false;
                        }
                    }
                }
            }

            // Smooth Y toward ground during combat
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                units[i].position.y += (0.0f - units[i].position.y) * 0.1f;
            }

            // Check round end
            int ba, ra;
            CountTeams(units, unitCount, &ba, &ra);
            if (ba == 0 || ra == 0)
            {
                if (ba > 0) { blueWins++; roundResultText = "BLUE WINS THE ROUND!"; }
                else if (ra > 0) { redWins++; roundResultText = "RED WINS THE ROUND!"; }
                else { roundResultText = "DRAW — NO SURVIVORS!"; }

                currentRound++;
                phase = PHASE_ROUND_OVER;
                roundOverTimer = 2.5f; // seconds to show result
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: ROUND_OVER — brief pause, then back to prep or game over
        //------------------------------------------------------------------------------
        else if (phase == PHASE_ROUND_OVER)
        {
            roundOverTimer -= dt;
            if (roundOverTimer <= 0.0f)
            {
                if (currentRound >= TOTAL_ROUNDS || blueWins > TOTAL_ROUNDS/2 || redWins > TOTAL_ROUNDS/2)
                {
                    phase = PHASE_GAME_OVER;
                }
                else
                {
                    // Restore units to pre-round positions & full HP
                    RestoreSnapshot(units, &unitCount, snapshots, snapshotCount);
                    phase = PHASE_PREP;
                }
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: GAME_OVER — show final result, press R to restart
        //------------------------------------------------------------------------------
        else if (phase == PHASE_GAME_OVER)
        {
            if (IsKeyPressed(KEY_R))
            {
                // Full reset
                unitCount = 0;
                snapshotCount = 0;
                currentRound = 0;
                blueWins = 0;
                redWins = 0;
                roundResultText = "";
                phase = PHASE_PREP;
            }
        }

        //==============================================================================
        // DRAW
        //==============================================================================
        BeginDrawing();
        ClearBackground(RAYWHITE);

        // Collect active blue units for HUD
        int blueHudUnits[BLUE_TEAM_MAX_SIZE];
        int blueHudCount = 0;
        for (int i = 0; i < unitCount && blueHudCount < BLUE_TEAM_MAX_SIZE; i++) {
            if (units[i].active && units[i].team == TEAM_BLUE)
                blueHudUnits[blueHudCount++] = i;
        }

        // Render unit portraits into offscreen textures
        for (int h = 0; h < blueHudCount; h++) {
            int ui = blueHudUnits[h];
            UnitType *type = &unitTypes[units[ui].typeIndex];
            if (!type->loaded) continue;

            // Auto-center camera on model
            BoundingBox bb = type->baseBounds;
            float centerY = (bb.min.y + bb.max.y) / 2.0f * type->scale;
            float extent = (bb.max.y - bb.min.y) * type->scale;
            portraitCam.target = (Vector3){ 0.0f, centerY, 0.0f };
            portraitCam.position = (Vector3){ 0.0f, centerY, extent * 2.5f };

            BeginTextureMode(portraits[h]);
                ClearBackground((Color){ 30, 30, 40, 255 });
                BeginMode3D(portraitCam);
                    DrawModel(type->model, (Vector3){ 0, 0, 0 }, type->scale, GetTeamTint(TEAM_BLUE));
                EndMode3D();
            EndTextureMode();
        }

        BeginMode3D(camera);
            DrawGrid(20, 10.0f);

            // Draw units
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                UnitType *type = &unitTypes[units[i].typeIndex];
                if (!type->loaded) continue;
                Color tint = GetTeamTint(units[i].team);
                DrawModel(type->model, units[i].position, type->scale, tint);

                if (units[i].selected)
                {
                    BoundingBox sb = GetUnitBounds(&units[i], type);
                    DrawBoundingBox(sb, GREEN);
                }
            }
        EndMode3D();

        // --- Floor label: Blue team unit count (projected from 3D floor position) ---
        {
            int blueCount = CountTeamUnits(units, unitCount, TEAM_BLUE);
            int redCount  = CountTeamUnits(units, unitCount, TEAM_RED);

            // Project a ground-level point to screen for Blue side
            Vector2 blueFloorPos = GetWorldToScreen((Vector3){ -60.0f, 0.5f, 30.0f }, camera);
            const char *blueSizeText = TextFormat("BLUE  %d / %d", blueCount, BLUE_TEAM_MAX_SIZE);
            int bstw = MeasureText(blueSizeText, 28);
            DrawText(blueSizeText, (int)blueFloorPos.x - bstw/2, (int)blueFloorPos.y, 28, (Color){80,120,220,180});

            // Red side (no cap, just show count)
            Vector2 redFloorPos = GetWorldToScreen((Vector3){ 60.0f, 0.5f, 30.0f }, camera);
            const char *redSizeText = TextFormat("RED  %d", redCount);
            int rstw = MeasureText(redSizeText, 28);
            DrawText(redSizeText, (int)redFloorPos.x - rstw/2, (int)redFloorPos.y, 28, (Color){220,80,80,180});
        }

        // 2D overlay: labels + health bars
        for (int i = 0; i < unitCount; i++)
        {
            if (!units[i].active) continue;
            UnitType *type = &unitTypes[units[i].typeIndex];
            if (!type->loaded) continue;
            const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];

            Vector2 sp = GetWorldToScreen(
                (Vector3){ units[i].position.x,
                           units[i].position.y + (type->baseBounds.max.y * type->scale) + 1.0f,
                           units[i].position.z }, camera);

            const char *teamTag = (units[i].team == TEAM_BLUE) ? "BLUE" : "RED";
            const char *label = TextFormat("[%s] %s", teamTag, type->name);
            int tw = MeasureText(label, 14);
            DrawText(label, (int)sp.x - tw/2, (int)sp.y - 12, 14,
                     (units[i].team == TEAM_BLUE) ? DARKBLUE : MAROON);

            // Health bar
            float hpRatio = units[i].currentHealth / stats->health;
            if (hpRatio < 0) hpRatio = 0;
            if (hpRatio > 1) hpRatio = 1;
            int bw = 40, bh = 5;
            int bx = (int)sp.x - bw/2, by = (int)sp.y + 4;
            DrawRectangle(bx, by, bw, bh, DARKGRAY);
            Color hpC = (hpRatio > 0.5f) ? GREEN : (hpRatio > 0.25f) ? ORANGE : RED;
            DrawRectangle(bx, by, (int)(bw * hpRatio), bh, hpC);
            DrawRectangleLines(bx, by, bw, bh, BLACK);

            const char *hpT = TextFormat("%.0f/%.0f", units[i].currentHealth, stats->health);
            int htw = MeasureText(hpT, 10);
            DrawText(hpT, (int)sp.x - htw/2, by + bh + 2, 10, DARKGRAY);
        }

        // ── Spawn buttons + Play — only during prep ──
        if (phase == PHASE_PREP)
        {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            int dHudTop = sh - HUD_TOTAL_HEIGHT;
            int dBtnXBlue = btnMargin;
            int dBtnXRed  = sw - btnWidth - btnMargin;
            int dBtnYStart = dHudTop - (unitTypeCount * (btnHeight + btnMargin)) - btnMargin;

            for (int i = 0; i < unitTypeCount; i++)
            {
                Rectangle r = { (float)dBtnXBlue, (float)(dBtnYStart + i*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                Color c = unitTypes[i].loaded ? (Color){100,140,230,255} : LIGHTGRAY;
                if (CheckCollisionPointRec(GetMousePosition(), r) && unitTypes[i].loaded) c = BLUE;
                DrawRectangleRec(r, c);
                DrawRectangleLinesEx(r, 2, unitTypes[i].loaded ? DARKBLUE : GRAY);
                const char *l = TextFormat("BLUE %s", unitTypes[i].name);
                int lw = MeasureText(l, 14);
                DrawText(l, r.x + (btnWidth-lw)/2, r.y + (btnHeight-14)/2, 14, WHITE);
            }

            for (int i = 0; i < unitTypeCount; i++)
            {
                Rectangle r = { (float)dBtnXRed, (float)(dBtnYStart + i*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                Color c = unitTypes[i].loaded ? (Color){230,100,100,255} : LIGHTGRAY;
                if (CheckCollisionPointRec(GetMousePosition(), r) && unitTypes[i].loaded) c = RED;
                DrawRectangleRec(r, c);
                DrawRectangleLinesEx(r, 2, unitTypes[i].loaded ? MAROON : GRAY);
                const char *l = TextFormat("RED %s", unitTypes[i].name);
                int lw = MeasureText(l, 14);
                DrawText(l, r.x + (btnWidth-lw)/2, r.y + (btnHeight-14)/2, 14, WHITE);
            }

            // PLAY button (centre-bottom)
            {
                Rectangle dPlayBtn = { (float)(sw/2 - playBtnW/2), (float)(dHudTop - playBtnH - btnMargin), (float)playBtnW, (float)playBtnH };
                int ba, ra;
                CountTeams(units, unitCount, &ba, &ra);
                bool canPlay = (ba > 0 && ra > 0);
                Color pc = canPlay ? (Color){50,180,80,255} : LIGHTGRAY;
                if (canPlay && CheckCollisionPointRec(GetMousePosition(), dPlayBtn))
                    pc = (Color){30,220,60,255};
                DrawRectangleRec(dPlayBtn, pc);
                DrawRectangleLinesEx(dPlayBtn, 2, canPlay ? DARKGREEN : GRAY);
                const char *pt = TextFormat("PLAY Round %d", currentRound + 1);
                int ptw = MeasureText(pt, 18);
                DrawText(pt, dPlayBtn.x + (playBtnW - ptw)/2, dPlayBtn.y + (playBtnH - 18)/2, 18, WHITE);
            }
        }

        // ── HUD: round + score info ──
        {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            DrawText(TextFormat("Round: %d / %d", currentRound < TOTAL_ROUNDS ? currentRound + 1 : TOTAL_ROUNDS, TOTAL_ROUNDS),
                     sw/2 - 60, 10, 20, BLACK);
            DrawText(TextFormat("BLUE: %d", blueWins), sw/2 - 120, 35, 18, DARKBLUE);
            DrawText(TextFormat("RED: %d", redWins),  sw/2 + 60, 35, 18, MAROON);
            DrawText(TextFormat("Units: %d / %d", unitCount, MAX_UNITS), 10, 30, 10, DARKGRAY);

            // Phase label
            if (phase == PHASE_COMBAT)
            {
                const char *fightText = "FIGHT!";
                int ftw = MeasureText(fightText, 28);
                DrawText(fightText, sw/2 - ftw/2, sh/2 - 60, 28, RED);
            }
            else if (phase == PHASE_ROUND_OVER)
            {
                int rtw = MeasureText(roundResultText, 26);
                DrawText(roundResultText, sw/2 - rtw/2, sh/2 - 40, 26, DARKPURPLE);
            }
            else if (phase == PHASE_GAME_OVER)
            {
                const char *winner = (blueWins > redWins) ? "BLUE TEAM WINS!" :
                                     (redWins > blueWins) ? "RED TEAM WINS!" : "IT'S A DRAW!";
                int ww = MeasureText(winner, 36);
                DrawText(winner, sw/2 - ww/2, sh/2 - 50, 36,
                         (blueWins > redWins) ? DARKBLUE : (redWins > blueWins) ? MAROON : DARKGRAY);

                const char *restartMsg = "Press R to restart";
                int rw = MeasureText(restartMsg, 20);
                DrawText(restartMsg, sw/2 - rw/2, sh/2, 20, GRAY);
            }
        }

        // Camera debug sliders
        Rectangle hBar = { 10, 60, 150, 20 };
        float hPerc = camHeight / 150.0f; if (hPerc > 1) hPerc = 1; if (hPerc < 0) hPerc = 0;
        DrawRectangleRec(hBar, LIGHTGRAY);
        DrawRectangle(10, 60, (int)(150*hPerc), 20, SKYBLUE);
        DrawText(TextFormat("Height: %.1f", camHeight), 170, 60, 10, BLACK);
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), hBar))
        { camHeight = (GetMousePosition().x - 10) / 150.0f * 150.0f; if (camHeight < 1) camHeight = 1; }

        Rectangle dBar = { 10, 90, 150, 20 };
        float dPerc = camDistance / 150.0f; if (dPerc > 1) dPerc = 1; if (dPerc < 0) dPerc = 0;
        DrawRectangleRec(dBar, LIGHTGRAY);
        DrawRectangle(10, 90, (int)(150*dPerc), 20, SKYBLUE);
        DrawText(TextFormat("Distance: %.1f", camDistance), 170, 90, 10, BLACK);
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), dBar))
        { camDistance = (GetMousePosition().x - 10) / 150.0f * 150.0f; if (camDistance < 1) camDistance = 1; }

        Rectangle fBar = { 10, 120, 150, 20 };
        float fPerc = camFOV / 120.0f; if (fPerc > 1) fPerc = 1; if (fPerc < 0) fPerc = 0;
        DrawRectangleRec(fBar, LIGHTGRAY);
        DrawRectangle(10, 120, (int)(150*fPerc), 20, SKYBLUE);
        DrawText(TextFormat("FOV: %.1f", camFOV), 170, 120, 10, BLACK);
        if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), fBar))
        { camFOV = (GetMousePosition().x - 10) / 150.0f * 120.0f; if (camFOV < 1) camFOV = 1; }

        // ── UNIT HUD BAR + SHOP ── (visible in all phases except GAME_OVER)
        if (phase != PHASE_GAME_OVER)
        {
            int hudSw = GetScreenWidth();
            int hudSh = GetScreenHeight();
            int hudTop = hudSh - HUD_TOTAL_HEIGHT;

            // --- Dark background panel (full width, bottom) ---
            DrawRectangle(0, hudTop, hudSw, HUD_TOTAL_HEIGHT, (Color){ 24, 24, 32, 230 });
            DrawRectangle(0, hudTop, hudSw, 2, (Color){ 60, 60, 80, 255 });

            // --- Unit cards (centered horizontally in the unit bar) ---
            int totalCardsW = BLUE_TEAM_MAX_SIZE * HUD_CARD_WIDTH
                            + (BLUE_TEAM_MAX_SIZE - 1) * HUD_CARD_SPACING;
            int cardsStartX = (hudSw - totalCardsW) / 2;
            int cardsY = hudTop + HUD_SHOP_HEIGHT + 5;

            for (int slot = 0; slot < BLUE_TEAM_MAX_SIZE; slot++)
            {
                int cardX = cardsStartX + slot * (HUD_CARD_WIDTH + HUD_CARD_SPACING);

                // Card background
                DrawRectangle(cardX, cardsY, HUD_CARD_WIDTH, HUD_CARD_HEIGHT,
                             (Color){ 35, 35, 50, 255 });
                DrawRectangleLines(cardX, cardsY, HUD_CARD_WIDTH, HUD_CARD_HEIGHT,
                                  (Color){ 60, 60, 80, 255 });

                if (slot < blueHudCount)
                {
                    int ui = blueHudUnits[slot];
                    UnitType *type = &unitTypes[units[ui].typeIndex];
                    const UnitStats *stats = &UNIT_STATS[units[ui].typeIndex];

                    // Selection highlight
                    if (units[ui].selected)
                        DrawRectangleLinesEx(
                            (Rectangle){ (float)(cardX - 1), (float)(cardsY - 1),
                                        (float)(HUD_CARD_WIDTH + 2), (float)(HUD_CARD_HEIGHT + 2) },
                            2, (Color){ 100, 255, 100, 255 });

                    // Portrait (left side of card) — Y-flipped for RenderTexture
                    Rectangle srcRect = { 0, 0, (float)HUD_PORTRAIT_SIZE, -(float)HUD_PORTRAIT_SIZE };
                    Rectangle dstRect = { (float)(cardX + 4), (float)(cardsY + 4),
                                          (float)HUD_PORTRAIT_SIZE, (float)HUD_PORTRAIT_SIZE };
                    DrawTexturePro(portraits[slot].texture, srcRect, dstRect,
                                  (Vector2){ 0, 0 }, 0.0f, WHITE);
                    DrawRectangleLines(cardX + 4, cardsY + 4,
                                      HUD_PORTRAIT_SIZE, HUD_PORTRAIT_SIZE,
                                      (Color){ 60, 60, 80, 255 });

                    // Unit name below portrait
                    const char *unitName = type->name;
                    int nameW = MeasureText(unitName, 12);
                    DrawText(unitName,
                            cardX + 4 + (HUD_PORTRAIT_SIZE - nameW) / 2,
                            cardsY + HUD_PORTRAIT_SIZE + 8,
                            12, (Color){ 200, 200, 220, 255 });

                    // Mini health bar
                    int hbX = cardX + 4;
                    int hbY = cardsY + HUD_PORTRAIT_SIZE + 22;
                    int hbW = HUD_PORTRAIT_SIZE;
                    int hbH = 6;
                    float hpRatio = units[ui].currentHealth / stats->health;
                    if (hpRatio < 0) hpRatio = 0;
                    if (hpRatio > 1) hpRatio = 1;
                    DrawRectangle(hbX, hbY, hbW, hbH, (Color){ 20, 20, 20, 255 });
                    Color hpCol = (hpRatio > 0.5f) ? GREEN : (hpRatio > 0.25f) ? ORANGE : RED;
                    DrawRectangle(hbX, hbY, (int)(hbW * hpRatio), hbH, hpCol);
                    DrawRectangleLines(hbX, hbY, hbW, hbH, (Color){ 60, 60, 80, 255 });

                    // 2x2 Ability slot grid (right side of card)
                    int abilStartX = cardX + HUD_PORTRAIT_SIZE + 12;
                    int abilStartY = cardsY + 8;
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
                    {
                        int col = a % 2;
                        int row = a / 2;
                        int ax = abilStartX + col * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                        int ay = abilStartY + row * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);

                        DrawRectangle(ax, ay, HUD_ABILITY_SLOT_SIZE,
                                     HUD_ABILITY_SLOT_SIZE, (Color){ 40, 40, 55, 255 });
                        DrawRectangleLines(ax, ay, HUD_ABILITY_SLOT_SIZE,
                                          HUD_ABILITY_SLOT_SIZE, (Color){ 90, 90, 110, 255 });

                        // Placeholder "?" in empty slots
                        if (units[ui].abilities[a].abilityId < 0)
                        {
                            const char *q = "?";
                            int qw = MeasureText(q, 16);
                            DrawText(q, ax + (HUD_ABILITY_SLOT_SIZE - qw) / 2,
                                    ay + (HUD_ABILITY_SLOT_SIZE - 16) / 2,
                                    16, (Color){ 80, 80, 100, 255 });
                        }
                    }
                }
                else
                {
                    // Empty slot placeholder
                    const char *emptyText = "EMPTY";
                    int ew = MeasureText(emptyText, 14);
                    DrawText(emptyText,
                            cardX + (HUD_CARD_WIDTH - ew) / 2,
                            cardsY + (HUD_CARD_HEIGHT - 14) / 2,
                            14, (Color){ 60, 60, 80, 255 });
                }
            }

            // --- Shop panel (only during PREP, above unit bar) ---
            if (phase == PHASE_PREP)
            {
                int shopY = hudTop + 2;
                int shopH = HUD_SHOP_HEIGHT - 2;
                DrawRectangle(0, shopY, hudSw, shopH, (Color){ 20, 20, 28, 240 });
                DrawRectangle(0, shopY + shopH - 1, hudSw, 1, (Color){ 60, 60, 80, 255 });

                // ROLL button (left)
                Rectangle rollBtn = { 20, (float)(shopY + 10), 80, 30 };
                Color rollColor = (Color){ 180, 140, 40, 255 };
                if (CheckCollisionPointRec(GetMousePosition(), rollBtn))
                    rollColor = (Color){ 220, 180, 60, 255 };
                DrawRectangleRec(rollBtn, rollColor);
                DrawRectangleLinesEx(rollBtn, 2, (Color){ 120, 90, 20, 255 });
                const char *rollText = "ROLL";
                int rollW = MeasureText(rollText, 16);
                DrawText(rollText, (int)(rollBtn.x + (80 - rollW) / 2),
                        (int)(rollBtn.y + (30 - 16) / 2), 16, WHITE);

                // Placeholder ability cards in shop (3 slots, centered)
                int shopCardW = 100;
                int shopCardH = 34;
                int shopCardGap = 10;
                int totalShopW = MAX_SHOP_SLOTS * shopCardW + (MAX_SHOP_SLOTS - 1) * shopCardGap;
                int shopCardsX = (hudSw - totalShopW) / 2;
                for (int s = 0; s < MAX_SHOP_SLOTS; s++)
                {
                    int scx = shopCardsX + s * (shopCardW + shopCardGap);
                    int scy = shopY + 8;
                    DrawRectangle(scx, scy, shopCardW, shopCardH, (Color){ 50, 50, 65, 255 });
                    DrawRectangleLines(scx, scy, shopCardW, shopCardH, (Color){ 90, 90, 110, 255 });
                    const char *placeholder = "???";
                    int pw = MeasureText(placeholder, 14);
                    DrawText(placeholder, scx + (shopCardW - pw) / 2,
                            scy + (shopCardH - 14) / 2, 14, (Color){ 100, 100, 120, 255 });
                }

                // Gold display (right side)
                const char *goldText = TextFormat("Gold: %d", 10);
                int gw = MeasureText(goldText, 18);
                DrawText(goldText, hudSw - gw - 20, shopY + 16, 18, (Color){ 240, 200, 60, 255 });
            }
        }

        DrawFPS(10, 10);
        EndDrawing();
    }

    // Cleanup
    if (nfcPipe) {
        pclose(nfcPipe);
        printf("[NFC] Bridge closed\n");
    }
    for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++) UnloadRenderTexture(portraits[i]);
    for (int i = 0; i < unitTypeCount; i++) UnloadModel(unitTypes[i].model);
    CloseWindow();
    return 0;
}
