/*******************************************************************************************
*
*   Unit Spawning System - raylib
*
*   Extensible unit spawning with click-and-drag movement.
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

typedef struct {
    const char *name;
    const char *modelPath;
    Model model;
    BoundingBox baseBounds;   // unscaled bounds from mesh
    float scale;
    bool loaded;              // whether the model loaded successfully
} UnitType;

typedef struct {
    int typeIndex;            // index into unitTypes[]
    Vector3 position;
    bool active;
    bool selected;
    bool dragging;
} Unit;

//------------------------------------------------------------------------------------
// Spawn a new unit of the given type at the origin
//------------------------------------------------------------------------------------
void SpawnUnit(Unit units[], int *unitCount, int typeIndex)
{
    if (*unitCount >= MAX_UNITS) return;
    units[*unitCount] = (Unit){
        .typeIndex = typeIndex,
        .position = (Vector3){ 0.0f, 0.0f, 0.0f },
        .active = true,
        .selected = false,
        .dragging = false,
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
// Program main entry point
//------------------------------------------------------------------------------------
int main(void)
{
    // Initialization
    //--------------------------------------------------------------------------------------
    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "Unit Spawner");

    // Define the camera to look into our 3d world
    Camera camera = { 0 };
    camera.position = (Vector3){ 50.0f, 50.0f, 50.0f };
    camera.target = (Vector3){ 0.0f, 12.0f, 0.0f };
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    // --- Unit Type Registry ---
    // To add a new unit type: add an entry here and increment unitTypeCount.
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
    const int btnX = screenWidth - btnWidth - btnMargin;

    SetTargetFPS(60);
    //--------------------------------------------------------------------------------------

    // Main game loop
    while (!WindowShouldClose())
    {
        // Update
        //----------------------------------------------------------------------------------

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

            // Check spawn buttons
            for (int i = 0; i < unitTypeCount; i++)
            {
                Rectangle btnRect = { (float)btnX, (float)(btnMargin + i * (btnHeight + btnMargin)), (float)btnWidth, (float)btnHeight };
                if (CheckCollisionPointRec(mouse, btnRect) && unitTypes[i].loaded)
                {
                    SpawnUnit(units, &unitCount, i);
                    clickedButton = true;
                    break;
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

                    DrawModel(type->model, units[i].position, type->scale, WHITE);

                    if (units[i].selected)
                    {
                        BoundingBox sb = GetUnitBounds(&units[i], type);
                        DrawBoundingBox(sb, GREEN);
                    }
                }

            EndMode3D();

            // Draw unit name labels above each unit
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                UnitType *type = &unitTypes[units[i].typeIndex];
                if (!type->loaded) continue;

                Vector2 screenPos = GetWorldToScreen(
                    (Vector3){ units[i].position.x,
                               units[i].position.y + (type->baseBounds.max.y * type->scale) + 1.0f,
                               units[i].position.z },
                    camera);
                int textW = MeasureText(type->name, 16);
                DrawText(type->name, (int)screenPos.x - textW / 2, (int)screenPos.y, 16, BLACK);
            }

            // Draw spawn buttons
            for (int i = 0; i < unitTypeCount; i++)
            {
                Rectangle btnRect = { (float)btnX, (float)(btnMargin + i * (btnHeight + btnMargin)), (float)btnWidth, (float)btnHeight };
                Color btnColor = unitTypes[i].loaded ? SKYBLUE : LIGHTGRAY;
                Color borderColor = unitTypes[i].loaded ? DARKBLUE : GRAY;
                
                // Hover effect
                if (CheckCollisionPointRec(GetMousePosition(), btnRect) && unitTypes[i].loaded)
                {
                    btnColor = BLUE;
                }

                DrawRectangleRec(btnRect, btnColor);
                DrawRectangleLinesEx(btnRect, 2, borderColor);

                const char *label = TextFormat("Spawn %s", unitTypes[i].name);
                int labelW = MeasureText(label, 14);
                DrawText(label, btnRect.x + (btnWidth - labelW) / 2, btnRect.y + (btnHeight - 14) / 2, 14, WHITE);
            }

            // Info
            DrawText(TextFormat("Units: %d / %d", unitCount, MAX_UNITS), 10, 30, 10, DARKGRAY);
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
