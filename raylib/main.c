/*******************************************************************************************
*
*   raylib [models] example - loading
*
*   Example complexity rating: [★☆☆☆] 1/4
*
*   NOTE: raylib supports multiple models file formats:
*
*     - OBJ  > Text file format. Must include vertex position-texcoords-normals information,
*              if .obj references some .mtl materials file, it will be tried to be loaded
*     - GLTF/GLB > Text/binary file formats. Includes lot of information and it could
*              also reference external files, mesh and materials data will be tried to be loaded
*     - IQM  > Binary file format. Includes mesh vertex data but also animation data,
*              meshes and animation data can be loaded
*     - VOX  > Binary file format. MagikaVoxel mesh format:
*              https://github.com/ephtracy/voxel-model/blob/master/MagicaVoxel-file-format-vox.txt
*     - M3D  > Binary file format. Model 3D format:
*              https://bztsrc.gitlab.io/model3d
*
*   Example originally created with raylib 2.0, last time updated with raylib 4.2
*
*   Example licensed under an unmodified zlib/libpng license, which is an OSI-certified,
*   BSD-like license that allows static linking with closed source software
*
*   Copyright (c) 2014-2025 Ramon Santamaria (@raysan5)
*
********************************************************************************************/

#include "raylib.h"

//------------------------------------------------------------------------------------
// Program main entry point
//------------------------------------------------------------------------------------
int main(void)
{
    // Initialization
    //--------------------------------------------------------------------------------------
    const int screenWidth = 800;
    const int screenHeight = 450;

    InitWindow(screenWidth, screenHeight, "raylib [models] example - loading");

    // Define the camera to look into our 3d world
    Camera camera = { 0 };
    camera.position = (Vector3){ 50.0f, 50.0f, 50.0f }; // Camera position
    camera.target = (Vector3){ 0.0f, 12.0f, 0.0f };     // Camera looking at point
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };          // Camera up vector (rotation towards target)
    camera.fovy = 45.0f;                                // Camera field-of-view Y
    camera.projection = CAMERA_PERSPECTIVE;             // Camera mode type

    Model model = LoadModel("MUSHROOMmixamotest.obj");                 // Load model
    Texture2D texture = LoadTexture(""); // Load model texture
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texture;            // Set map diffuse texture

    Vector3 position = { 0.0f, 0.0f, 0.0f };                    // Set model position

    BoundingBox bounds = GetMeshBoundingBox(model.meshes[0]);   // Set model bounds

    // NOTE: bounds are calculated from the original size of the model,
    // if model is scaled on drawing, bounds must be also scaled

    bool selected = false;          // Selected object flag
    bool dragging = false;          // Dragging flag
    float modelScale = 0.1f;        // Model scale factor

    Vector2 mushroomScreenPosition = { 0.0f, 0.0f };

    SetTargetFPS(60);               // Set our game to run at 60 frames-per-second
    //--------------------------------------------------------------------------------------

    // Main game loop
    while (!WindowShouldClose())    // Detect window close button or ESC key
    {
        // Update
        //----------------------------------------------------------------------------------
        // UpdateCamera(&camera, CAMERA_ORBITAL);

        // Update mushroom position if dragging
        float targetY = dragging ? 5.0f : 0.0f;
        position.y += (targetY - position.y) * 0.1f; // Smooth lifting effect

        if (dragging)
        {
            Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
            // Check collision with ground plane (y=0)
            RayCollision groundHit = GetRayCollisionQuad(ray, 
                (Vector3){ -100.0f, 0.0f, -100.0f }, 
                (Vector3){ -100.0f, 0.0f,  100.0f }, 
                (Vector3){  100.0f, 0.0f,  100.0f }, 
                (Vector3){  100.0f, 0.0f, -100.0f });
            
            if (groundHit.hit)
            {
                position.x = groundHit.point.x;
                position.z = groundHit.point.z;
            }
            
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging = false;
        }

        mushroomScreenPosition = GetWorldToScreen((Vector3){position.x, position.y + (bounds.max.y * modelScale) + 1.0f, position.z}, camera);

        // Load new models/textures on drag&drop
        if (IsFileDropped())
        {
            FilePathList droppedFiles = LoadDroppedFiles();

            if (droppedFiles.count == 1) // Only support one file dropped
            {
                if (IsFileExtension(droppedFiles.paths[0], ".obj") ||
                    IsFileExtension(droppedFiles.paths[0], ".gltf") ||
                    IsFileExtension(droppedFiles.paths[0], ".glb") ||
                    IsFileExtension(droppedFiles.paths[0], ".vox") ||
                    IsFileExtension(droppedFiles.paths[0], ".iqm") ||
                    IsFileExtension(droppedFiles.paths[0], ".m3d"))       // Model file formats supported
                {
                    UnloadModel(model);                         // Unload previous model
                    model = LoadModel(droppedFiles.paths[0]);   // Load new model
                    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texture; // Set current map diffuse texture

                    bounds = GetMeshBoundingBox(model.meshes[0]);

                    // Move camera position from target enough distance to visualize model properly
                    camera.position.x = bounds.max.x + 10.0f;
                    camera.position.y = bounds.max.y + 10.0f;
                    camera.position.z = bounds.max.z + 10.0f;
                }
                else if (IsFileExtension(droppedFiles.paths[0], ".png"))  // Texture file formats supported
                {
                    // Unload current model texture and load new one
                    UnloadTexture(texture);
                    texture = LoadTexture(droppedFiles.paths[0]);
                    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texture;
                }
            }

            UnloadDroppedFiles(droppedFiles);    // Unload filepaths from memory
        }

        // Select model on mouse click
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            // Calculate scaled and translated bounding box
            BoundingBox scaledBounds = {
                (Vector3){ position.x + bounds.min.x * modelScale, position.y + bounds.min.y * modelScale, position.z + bounds.min.z * modelScale },
                (Vector3){ position.x + bounds.max.x * modelScale, position.y + bounds.max.y * modelScale, position.z + bounds.max.z * modelScale }
            };

            // Check collision between ray and box
            if (GetRayCollisionBox(GetScreenToWorldRay(GetMousePosition(), camera), scaledBounds).hit) 
            {
                selected = true;
                dragging = true;
            }
            else 
            {
                selected = false;
            }
        }
        //----------------------------------------------------------------------------------

        // Draw
        //----------------------------------------------------------------------------------
        BeginDrawing();

            ClearBackground(RAYWHITE);

            BeginMode3D(camera);

                DrawModel(model, position, modelScale, WHITE);        // Draw 3d model with texture

                DrawGrid(20, 10.0f);         // Draw a grid

                if (selected) 
                {
                    // Draw scaled bounding box
                    BoundingBox scaledBounds = {
                        (Vector3){ position.x + bounds.min.x * modelScale, position.y + bounds.min.y * modelScale, position.z + bounds.min.z * modelScale },
                        (Vector3){ position.x + bounds.max.x * modelScale, position.y + bounds.max.y * modelScale, position.z + bounds.max.z * modelScale }
                    };
                    DrawBoundingBox(scaledBounds, GREEN);
                }

            EndMode3D();

            DrawText("Mushroom man", (int)mushroomScreenPosition.x - MeasureText("Mushroom man", 20)/2, (int)mushroomScreenPosition.y, 20, BLACK);

            DrawText("Drag & drop model to load mesh/texture.", 10, GetScreenHeight() - 20, 10, DARKGRAY);
            if (selected) DrawText("MODEL SELECTED", GetScreenWidth() - 110, 10, 10, GREEN);

            DrawText("(c) Castle 3D model by Alberto Cano", screenWidth - 200, screenHeight - 20, 10, GRAY);

            DrawFPS(10, 10);

        EndDrawing();
        //----------------------------------------------------------------------------------
    }

    // De-Initialization
    //--------------------------------------------------------------------------------------
    UnloadTexture(texture);     // Unload texture
    UnloadModel(model);         // Unload model

    CloseWindow();              // Close window and OpenGL context
    //--------------------------------------------------------------------------------------

    return 0;
}
