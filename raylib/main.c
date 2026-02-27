/*******************************************************************************************
*
*   Unit Spawning System - raylib
*
*   Extensible unit spawning with click-and-drag movement.
*   Two-team autochess-style system with per-unit stats.
*   Currently supports: Mushroom, Goblin
*
********************************************************************************************/

#include "raylib.h"
#include <string.h>

//------------------------------------------------------------------------------------
// Data Structures
//------------------------------------------------------------------------------------
#define MAX_UNIT_TYPES 8
#define MAX_UNITS 64

//------------------------------------------------------------------------------------
// Unit Stats — "Master Library" for balancing.
// Change numbers here; every spawned unit picks them up automatically.
//------------------------------------------------------------------------------------
typedef struct {
    float health;
    float movementSpeed;
    float attackDamage;
    float attackSpeed;     // seconds between attacks
} UnitStats;

// Indexed by unit-type index (same order as unitTypes[]).
// To add a new unit type: add a row here AND a UnitType entry below.
static const UnitStats UNIT_STATS[] = {
    /* 0  Mushroom */ { .health = 15.0f, .movementSpeed = 2.0f, .attackDamage = 3.0f, .attackSpeed = 1.2f },
    /* 1  Goblin   */ { .health =  5.0f, .movementSpeed = 5.0f, .attackDamage = 2.0f, .attackSpeed = 0.5f },
};

//------------------------------------------------------------------------------------
// Team enum
//------------------------------------------------------------------------------------
typedef enum {
    TEAM_BLUE = 0,
    TEAM_RED  = 1,
} Team;

//------------------------------------------------------------------------------------
// Unit type (visual info — model, scale, name)
//------------------------------------------------------------------------------------
typedef struct {
    const char *name;
    const char *modelPath;
    Model model;
    BoundingBox baseBounds;   // unscaled bounds from mesh
    float scale;
    bool loaded;              // whether the model loaded successfully
} UnitType;

//------------------------------------------------------------------------------------
// Runtime unit instance
//------------------------------------------------------------------------------------
typedef struct {
    int typeIndex;            // index into unitTypes[]
    Vector3 position;
    Team team;
    float currentHealth;      // starts at UNIT_STATS[typeIndex].health
    float attackCooldown;     // counts down each frame (seconds)
    bool active;
    bool selected;
    bool dragging;
} Unit;

//------------------------------------------------------------------------------------
// Spawn a new unit of the given type on the given team
//------------------------------------------------------------------------------------
void SpawnUnit(Unit units[], int *unitCount, int typeIndex, Team team)
{
    if (*unitCount >= MAX_UNITS) return;

    const UnitStats *stats = &UNIT_STATS[typeIndex];

    units[*unitCount] = (Unit){
        .typeIndex     = typeIndex,
        .position      = (Vector3){ 0.0f, 0.0f, 0.0f },
        .team          = team,
        .currentHealth = stats->health,
        .attackCooldown = 0.0f,
        .active        = true,
        .selected      = false,
        .dragging      = false,
    };
    (*unitCount)++;
}

//------------------------------------------------------------------------------------
// Helper: get the world-space scaled bounding box for a unit
//------------------------------------------------------------------------------------
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

//------------------------------------------------------------------------------------
// Helper: team colour tint
//------------------------------------------------------------------------------------
Color GetTeamTint(Team team)
{
    if (team == TEAM_BLUE) return (Color){ 150, 180, 255, 255 };  // light blue
    else                   return (Color){ 255, 150, 150, 255 };  // light red
}

//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
int main(void)
{
    // Initialization
    //--------------------------------------------------------------------------------------
    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "Unit Spawner — Autochess");

    // --- Camera Parameters for Adjustment ---
    float camHeight = 102.0f;
    float camDistance = 104.0f;
    float camFOV = 52.0f;

    // Define the camera to look into our 3d world
    Camera camera = { 0 };
    camera.position = (Vector3){ 0.0f, camHeight, camDistance };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = camFOV;
    camera.projection = CAMERA_PERSPECTIVE;

    // --- Unit Type Registry ---
    // To add a new unit type: add an entry here, a matching UNIT_STATS row above,
    // and increment unitTypeCount.
    int unitTypeCount = 2;
    UnitType unitTypes[MAX_UNIT_TYPES] = { 0 };

    // Mushroom
    unitTypes[0].name = "Mushroom";
    unitTypes[0].modelPath = "MUSHROOMmixamotest.obj";
    unitTypes[0].scale = 0.1f;

    // Goblin
    unitTypes[1].name = "Goblin";
    unitTypes[1].modelPath = "goblin.obj";
    unitTypes[1].scale = 0.1f;

    // Load all unit type models
    for (int i = 0; i < unitTypeCount; i++)
    {
        unitTypes[i].model = LoadModel(unitTypes[i].modelPath);
        if (unitTypes[i].model.meshCount > 0)
        {
            unitTypes[i].baseBounds = GetMeshBoundingBox(unitTypes[i].model.meshes[0]);
            unitTypes[i].loaded = true;
        }
        else
        {
            unitTypes[i].loaded = false;
        }
    }

    // --- Units array ---
    Unit units[MAX_UNITS] = { 0 };
    int unitCount = 0;

    // --- UI Button definitions ---
    const int btnWidth = 150;
    const int btnHeight = 30;
    const int btnMargin = 10;
    // Blue-team buttons on the left, Red-team buttons on the right
    const int btnXBlue = btnMargin;
    const int btnXRed  = screenWidth - btnWidth - btnMargin;
    const int btnYStart = screenHeight - (unitTypeCount * (btnHeight + btnMargin));

    SetTargetFPS(60);
    //--------------------------------------------------------------------------------------

    // Main game loop
    while (!WindowShouldClose())
    {
        // Update
        //----------------------------------------------------------------------------------

        // Update camera from parameters
        camera.position.y = camHeight;
        camera.position.z = camDistance;
        camera.fovy = camFOV;

        // Smooth Y lift for all units
        for (int i = 0; i < unitCount; i++)
        {
            if (!units[i].active) continue;
            float targetY = units[i].dragging ? 5.0f : 0.0f;
            units[i].position.y += (targetY - units[i].position.y) * 0.1f;
        }

        // Dragging logic
        for (int i = 0; i < unitCount; i++)
        {
            if (!units[i].active || !units[i].dragging) continue;

            Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
            RayCollision groundHit = GetRayCollisionQuad(ray,
                (Vector3){ -500.0f, 0.0f, -500.0f },
                (Vector3){ -500.0f, 0.0f,  500.0f },
                (Vector3){  500.0f, 0.0f,  500.0f },
                (Vector3){  500.0f, 0.0f, -500.0f });

            if (groundHit.hit)
            {
                units[i].position.x = groundHit.point.x;
                units[i].position.z = groundHit.point.z;
            }

            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
            {
                units[i].dragging = false;
            }
        }

        // Click handling: check spawn buttons first, then unit selection
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            Vector2 mouse = GetMousePosition();
            bool clickedButton = false;

            // Check BLUE spawn buttons (bottom-left)
            for (int i = 0; i < unitTypeCount; i++)
            {
                Rectangle btnRect = {
                    (float)btnXBlue,
                    (float)(btnYStart + i * (btnHeight + btnMargin)),
                    (float)btnWidth,
                    (float)btnHeight
                };
                if (CheckCollisionPointRec(mouse, btnRect) && unitTypes[i].loaded)
                {
                    SpawnUnit(units, &unitCount, i, TEAM_BLUE);
                    clickedButton = true;
                    break;
                }
            }

            // Check RED spawn buttons (bottom-right)
            if (!clickedButton)
            {
                for (int i = 0; i < unitTypeCount; i++)
                {
                    Rectangle btnRect = {
                        (float)btnXRed,
                        (float)(btnYStart + i * (btnHeight + btnMargin)),
                        (float)btnWidth,
                        (float)btnHeight
                    };
                    if (CheckCollisionPointRec(mouse, btnRect) && unitTypes[i].loaded)
                    {
                        SpawnUnit(units, &unitCount, i, TEAM_RED);
                        clickedButton = true;
                        break;
                    }
                }
            }

            // If no button was clicked, check unit selection
            if (!clickedButton)
            {
                bool hitAny = false;
                // Check in reverse order so top-drawn (later) units get priority
                for (int i = unitCount - 1; i >= 0; i--)
                {
                    if (!units[i].active) continue;
                    UnitType *type = &unitTypes[units[i].typeIndex];
                    BoundingBox sb = GetUnitBounds(&units[i], type);

                    if (GetRayCollisionBox(GetScreenToWorldRay(mouse, camera), sb).hit)
                    {
                        units[i].selected = true;
                        units[i].dragging = true;
                        hitAny = true;
                        // Deselect all others
                        for (int j = 0; j < unitCount; j++)
                        {
                            if (j != i) units[j].selected = false;
                        }
                        break;
                    }
                }
                if (!hitAny)
                {
                    for (int j = 0; j < unitCount; j++) units[j].selected = false;
                }
            }
        }
        //----------------------------------------------------------------------------------

        // Draw
        //----------------------------------------------------------------------------------
        BeginDrawing();

            ClearBackground(RAYWHITE);

            BeginMode3D(camera);

                DrawGrid(20, 10.0f);

                // Draw all units
                for (int i = 0; i < unitCount; i++)
                {
                    if (!units[i].active) continue;
                    UnitType *type = &unitTypes[units[i].typeIndex];
                    if (!type->loaded) continue;

                    // Tint the model by team colour
                    Color tint = GetTeamTint(units[i].team);
                    DrawModel(type->model, units[i].position, type->scale, tint);

                    if (units[i].selected)
                    {
                        BoundingBox sb = GetUnitBounds(&units[i], type);
                        DrawBoundingBox(sb, GREEN);
                    }
                }

            EndMode3D();

            // Draw unit labels + health bars above each unit (2D overlay)
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                UnitType *type = &unitTypes[units[i].typeIndex];
                if (!type->loaded) continue;

                const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];

                // Project 3D position to screen
                Vector2 screenPos = GetWorldToScreen(
                    (Vector3){ units[i].position.x,
                               units[i].position.y + (type->baseBounds.max.y * type->scale) + 1.0f,
                               units[i].position.z },
                    camera);

                // --- Team + name label ---
                const char *teamTag = (units[i].team == TEAM_BLUE) ? "BLUE" : "RED";
                const char *label = TextFormat("[%s] %s", teamTag, type->name);
                int textW = MeasureText(label, 14);
                DrawText(label, (int)screenPos.x - textW / 2, (int)screenPos.y - 12, 14,
                         (units[i].team == TEAM_BLUE) ? DARKBLUE : MAROON);

                // --- Health bar ---
                float hpRatio = units[i].currentHealth / stats->health;
                if (hpRatio < 0.0f) hpRatio = 0.0f;
                if (hpRatio > 1.0f) hpRatio = 1.0f;

                int barWidth = 40;
                int barHeight = 5;
                int barX = (int)screenPos.x - barWidth / 2;
                int barY = (int)screenPos.y + 4;

                DrawRectangle(barX, barY, barWidth, barHeight, DARKGRAY);
                Color hpColor = (hpRatio > 0.5f) ? GREEN : (hpRatio > 0.25f) ? ORANGE : RED;
                DrawRectangle(barX, barY, (int)(barWidth * hpRatio), barHeight, hpColor);
                DrawRectangleLines(barX, barY, barWidth, barHeight, BLACK);

                // --- HP text ---
                const char *hpText = TextFormat("%.0f/%.0f", units[i].currentHealth, stats->health);
                int hpTextW = MeasureText(hpText, 10);
                DrawText(hpText, (int)screenPos.x - hpTextW / 2, barY + barHeight + 2, 10, DARKGRAY);
            }

            // ── BLUE team spawn buttons (bottom-left) ──
            for (int i = 0; i < unitTypeCount; i++)
            {
                Rectangle btnRect = {
                    (float)btnXBlue,
                    (float)(btnYStart + i * (btnHeight + btnMargin)),
                    (float)btnWidth,
                    (float)btnHeight
                };
                Color btnColor = unitTypes[i].loaded ? (Color){ 100, 140, 230, 255 } : LIGHTGRAY;
                Color borderColor = unitTypes[i].loaded ? DARKBLUE : GRAY;

                if (CheckCollisionPointRec(GetMousePosition(), btnRect) && unitTypes[i].loaded)
                    btnColor = BLUE;

                DrawRectangleRec(btnRect, btnColor);
                DrawRectangleLinesEx(btnRect, 2, borderColor);

                const char *label = TextFormat("BLUE %s", unitTypes[i].name);
                int labelW = MeasureText(label, 14);
                DrawText(label, btnRect.x + (btnWidth - labelW) / 2,
                         btnRect.y + (btnHeight - 14) / 2, 14, WHITE);
            }

            // ── RED team spawn buttons (bottom-right) ──
            for (int i = 0; i < unitTypeCount; i++)
            {
                Rectangle btnRect = {
                    (float)btnXRed,
                    (float)(btnYStart + i * (btnHeight + btnMargin)),
                    (float)btnWidth,
                    (float)btnHeight
                };
                Color btnColor = unitTypes[i].loaded ? (Color){ 230, 100, 100, 255 } : LIGHTGRAY;
                Color borderColor = unitTypes[i].loaded ? MAROON : GRAY;

                if (CheckCollisionPointRec(GetMousePosition(), btnRect) && unitTypes[i].loaded)
                    btnColor = RED;

                DrawRectangleRec(btnRect, btnColor);
                DrawRectangleLinesEx(btnRect, 2, borderColor);

                const char *label = TextFormat("RED %s", unitTypes[i].name);
                int labelW = MeasureText(label, 14);
                DrawText(label, btnRect.x + (btnWidth - labelW) / 2,
                         btnRect.y + (btnHeight - 14) / 2, 14, WHITE);
            }

            // Info
            DrawText(TextFormat("Units: %d / %d", unitCount, MAX_UNITS), 10, 30, 10, DARKGRAY);

            // --- Simple Sliders for Camera Debugging ---
            // Height Slider
            Rectangle hBar = { 10, 60, 150, 20 };
            float hPerc = (camHeight / 150.0f);
            if (hPerc > 1.0f) hPerc = 1.0f;
            if (hPerc < 0.0f) hPerc = 0.0f;
            DrawRectangleRec(hBar, LIGHTGRAY);
            DrawRectangle(10, 60, (int)(150 * hPerc), 20, SKYBLUE);
            DrawText(TextFormat("Height: %.1f", camHeight), 170, 60, 10, BLACK);
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), hBar))
            {
                camHeight = (GetMousePosition().x - 10) / 150.0f * 150.0f;
                if (camHeight < 1.0f) camHeight = 1.0f;
            }

            // Distance Slider
            Rectangle dBar = { 10, 90, 150, 20 };
            float dPerc = (camDistance / 150.0f);
            if (dPerc > 1.0f) dPerc = 1.0f;
            if (dPerc < 0.0f) dPerc = 0.0f;
            DrawRectangleRec(dBar, LIGHTGRAY);
            DrawRectangle(10, 90, (int)(150 * dPerc), 20, SKYBLUE);
            DrawText(TextFormat("Distance: %.1f", camDistance), 170, 90, 10, BLACK);
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), dBar))
            {
                camDistance = (GetMousePosition().x - 10) / 150.0f * 150.0f;
                if (camDistance < 1.0f) camDistance = 1.0f;
            }

            // FOV Slider
            Rectangle fBar = { 10, 120, 150, 20 };
            float fPerc = (camFOV / 120.0f);
            if (fPerc > 1.0f) fPerc = 1.0f;
            if (fPerc < 0.0f) fPerc = 0.0f;
            DrawRectangleRec(fBar, LIGHTGRAY);
            DrawRectangle(10, 120, (int)(150 * fPerc), 20, SKYBLUE);
            DrawText(TextFormat("FOV: %.1f", camFOV), 170, 120, 10, BLACK);
            if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), fBar))
            {
                camFOV = (GetMousePosition().x - 10) / 150.0f * 120.0f;
                if (camFOV < 1.0f) camFOV = 1.0f;
            }

            DrawFPS(10, 10);

        EndDrawing();
        //----------------------------------------------------------------------------------
    }

    // De-Initialization
    //--------------------------------------------------------------------------------------
    for (int i = 0; i < unitTypeCount; i++)
    {
        UnloadModel(unitTypes[i].model);
    }

    CloseWindow();
    //--------------------------------------------------------------------------------------

    return 0;
}
