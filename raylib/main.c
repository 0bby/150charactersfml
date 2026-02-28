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
#include "raymath.h"
#include "rlgl.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#define RLIGHTS_IMPLEMENTATION
#include "rlights.h"

#define GLSL_VERSION 330

#include "game.h"
#include "helpers.h"
#include "leaderboard.h"
#include "net_client.h"
#include "pve_waves.h"

// --- Hit flash ---
#define HIT_FLASH_DURATION 0.12f

// --- Win/loss sound split point (seconds) — tweak & re-split with ffmpeg if needed ---
// Loss = first 6.5s of "cgj loss and win demo 2.mp3" → sfx/loss.wav
// Win  = from 6.5s onward                            → sfx/win.wav
#define ENDGAME_SFX_VOL  0.8f

//------------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------------
int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "Autochess — Set in Stone");
    SetWindowMinSize(640, 360);
    InitAudioDevice();

    // Win/loss sounds — pre-split into separate files
    Sound sfxWin  = LoadSound("sfx/win.wav");
    Sound sfxLoss = LoadSound("sfx/loss.wav");
    SetSoundVolume(sfxWin,  ENDGAME_SFX_VOL);
    SetSoundVolume(sfxLoss, ENDGAME_SFX_VOL);
    bool lastOutcomeWin = false;

    // Camera presets — prep (top-down auto-chess) vs combat (diagonal MOBA)
    const float prepHeight = 200.0f, prepDistance = 150.0f, prepFOV = 48.0f, prepX = 0.0f;
    const float combatHeight = 135.0f, combatDistance = 165.0f, combatFOV = 55.0f, combatX = 37.0f;
    const float camLerpSpeed = 2.5f;

    float camHeight = prepHeight;
    float camDistance = prepDistance;
    float camFOV = prepFOV;
    float camX = prepX;
    Camera camera = { 0 };
    camera.position = (Vector3){ camX, camHeight, camDistance };
    camera.target   = (Vector3){ 0.0f, 0.0f, 35.0f };
    camera.up       = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy     = camFOV;
    camera.projection = CAMERA_PERSPECTIVE;

    // Unit types
    int unitTypeCount = 2;
    UnitType unitTypes[MAX_UNIT_TYPES] = { 0 };
    unitTypes[0].name = "Mushroom";
    unitTypes[0].modelPath = "MUSHROOMmixamotest.obj";
    unitTypes[0].scale = 0.07f;
    unitTypes[0].yOffset = 1.5f;
    unitTypes[1].name = "Goblin";
    unitTypes[1].modelPath = "assets/goblin/animations/PluginGoblinWalk.glb";
    unitTypes[1].scale = 9.0f;

    for (int i = 0; i < unitTypeCount; i++)
    {
        unitTypes[i].model = LoadModel(unitTypes[i].modelPath);
        if (unitTypes[i].model.meshCount > 0)
        {
            unitTypes[i].baseBounds = GetMeshBoundingBox(unitTypes[i].model.meshes[0]);
            unitTypes[i].loaded = true;
        }
        else unitTypes[i].loaded = false;

        // Fix GLB alpha: force all material diffuse maps to full opacity
        for (int m = 0; m < unitTypes[i].model.materialCount; m++)
            unitTypes[i].model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    }

    // Load goblin animations from separate GLBs
    int walkAnimCount = 0, idleAnimCount = 0;
    ModelAnimation *walkAnims = LoadModelAnimations("assets/goblin/animations/PluginGoblinWalk.glb", &walkAnimCount);
    ModelAnimation *idleAnims = LoadModelAnimations("assets/goblin/animations/PluginGoblinIdle.glb", &idleAnimCount);
    // Store walk anims as the main array, keep idle separate
    unitTypes[1].anims = walkAnims;
    unitTypes[1].animCount = walkAnimCount;
    unitTypes[1].idleAnims = idleAnims;
    unitTypes[1].idleAnimCount = idleAnimCount;
    for (int s = 0; s < ANIM_COUNT; s++) unitTypes[1].animIndex[s] = -1;
    if (walkAnimCount > 0) unitTypes[1].animIndex[ANIM_WALK] = 0;
    if (idleAnimCount > 0) unitTypes[1].animIndex[ANIM_IDLE] = 0;
    unitTypes[1].hasAnimations = (walkAnimCount > 0 || idleAnimCount > 0);

    // Portrait render textures for HUD (one per max blue unit)
    RenderTexture2D portraits[BLUE_TEAM_MAX_SIZE];
    for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++)
        portraits[i] = LoadRenderTexture(HUD_PORTRAIT_SIZE, HUD_PORTRAIT_SIZE);

    // Intro screen render texture (larger for cinematic model display)
    RenderTexture2D introModelRT = LoadRenderTexture(512, 512);

    // Dedicated camera for portrait rendering
    Camera portraitCam = { 0 };
    portraitCam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    portraitCam.fovy = 35.0f;
    portraitCam.projection = CAMERA_PERSPECTIVE;

    // --- Lighting setup ---
    Shader lightShader = LoadShader(
        TextFormat("resources/shaders/glsl%i/lighting.vs", GLSL_VERSION),
        TextFormat("resources/shaders/glsl%i/lighting.fs", GLSL_VERSION));
    lightShader.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(lightShader, "viewPos");

    int ambientLoc = GetShaderLocation(lightShader, "ambient");
    SetShaderValue(lightShader, ambientLoc, (float[4]){ 0.25f, 0.22f, 0.18f, 1.0f }, SHADER_UNIFORM_VEC4);

    int fogColorLoc = GetShaderLocation(lightShader, "fogColor");
    int fogDensityLoc = GetShaderLocation(lightShader, "fogDensity");
    SetShaderValue(lightShader, fogColorLoc, (float[3]){ 0.176f, 0.157f, 0.137f }, SHADER_UNIFORM_VEC3);
    float fogDensity = 0.003f;
    SetShaderValue(lightShader, fogDensityLoc, &fogDensity, SHADER_UNIFORM_FLOAT);

    Light lights[MAX_LIGHTS] = { 0 };
    lights[0] = CreateLight(LIGHT_DIRECTIONAL, (Vector3){ 40, 60, 30 }, Vector3Zero(), (Color){245, 230, 200, 255}, lightShader);
    lights[1] = CreateLight(LIGHT_POINT, (Vector3){ 0, 40, 0 }, Vector3Zero(), (Color){220, 200, 170, 255}, lightShader);

    // Assign lighting shader to all loaded models
    for (int i = 0; i < unitTypeCount; i++)
    {
        if (!unitTypes[i].loaded) continue;
        for (int m = 0; m < unitTypes[i].model.materialCount; m++)
            unitTypes[i].model.materials[m].shader = lightShader;
    }

    // --- Tile floor setup ---
    #define TILE_VARIANTS   5
    #define TILE_GRID_SIZE  10
    #define TILE_WORLD_SIZE 20.0f

    Model tileModels[TILE_VARIANTS];
    Vector3 tileCenters[TILE_VARIANTS]; // OBJ-space center offset per variant
    const char *tilePaths[TILE_VARIANTS] = {
        "assets/goblin/environment/tiles/Tile1.obj",
        "assets/goblin/environment/tiles/Tile2.obj",
        "assets/goblin/environment/tiles/Tile3.obj",
        "assets/goblin/environment/tiles/Tile4.obj",
        "assets/goblin/environment/tiles/Tile5.obj",
    };
    Texture2D tileDiffuse = LoadTexture("assets/goblin/environment/tiles/T_Tiles_BC.png");

    for (int i = 0; i < TILE_VARIANTS; i++) {
        tileModels[i] = LoadModel(tilePaths[i]);
        // Compute OBJ-space center from bounding box
        BoundingBox bb = GetMeshBoundingBox(tileModels[i].meshes[0]);
        tileCenters[i] = (Vector3){
            (bb.min.x + bb.max.x) * 0.5f,
            (bb.min.y + bb.max.y) * 0.5f,
            (bb.min.z + bb.max.z) * 0.5f,
        };
        // Assign diffuse texture and lighting shader to all materials
        for (int m = 0; m < tileModels[i].materialCount; m++) {
            tileModels[i].materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = tileDiffuse;
            tileModels[i].materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            tileModels[i].materials[m].shader = lightShader;
        }
    }

    // Tile layout system: 0=random, 1=checkerboard, 2=amongus
    #define TILE_LAYOUT_COUNT 3
    int tileLayout = 0;
    int tileVariantGrid[TILE_GRID_SIZE][TILE_GRID_SIZE];
    float tileRotationGrid[TILE_GRID_SIZE][TILE_GRID_SIZE];
    float tileJitterAngle[TILE_GRID_SIZE][TILE_GRID_SIZE];  // small random rotation offset (degrees)
    float tileJitterX[TILE_GRID_SIZE][TILE_GRID_SIZE];      // small random X position offset
    float tileJitterZ[TILE_GRID_SIZE][TILE_GRID_SIZE];      // small random Z position offset
    float tileWobble[TILE_GRID_SIZE][TILE_GRID_SIZE];       // current wobble amplitude (degrees)
    float tileWobbleTime[TILE_GRID_SIZE][TILE_GRID_SIZE];  // elapsed time since wobble started
    float tileWobbleDirX[TILE_GRID_SIZE][TILE_GRID_SIZE];  // tilt axis direction (from impact)
    float tileWobbleDirZ[TILE_GRID_SIZE][TILE_GRID_SIZE];
    memset(tileWobble, 0, sizeof(tileWobble));
    memset(tileWobbleTime, 0, sizeof(tileWobbleTime));
    memset(tileWobbleDirX, 0, sizeof(tileWobbleDirX));
    memset(tileWobbleDirZ, 0, sizeof(tileWobbleDirZ));
    #define TILE_WOBBLE_MAX   25.0f   // max tilt angle at impact center (degrees)
    #define TILE_WOBBLE_DECAY  3.0f   // exponential decay rate (per second)
    #define TILE_WOBBLE_FREQ   6.0f   // oscillation frequency (Hz)
    #define TILE_WOBBLE_RADIUS 90.0f  // max radius of effect in game units
    #define TILE_WOBBLE_BOUNCE 3.0f   // max Y bounce at impact center (game units)
    const float tileRotations[4] = { 0.0f, 90.0f, 180.0f, 270.0f };
    const char *tileLayoutNames[TILE_LAYOUT_COUNT] = { "Random", "Checkerboard", "Amongus" };

    // Amongus pixel art: 1 = dark tile (variant 0-1), 0 = light tile (variant 2-4)
    const int amongusPattern[TILE_GRID_SIZE][TILE_GRID_SIZE] = {
        { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0 },
        { 0, 0, 1, 1, 1, 1, 1, 1, 0, 0 },
        { 0, 1, 1, 0, 0, 0, 0, 1, 1, 0 },
        { 0, 1, 1, 0, 0, 0, 0, 1, 1, 0 },
        { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
        { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
        { 0, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
        { 0, 0, 1, 1, 1, 0, 1, 1, 0, 0 },
        { 0, 0, 1, 1, 0, 0, 0, 1, 1, 0 },
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    };

    // Generate tile grid for current layout
    #define TILE_JITTER_ANGLE 3.0f   // max rotation jitter in degrees (+/-)
    #define TILE_JITTER_POS   0.4f   // max position jitter in game units (+/-)
    #define GENERATE_TILE_GRID() do { \
        for (int r = 0; r < TILE_GRID_SIZE; r++) { \
            for (int c = 0; c < TILE_GRID_SIZE; c++) { \
                if (tileLayout == 0) { \
                    tileVariantGrid[r][c] = GetRandomValue(0, TILE_VARIANTS - 1); \
                    tileRotationGrid[r][c] = tileRotations[GetRandomValue(0, 3)]; \
                } else if (tileLayout == 1) { \
                    int dark = (r + c) % 2; \
                    tileVariantGrid[r][c] = dark ? GetRandomValue(0, 1) : GetRandomValue(2, TILE_VARIANTS - 1); \
                    tileRotationGrid[r][c] = tileRotations[GetRandomValue(0, 3)]; \
                } else { \
                    int dark = amongusPattern[r][c]; \
                    tileVariantGrid[r][c] = dark ? GetRandomValue(0, 1) : GetRandomValue(2, TILE_VARIANTS - 1); \
                    tileRotationGrid[r][c] = tileRotations[GetRandomValue(0, 3)]; \
                } \
                tileJitterAngle[r][c] = (GetRandomValue(-100, 100) / 100.0f) * TILE_JITTER_ANGLE; \
                tileJitterX[r][c] = (GetRandomValue(-100, 100) / 100.0f) * TILE_JITTER_POS; \
                tileJitterZ[r][c] = (GetRandomValue(-100, 100) / 100.0f) * TILE_JITTER_POS; \
            } \
        } \
    } while(0)
    GENERATE_TILE_GRID();
    const float tileScale = TILE_WORLD_SIZE / 156.0f * 0.9f;

    // Border barrier shader + mesh
    Shader borderShader = LoadShader("resources/shaders/glsl330/border.vs",
                                     "resources/shaders/glsl330/border.fs");
    int borderTimeLoc = GetShaderLocation(borderShader, "time");
    int borderProximityLoc = GetShaderLocation(borderShader, "proximity");

    Mesh borderMesh = { 0 };
    borderMesh.vertexCount = 4;
    borderMesh.triangleCount = 2;
    borderMesh.vertices = (float *)MemAlloc(4 * 3 * sizeof(float));
    borderMesh.texcoords = (float *)MemAlloc(4 * 2 * sizeof(float));
    borderMesh.indices = (unsigned short *)MemAlloc(6 * sizeof(unsigned short));
    // Quad: X=-100..100, Y=0..40, Z=ARENA_BOUNDARY_Z
    // Vertex 0: bottom-left
    borderMesh.vertices[0] = -100.0f; borderMesh.vertices[1] = 0.0f;  borderMesh.vertices[2] = ARENA_BOUNDARY_Z;
    borderMesh.texcoords[0] = 0.0f;   borderMesh.texcoords[1] = 0.0f;
    // Vertex 1: bottom-right
    borderMesh.vertices[3] = 100.0f;  borderMesh.vertices[4] = 0.0f;  borderMesh.vertices[5] = ARENA_BOUNDARY_Z;
    borderMesh.texcoords[2] = 1.0f;   borderMesh.texcoords[3] = 0.0f;
    // Vertex 2: top-right
    borderMesh.vertices[6] = 100.0f;  borderMesh.vertices[7] = 40.0f; borderMesh.vertices[8] = ARENA_BOUNDARY_Z;
    borderMesh.texcoords[4] = 1.0f;   borderMesh.texcoords[5] = 1.0f;
    // Vertex 3: top-left
    borderMesh.vertices[9] = -100.0f; borderMesh.vertices[10] = 40.0f; borderMesh.vertices[11] = ARENA_BOUNDARY_Z;
    borderMesh.texcoords[6] = 0.0f;   borderMesh.texcoords[7] = 1.0f;
    // Two triangles: 0-1-2, 0-2-3
    borderMesh.indices[0] = 0; borderMesh.indices[1] = 1; borderMesh.indices[2] = 2;
    borderMesh.indices[3] = 0; borderMesh.indices[4] = 2; borderMesh.indices[5] = 3;
    UploadMesh(&borderMesh, false);

    Material borderMaterial = LoadMaterialDefault();
    borderMaterial.shader = borderShader;

    // Units
    Unit units[MAX_UNITS] = { 0 };
    int unitCount = 0;

    // Snapshot for round-reset
    UnitSnapshot snapshots[MAX_UNITS] = { 0 };
    int snapshotCount = 0;

    // Modifiers, projectiles, economy
    Modifier modifiers[MAX_MODIFIERS] = { 0 };
    Projectile projectiles[MAX_PROJECTILES] = { 0 };
    Particle particles[MAX_PARTICLES] = { 0 };
    int playerGold = 100; // DEBUG: was 10
    int goldPerRound = 5;
    int rollCost = 2;
    ShopSlot shopSlots[MAX_SHOP_SLOTS];
    for (int i = 0; i < MAX_SHOP_SLOTS; i++) shopSlots[i].abilityId = -1;
    InventorySlot inventory[MAX_INVENTORY_SLOTS];
    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
    DragState dragState = { 0 };
    int removeConfirmUnit = -1;  // unit index awaiting removal confirmation (-1 = none)
    ScreenShake shake = {0};
    FloatingText floatingTexts[MAX_FLOATING_TEXTS] = {0};
    Fissure fissures[MAX_FISSURES] = {0};
    UnitIntro intro = { .active = false, .timer = 0.0f };
    StatueSpawn statueSpawn = { .phase = SSPAWN_INACTIVE };
    int hoverAbilityId = -1;
    int hoverAbilityLevel = 0;
    float hoverTimer = 0.0f;
    const float tooltipDelay = 0.5f;

    // Round / score state
    GamePhase phase = PHASE_MENU;
    int currentRound = 0;          // 0-indexed, displayed as 1-indexed
    int blueWins = 0;
    int redWins  = 0;
    float roundOverTimer = 0.0f;   // brief pause after a round ends
    const char *roundResultText = "";
    bool debugMode = true;

    // Leaderboard & prestige state
    Leaderboard leaderboard = {0};
    LoadLeaderboard(&leaderboard);
    bool showLeaderboard = false;
    int leaderboardScroll = 0;
    int lastMilestoneRound = 0;
    bool blueLostLastRound = false;
    bool deathPenalty = false;
    char playerName[32] = "Player";
    int playerNameLen = 6;
    bool nameInputActive = false;

    // NFC emulation input
    char nfcInputBuf[32] = "";
    int nfcInputLen = 0;
    bool nfcInputActive = false;
    char nfcInputError[64] = "";
    float nfcInputErrorTimer = 0.0f;

    // --- Multiplayer state ---
    NetClient netClient;
    net_client_init(&netClient);
    bool isMultiplayer = false;
    bool playerReady = false;
    bool mpNameFieldFocused = true;
    char joinCodeInput[LOBBY_CODE_LEN + 1] = {0};
    int joinCodeLen = 0;
    const char *serverHost = "autochess.kenzhiyilin.com";
    bool waitingForOpponent = false;
    char menuError[128] = {0};
    bool currentRoundIsPve = false;

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
        GamePhase prevPhase = phase;
        UpdateShake(&shake, dt);
        if (IsKeyPressed(KEY_F1)) debugMode = !debugMode;

        // Debug: cycle tile layouts with arrow keys
        if (debugMode) {
            if (IsKeyPressed(KEY_RIGHT)) {
                tileLayout = (tileLayout + 1) % TILE_LAYOUT_COUNT;
                GENERATE_TILE_GRID();
            }
            if (IsKeyPressed(KEY_LEFT)) {
                tileLayout = (tileLayout - 1 + TILE_LAYOUT_COUNT) % TILE_LAYOUT_COUNT;
                GENERATE_TILE_GRID();
            }
        }

        // Update unit intro animation
        if (intro.active) {
            intro.timer += dt;
            UnitType *itype = &unitTypes[intro.typeIndex];
            if (itype->hasAnimations && itype->animIndex[ANIM_IDLE] >= 0) {
                int fc = itype->idleAnims[itype->animIndex[ANIM_IDLE]].frameCount;
                if (fc > 0) intro.animFrame = (intro.animFrame + 1) % fc;
            }
            if (intro.timer >= INTRO_DURATION) {
                intro.active = false;
                // Trigger statue spawn for blue units
                if (intro.unitIndex >= 0 && intro.unitIndex < unitCount &&
                    units[intro.unitIndex].active && units[intro.unitIndex].team == TEAM_BLUE) {
                    // Force-finish any previous spawn anim (snap old unit to ground)
                    if (statueSpawn.phase != SSPAWN_INACTIVE) {
                        int old = statueSpawn.unitIndex;
                        if (old >= 0 && old < unitCount && units[old].active)
                            units[old].position.y = 0.0f;
                        statueSpawn.phase = SSPAWN_INACTIVE;
                    }
                    // Randomize landing position on player side of field + random facing angle
                    int idx2 = intro.unitIndex;
                    float gridLim = ARENA_GRID_HALF - 10.0f; // 90
                    units[idx2].position.x = (float)GetRandomValue((int)(-gridLim), (int)(gridLim));
                    units[idx2].position.z = (float)GetRandomValue((int)(ARENA_BOUNDARY_Z + 5.0f), (int)(gridLim));
                    units[idx2].facingAngle = (float)GetRandomValue(0, 359);
                    StartStatueSpawn(&statueSpawn, idx2);
                }
            }
        }

        // Update statue spawn animation
        if (statueSpawn.phase != SSPAWN_INACTIVE) {
            // Guard: if unit became inactive, cancel spawn
            int si = statueSpawn.unitIndex;
            if (si < 0 || si >= unitCount || !units[si].active) {
                statueSpawn.phase = SSPAWN_INACTIVE;
            } else {
                UpdateStatueSpawn(&statueSpawn, particles, &shake, units[si].position, dt);
                if (statueSpawn.phase == SSPAWN_DONE) {
                    // Trigger tile wobble from impact point
                    {
                        float impX = units[si].position.x;
                        float impZ = units[si].position.z;
                        float gridOriginW = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                        for (int tr = 0; tr < TILE_GRID_SIZE; tr++) {
                            for (int tc = 0; tc < TILE_GRID_SIZE; tc++) {
                                float cx = gridOriginW + (tc + 0.5f) * TILE_WORLD_SIZE;
                                float cz = gridOriginW + (tr + 0.5f) * TILE_WORLD_SIZE;
                                float dx = cx - impX, dz = cz - impZ;
                                float dist = sqrtf(dx*dx + dz*dz);
                                if (dist < TILE_WOBBLE_RADIUS) {
                                    float strength = expf(-2.5f * dist / TILE_WOBBLE_RADIUS);
                                    tileWobble[tr][tc] = TILE_WOBBLE_MAX * strength;
                                    tileWobbleTime[tr][tc] = -(dist * 0.008f); // negative = propagation delay
                                    // Tilt direction: away from impact point
                                    float len = dist > 0.1f ? dist : 1.0f;
                                    tileWobbleDirX[tr][tc] = dz / len; // tilt around X pushes Z edge up
                                    tileWobbleDirZ[tr][tc] = -dx / len; // tilt around Z pushes X edge up
                                }
                            }
                        }
                    }
                    units[si].position.y = 0.0f;
                    units[si].currentAnim = ANIM_IDLE;
                    units[si].animFrame = 0;
                    statueSpawn.phase = SSPAWN_INACTIVE;
                }
            }
        }

        // Update tile wobble timers
        for (int tr = 0; tr < TILE_GRID_SIZE; tr++)
            for (int tc = 0; tc < TILE_GRID_SIZE; tc++)
                if (tileWobble[tr][tc] > 0.01f)
                    tileWobbleTime[tr][tc] += dt;
                else
                    tileWobble[tr][tc] = 0.0f;

        // Hover tooltip tracking
        int prevHoverAbilityId = hoverAbilityId;
        hoverAbilityId = -1;
        hoverAbilityLevel = 0;

        // Lerp camera toward phase preset
        {
            bool combat = (phase == PHASE_COMBAT);
            float tgtH = combat ? combatHeight : prepHeight;
            float tgtD = combat ? combatDistance : prepDistance;
            float tgtF = combat ? combatFOV : prepFOV;
            float tgtX = combat ? combatX : prepX;
            // Mirror camera for player 2 during PVP combat
            if (combat && isMultiplayer && netClient.playerSlot == 1 && !currentRoundIsPve) {
                tgtX = -tgtX;
                tgtD = -tgtD;
            }
            float t = camLerpSpeed * dt;
            if (t > 1.0f) t = 1.0f;
            camHeight  += (tgtH - camHeight)  * t;
            camDistance += (tgtD - camDistance) * t;
            camFOV     += (tgtF - camFOV)     * t;
            camX       += (tgtX - camX)       * t;
        }

        // Update camera
        camera.position.x = camX;
        camera.position.y = camHeight;
        camera.position.z = camDistance;
        camera.fovy = camFOV;

        // Update lighting shader with camera position
        float cameraPos[3] = { camera.position.x, camera.position.y, camera.position.z };
        SetShaderValue(lightShader, lightShader.locs[SHADER_LOC_VECTOR_VIEW], cameraPos, SHADER_UNIFORM_VEC3);

        // Poll NFC bridge for tag scans (only spawn during prep, blocked during intro)
        if (nfcPipe && phase == PHASE_PREP && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE) {
            char nfcBuf[64];
            if (fgets(nfcBuf, sizeof(nfcBuf), nfcPipe)) {
                nfcBuf[strcspn(nfcBuf, "\r\n")] = '\0';
                // Parse optional reader prefix "N:" (e.g. "1:0MM1..." or "2:1DG2...")
                int nfcReader = 0;
                const char *nfcCode = nfcBuf;
                if (nfcBuf[0] >= '1' && nfcBuf[0] <= '9' && nfcBuf[1] == ':') {
                    nfcReader = nfcBuf[0] - '0';
                    nfcCode = nfcBuf + 2;
                }
                int nfcTypeIndex;
                AbilitySlot nfcAbilities[MAX_ABILITIES_PER_UNIT];
                if (ParseUnitCode(nfcCode, &nfcTypeIndex, nfcAbilities)) {
                    if (nfcTypeIndex < unitTypeCount && SpawnUnit(units, &unitCount, nfcTypeIndex, TEAM_BLUE)) {
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
                            units[unitCount - 1].abilities[a] = nfcAbilities[a];
                        printf("[NFC] Reader %d: '%s' -> Spawning %s (Blue)\n",
                            nfcReader, nfcCode, unitTypes[nfcTypeIndex].name);
                        intro = (UnitIntro){ .active = true, .timer = 0.0f,
                            .typeIndex = nfcTypeIndex, .unitIndex = unitCount - 1, .animFrame = 0 };
                    } else {
                        printf("[NFC] Reader %d: '%s' -> Blue team full or unknown type\n", nfcReader, nfcCode);
                    }
                } else {
                    printf("[NFC] Invalid unit code: '%s'\n", nfcBuf);
                }
            }
        }

        //------------------------------------------------------------------------------
        // PHASE: MENU — main menu + leaderboard view
        //------------------------------------------------------------------------------
        if (phase == PHASE_MENU)
        {
            // Name input handling
            if (nameInputActive) {
                int key = GetCharPressed();
                while (key > 0) {
                    if (key >= 32 && key <= 125 && playerNameLen < 30) {
                        playerName[playerNameLen] = (char)key;
                        playerNameLen++;
                        playerName[playerNameLen] = '\0';
                    }
                    key = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && playerNameLen > 0) {
                    playerNameLen--;
                    playerName[playerNameLen] = '\0';
                }
                if (IsKeyPressed(KEY_ENTER)) nameInputActive = false;
            }

            // Click handling
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();

                if (showLeaderboard) {
                    // Close button for leaderboard overlay
                    Rectangle closeBtn = { (float)(sw/2 + 280), (float)(sh/2 - 250), 40, 40 };
                    if (CheckCollisionPointRec(mouse, closeBtn))
                        showLeaderboard = false;
                } else {
                    // Name input field
                    Rectangle nameField = { (float)(sw/2 - 120), (float)(sh/2 - 40), 240, 36 };
                    if (CheckCollisionPointRec(mouse, nameField))
                        nameInputActive = true;
                    else
                        nameInputActive = false;

                    // PLAY button
                    Rectangle playMenuBtn = { (float)(sw/2 - 80), (float)(sh/2 + 20), 160, 50 };
                    if (CheckCollisionPointRec(mouse, playMenuBtn)) {
                        // Initialize game state
                        unitCount = 0;
                        snapshotCount = 0;
                        currentRound = 0;
                        blueWins = 0;
                        redWins = 0;
                        lastMilestoneRound = 0;
                        blueLostLastRound = false;
                        deathPenalty = false;
                        roundResultText = "";
                        ClearAllModifiers(modifiers);
                        ClearAllProjectiles(projectiles);
                        ClearAllParticles(particles);
                        ClearAllFloatingTexts(floatingTexts);
                        ClearAllFissures(fissures);
                        statueSpawn.phase = SSPAWN_INACTIVE;
                        playerGold = 100;
                        for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                        RollShop(shopSlots, &playerGold, 0);
                        dragState.dragging = false;
                        SpawnWave(units, &unitCount, 0, unitTypeCount);
                        phase = PHASE_PREP;
                    }

                    // LEADERBOARD button
                    Rectangle lbBtn = { (float)(sw/2 - 80), (float)(sh/2 + 80), 160, 40 };
                    if (CheckCollisionPointRec(mouse, lbBtn)) {
                        showLeaderboard = true;
                        leaderboardScroll = 0;
                    }

                    // --- Multiplayer buttons ---
                    // CREATE LOBBY button
                    Rectangle createBtn = { (float)(sw/2 - 80), (float)(sh/2 + 130), 160, 40 };
                    if (CheckCollisionPointRec(mouse, createBtn)) {
                        menuError[0] = '\0';
                        isMultiplayer = true;
                        playerReady = false;
                        if (net_client_connect(&netClient, serverHost, NET_PORT, NULL, playerName) == 0) {
                            phase = PHASE_LOBBY;
                        } else {
                            strncpy(menuError, netClient.errorMsg, sizeof(menuError) - 1);
                            isMultiplayer = false;
                        }
                    }

                    // JOIN LOBBY button (only if 4-char code entered)
                    Rectangle joinBtn = { (float)(sw/2 - 80), (float)(sh/2 + 180), 160, 40 };
                    if (joinCodeLen == LOBBY_CODE_LEN &&
                        CheckCollisionPointRec(mouse, joinBtn)) {
                        menuError[0] = '\0';
                        isMultiplayer = true;
                        playerReady = false;
                        if (net_client_connect(&netClient, serverHost, NET_PORT, joinCodeInput, playerName) == 0) {
                            phase = PHASE_LOBBY;
                        } else {
                            strncpy(menuError, netClient.errorMsg, sizeof(menuError) - 1);
                            isMultiplayer = false;
                        }
                    }

                    // Join code input field focus
                    Rectangle codeBox = { (float)(sw/2 - 60), (float)(sh/2 + 225), 120, 30 };
                    if (CheckCollisionPointRec(mouse, codeBox))
                        mpNameFieldFocused = false;
                }
            }

            // Multiplayer join code text input
            if (!showLeaderboard && !nameInputActive) {
                int key = GetCharPressed();
                while (key > 0) {
                    if (joinCodeLen < LOBBY_CODE_LEN && ((key >= 'A' && key <= 'Z') ||
                        (key >= 'a' && key <= 'z') || (key >= '0' && key <= '9'))) {
                        joinCodeInput[joinCodeLen] = (key >= 'a' && key <= 'z') ? (key - 32) : (char)key;
                        joinCodeLen++;
                        joinCodeInput[joinCodeLen] = '\0';
                    }
                    key = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && joinCodeLen > 0 && !nameInputActive) {
                    joinCodeLen--;
                    joinCodeInput[joinCodeLen] = '\0';
                }
            }

            // ESC closes leaderboard overlay
            if (showLeaderboard && IsKeyPressed(KEY_ESCAPE))
                showLeaderboard = false;

            // Leaderboard scroll
            if (showLeaderboard) {
                int wheel = (int)GetMouseWheelMove();
                leaderboardScroll -= wheel * 40;
                if (leaderboardScroll < 0) leaderboardScroll = 0;
                int maxScroll = leaderboard.entryCount * 80 - 400;
                if (maxScroll < 0) maxScroll = 0;
                if (leaderboardScroll > maxScroll) leaderboardScroll = maxScroll;
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: LOBBY — waiting for opponent / game start
        //------------------------------------------------------------------------------
        else if (phase == PHASE_LOBBY)
        {
            net_client_poll(&netClient);

            if (netClient.state == NET_ERROR) {
                strncpy(menuError, netClient.errorMsg, sizeof(menuError) - 1);
                net_client_disconnect(&netClient);
                isMultiplayer = false;
                phase = PHASE_MENU;
            }

            if (netClient.gameStarted) {
                netClient.gameStarted = false;
                playerGold = netClient.currentGold;
            }

            if (netClient.prepStarted) {
                netClient.prepStarted = false;
                playerGold = netClient.currentGold;
                currentRound = netClient.currentRound;
                currentRoundIsPve = netClient.isPveRound;
                for (int i = 0; i < MAX_SHOP_SLOTS; i++)
                    shopSlots[i] = netClient.serverShop[i];
                // Reset multiplayer game state
                unitCount = 0;
                snapshotCount = 0;
                blueWins = 0;
                redWins = 0;
                roundResultText = "";
                ClearAllModifiers(modifiers);
                ClearAllProjectiles(projectiles);
                ClearAllParticles(particles);
                ClearAllFloatingTexts(floatingTexts);
                ClearAllFissures(fissures);
                for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                dragState.dragging = false;
                playerReady = false;
                waitingForOpponent = false;
                phase = PHASE_PREP;
            }

            if (IsKeyPressed(KEY_ESCAPE)) {
                net_client_disconnect(&netClient);
                isMultiplayer = false;
                phase = PHASE_MENU;
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: PREP — place units, click Play to start
        //------------------------------------------------------------------------------
        else if (phase == PHASE_PREP)
        {
            // --- Multiplayer: poll network and handle server messages ---
            if (isMultiplayer) {
                net_client_poll(&netClient);
                if (netClient.state == NET_ERROR) {
                    net_client_disconnect(&netClient);
                    isMultiplayer = false;
                    phase = PHASE_MENU;
                }
                if (netClient.shopUpdated) {
                    netClient.shopUpdated = false;
                    for (int i = 0; i < MAX_SHOP_SLOTS; i++)
                        shopSlots[i] = netClient.serverShop[i];
                }
                if (netClient.goldUpdated) {
                    netClient.goldUpdated = false;
                    playerGold = netClient.currentGold;
                }
                if (netClient.opponentReady) {
                    netClient.opponentReady = false;
                    waitingForOpponent = false;
                }
                // Combat started — server sends serialized units
                if (netClient.combatStarted) {
                    netClient.combatStarted = false;
                    unitCount = deserialize_units(netClient.combatNetUnits,
                        netClient.combatNetUnitCount, units, MAX_UNITS);
                    SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
                    phase = PHASE_COMBAT;
                    ClearAllModifiers(modifiers);
                    ClearAllProjectiles(projectiles);
                    ClearAllParticles(particles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    dragState.dragging = false;
                    removeConfirmUnit = -1;
                    for (int j = 0; j < unitCount; j++) {
                        units[j].selected = false;
                        units[j].dragging = false;
                        units[j].nextAbilitySlot = 0;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            units[j].abilities[a].cooldownRemaining = 0;
                            units[j].abilities[a].triggered = false;
                        }
                    }
                }
            }

            // NFC input error timer countdown
            if (nfcInputErrorTimer > 0.0f) {
                nfcInputErrorTimer -= dt;
                if (nfcInputErrorTimer <= 0.0f) nfcInputError[0] = '\0';
            }

            // NFC emulation text input handling (debug only)
            if (debugMode && nfcInputActive && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE) {
                int key = GetCharPressed();
                while (key > 0) {
                    // Uppercase the character
                    if (key >= 'a' && key <= 'z') key = key - 'a' + 'A';
                    if (((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) && nfcInputLen < 13) {
                        nfcInputBuf[nfcInputLen] = (char)key;
                        nfcInputLen++;
                        nfcInputBuf[nfcInputLen] = '\0';
                    }
                    key = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && nfcInputLen > 0) {
                    nfcInputLen--;
                    nfcInputBuf[nfcInputLen] = '\0';
                }
                if (IsKeyPressed(KEY_ESCAPE)) {
                    nfcInputActive = false;
                }
                if (IsKeyPressed(KEY_ENTER) && nfcInputLen > 0) {
                    int emTypeIndex;
                    AbilitySlot emAbilities[MAX_ABILITIES_PER_UNIT];
                    if (ParseUnitCode(nfcInputBuf, &emTypeIndex, emAbilities)) {
                        if (emTypeIndex >= unitTypeCount) {
                            snprintf(nfcInputError, sizeof(nfcInputError), "Unknown unit type %d", emTypeIndex);
                            nfcInputErrorTimer = 2.0f;
                        } else if (!SpawnUnit(units, &unitCount, emTypeIndex, TEAM_BLUE)) {
                            snprintf(nfcInputError, sizeof(nfcInputError), "Team full (%d/%d)", BLUE_TEAM_MAX_SIZE, BLUE_TEAM_MAX_SIZE);
                            nfcInputErrorTimer = 2.0f;
                        } else {
                            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
                                units[unitCount - 1].abilities[a] = emAbilities[a];
                            intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                .typeIndex = emTypeIndex, .unitIndex = unitCount - 1, .animFrame = 0 };
                            nfcInputBuf[0] = '\0';
                            nfcInputLen = 0;
                            nfcInputActive = false;
                        }
                    } else {
                        snprintf(nfcInputError, sizeof(nfcInputError), "Bad format: %s", nfcInputBuf);
                        nfcInputErrorTimer = 2.0f;
                    }
                }
            }

            // Smooth Y lift (skip units in statue spawn so gravity isn't fought)
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                if (IsUnitInStatueSpawn(&statueSpawn, i)) continue;
                float targetY = units[i].dragging ? 5.0f : 0.0f;
                units[i].position.y += (targetY - units[i].position.y) * 0.1f;
            }

            // Update particles during prep (so impact particles decay)
            UpdateParticles(particles, dt);

            // Dragging
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active || !units[i].dragging) continue;
                Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
                RayCollision groundHit = GetRayCollisionQuad(ray,
                    (Vector3){ -500, 0, -500 }, (Vector3){ -500, 0, 500 },
                    (Vector3){  500, 0,  500 }, (Vector3){  500, 0, -500 });
                // Only allow dragging red units in debug mode
                if (units[i].team == TEAM_RED && !debugMode) {
                    units[i].dragging = false;
                    continue;
                }
                if (groundHit.hit)
                {
                    units[i].position.x = groundHit.point.x;
                    units[i].position.z = groundHit.point.z;
                    // Clamp blue units to their half (positive Z = blue side)
                    if (units[i].team == TEAM_BLUE) {
                        if (units[i].position.z < ARENA_BOUNDARY_Z)
                            units[i].position.z = ARENA_BOUNDARY_Z;
                    }
                    // Clamp all units to grid bounds (X and Z)
                    float gridLimit = ARENA_GRID_HALF - 5.0f; // 95
                    if (units[i].position.x < -gridLimit) units[i].position.x = -gridLimit;
                    if (units[i].position.x >  gridLimit) units[i].position.x =  gridLimit;
                    if (units[i].position.z < -gridLimit) units[i].position.z = -gridLimit;
                    if (units[i].position.z >  gridLimit) units[i].position.z =  gridLimit;
                }
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) units[i].dragging = false;
            }

            // Clicks (blocked during intro)
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE)
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

                // NFC input box click check (debug only)
                if (debugMode) {
                    int nfcBoxW = 200, nfcBoxH = 28;
                    int nfcBoxX = sw/2 - nfcBoxW/2;
                    int nfcBoxY = btnYStart - 55;
                    Rectangle nfcRect = { (float)nfcBoxX, (float)nfcBoxY, (float)nfcBoxW, (float)nfcBoxH };
                    if (CheckCollisionPointRec(mouse, nfcRect)) {
                        nfcInputActive = true;
                        clickedButton = true;
                    } else if (nfcInputActive) {
                        nfcInputActive = false;
                    }
                }

                // Confirm removal popup (takes priority over everything)
                if (removeConfirmUnit >= 0) {
                    int popW = 220, popH = 80;
                    int popX = sw / 2 - popW / 2;
                    int popY = sh / 2 - popH / 2;
                    Rectangle yesBtn = { (float)(popX + 20), (float)(popY + popH - 32), 80, 24 };
                    Rectangle noBtn  = { (float)(popX + popW - 100), (float)(popY + popH - 32), 80, 24 };
                    if (CheckCollisionPointRec(mouse, yesBtn)) {
                        // Remove the unit: return abilities to inventory, deactivate
                        int ri = removeConfirmUnit;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            if (units[ri].abilities[a].abilityId < 0) continue;
                            // Find empty inventory slot
                            for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
                                if (inventory[inv].abilityId < 0) {
                                    inventory[inv].abilityId = units[ri].abilities[a].abilityId;
                                    inventory[inv].level = units[ri].abilities[a].level;
                                    break;
                                }
                            }
                            units[ri].abilities[a].abilityId = -1;
                        }
                        units[ri].active = false;
                        removeConfirmUnit = -1;
                        clickedButton = true;
                    } else if (CheckCollisionPointRec(mouse, noBtn)) {
                        removeConfirmUnit = -1;
                        clickedButton = true;
                    } else {
                        // Click outside popup = cancel
                        removeConfirmUnit = -1;
                        clickedButton = true;
                    }
                }

                // Play / Ready button
                if (CheckCollisionPointRec(mouse, playBtn) && unitCount > 0)
                {
                    if (isMultiplayer) {
                        // Multiplayer: send READY with army
                        if (!playerReady) {
                            int ba = CountTeamUnits(units, unitCount, TEAM_BLUE);
                            if (ba > 0) {
                                net_client_send_ready(&netClient, units, unitCount);
                                playerReady = true;
                                waitingForOpponent = true;
                                clickedButton = true;
                            }
                        }
                    } else {
                        // Solo: check both teams have units, start combat
                        int ba, ra;
                        CountTeams(units, unitCount, &ba, &ra);
                        if (ba > 0 && ra > 0)
                        {
                            SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
                            phase = PHASE_COMBAT;
                            ClearAllModifiers(modifiers);
                            ClearAllProjectiles(projectiles);
                            ClearAllParticles(particles);
                            ClearAllFloatingTexts(floatingTexts);
                            ClearAllFissures(fissures);
                            // Snap any mid-fall statue to ground before combat
                            if (statueSpawn.phase != SSPAWN_INACTIVE) {
                                int si2 = statueSpawn.unitIndex;
                                if (si2 >= 0 && si2 < unitCount && units[si2].active)
                                    units[si2].position.y = 0.0f;
                            }
                            statueSpawn.phase = SSPAWN_INACTIVE;
                            dragState.dragging = false;
                            removeConfirmUnit = -1;
                            // Reset ability state for combat start
                            for (int j = 0; j < unitCount; j++) {
                                units[j].selected = false;
                                units[j].dragging = false;
                                units[j].nextAbilitySlot = 0;
                                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                    units[j].abilities[a].cooldownRemaining = 0;
                                    units[j].abilities[a].triggered = false;
                                }
                            }
                            clickedButton = true;
                        }
                    }
                }

                // Blue spawn buttons (debug only)
                if (!clickedButton && debugMode)
                {
                    for (int i = 0; i < unitTypeCount; i++)
                    {
                        Rectangle r = { (float)btnXBlue, (float)(btnYStart + i*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                        if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded)
                        {
                            if (SpawnUnit(units, &unitCount, i, TEAM_BLUE)) {
                                intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                    .typeIndex = i, .unitIndex = unitCount - 1, .animFrame = 0 };
                            }
                            clickedButton = true; break;
                        }
                    }
                }
                // Red spawn buttons (debug only)
                if (!clickedButton && debugMode)
                {
                    for (int i = 0; i < unitTypeCount; i++)
                    {
                        Rectangle r = { (float)btnXRed, (float)(btnYStart + i*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                        if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded) {
                            if (SpawnUnit(units, &unitCount, i, TEAM_RED))
                                AssignRandomAbilities(&units[unitCount-1], GetRandomValue(1, 2));
                            clickedButton = true; break;
                        }
                    }
                }
                // --- Shop: ROLL button click ---
                if (!clickedButton && !(isMultiplayer && playerReady)) {
                    int shopY = hudTop + 2;
                    Rectangle rollBtn = { 20, (float)(shopY + 10), 80, 30 };
                    if (CheckCollisionPointRec(mouse, rollBtn)) {
                        if (isMultiplayer) {
                            net_client_send_roll(&netClient);
                        } else {
                            RollShop(shopSlots, &playerGold, rollCost);
                        }
                        TriggerShake(&shake, 2.0f, 0.15f);
                        clickedButton = true;
                    }
                }
                // --- Shop: Buy ability card click ---
                if (!clickedButton && !(isMultiplayer && playerReady)) {
                    int shopY = hudTop + 2;
                    int shopCardW = 100, shopCardH = 34, shopCardGap = 10;
                    int totalShopW = MAX_SHOP_SLOTS * shopCardW + (MAX_SHOP_SLOTS - 1) * shopCardGap;
                    int shopCardsX = (sw - totalShopW) / 2;
                    for (int s = 0; s < MAX_SHOP_SLOTS; s++) {
                        int scx = shopCardsX + s * (shopCardW + shopCardGap);
                        Rectangle r = { (float)scx, (float)(shopY + 8), (float)shopCardW, (float)shopCardH };
                        if (CheckCollisionPointRec(mouse, r) && shopSlots[s].abilityId >= 0) {
                            if (isMultiplayer) {
                                net_client_send_buy(&netClient, s);
                            } else {
                                BuyAbility(&shopSlots[s], inventory, units, unitCount, &playerGold);
                            }
                            clickedButton = true;
                            break;
                        }
                    }
                }
                // --- Drag start: inventory slots ---
                if (!clickedButton && !dragState.dragging) {
                    int totalCardsW = BLUE_TEAM_MAX_SIZE * HUD_CARD_WIDTH + (BLUE_TEAM_MAX_SIZE - 1) * HUD_CARD_SPACING;
                    int cardsStartX = (sw - totalCardsW) / 2;
                    int invStartX = cardsStartX - (HUD_INVENTORY_COLS * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP)) - 20;
                    int invStartY = hudTop + HUD_SHOP_HEIGHT + 15;
                    for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
                        int col = inv % HUD_INVENTORY_COLS;
                        int row = inv / HUD_INVENTORY_COLS;
                        int ix = invStartX + col * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                        int iy = invStartY + row * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                        Rectangle r = { (float)ix, (float)iy, (float)HUD_ABILITY_SLOT_SIZE, (float)HUD_ABILITY_SLOT_SIZE };
                        if (CheckCollisionPointRec(mouse, r) && inventory[inv].abilityId >= 0) {
                            dragState = (DragState){ .dragging = true, .sourceType = 0,
                                .sourceIndex = inv, .sourceUnitIndex = -1,
                                .abilityId = inventory[inv].abilityId, .level = inventory[inv].level };
                            inventory[inv].abilityId = -1;
                            clickedButton = true;
                            break;
                        }
                    }
                }
                // --- Drag start: unit ability slots on HUD ---
                if (!clickedButton && !dragState.dragging) {
                    // Need blueHudUnits — build it here too
                    int tmpBlue[BLUE_TEAM_MAX_SIZE]; int tmpCount = 0;
                    for (int i2 = 0; i2 < unitCount && tmpCount < BLUE_TEAM_MAX_SIZE; i2++)
                        if (units[i2].active && units[i2].team == TEAM_BLUE) tmpBlue[tmpCount++] = i2;
                    int totalCardsW = BLUE_TEAM_MAX_SIZE * HUD_CARD_WIDTH + (BLUE_TEAM_MAX_SIZE - 1) * HUD_CARD_SPACING;
                    int cardsStartX = (sw - totalCardsW) / 2;
                    int cardsY = hudTop + HUD_SHOP_HEIGHT + 5;
                    for (int h = 0; h < tmpCount && !clickedButton; h++) {
                        int cardX = cardsStartX + h * (HUD_CARD_WIDTH + HUD_CARD_SPACING);
                        int abilStartX = cardX + HUD_PORTRAIT_SIZE + 12;
                        int abilStartY = cardsY + 8;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            int col = a % 2, row = a / 2;
                            int ax = abilStartX + col * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                            int ay = abilStartY + row * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                            Rectangle r = { (float)ax, (float)ay, (float)HUD_ABILITY_SLOT_SIZE, (float)HUD_ABILITY_SLOT_SIZE };
                            int ui = tmpBlue[h];
                            if (CheckCollisionPointRec(mouse, r) && units[ui].abilities[a].abilityId >= 0) {
                                dragState = (DragState){ .dragging = true, .sourceType = 1,
                                    .sourceIndex = a, .sourceUnitIndex = ui,
                                    .abilityId = units[ui].abilities[a].abilityId,
                                    .level = units[ui].abilities[a].level };
                                units[ui].abilities[a].abilityId = -1;
                                clickedButton = true;
                                break;
                            }
                        }
                    }
                }
                // --- X button on unit cards to remove ---
                if (!clickedButton && !dragState.dragging) {
                    int tmpBlue2[BLUE_TEAM_MAX_SIZE]; int tmpCount2 = 0;
                    for (int i2 = 0; i2 < unitCount && tmpCount2 < BLUE_TEAM_MAX_SIZE; i2++)
                        if (units[i2].active && units[i2].team == TEAM_BLUE) tmpBlue2[tmpCount2++] = i2;
                    int totalCardsW2 = BLUE_TEAM_MAX_SIZE * HUD_CARD_WIDTH + (BLUE_TEAM_MAX_SIZE - 1) * HUD_CARD_SPACING;
                    int cardsStartX2 = (sw - totalCardsW2) / 2;
                    int cardsY2 = hudTop + HUD_SHOP_HEIGHT + 5;
                    for (int h = 0; h < tmpCount2; h++) {
                        int cardX = cardsStartX2 + h * (HUD_CARD_WIDTH + HUD_CARD_SPACING);
                        int xBtnSize = 16;
                        Rectangle xBtn = { (float)(cardX + HUD_CARD_WIDTH - xBtnSize - 2),
                                           (float)(cardsY2 + 2), (float)xBtnSize, (float)xBtnSize };
                        if (CheckCollisionPointRec(mouse, xBtn)) {
                            removeConfirmUnit = tmpBlue2[h];
                            clickedButton = true;
                            break;
                        }
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

            // --- Drag-and-drop release handling ---
            if (dragState.dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE)
            {
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();
                int hudTop2 = sh - HUD_TOTAL_HEIGHT;
                bool placed = false;

                // Collect blue units
                int dropBlue[BLUE_TEAM_MAX_SIZE]; int dropCount = 0;
                for (int i2 = 0; i2 < unitCount && dropCount < BLUE_TEAM_MAX_SIZE; i2++)
                    if (units[i2].active && units[i2].team == TEAM_BLUE) dropBlue[dropCount++] = i2;

                int totalCardsW = BLUE_TEAM_MAX_SIZE * HUD_CARD_WIDTH + (BLUE_TEAM_MAX_SIZE - 1) * HUD_CARD_SPACING;
                int cardsStartX = (sw - totalCardsW) / 2;
                int cardsY = hudTop2 + HUD_SHOP_HEIGHT + 5;

                // Check drop on unit ability slot
                for (int h = 0; h < dropCount && !placed; h++) {
                    int cardX = cardsStartX + h * (HUD_CARD_WIDTH + HUD_CARD_SPACING);
                    int abilStartX = cardX + HUD_PORTRAIT_SIZE + 12;
                    int abilStartY = cardsY + 8;
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        int col = a % 2, row = a / 2;
                        int ax = abilStartX + col * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                        int ay = abilStartY + row * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                        Rectangle r = { (float)ax, (float)ay, (float)HUD_ABILITY_SLOT_SIZE, (float)HUD_ABILITY_SLOT_SIZE };
                        if (CheckCollisionPointRec(mouse, r)) {
                            int ui = dropBlue[h];
                            // Dropping on the same slot we picked from — just restore it
                            if (dragState.sourceType == 1 && dragState.sourceUnitIndex == ui && dragState.sourceIndex == a) {
                                units[ui].abilities[a].abilityId = dragState.abilityId;
                                units[ui].abilities[a].level = dragState.level;
                                placed = true; break;
                            }
                            // Swap
                            int oldId = units[ui].abilities[a].abilityId;
                            int oldLv = units[ui].abilities[a].level;
                            units[ui].abilities[a].abilityId = dragState.abilityId;
                            units[ui].abilities[a].level = dragState.level;
                            units[ui].abilities[a].cooldownRemaining = 0;
                            units[ui].abilities[a].triggered = false;
                            // Put old ability back to source
                            if (dragState.sourceType == 0) {
                                inventory[dragState.sourceIndex].abilityId = oldId;
                                inventory[dragState.sourceIndex].level = oldLv;
                            } else {
                                units[dragState.sourceUnitIndex].abilities[dragState.sourceIndex].abilityId = oldId;
                                units[dragState.sourceUnitIndex].abilities[dragState.sourceIndex].level = oldLv;
                            }
                            placed = true; break;
                        }
                    }
                }
                // Check drop on inventory slot
                if (!placed) {
                    int invStartX = cardsStartX - (HUD_INVENTORY_COLS * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP)) - 20;
                    int invStartY = hudTop2 + HUD_SHOP_HEIGHT + 15;
                    for (int inv = 0; inv < MAX_INVENTORY_SLOTS && !placed; inv++) {
                        int col = inv % HUD_INVENTORY_COLS;
                        int row = inv / HUD_INVENTORY_COLS;
                        int ix = invStartX + col * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                        int iy = invStartY + row * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                        Rectangle r = { (float)ix, (float)iy, (float)HUD_ABILITY_SLOT_SIZE, (float)HUD_ABILITY_SLOT_SIZE };
                        if (CheckCollisionPointRec(mouse, r)) {
                            // Dropping on the same inventory slot we picked from — just restore it
                            if (dragState.sourceType == 0 && dragState.sourceIndex == inv) {
                                inventory[inv].abilityId = dragState.abilityId;
                                inventory[inv].level = dragState.level;
                                placed = true; break;
                            }
                            int oldId = inventory[inv].abilityId;
                            int oldLv = inventory[inv].level;
                            inventory[inv].abilityId = dragState.abilityId;
                            inventory[inv].level = dragState.level;
                            if (dragState.sourceType == 0) {
                                inventory[dragState.sourceIndex].abilityId = oldId;
                                inventory[dragState.sourceIndex].level = oldLv;
                            } else {
                                units[dragState.sourceUnitIndex].abilities[dragState.sourceIndex].abilityId = oldId;
                                units[dragState.sourceUnitIndex].abilities[dragState.sourceIndex].level = oldLv;
                            }
                            placed = true;
                        }
                    }
                }
                // Not placed — return to source
                if (!placed) {
                    if (dragState.sourceType == 0) {
                        inventory[dragState.sourceIndex].abilityId = dragState.abilityId;
                        inventory[dragState.sourceIndex].level = dragState.level;
                    } else {
                        units[dragState.sourceUnitIndex].abilities[dragState.sourceIndex].abilityId = dragState.abilityId;
                        units[dragState.sourceUnitIndex].abilities[dragState.sourceIndex].level = dragState.level;
                    }
                }
                dragState.dragging = false;
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: COMBAT — abilities, modifiers, projectiles, movement, attack
        //------------------------------------------------------------------------------
        else if (phase == PHASE_COMBAT)
        {
            // === STEP 1: Tick modifiers ===
            for (int m = 0; m < MAX_MODIFIERS; m++) {
                if (!modifiers[m].active) continue;
                int ui = modifiers[m].unitIndex;
                if (ui < 0 || ui >= unitCount || !units[ui].active) {
                    modifiers[m].active = false; continue;
                }
                if (modifiers[m].duration > 0) {
                    modifiers[m].duration -= dt;
                    if (modifiers[m].duration <= 0) {
                        if (modifiers[m].type == MOD_SHIELD) units[ui].shieldHP = 0;
                        modifiers[m].active = false; continue;
                    }
                }
                // Per-tick effects
                if (modifiers[m].type == MOD_DIG_HEAL) {
                    const UnitStats *s = &UNIT_STATS[units[ui].typeIndex];
                    units[ui].currentHealth += modifiers[m].value * dt;
                    if (units[ui].currentHealth > s->health) units[ui].currentHealth = s->health;
                }
            }

            // === STEP 1b: Spawn dig particles + update all particles ===
            for (int i = 0; i < unitCount; i++) {
                if (!units[i].active) continue;
                if (UnitHasModifier(modifiers, i, MOD_DIG_HEAL)) {
                    UnitType *dtype = &unitTypes[units[i].typeIndex];
                    float modelH = (dtype->baseBounds.max.y - dtype->baseBounds.min.y) * dtype->scale;
                    float modelR = (dtype->baseBounds.max.x - dtype->baseBounds.min.x) * dtype->scale * 0.6f;
                    // Spawn brown dirt particles around the model
                    for (int pp = 0; pp < 3; pp++) {
                        float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
                        float r = modelR + (float)GetRandomValue(5, 20) / 10.0f;
                        Vector3 pos = {
                            units[i].position.x + cosf(angle) * r,
                            units[i].position.y + (float)GetRandomValue(0, (int)(modelH * 10.0f)) / 10.0f,
                            units[i].position.z + sinf(angle) * r
                        };
                        Vector3 vel = {
                            cosf(angle) * 3.0f,
                            (float)GetRandomValue(20, 60) / 10.0f,
                            sinf(angle) * 3.0f
                        };
                        int shade = GetRandomValue(100, 180);
                        Color brown = { (unsigned char)shade, (unsigned char)(shade * 0.6f),
                                        (unsigned char)(shade * 0.3f), 255 };
                        float sz = (float)GetRandomValue(3, 8) / 10.0f;
                        SpawnParticle(particles, pos, vel, 0.5f + (float)GetRandomValue(0, 3) / 10.0f, sz, brown);
                    }
                }
            }
            UpdateParticles(particles, dt);
            UpdateFloatingTexts(floatingTexts, dt);

            // === STEP 2: Update projectiles ===
            for (int p = 0; p < MAX_PROJECTILES; p++) {
                if (!projectiles[p].active) continue;
                int ti = projectiles[p].targetIndex;
                // Target gone?
                if (ti < 0 || ti >= unitCount || !units[ti].active) {
                    if ((projectiles[p].type == PROJ_CHAIN_FROST || projectiles[p].type == PROJ_MAELSTROM) && projectiles[p].bouncesRemaining > 0) {
                        int next = FindChainFrostTarget(units, unitCount, projectiles[p].position,
                            projectiles[p].sourceTeam, projectiles[p].lastHitUnit, projectiles[p].bounceRange);
                        if (next >= 0) { projectiles[p].targetIndex = next; continue; }
                    }
                    projectiles[p].active = false; continue;
                }
                // Move toward target
                Vector3 tgt = { units[ti].position.x, units[ti].position.y + 3.0f, units[ti].position.z };
                float pdx = tgt.x - projectiles[p].position.x;
                float pdy = tgt.y - projectiles[p].position.y;
                float pdz = tgt.z - projectiles[p].position.z;
                float pdist = sqrtf(pdx*pdx + pdy*pdy + pdz*pdz);
                float pstep = projectiles[p].speed * dt;

                if (pdist <= pstep) {
                    // HIT — Hook: pull target to caster, damage by distance
                    if (projectiles[p].type == PROJ_HOOK) {
                        if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                            float hookDist = DistXZ(units[ti].position, units[projectiles[p].sourceIndex].position);
                            float hitDmg = hookDist * projectiles[p].damage;
                            if (units[ti].shieldHP > 0) {
                                if (hitDmg <= units[ti].shieldHP) { units[ti].shieldHP -= hitDmg; hitDmg = 0; }
                                else { hitDmg -= units[ti].shieldHP; units[ti].shieldHP = 0; }
                            }
                            units[ti].currentHealth -= hitDmg;
                            units[ti].hitFlash = HIT_FLASH_DURATION;
                            // Teleport target to caster
                            units[ti].position.x = units[projectiles[p].sourceIndex].position.x;
                            units[ti].position.z = units[projectiles[p].sourceIndex].position.z;
                            TriggerShake(&shake, 6.0f, 0.3f);
                            if (units[ti].currentHealth <= 0) units[ti].active = false;
                        }
                        projectiles[p].active = false;
                    }
                    // HIT — Maelstrom: bounce like chain frost
                    else if (projectiles[p].type == PROJ_MAELSTROM) {
                        if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                            float hitDmg = projectiles[p].damage;
                            if (units[ti].shieldHP > 0) {
                                if (hitDmg <= units[ti].shieldHP) { units[ti].shieldHP -= hitDmg; hitDmg = 0; }
                                else { hitDmg -= units[ti].shieldHP; units[ti].shieldHP = 0; }
                            }
                            units[ti].currentHealth -= hitDmg;
                            units[ti].hitFlash = HIT_FLASH_DURATION;
                            if (units[ti].currentHealth <= 0) units[ti].active = false;
                        }
                        if (projectiles[p].bouncesRemaining > 0) {
                            projectiles[p].bouncesRemaining--;
                            projectiles[p].lastHitUnit = ti;
                            projectiles[p].position = units[ti].position;
                            projectiles[p].position.y += 3.0f;
                            int next = FindChainFrostTarget(units, unitCount, units[ti].position,
                                projectiles[p].sourceTeam, ti, projectiles[p].bounceRange);
                            if (next >= 0) projectiles[p].targetIndex = next;
                            else projectiles[p].active = false;
                        } else {
                            projectiles[p].active = false;
                        }
                    }
                    // HIT — normal (Magic Missile / Chain Frost)
                    else {
                    if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                        float hitDmg = projectiles[p].damage;
                        // Magic Missile: damage is a fraction of target max HP
                        if (projectiles[p].type == PROJ_MAGIC_MISSILE)
                            hitDmg *= UNIT_STATS[units[ti].typeIndex].health;
                        // Shield absorption
                        if (units[ti].shieldHP > 0) {
                            if (hitDmg <= units[ti].shieldHP) { units[ti].shieldHP -= hitDmg; hitDmg = 0; }
                            else { hitDmg -= units[ti].shieldHP; units[ti].shieldHP = 0; }
                        }
                        units[ti].currentHealth -= hitDmg;
                        units[ti].hitFlash = HIT_FLASH_DURATION;
                        if (projectiles[p].stunDuration > 0) {
                            AddModifier(modifiers, ti, MOD_STUN, projectiles[p].stunDuration, 0);
                            TriggerShake(&shake, 5.0f, 0.25f);
                        }
                        if (units[ti].currentHealth <= 0) units[ti].active = false;
                    }
                    // Chain Frost bounce
                    if (projectiles[p].type == PROJ_CHAIN_FROST && projectiles[p].bouncesRemaining > 0) {
                        projectiles[p].bouncesRemaining--;
                        projectiles[p].lastHitUnit = ti;
                        projectiles[p].position = units[ti].position;
                        projectiles[p].position.y += 3.0f;
                        int next = FindChainFrostTarget(units, unitCount, units[ti].position,
                            projectiles[p].sourceTeam, ti, projectiles[p].bounceRange);
                        if (next >= 0) projectiles[p].targetIndex = next;
                        else projectiles[p].active = false;
                    } else {
                        projectiles[p].active = false;
                    }
                    } // end else (normal projectile hit)
                } else {
                    projectiles[p].position.x += (pdx / pdist) * pstep;
                    projectiles[p].position.y += (pdy / pdist) * pstep;
                    projectiles[p].position.z += (pdz / pdist) * pstep;
                }
            }

            // Update fissure lifetimes
            UpdateFissures(fissures, dt);

            // Build shared combat state for ability handlers
            CombatState combatState = {
                .units = units, .unitCount = unitCount,
                .modifiers = modifiers, .projectiles = projectiles,
                .particles = particles, .fissures = fissures,
                .floatingTexts = floatingTexts, .shake = &shake,
            };

            // === STEP 3: Process each unit ===
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];
                bool stunned = UnitHasModifier(modifiers, i, MOD_STUN);

                // Tick ability cooldowns
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                    if (units[i].abilities[a].abilityId < 0) continue;
                    if (units[i].abilities[a].cooldownRemaining > 0)
                        units[i].abilities[a].cooldownRemaining -= dt;
                }

                // Passive triggers (Dig, Sunder) — blocked by stun
                if (!stunned) {
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        AbilitySlot *slot = &units[i].abilities[a];
                        if (slot->abilityId == ABILITY_DIG) {
                            if (slot->triggered || slot->cooldownRemaining > 0) continue;
                            const AbilityDef *def = &ABILITY_DEFS[ABILITY_DIG];
                            float threshold = def->values[slot->level][AV_DIG_HP_THRESH];
                            float unitMaxHP = stats->health * units[i].hpMultiplier;
                            if (units[i].currentHealth > 0 && units[i].currentHealth <= unitMaxHP * threshold) {
                                slot->triggered = true;
                                slot->cooldownRemaining = def->cooldown[slot->level];
                                float healDur = def->values[slot->level][AV_DIG_HEAL_DUR];
                                float healPerSec = unitMaxHP / healDur;
                                AddModifier(modifiers, i, MOD_INVULNERABLE, healDur, 0);
                                AddModifier(modifiers, i, MOD_DIG_HEAL, healDur, healPerSec);
                            }
                        } else if (slot->abilityId == ABILITY_SUNDER) {
                            CheckPassiveSunder(&combatState, i);
                        }
                    }
                }

                bool digging = UnitHasModifier(modifiers, i, MOD_DIG_HEAL);
                if (stunned || digging) continue;

                // Find target
                int target = FindClosestEnemy(units, unitCount, i);
                units[i].targetIndex = target;

                // Smooth rotation towards target
                if (target >= 0 && units[target].active) {
                    float dx = units[target].position.x - units[i].position.x;
                    float dz = units[target].position.z - units[i].position.z;
                    float goalAngle = atan2f(dx, dz) * (180.0f / PI);
                    float diff = goalAngle - units[i].facingAngle;
                    // Normalize to [-180, 180]
                    while (diff > 180.0f) diff -= 360.0f;
                    while (diff < -180.0f) diff += 360.0f;
                    float turnSpeed = 360.0f; // degrees per second
                    if (fabsf(diff) < turnSpeed * dt)
                        units[i].facingAngle = goalAngle;
                    else
                        units[i].facingAngle += (diff > 0 ? 1.0f : -1.0f) * turnSpeed * dt;
                }

                // Tick ability cast delay
                if (units[i].abilityCastDelay > 0)
                    units[i].abilityCastDelay -= dt;

                // Active ability casting — one per frame, clockwise rotation
                bool castThisFrame = false;
                if (units[i].abilityCastDelay <= 0)
                for (int attempt = 0; attempt < MAX_ABILITIES_PER_UNIT && !castThisFrame; attempt++) {
                    int slotIdx = ACTIVATION_ORDER[units[i].nextAbilitySlot];
                    units[i].nextAbilitySlot = (units[i].nextAbilitySlot + 1) % MAX_ABILITIES_PER_UNIT;

                    AbilitySlot *slot = &units[i].abilities[slotIdx];
                    if (slot->abilityId < 0 || slot->cooldownRemaining > 0) continue;
                    const AbilityDef *def = &ABILITY_DEFS[slot->abilityId];
                    if (def->isPassive) continue; // skip passives (Dig, Sunder)

                    // Range gate: if ability has a cast range, check closest enemy is within it
                    float castRange = def->range[slot->level];
                    if (castRange > 0 && target >= 0) {
                        float d = DistXZ(units[i].position, units[target].position);
                        if (d > castRange) continue;
                    } else if (castRange > 0 && target < 0) {
                        continue; // need a target but none exists
                    }

                    switch (slot->abilityId) {
                    case ABILITY_MAGIC_MISSILE: castThisFrame = CastMagicMissile(&combatState, i, slot, target); break;
                    case ABILITY_VACUUM:        castThisFrame = CastVacuum(&combatState, i, slot); break;
                    case ABILITY_CHAIN_FROST:   castThisFrame = CastChainFrost(&combatState, i, slot, target); break;
                    case ABILITY_BLOOD_RAGE:    castThisFrame = CastBloodRage(&combatState, i, slot); break;
                    case ABILITY_EARTHQUAKE:
                        castThisFrame = CastEarthquake(&combatState, i, slot);
                        if (castThisFrame) {
                            // Aggressive tile ripple from earthquake epicenter
                            float eqX = units[i].position.x;
                            float eqZ = units[i].position.z;
                            float eqRadius = ABILITY_DEFS[ABILITY_EARTHQUAKE].values[slot->level][AV_EQ_RADIUS];
                            float gridOriginEq = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                            for (int tr = 0; tr < TILE_GRID_SIZE; tr++) {
                                for (int tc = 0; tc < TILE_GRID_SIZE; tc++) {
                                    float cx = gridOriginEq + (tc + 0.5f) * TILE_WORLD_SIZE;
                                    float cz = gridOriginEq + (tr + 0.5f) * TILE_WORLD_SIZE;
                                    float dxw = cx - eqX, dzw = cz - eqZ;
                                    float dist = sqrtf(dxw*dxw + dzw*dzw);
                                    float wobbleR = eqRadius * 3.0f;
                                    if (dist < wobbleR) {
                                        float strength = expf(-1.5f * dist / wobbleR);
                                        tileWobble[tr][tc] = TILE_WOBBLE_MAX * 1.5f * strength;
                                        tileWobbleTime[tr][tc] = -(dist * 0.012f);
                                        float len = dist > 0.1f ? dist : 1.0f;
                                        tileWobbleDirX[tr][tc] = dzw / len;
                                        tileWobbleDirZ[tr][tc] = -dxw / len;
                                    }
                                }
                            }
                        }
                        break;
                    case ABILITY_SPELL_PROTECT: castThisFrame = CastSpellProtect(&combatState, i, slot); break;
                    case ABILITY_CRAGGY_ARMOR:  castThisFrame = CastCraggyArmor(&combatState, i, slot); break;
                    case ABILITY_STONE_GAZE:    castThisFrame = CastStoneGaze(&combatState, i, slot); break;
                    case ABILITY_FISSURE:       castThisFrame = CastFissure(&combatState, i, slot, target); break;
                    case ABILITY_VLAD_AURA:     castThisFrame = CastVladAura(&combatState, i, slot); break;
                    case ABILITY_MAELSTROM:     castThisFrame = CastMaelstrom(&combatState, i, slot); break;
                    case ABILITY_SWAP:          castThisFrame = CastSwap(&combatState, i, slot); break;
                    case ABILITY_APHOTIC_SHIELD:castThisFrame = CastAphoticShield(&combatState, i, slot); break;
                    case ABILITY_HOOK:          castThisFrame = CastHook(&combatState, i, slot); break;
                    case ABILITY_PRIMAL_CHARGE: castThisFrame = CastPrimalCharge(&combatState, i, slot); break;
                    default: break;
                    }
                    if (castThisFrame) {
                        SpawnFloatingText(floatingTexts, units[i].position,
                            def->name, def->color, 1.0f);
                        units[i].abilityCastDelay = 0.75f;
                    }
                }

                // Primal Charge movement — overrides normal movement
                if (units[i].chargeTarget >= 0) {
                    int ct = units[i].chargeTarget;
                    if (ct >= unitCount || !units[ct].active) {
                        units[i].chargeTarget = -1;
                    } else {
                        float chargeDist = DistXZ(units[i].position, units[ct].position);
                        float chargeSpeed = GetModifierValue(modifiers, i, MOD_CHARGING);
                        if (chargeSpeed <= 0) chargeSpeed = 80.0f;
                        if (chargeDist <= ATTACK_RANGE) {
                            // IMPACT — AoE damage + knockback
                            int chargeLvl = 0;
                            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                if (units[i].abilities[a].abilityId == ABILITY_PRIMAL_CHARGE) {
                                    chargeLvl = units[i].abilities[a].level; break;
                                }
                            }
                            const AbilityDef *pcDef = &ABILITY_DEFS[ABILITY_PRIMAL_CHARGE];
                            float pcDmg = pcDef->values[chargeLvl][AV_PC_DAMAGE];
                            float pcKnock = pcDef->values[chargeLvl][AV_PC_KNOCKBACK];
                            float pcRadius = pcDef->values[chargeLvl][AV_PC_AOE_RADIUS];
                            for (int j = 0; j < unitCount; j++) {
                                if (!units[j].active || units[j].team == units[i].team) continue;
                                if (UnitHasModifier(modifiers, j, MOD_INVULNERABLE)) continue;
                                float dd = DistXZ(units[ct].position, units[j].position);
                                if (dd <= pcRadius) {
                                    float dmgHit = pcDmg;
                                    if (units[j].shieldHP > 0) {
                                        if (dmgHit <= units[j].shieldHP) { units[j].shieldHP -= dmgHit; dmgHit = 0; }
                                        else { dmgHit -= units[j].shieldHP; units[j].shieldHP = 0; }
                                    }
                                    units[j].currentHealth -= dmgHit;
                                    units[j].hitFlash = HIT_FLASH_DURATION;
                                    if (units[j].currentHealth <= 0) units[j].active = false;
                                    // Knockback
                                    float kx = units[j].position.x - units[ct].position.x;
                                    float kz = units[j].position.z - units[ct].position.z;
                                    float klen = sqrtf(kx*kx + kz*kz);
                                    if (klen > 0.001f) {
                                        units[j].position.x += (kx/klen) * pcKnock;
                                        units[j].position.z += (kz/klen) * pcKnock;
                                    }
                                }
                            }
                            TriggerShake(&shake, 8.0f, 0.4f);
                            units[i].chargeTarget = -1;
                            // Remove charging modifier
                            for (int m = 0; m < MAX_MODIFIERS; m++) {
                                if (modifiers[m].active && modifiers[m].unitIndex == i && modifiers[m].type == MOD_CHARGING)
                                    modifiers[m].active = false;
                            }
                        } else {
                            float cdx = units[ct].position.x - units[i].position.x;
                            float cdz = units[ct].position.z - units[i].position.z;
                            float clen = sqrtf(cdx*cdx + cdz*cdz);
                            units[i].position.x += (cdx/clen) * chargeSpeed * dt;
                            units[i].position.z += (cdz/clen) * chargeSpeed * dt;
                        }
                        continue; // skip normal movement while charging
                    }
                }

                // Movement + basic attack
                if (target < 0) continue;
                float moveSpeed = stats->movementSpeed;
                float speedMult = GetModifierValue(modifiers, i, MOD_SPEED_MULT);
                if (speedMult > 0) moveSpeed *= speedMult;

                float dist = DistXZ(units[i].position, units[target].position);
                if (dist > ATTACK_RANGE)
                {
                    Vector3 oldPos = units[i].position;
                    float dx = units[target].position.x - units[i].position.x;
                    float dz = units[target].position.z - units[i].position.z;
                    float len = sqrtf(dx*dx + dz*dz);
                    if (len > 0.001f) {
                        units[i].position.x += (dx/len) * moveSpeed * dt;
                        units[i].position.z += (dz/len) * moveSpeed * dt;
                    }
                    // Fissure collision — slide along impassable terrain
                    float unitRadius = 2.0f;
                    units[i].position = ResolveFissureCollision(fissures, units[i].position, oldPos, unitRadius);

                    // Unit-unit collision — push overlapping units apart on XZ plane
                    for (int j = 0; j < unitCount; j++) {
                        if (j == i || !units[j].active) continue;
                        float cdist = DistXZ(units[i].position, units[j].position);
                        float minDist = UNIT_COLLISION_RADIUS * 2.0f;
                        if (cdist < minDist && cdist > 0.001f) {
                            float overlap = minDist - cdist;
                            float pushX = (units[i].position.x - units[j].position.x) / cdist;
                            float pushZ = (units[i].position.z - units[j].position.z) / cdist;
                            units[i].position.x += pushX * overlap * 0.5f;
                            units[i].position.z += pushZ * overlap * 0.5f;
                            units[j].position.x -= pushX * overlap * 0.5f;
                            units[j].position.z -= pushZ * overlap * 0.5f;
                        }
                    }
                }
                else
                {
                    units[i].attackCooldown -= dt;
                    if (units[i].attackCooldown <= 0.0f)
                    {
                        if (!UnitHasModifier(modifiers, target, MOD_INVULNERABLE)) {
                            float dmg = stats->attackDamage * units[i].dmgMultiplier;
                            float armor = GetModifierValue(modifiers, target, MOD_ARMOR);
                            dmg -= armor;
                            if (dmg < 0) dmg = 0;
                            // Shield absorption
                            if (units[target].shieldHP > 0) {
                                if (dmg <= units[target].shieldHP) { units[target].shieldHP -= dmg; dmg = 0; }
                                else { dmg -= units[target].shieldHP; units[target].shieldHP = 0; }
                            }
                            units[target].currentHealth -= dmg;
                            units[target].hitFlash = HIT_FLASH_DURATION;
                            // Lifesteal
                            float ls = GetModifierValue(modifiers, i, MOD_LIFESTEAL);
                            if (ls > 0) {
                                float maxHP = stats->health * units[i].hpMultiplier;
                                units[i].currentHealth += dmg * ls;
                                if (units[i].currentHealth > maxHP)
                                    units[i].currentHealth = maxHP;
                            }
                            // Craggy Armor retaliation — chance to stun attacker
                            CheckCraggyArmorRetaliation(&combatState, i, target);
                            // Maelstrom on-hit proc
                            if (UnitHasModifier(modifiers, i, MOD_MAELSTROM)) {
                                float procChance = GetModifierValue(modifiers, i, MOD_MAELSTROM);
                                float roll = (float)GetRandomValue(0, 100) / 100.0f;
                                if (roll < procChance) {
                                    // Find maelstrom ability level
                                    int mlLvl = 0;
                                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                        if (units[i].abilities[a].abilityId == ABILITY_MAELSTROM) {
                                            mlLvl = units[i].abilities[a].level; break;
                                        }
                                    }
                                    const AbilityDef *mlDef = &ABILITY_DEFS[ABILITY_MAELSTROM];
                                    SpawnMaelstromProjectile(projectiles,
                                        units[target].position, target, i, units[i].team, mlLvl,
                                        mlDef->values[mlLvl][AV_ML_SPEED],
                                        mlDef->values[mlLvl][AV_ML_DAMAGE],
                                        (int)mlDef->values[mlLvl][AV_ML_BOUNCES],
                                        mlDef->values[mlLvl][AV_ML_BOUNCE_RANGE]);
                                }
                            }
                            if (units[target].currentHealth <= 0) units[target].active = false;
                        }
                        units[i].attackCooldown = stats->attackSpeed;
                    }
                }
            }

            // Stone Gaze update — enemies facing a stone-gazer accumulate gaze
            for (int i = 0; i < unitCount; i++) {
                if (!units[i].active) continue;
                // Check if any enemy has Stone Gaze active
                bool beingGazed = false;
                for (int g = 0; g < unitCount; g++) {
                    if (!units[g].active || units[g].team == units[i].team) continue;
                    if (!UnitHasModifier(modifiers, g, MOD_STONE_GAZE)) continue;
                    // Check if unit i is facing toward gazer g (within cone)
                    float dx = units[g].position.x - units[i].position.x;
                    float dz = units[g].position.z - units[i].position.z;
                    float distToGazer = sqrtf(dx*dx + dz*dz);
                    if (distToGazer < 0.1f) continue;
                    // Unit i's facing direction
                    float facingRad = units[i].facingAngle * (PI / 180.0f);
                    float faceDirX = sinf(facingRad);
                    float faceDirZ = cosf(facingRad);
                    // Dot product to check if facing toward gazer
                    float dot = (dx/distToGazer) * faceDirX + (dz/distToGazer) * faceDirZ;
                    float coneAngle = 45.0f; // default cone half-angle
                    // Get cone angle from the gazer's Stone Gaze ability
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        if (units[g].abilities[a].abilityId == ABILITY_STONE_GAZE) {
                            int lvl = units[g].abilities[a].level;
                            coneAngle = ABILITY_DEFS[ABILITY_STONE_GAZE].values[lvl][AV_SG_CONE_ANGLE];
                            break;
                        }
                    }
                    float coneThresh = cosf(coneAngle * (PI / 180.0f));
                    if (dot >= coneThresh) {
                        units[i].gazeAccum += dt;
                        beingGazed = true;
                        // Check if threshold reached — find gazer's ability level
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            if (units[g].abilities[a].abilityId == ABILITY_STONE_GAZE) {
                                int lvl = units[g].abilities[a].level;
                                float thresh = ABILITY_DEFS[ABILITY_STONE_GAZE].values[lvl][AV_SG_GAZE_THRESH];
                                float stunDur = ABILITY_DEFS[ABILITY_STONE_GAZE].values[lvl][AV_SG_STUN_DUR];
                                if (units[i].gazeAccum >= thresh) {
                                    AddModifier(modifiers, i, MOD_STUN, stunDur, 0);
                                    units[i].gazeAccum = 0;
                                    TriggerShake(&shake, 3.0f, 0.2f);
                                    SpawnFloatingText(floatingTexts, units[i].position,
                                        "PETRIFIED!", (Color){160, 80, 200, 255}, 1.0f);
                                }
                                break;
                            }
                        }
                        break; // only accumulate from one gazer at a time
                    }
                }
                if (!beingGazed && units[i].gazeAccum > 0) {
                    units[i].gazeAccum -= dt * 2.0f; // decay twice as fast
                    if (units[i].gazeAccum < 0) units[i].gazeAccum = 0;
                }
            }

            // Smooth Y toward ground during combat
            for (int i = 0; i < unitCount; i++) {
                if (!units[i].active) continue;
                units[i].position.y += (0.0f - units[i].position.y) * 0.1f;
            }

            // Check round end
            if (isMultiplayer) {
                // In multiplayer, poll for server result
                net_client_poll(&netClient);
                if (netClient.roundResultReady) {
                    netClient.roundResultReady = false;
                    if (netClient.roundWinner == 0) { blueWins++; roundResultText = "YOU WIN THE ROUND!"; }
                    else if (netClient.roundWinner == 1) { redWins++; roundResultText = "OPPONENT WINS!"; }
                    else roundResultText = "DRAW — NO SURVIVORS!";
                    currentRound = netClient.currentRound;
                    lastOutcomeWin = (netClient.roundWinner == 0);
                    phase = PHASE_ROUND_OVER;
                    roundOverTimer = 2.5f;
                    ClearAllParticles(particles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                }
                if (netClient.gameOver) {
                    netClient.gameOver = false;
                    if (netClient.gameWinner == 0) roundResultText = "YOU WIN THE MATCH!";
                    else roundResultText = "OPPONENT WINS THE MATCH!";
                    lastOutcomeWin = (netClient.gameWinner == 0);
                    phase = PHASE_GAME_OVER;
                    ClearAllParticles(particles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                }
            } else {
                int ba, ra;
                CountTeams(units, unitCount, &ba, &ra);
                if (ba == 0 || ra == 0) {
                    if (ba > 0) { blueWins++; roundResultText = "BLUE WINS THE ROUND!"; blueLostLastRound = false; }
                    else if (ra > 0) { redWins++; roundResultText = "RED WINS THE ROUND!"; blueLostLastRound = true; }
                    else { roundResultText = "DRAW — NO SURVIVORS!"; blueLostLastRound = true; }
                    currentRound++;
                    lastOutcomeWin = (ba > 0);
                    phase = PHASE_ROUND_OVER;
                    roundOverTimer = 2.5f;
                    ClearAllParticles(particles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    statueSpawn.phase = SSPAWN_INACTIVE;
                }
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: ROUND_OVER — brief pause, then milestone/death/prep
        //------------------------------------------------------------------------------
        else if (phase == PHASE_ROUND_OVER)
        {
            // Multiplayer: poll for next prep from server
            if (isMultiplayer) {
                net_client_poll(&netClient);
                roundOverTimer -= dt;
                if (netClient.prepStarted) {
                    netClient.prepStarted = false;
                    playerGold = netClient.currentGold;
                    currentRound = netClient.currentRound;
                    currentRoundIsPve = netClient.isPveRound;
                    for (int i = 0; i < MAX_SHOP_SLOTS; i++)
                        shopSlots[i] = netClient.serverShop[i];
                    RestoreSnapshot(units, &unitCount, snapshots, snapshotCount);
                    for (int i = 0; i < unitCount; i++)
                        if (units[i].team == TEAM_RED) units[i].active = false;
                    ClearAllModifiers(modifiers);
                    ClearAllProjectiles(projectiles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    playerReady = false;
                    waitingForOpponent = false;
                    phase = PHASE_PREP;
                }
                if (netClient.gameOver) {
                    netClient.gameOver = false;
                    if (netClient.gameWinner == 0) roundResultText = "YOU WIN THE MATCH!";
                    else roundResultText = "OPPONENT WINS THE MATCH!";
                    lastOutcomeWin = (netClient.gameWinner == 0);
                    phase = PHASE_GAME_OVER;
                }
            }
            // Solo: original logic
            else {
            roundOverTimer -= dt;
            if (roundOverTimer <= 0.0f)
            {
                if (blueLostLastRound && lastMilestoneRound > 0) {
                    // DEATH PENALTY: lost after a milestone — units gone
                    deathPenalty = true;
                    lastOutcomeWin = false;
                    phase = PHASE_GAME_OVER;
                } else if (currentRound > 0 && currentRound % 5 == 0) {
                    // Milestone reached — go to selection screen
                    // Restore blue units for milestone screen
                    RestoreSnapshot(units, &unitCount, snapshots, snapshotCount);
                    for (int i = 0; i < unitCount; i++) {
                        units[i].nextAbilitySlot = 0;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            units[i].abilities[a].cooldownRemaining = 0;
                            units[i].abilities[a].triggered = false;
                        }
                    }
                    ClearAllModifiers(modifiers);
                    ClearAllProjectiles(projectiles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    ClearRedUnits(units, &unitCount);
                    phase = PHASE_MILESTONE;
                } else {
                    // Normal round transition
                    RestoreSnapshot(units, &unitCount, snapshots, snapshotCount);
                    for (int i = 0; i < unitCount; i++) {
                        units[i].nextAbilitySlot = 0;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            units[i].abilities[a].cooldownRemaining = 0;
                            units[i].abilities[a].triggered = false;
                        }
                    }
                    ClearAllModifiers(modifiers);
                    ClearAllProjectiles(projectiles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    ClearRedUnits(units, &unitCount);
                    SpawnWave(units, &unitCount, currentRound, unitTypeCount);
                    playerGold += goldPerRound;
                    RollShop(shopSlots, &playerGold, 0);
                    phase = PHASE_PREP;
                }
            }
            } // end solo else
        }
        //------------------------------------------------------------------------------
        // PHASE: MILESTONE — "Set in Stone" selection screen
        //------------------------------------------------------------------------------
        else if (phase == PHASE_MILESTONE)
        {
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();

                // Collect active blue units
                int msBlue[BLUE_TEAM_MAX_SIZE]; int msCount = 0;
                for (int i = 0; i < unitCount && msCount < BLUE_TEAM_MAX_SIZE; i++)
                    if (units[i].active && units[i].team == TEAM_BLUE) msBlue[msCount++] = i;

                // Card layout (display only, no toggles)
                int cardW = 200, cardH = 140, cardGap = 20;
                int totalW = msCount * cardW + (msCount > 1 ? (msCount - 1) * cardGap : 0);
                int startX = (sw - totalW) / 2;
                int cardY = sh / 2 - cardH / 2 - 20;
                (void)totalW; (void)startX; // positioning computed for drawing code below

                // Buttons (two: SET IN STONE, CONTINUE)
                int btnW = 180, btnH = 44;
                int btnY = cardY + cardH + 40;
                int btnGap = 30;
                int totalBtnW = 2 * btnW + btnGap;
                int btnStartX = (sw - totalBtnW) / 2;

                // SET IN STONE button — saves entire party to leaderboard, then game over
                Rectangle setBtn = { (float)btnStartX, (float)btnY, (float)btnW, (float)btnH };
                if (CheckCollisionPointRec(mouse, setBtn) && msCount > 0) {
                    // Build leaderboard entry from all blue units
                    LeaderboardEntry entry = {0};
                    strncpy(entry.playerName, playerName, 31);
                    entry.playerName[31] = '\0';
                    entry.highestRound = currentRound;
                    entry.unitCount = msCount;
                    for (int h = 0; h < msCount; h++) {
                        int ui = msBlue[h];
                        entry.units[h].typeIndex = units[ui].typeIndex;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            entry.units[h].abilities[a].abilityId = units[ui].abilities[a].abilityId;
                            entry.units[h].abilities[a].level = units[ui].abilities[a].level;
                        }
                    }
                    InsertLeaderboardEntry(&leaderboard, &entry);
                    SaveLeaderboard(&leaderboard);

                    lastMilestoneRound = currentRound;
                    deathPenalty = false;
                    lastOutcomeWin = true;
                    phase = PHASE_GAME_OVER;
                }

                // CONTINUE button — skip prestige, keep playing
                Rectangle contBtn = { (float)(btnStartX + btnW + btnGap), (float)btnY, (float)btnW, (float)btnH };
                if (CheckCollisionPointRec(mouse, contBtn)) {
                    lastMilestoneRound = currentRound;
                    SpawnWave(units, &unitCount, currentRound, unitTypeCount);
                    playerGold += goldPerRound;
                    RollShop(shopSlots, &playerGold, 0);
                    phase = PHASE_PREP;
                }
            }
        }
        //------------------------------------------------------------------------------
        // PHASE: GAME_OVER — show final result, press R to return to menu
        //------------------------------------------------------------------------------
        else if (phase == PHASE_GAME_OVER)
        {
            // Multiplayer: press R to return to menu
            if (isMultiplayer && IsKeyPressed(KEY_R)) {
                net_client_disconnect(&netClient);
                isMultiplayer = false;
                unitCount = 0;
                snapshotCount = 0;
                currentRound = 0;
                blueWins = 0;
                redWins = 0;
                roundResultText = "";
                ClearAllModifiers(modifiers);
                ClearAllProjectiles(projectiles);
                ClearAllParticles(particles);
                ClearAllFloatingTexts(floatingTexts);
                ClearAllFissures(fissures);
                playerGold = 100;
                for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                dragState.dragging = false;
                joinCodeLen = 0;
                joinCodeInput[0] = '\0';
                phase = PHASE_MENU;
            }

            // Solo: existing game over logic
            if (!isMultiplayer && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !deathPenalty) {
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();

                // Collect surviving blue units for withdraw
                int goBlue[BLUE_TEAM_MAX_SIZE]; int goCount = 0;
                for (int i = 0; i < unitCount && goCount < BLUE_TEAM_MAX_SIZE; i++)
                    if (units[i].active && units[i].team == TEAM_BLUE) goBlue[goCount++] = i;

                // Withdraw buttons per unit card
                int cardW = 200, cardH = 140, cardGap = 20;
                int totalW = goCount * cardW + (goCount > 1 ? (goCount - 1) * cardGap : 0);
                int startX = (sw - totalW) / 2;
                int cardY = sh / 2 - 40;
                for (int h = 0; h < goCount; h++) {
                    int cx = startX + h * (cardW + cardGap);
                    Rectangle wdBtn = { (float)(cx + 10), (float)(cardY + cardH - 34), (float)(cardW - 20), 28 };
                    if (CheckCollisionPointRec(mouse, wdBtn)) {
                        // Withdraw placeholder — mark unit for NFC export
                        printf("[WITHDRAW] Unit %d (%s) withdrawn for NFC export\n",
                               goBlue[h], unitTypes[units[goBlue[h]].typeIndex].name);
                        units[goBlue[h]].active = false;
                        CompactBlueUnits(units, &unitCount);
                        break; // re-layout next frame
                    }
                }

                // RESET button
                int resetBtnW = 180, resetBtnH = 44;
                int resetBtnY = cardY + cardH + 30;
                Rectangle resetBtn = { (float)(sw/2 - resetBtnW/2), (float)resetBtnY, (float)resetBtnW, (float)resetBtnH };
                if (CheckCollisionPointRec(mouse, resetBtn)) {
                    // Full reset — go to menu
                    unitCount = 0;
                    snapshotCount = 0;
                    currentRound = 0;
                    blueWins = 0;
                    redWins = 0;
                    roundResultText = "";
                    lastMilestoneRound = 0;
                    blueLostLastRound = false;
                    deathPenalty = false;
                    ClearAllModifiers(modifiers);
                    ClearAllProjectiles(projectiles);
                    ClearAllParticles(particles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    statueSpawn.phase = SSPAWN_INACTIVE;
                    playerGold = 100;
                    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                    dragState.dragging = false;
                    phase = PHASE_MENU;
                }
            }

            // Death penalty: just press R (no withdraw possible)
            if (deathPenalty && IsKeyPressed(KEY_R)) {
                unitCount = 0;
                snapshotCount = 0;
                currentRound = 0;
                blueWins = 0;
                redWins = 0;
                roundResultText = "";
                lastMilestoneRound = 0;
                blueLostLastRound = false;
                deathPenalty = false;
                ClearAllModifiers(modifiers);
                ClearAllProjectiles(projectiles);
                ClearAllParticles(particles);
                ClearAllFloatingTexts(floatingTexts);
                ClearAllFissures(fissures);
                statueSpawn.phase = SSPAWN_INACTIVE;
                playerGold = 100;
                for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                dragState.dragging = false;
                phase = PHASE_MENU;
            }
        }

        //==============================================================================
        // ANIMATION UPDATE
        //==============================================================================
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active) continue;
            if (units[i].hitFlash > 0) units[i].hitFlash -= dt;
            if (IsUnitInStatueSpawn(&statueSpawn, i)) continue; // frozen as statue
            UnitType *type = &unitTypes[units[i].typeIndex];
            if (!type->hasAnimations) continue;

            // Determine desired anim state
            AnimState desired = ANIM_IDLE;
            if (phase == PHASE_COMBAT && units[i].targetIndex >= 0) {
                float dist = DistXZ(units[i].position, units[units[i].targetIndex].position);
                if (dist > ATTACK_RANGE) desired = ANIM_WALK;
            }

            // Reset frame on anim change
            if (desired != units[i].currentAnim) {
                units[i].currentAnim = desired;
                units[i].animFrame = 0;
            }

            // Advance frame
            int idx = type->animIndex[units[i].currentAnim];
            if (idx >= 0) {
                ModelAnimation *arr = (units[i].currentAnim == ANIM_IDLE) ? type->idleAnims : type->anims;
                int frameCount = arr[idx].frameCount;
                if (frameCount > 0)
                    units[i].animFrame = (units[i].animFrame + 1) % frameCount;
            }
        }

        //==============================================================================
        // WIN/LOSS SFX
        //==============================================================================
        if (phase != prevPhase && (phase == PHASE_GAME_OVER || phase == PHASE_ROUND_OVER)) {
            StopSound(sfxWin);
            StopSound(sfxLoss);
            PlaySound(lastOutcomeWin ? sfxWin : sfxLoss);
        }

        //==============================================================================
        // DRAW
        //==============================================================================
        BeginDrawing();
        ClearBackground((Color){ 45, 40, 35, 255 });

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
                    if (type->hasAnimations && type->animIndex[ANIM_IDLE] >= 0)
                        UpdateModelAnimation(type->model, type->idleAnims[type->animIndex[ANIM_IDLE]], 0);
                    DrawModel(type->model, (Vector3){ 0, 0, 0 }, type->scale, GetTeamTint(TEAM_BLUE));
                EndMode3D();
            EndTextureMode();
        }

        // Apply screen shake offset to camera
        Vector3 camSaved = camera.position;
        camera.position.x += shake.offset.x;
        camera.position.y += shake.offset.y;

        BeginMode3D(camera);
            // Draw tiled floor
            {
                float gridOrigin = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                for (int r = 0; r < TILE_GRID_SIZE; r++) {
                    for (int c = 0; c < TILE_GRID_SIZE; c++) {
                        int vi = tileVariantGrid[r][c];
                        float cellX = gridOrigin + (c + 0.5f) * TILE_WORLD_SIZE + tileJitterX[r][c];
                        float cellZ = gridOrigin + (r + 0.5f) * TILE_WORLD_SIZE + tileJitterZ[r][c];
                        float totalRot = tileRotationGrid[r][c] + tileJitterAngle[r][c];
                        // DrawModelEx applies scale→rotate→translate, so the OBJ-space
                        // center offset gets rotated. Rotate it by the same angle to compensate.
                        float angle = totalRot * DEG2RAD;
                        float cosA = cosf(angle);
                        float sinA = sinf(angle);
                        float sxo = tileCenters[vi].x * tileScale;
                        float szo = tileCenters[vi].z * tileScale;
                        float rxo = sxo * cosA + szo * sinA;
                        float rzo = -sxo * sinA + szo * cosA;

                        // Wobble: tilt tile around its cell center (propagating wave)
                        float wobbleY = 0.0f;
                        float wobbleTiltX = 0.0f, wobbleTiltZ = 0.0f;
                        float wt = tileWobbleTime[r][c];
                        if (tileWobble[r][c] > 0.01f && wt > 0.0f) {
                            float envelope = tileWobble[r][c] * expf(-TILE_WOBBLE_DECAY * wt);
                            float osc = sinf(wt * TILE_WOBBLE_FREQ * 2.0f * PI);
                            wobbleTiltX = envelope * osc * tileWobbleDirX[r][c];
                            wobbleTiltZ = envelope * osc * tileWobbleDirZ[r][c];
                            wobbleY = envelope * fabsf(osc) * (TILE_WOBBLE_BOUNCE / TILE_WOBBLE_MAX);
                            // Kill wobble when envelope is negligible
                            if (envelope < 0.05f) tileWobble[r][c] = 0.0f;
                        }

                        Vector3 pos = {
                            cellX - rxo,
                            wobbleY - tileCenters[vi].y * tileScale - 0.5f,
                            cellZ - rzo,
                        };
                        // Apply tilt via rlgl matrix if wobbling
                        if (wobbleTiltX != 0.0f || wobbleTiltZ != 0.0f) {
                            rlPushMatrix();
                            rlTranslatef(cellX, 0.0f, cellZ);
                            rlRotatef(wobbleTiltX, 1.0f, 0.0f, 0.0f);
                            rlRotatef(wobbleTiltZ, 0.0f, 0.0f, 1.0f);
                            rlTranslatef(-cellX, 0.0f, -cellZ);
                        }
                        DrawModelEx(tileModels[vi], pos,
                            (Vector3){ 0.0f, 1.0f, 0.0f }, totalRot,
                            (Vector3){ tileScale, tileScale, tileScale }, WHITE);
                        if (wobbleTiltX != 0.0f || wobbleTiltZ != 0.0f) {
                            rlPopMatrix();
                        }
                    }
                }
            }

            // Draw units
            for (int i = 0; i < unitCount; i++)
            {
                if (!units[i].active) continue;
                if (IsUnitInStatueSpawn(&statueSpawn, i)) continue; // drawn separately as falling statue
                if (intro.active && intro.unitIndex == i) continue; // hidden during intro splash
                UnitType *type = &unitTypes[units[i].typeIndex];
                if (!type->loaded) continue;
                Color tint = GetTeamTint(units[i].team);
                if (units[i].hitFlash > 0) {
                    float f = units[i].hitFlash / HIT_FLASH_DURATION;
                    if (f > 1.0f) f = 1.0f;
                    tint.r = (unsigned char)(tint.r + (255 - tint.r) * f);
                    tint.g = (unsigned char)(tint.g + (255 - tint.g) * f);
                    tint.b = (unsigned char)(tint.b + (255 - tint.b) * f);
                }
                if (type->hasAnimations) {
                    int idx = type->animIndex[units[i].currentAnim];
                    if (idx >= 0) {
                        ModelAnimation *arr = (units[i].currentAnim == ANIM_IDLE) ? type->idleAnims : type->anims;
                        UpdateModelAnimation(type->model, arr[idx], units[i].animFrame);
                    }
                }
                float s = type->scale * units[i].scaleOverride;
                Vector3 drawPos = units[i].position;
                drawPos.y += type->yOffset;
                DrawModelEx(type->model, drawPos, (Vector3){0,1,0}, units[i].facingAngle,
                    (Vector3){s, s, s}, tint);

                if (units[i].selected)
                {
                    BoundingBox sb = GetUnitBounds(&units[i], type);
                    DrawBoundingBox(sb, GREEN);
                }
            }

            // Draw falling statue (spawning unit rendered separately with stone tint at elevated Y + drift)
            if (statueSpawn.phase == SSPAWN_FALLING) {
                int si = statueSpawn.unitIndex;
                if (si >= 0 && si < unitCount && units[si].active) {
                    UnitType *stype = &unitTypes[units[si].typeIndex];
                    if (stype->loaded) {
                        // Force idle frame 0 pose (frozen statue)
                        if (stype->hasAnimations && stype->animIndex[ANIM_IDLE] >= 0)
                            UpdateModelAnimation(stype->model, stype->idleAnims[stype->animIndex[ANIM_IDLE]], 0);
                        float ss = stype->scale * units[si].scaleOverride;
                        // Compute drift offset based on height fraction
                        float hRange = SPAWN_ANIM_START_Y - statueSpawn.targetY;
                        float dFrac = (hRange > 0.0f) ? (statueSpawn.currentY - statueSpawn.targetY) / hRange : 0.0f;
                        if (dFrac < 0.0f) dFrac = 0.0f;
                        if (dFrac > 1.0f) dFrac = 1.0f;
                        Vector3 statuePos = {
                            units[si].position.x + statueSpawn.driftX * dFrac,
                            statueSpawn.currentY,
                            units[si].position.z + statueSpawn.driftZ * dFrac
                        };
                        Color stoneTint = { 160, 160, 170, 255 }; // grayish stone tint
                        DrawModelEx(stype->model, statuePos, (Vector3){0,1,0}, units[si].facingAngle,
                            (Vector3){ss, ss, ss}, stoneTint);
                    }
                }
            }

            // Draw modifier timer rings (duration-aware arcs, stacked outward per modifier type)
            {
                // Fixed ordering for ring stacking
                const ModifierType ringOrder[] = {
                    MOD_STUN, MOD_SPELL_PROTECT, MOD_CRAGGY_ARMOR, MOD_STONE_GAZE,
                    MOD_INVULNERABLE, MOD_LIFESTEAL, MOD_ARMOR, MOD_DIG_HEAL, MOD_SPEED_MULT,
                    MOD_SHIELD, MOD_MAELSTROM, MOD_VLAD_AURA, MOD_CHARGING,
                };
                const Color ringColors[] = {
                    {255,255,0,255},     // STUN - yellow
                    {200,240,255,255},   // SPELL_PROTECT - cyan
                    {140,140,160,255},   // CRAGGY_ARMOR - gray
                    {160,80,200,255},    // STONE_GAZE - purple
                    {135,206,235,255},   // INVULNERABLE - skyblue
                    {230,40,40,255},     // LIFESTEAL - red
                    {130,130,130,255},   // ARMOR - gray
                    {139,90,43,255},     // DIG_HEAL - brown
                    {0,228,48,255},      // SPEED_MULT - green
                    {80,160,255,255},    // SHIELD - blue
                    {255,230,50,255},    // MAELSTROM - yellow lightning
                    {180,30,30,255},     // VLAD_AURA - dark red
                    {255,140,0,255},     // CHARGING - orange
                };
                const int ringOrderCount = sizeof(ringOrder) / sizeof(ringOrder[0]);

                for (int i = 0; i < unitCount; i++) {
                    if (!units[i].active) continue;
                    Vector3 ringPos = { units[i].position.x, units[i].position.y + 0.3f, units[i].position.z };
                    int ringIdx = 0;
                    for (int r = 0; r < ringOrderCount; r++) {
                        // Find this modifier on unit i
                        Modifier *found = NULL;
                        for (int m = 0; m < MAX_MODIFIERS; m++) {
                            if (modifiers[m].active && modifiers[m].unitIndex == i && modifiers[m].type == ringOrder[r]) {
                                found = &modifiers[m];
                                break;
                            }
                        }
                        if (!found) continue;
                        float radius = 3.5f + ringIdx * 1.5f;
                        float frac = (found->maxDuration > 0.0f) ? found->duration / found->maxDuration : 0.0f;
                        if (frac < 0.0f) frac = 0.0f;
                        if (frac > 1.0f) frac = 1.0f;
                        Color bright = ringColors[r];
                        Color dim = { (unsigned char)(bright.r / 4), (unsigned char)(bright.g / 4), (unsigned char)(bright.b / 4), 100 };
                        // Track ring (full circle in dim)
                        DrawArc3D(ringPos, radius, 1.0f, dim);
                        // Active arc (partial in bright) — draw 3 concentric for thickness
                        DrawArc3D(ringPos, radius - 0.15f, frac, bright);
                        DrawArc3D(ringPos, radius, frac, bright);
                        DrawArc3D(ringPos, radius + 0.15f, frac, bright);
                        ringIdx++;
                    }
                }
            }

            // Draw projectiles
            for (int p = 0; p < MAX_PROJECTILES; p++) {
                if (!projectiles[p].active) continue;
                DrawSphere(projectiles[p].position, 1.5f, projectiles[p].color);
            }

            // Draw particles
            for (int p = 0; p < MAX_PARTICLES; p++) {
                if (!particles[p].active) continue;
                DrawSphere(particles[p].position, particles[p].size, particles[p].color);
            }

            // Draw fissures (gray cubes along the line)
            for (int f = 0; f < MAX_FISSURES; f++) {
                if (!fissures[f].active) continue;
                float rot = fissures[f].rotation * (PI / 180.0f);
                float dirX = sinf(rot), dirZ = cosf(rot);
                int numSegments = (int)(fissures[f].length / 7.0f);
                if (numSegments < 1) numSegments = 1;
                float segLen = fissures[f].length / numSegments;
                float startOffset = -fissures[f].length * 0.5f;
                for (int s = 0; s < numSegments; s++) {
                    float t = startOffset + segLen * (s + 0.5f);
                    Vector3 segPos = {
                        fissures[f].position.x + dirX * t,
                        fissures[f].position.y + 2.5f,
                        fissures[f].position.z + dirZ * t,
                    };
                    DrawCube(segPos, fissures[f].width, 5.0f, segLen * 0.95f, (Color){100, 95, 85, 255});
                    DrawCubeWires(segPos, fissures[f].width, 5.0f, segLen * 0.95f, (Color){70, 65, 55, 255});
                }
            }

            // (modifier rings now drawn above via DrawArc3D)

            // Arena boundary wall (fades in as blue unit is dragged near it)
            if (phase == PHASE_PREP) {
                float closestDragZ = 999.0f;
                for (int i = 0; i < unitCount; i++) {
                    if (units[i].active && units[i].dragging && units[i].team == TEAM_BLUE) {
                        if (units[i].position.z < closestDragZ) closestDragZ = units[i].position.z;
                    }
                }
                if (closestDragZ < 999.0f) {
                    float fadeRange = 40.0f;
                    float dz = closestDragZ - ARENA_BOUNDARY_Z;
                    float proximity = 1.0f - fminf(fmaxf(dz / fadeRange, 0.0f), 1.0f);
                    if (proximity > 0.01f) {
                        float currentTime = (float)GetTime();
                        SetShaderValue(borderShader, borderTimeLoc, &currentTime, SHADER_UNIFORM_FLOAT);
                        SetShaderValue(borderShader, borderProximityLoc, &proximity, SHADER_UNIFORM_FLOAT);
                        rlDisableBackfaceCulling();
                        rlDisableDepthMask();
                        BeginBlendMode(BLEND_ADDITIVE);
                            DrawMesh(borderMesh, borderMaterial, MatrixIdentity());
                        EndBlendMode();
                        rlEnableDepthMask();
                        rlEnableBackfaceCulling();

                    }
                }
            }
        EndMode3D();

        // Restore camera position after shake
        camera.position = camSaved;


        // 2D overlay: labels + health bars
        for (int i = 0; i < unitCount; i++)
        {
            if (!units[i].active) continue;
            if (intro.active && intro.unitIndex == i) continue; // hidden during intro splash
            if (IsUnitInStatueSpawn(&statueSpawn, i) && statueSpawn.phase == SSPAWN_DELAY) continue; // hidden during pre-fall delay
            UnitType *type = &unitTypes[units[i].typeIndex];
            if (!type->loaded) continue;
            const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];

            // Use statue spawn position (with drift) for falling units
            Vector3 labelWorldPos = units[i].position;
            if (IsUnitInStatueSpawn(&statueSpawn, i) && statueSpawn.phase == SSPAWN_FALLING) {
                float hRange = SPAWN_ANIM_START_Y - statueSpawn.targetY;
                float dFrac = (hRange > 0.0f) ? (statueSpawn.currentY - statueSpawn.targetY) / hRange : 0.0f;
                if (dFrac < 0.0f) dFrac = 0.0f;
                if (dFrac > 1.0f) dFrac = 1.0f;
                labelWorldPos.x += statueSpawn.driftX * dFrac;
                labelWorldPos.y = statueSpawn.currentY;
                labelWorldPos.z += statueSpawn.driftZ * dFrac;
            }
            Vector2 sp = GetWorldToScreen(
                (Vector3){ labelWorldPos.x,
                           labelWorldPos.y + (type->baseBounds.max.y * type->scale) + 1.0f,
                           labelWorldPos.z }, camera);

            const char *label = type->name;
            int tw = MeasureText(label, 14);
            DrawText(label, (int)sp.x - tw/2, (int)sp.y - 12, 14,
                     (units[i].team == TEAM_BLUE) ? DARKBLUE : MAROON);

            // Health bar
            float maxHP = stats->health * units[i].hpMultiplier;
            float hpRatio = units[i].currentHealth / maxHP;
            if (hpRatio < 0) hpRatio = 0;
            if (hpRatio > 1) hpRatio = 1;
            int bw = 40, bh = 5;
            int bx = (int)sp.x - bw/2, by = (int)sp.y + 4;
            DrawRectangle(bx, by, bw, bh, DARKGRAY);
            Color hpC = (hpRatio > 0.5f) ? GREEN : (hpRatio > 0.25f) ? ORANGE : RED;
            DrawRectangle(bx, by, (int)(bw * hpRatio), bh, hpC);
            // Shield bar (blue) extending rightward from HP
            if (units[i].shieldHP > 0) {
                float shieldRatio = units[i].shieldHP / maxHP;
                if (shieldRatio > 1) shieldRatio = 1;
                int shieldW = (int)(bw * shieldRatio);
                int shieldX = bx + (int)(bw * hpRatio);
                if (shieldX + shieldW > bx + bw) shieldW = bx + bw - shieldX;
                DrawRectangle(shieldX, by, shieldW, bh, (Color){80, 160, 255, 200});
            }
            DrawRectangleLines(bx, by, bw, bh, BLACK);

            const char *hpT = TextFormat("%.0f/%.0f", units[i].currentHealth, maxHP);
            int htw = MeasureText(hpT, 10);
            DrawText(hpT, (int)sp.x - htw/2, by + bh + 2, 10, DARKGRAY);

            // Modifier labels (deduplicated — only one per type due to AddModifier dedup)
            // Duration-colored text: active portion in modColor, expired portion in dim gray
            int modY = by + bh + 14;
            for (int m = 0; m < MAX_MODIFIERS; m++) {
                if (!modifiers[m].active || modifiers[m].unitIndex != i) continue;
                const char *modLabel = NULL;
                Color modColor = WHITE;
                switch (modifiers[m].type) {
                    case MOD_STUN:          modLabel = "STUNNED";      modColor = YELLOW;                  break;
                    case MOD_INVULNERABLE:  modLabel = "INVULN";       modColor = SKYBLUE;                 break;
                    case MOD_LIFESTEAL:     modLabel = "LIFESTEAL";    modColor = RED;                     break;
                    case MOD_SPEED_MULT:    modLabel = "SPEED";        modColor = GREEN;                   break;
                    case MOD_ARMOR:         modLabel = "ARMOR";        modColor = GRAY;                    break;
                    case MOD_DIG_HEAL:      modLabel = "DIGGING";      modColor = BROWN;                   break;
                    case MOD_SPELL_PROTECT: modLabel = "SPELL SHIELD"; modColor = (Color){200,240,255,255}; break;
                    case MOD_CRAGGY_ARMOR:  modLabel = "CRAGGY";       modColor = (Color){140,140,160,255}; break;
                    case MOD_STONE_GAZE:    modLabel = "STONE GAZE";   modColor = (Color){160,80,200,255};  break;
                    case MOD_SHIELD:        modLabel = "SHIELD";       modColor = (Color){80,160,255,255};  break;
                    case MOD_MAELSTROM:     modLabel = "MAELSTROM";    modColor = (Color){255,230,50,255};  break;
                    case MOD_VLAD_AURA:     modLabel = "VLAD AURA";    modColor = (Color){180,30,30,255};   break;
                    case MOD_CHARGING:      modLabel = "CHARGING";     modColor = (Color){255,140,0,255};   break;
                }
                if (modLabel) {
                    int totalLen = (int)strlen(modLabel);
                    int mlw = MeasureText(modLabel, 9);
                    int startX = (int)sp.x - mlw / 2;
                    float frac = (modifiers[m].maxDuration > 0.0f)
                        ? modifiers[m].duration / modifiers[m].maxDuration : 0.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;
                    int activeChars = (int)(frac * totalLen + 0.5f);
                    Color dimGray = { 100, 100, 120, 255 };
                    int cx = startX;
                    char tmp[2] = { 0, 0 };
                    for (int k = 0; k < totalLen; k++) {
                        tmp[0] = modLabel[k];
                        Color charCol = (k < activeChars) ? modColor : dimGray;
                        DrawText(tmp, cx, modY, 9, charCol);
                        cx += MeasureText(tmp, 9);
                    }
                    modY += 10;
                }
            }
        }

        // 2D overlay: Stone Gaze progress bars
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active || units[i].gazeAccum <= 0) continue;
            // Find the gaze threshold from the active Stone Gaze buff on an enemy
            float gazeThresh = 2.0f; // default
            for (int g = 0; g < unitCount; g++) {
                if (!units[g].active || units[g].team == units[i].team) continue;
                if (!UnitHasModifier(modifiers, g, MOD_STONE_GAZE)) continue;
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                    if (units[g].abilities[a].abilityId == ABILITY_STONE_GAZE) {
                        gazeThresh = ABILITY_DEFS[ABILITY_STONE_GAZE].values[units[g].abilities[a].level][AV_SG_GAZE_THRESH];
                        break;
                    }
                }
                break;
            }
            Vector2 gsp = GetWorldToScreen(units[i].position, camera);
            float progress = units[i].gazeAccum / gazeThresh;
            if (progress > 1.0f) progress = 1.0f;
            int barW = 30, barH = 4;
            int gx = (int)gsp.x - barW/2;
            int gy = (int)gsp.y - 30;
            DrawRectangle(gx, gy, barW, barH, (Color){40,20,60,180});
            DrawRectangle(gx, gy, (int)(barW * progress), barH, (Color){160,80,200,220});
            DrawRectangleLines(gx, gy, barW, barH, (Color){160,80,200,255});
        }

        // 2D overlay: floating texts (spell shouts)
        for (int i = 0; i < MAX_FLOATING_TEXTS; i++) {
            if (!floatingTexts[i].active) continue;
            Vector2 fsp = GetWorldToScreen(floatingTexts[i].position, camera);
            float alpha = floatingTexts[i].life / floatingTexts[i].maxLife;
            int fontSize = 16;
            int ftw = MeasureText(floatingTexts[i].text, fontSize);
            Color ftc = floatingTexts[i].color;
            ftc.a = (unsigned char)(255.0f * alpha);
            DrawText(floatingTexts[i].text, (int)fsp.x - ftw/2, (int)fsp.y, fontSize, ftc);
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

            // Spawn buttons (debug mode only — F1 to toggle)
            if (debugMode) {
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

                DrawText("[F1] DEBUG MODE", dBtnXBlue, dBtnYStart - 20, 12, YELLOW);
                DrawText(TextFormat("[</>] Tiles: %s", tileLayoutNames[tileLayout]), dBtnXBlue, dBtnYStart - 36, 12, YELLOW);
            }

            // Round info label
            {
                const char *waveLabel = TextFormat("Wave %d", currentRound + 1);
                int wlw = MeasureText(waveLabel, 16);
                DrawText(waveLabel, sw/2 - wlw/2, dBtnYStart - 25, 16, WHITE);
            }

            // NFC emulation input box (debug only)
            if (debugMode) {
                int nfcBoxW = 200, nfcBoxH = 28;
                int nfcBoxX = sw/2 - nfcBoxW/2;
                int nfcBoxY = dBtnYStart - 55;
                int labelW = MeasureText("NFC Code:", 14);

                // Label
                DrawText("NFC Code:", nfcBoxX - labelW - 8, nfcBoxY + 6, 14, (Color){180,180,200,255});

                // Input field background
                Color boxBg = nfcInputActive ? (Color){50,50,70,255} : (Color){30,30,45,255};
                Color boxBorder = nfcInputActive ? (Color){100,140,255,255} : (Color){70,70,90,255};
                DrawRectangle(nfcBoxX, nfcBoxY, nfcBoxW, nfcBoxH, boxBg);
                DrawRectangleLinesEx((Rectangle){(float)nfcBoxX,(float)nfcBoxY,(float)nfcBoxW,(float)nfcBoxH}, 1, boxBorder);

                // Text content or placeholder
                if (nfcInputLen > 0) {
                    DrawText(nfcInputBuf, nfcBoxX + 6, nfcBoxY + 6, 14, WHITE);
                    // Blinking cursor when active
                    if (nfcInputActive && ((int)(GetTime() * 2.0) % 2 == 0)) {
                        int tw = MeasureText(nfcInputBuf, 14);
                        DrawText("|", nfcBoxX + 6 + tw, nfcBoxY + 5, 14, (Color){200,200,255,255});
                    }
                } else {
                    if (nfcInputActive) {
                        // Blinking cursor
                        if ((int)(GetTime() * 2.0) % 2 == 0)
                            DrawText("|", nfcBoxX + 6, nfcBoxY + 5, 14, (Color){200,200,255,255});
                    } else {
                        DrawText("e.g. 1MM1DG2XXCF3", nfcBoxX + 6, nfcBoxY + 6, 12, (Color){100,100,120,255});
                    }
                }

                // Error message below
                if (nfcInputErrorTimer > 0.0f) {
                    float alpha = nfcInputErrorTimer > 1.0f ? 1.0f : nfcInputErrorTimer;
                    Color errColor = { 255, 80, 80, (unsigned char)(255 * alpha) };
                    DrawText(nfcInputError, nfcBoxX, nfcBoxY + nfcBoxH + 4, 12, errColor);
                }
            }

            // Danger zone indicator (pushing past a milestone)
            if (lastMilestoneRound > 0) {
                const char *dangerText = "DANGER ZONE - Losing means permanent death!";
                int dtw = MeasureText(dangerText, 18);
                DrawText(dangerText, sw/2 - dtw/2, 60, 18, RED);
                int nextMilestone = ((currentRound / 5) + 1) * 5;
                const char *nextText = TextFormat("Next milestone: Wave %d", nextMilestone);
                int ntw = MeasureText(nextText, 14);
                DrawText(nextText, sw/2 - ntw/2, 82, 14, ORANGE);
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

                const char *scoreText = TextFormat("Score: %d - %d", blueWins, redWins);
                int stw = MeasureText(scoreText, 18);
                DrawText(scoreText, sw/2 - stw/2, sh/2 - 10, 18, WHITE);
            }
            else if (phase == PHASE_GAME_OVER)
            {
                if (deathPenalty) {
                    const char *deathMsg = TextFormat("YOUR UNITS HAVE FALLEN - Wave %d", currentRound);
                    int dw = MeasureText(deathMsg, 30);
                    DrawText(deathMsg, sw/2 - dw/2, sh/2 - 50, 30, RED);

                    const char *deathSub = "Defeated! Your units are lost forever!";
                    int dsw2 = MeasureText(deathSub, 18);
                    DrawText(deathSub, sw/2 - dsw2/2, sh/2 - 10, 18, (Color){255,100,100,255});

                    const char *restartMsg = "Press R to return to menu";
                    int rw2 = MeasureText(restartMsg, 20);
                    DrawText(restartMsg, sw/2 - rw2/2, sh/2 + 30, 20, GRAY);
                }
                // Non-death game over is drawn as a full overlay below
            }
        }

        // F1 debug hint (always visible, top-right)
        {
            const char *dbgHint = "[F1] Debug";
            int dbgW = MeasureText(dbgHint, 14);
            Color dbgCol = debugMode ? YELLOW : (Color){180,180,180,120};
            DrawText(dbgHint, GetScreenWidth() - dbgW - 10, 10, 14, dbgCol);
        }

        // Camera debug sliders (debug mode only)
        if (debugMode) {
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
        }

        // ── UNIT HUD BAR + SHOP ── (visible during prep, combat, round_over only)
        if (phase != PHASE_GAME_OVER && phase != PHASE_MENU && phase != PHASE_MILESTONE)
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

                    // X button (remove unit) — prep phase only
                    if (phase == PHASE_PREP) {
                        int xBtnSize = 16;
                        int xBtnX = cardX + HUD_CARD_WIDTH - xBtnSize - 2;
                        int xBtnY = cardsY + 2;
                        Color xBg = (Color){ 180, 50, 50, 200 };
                        if (CheckCollisionPointRec(GetMousePosition(),
                            (Rectangle){ (float)xBtnX, (float)xBtnY, (float)xBtnSize, (float)xBtnSize }))
                            xBg = (Color){ 230, 70, 70, 255 };
                        DrawRectangle(xBtnX, xBtnY, xBtnSize, xBtnSize, xBg);
                        DrawRectangleLines(xBtnX, xBtnY, xBtnSize, xBtnSize, (Color){100,30,30,255});
                        int xw = MeasureText("X", 12);
                        DrawText("X", xBtnX + (xBtnSize - xw) / 2, xBtnY + 2, 12, WHITE);
                    }

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
                    float cardMaxHP = stats->health * units[ui].hpMultiplier;
                    float hpRatio = units[ui].currentHealth / cardMaxHP;
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

                        AbilitySlot *aslot = &units[ui].abilities[a];
                        if (aslot->abilityId >= 0 && aslot->abilityId < ABILITY_COUNT) {
                            // Filled slot — colored background
                            DrawRectangle(ax, ay, HUD_ABILITY_SLOT_SIZE, HUD_ABILITY_SLOT_SIZE,
                                         ABILITY_DEFS[aslot->abilityId].color);
                            // Hover detection
                            bool slotHovered = CheckCollisionPointRec(GetMousePosition(),
                                (Rectangle){(float)ax,(float)ay,(float)HUD_ABILITY_SLOT_SIZE,(float)HUD_ABILITY_SLOT_SIZE});
                            if (slotHovered) { hoverAbilityId = aslot->abilityId; hoverAbilityLevel = aslot->level; }
                            // Abbreviation (scale up when charging tooltip)
                            int abbrSize = 11;
                            if (slotHovered && hoverTimer > 0 && hoverTimer < tooltipDelay)
                                abbrSize = 11 + (int)(3.0f * (hoverTimer / tooltipDelay));
                            const char *abbr = ABILITY_DEFS[aslot->abilityId].abbrev;
                            int aw2 = MeasureText(abbr, abbrSize);
                            DrawText(abbr, ax + (HUD_ABILITY_SLOT_SIZE - aw2) / 2,
                                    ay + (HUD_ABILITY_SLOT_SIZE - abbrSize) / 2, abbrSize, WHITE);
                            // Level indicator (bottom-left)
                            const char *lvl = TextFormat("L%d", aslot->level + 1);
                            DrawText(lvl, ax + 2, ay + HUD_ABILITY_SLOT_SIZE - 9, 8, (Color){220,220,220,200});
                            // Cooldown overlay (combat only)
                            if (aslot->cooldownRemaining > 0 && phase == PHASE_COMBAT) {
                                const AbilityDef *adef = &ABILITY_DEFS[aslot->abilityId];
                                float cdFrac = aslot->cooldownRemaining / adef->cooldown[aslot->level];
                                if (cdFrac > 1) cdFrac = 1;
                                int overlayH = (int)(HUD_ABILITY_SLOT_SIZE * cdFrac);
                                DrawRectangle(ax, ay, HUD_ABILITY_SLOT_SIZE, overlayH, (Color){0,0,0,150});
                                const char *cdTxt = TextFormat("%.0f", aslot->cooldownRemaining);
                                int cdw = MeasureText(cdTxt, 12);
                                DrawText(cdTxt, ax + (HUD_ABILITY_SLOT_SIZE - cdw)/2,
                                        ay + (HUD_ABILITY_SLOT_SIZE - 12)/2, 12, WHITE);
                            }
                        } else {
                            // Empty slot
                            DrawRectangle(ax, ay, HUD_ABILITY_SLOT_SIZE, HUD_ABILITY_SLOT_SIZE,
                                         (Color){ 40, 40, 55, 255 });
                            const char *q = "?";
                            int qw = MeasureText(q, 16);
                            DrawText(q, ax + (HUD_ABILITY_SLOT_SIZE - qw) / 2,
                                    ay + (HUD_ABILITY_SLOT_SIZE - 16) / 2, 16, (Color){ 80, 80, 100, 255 });
                        }
                        DrawRectangleLines(ax, ay, HUD_ABILITY_SLOT_SIZE, HUD_ABILITY_SLOT_SIZE,
                                          (Color){ 90, 90, 110, 255 });
                        // Activation order number (top-right corner)
                        // Find which activation position this slot is
                        int orderNum = 0;
                        for (int o = 0; o < MAX_ABILITIES_PER_UNIT; o++)
                            if (ACTIVATION_ORDER[o] == a) { orderNum = o + 1; break; }
                        Color orderCol = (Color){100,100,120,255};
                        if (phase == PHASE_COMBAT && ACTIVATION_ORDER[units[ui].nextAbilitySlot] == a)
                            orderCol = YELLOW;
                        DrawText(TextFormat("%d", orderNum), ax + HUD_ABILITY_SLOT_SIZE - 8, ay + 1, 8, orderCol);
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

            // --- Inventory (left of unit cards) ---
            {
                int invStartX = cardsStartX - (HUD_INVENTORY_COLS * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP)) - 20;
                int invStartY = cardsY + 15;
                DrawText("INV", invStartX, invStartY - 14, 10, (Color){160,160,180,255});
                for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
                    int icol = inv % HUD_INVENTORY_COLS;
                    int irow = inv / HUD_INVENTORY_COLS;
                    int ix = invStartX + icol * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                    int iy = invStartY + irow * (HUD_ABILITY_SLOT_SIZE + HUD_ABILITY_SLOT_GAP);
                    DrawRectangle(ix, iy, HUD_ABILITY_SLOT_SIZE, HUD_ABILITY_SLOT_SIZE, (Color){40,40,55,255});
                    DrawRectangleLines(ix, iy, HUD_ABILITY_SLOT_SIZE, HUD_ABILITY_SLOT_SIZE, (Color){90,90,110,255});
                    if (inventory[inv].abilityId >= 0 && inventory[inv].abilityId < ABILITY_COUNT) {
                        DrawRectangle(ix+1, iy+1, HUD_ABILITY_SLOT_SIZE-2, HUD_ABILITY_SLOT_SIZE-2,
                                      ABILITY_DEFS[inventory[inv].abilityId].color);
                        // Hover detection
                        bool invHovered = CheckCollisionPointRec(GetMousePosition(),
                            (Rectangle){(float)ix,(float)iy,(float)HUD_ABILITY_SLOT_SIZE,(float)HUD_ABILITY_SLOT_SIZE});
                        if (invHovered) { hoverAbilityId = inventory[inv].abilityId; hoverAbilityLevel = inventory[inv].level; }
                        int invAbbrSize = 11;
                        if (invHovered && hoverTimer > 0 && hoverTimer < tooltipDelay)
                            invAbbrSize = 11 + (int)(3.0f * (hoverTimer / tooltipDelay));
                        const char *iabbr = ABILITY_DEFS[inventory[inv].abilityId].abbrev;
                        int iaw = MeasureText(iabbr, invAbbrSize);
                        DrawText(iabbr, ix + (HUD_ABILITY_SLOT_SIZE-iaw)/2,
                                 iy + (HUD_ABILITY_SLOT_SIZE-invAbbrSize)/2, invAbbrSize, WHITE);
                        const char *ilvl = TextFormat("L%d", inventory[inv].level + 1);
                        DrawText(ilvl, ix + 2, iy + HUD_ABILITY_SLOT_SIZE - 9, 8, (Color){220,220,220,200});
                    }
                }
            }

            // --- Drag ghost ---
            if (dragState.dragging && dragState.abilityId >= 0 && dragState.abilityId < ABILITY_COUNT) {
                Vector2 dmouse = GetMousePosition();
                DrawRectangle((int)dmouse.x - 16, (int)dmouse.y - 16, 32, 32,
                              ABILITY_DEFS[dragState.abilityId].color);
                DrawRectangleLines((int)dmouse.x - 16, (int)dmouse.y - 16, 32, 32, WHITE);
                const char *dabbr = ABILITY_DEFS[dragState.abilityId].abbrev;
                int daw = MeasureText(dabbr, 11);
                DrawText(dabbr, (int)dmouse.x - daw/2, (int)dmouse.y - 5, 11, WHITE);
            }

            // --- Shop panel (only during PREP, above unit bar) ---
            if (phase == PHASE_PREP)
            {
                int shopY = hudTop + 2;
                int shopH = HUD_SHOP_HEIGHT - 2;
                DrawRectangle(0, shopY, hudSw, shopH, (Color){ 20, 20, 28, 240 });
                DrawRectangle(0, shopY + shopH - 1, hudSw, 1, (Color){ 60, 60, 80, 255 });

                // ROLL button (left) — show cost
                Rectangle rollBtn = { 20, (float)(shopY + 10), 80, 30 };
                bool canRoll = (playerGold >= rollCost);
                Color rollColor = canRoll ? (Color){ 180, 140, 40, 255 } : (Color){ 80, 70, 40, 255 };
                if (canRoll && CheckCollisionPointRec(GetMousePosition(), rollBtn))
                    rollColor = (Color){ 220, 180, 60, 255 };
                DrawRectangleRec(rollBtn, rollColor);
                DrawRectangleLinesEx(rollBtn, 2, (Color){ 120, 90, 20, 255 });
                const char *rollText = TextFormat("ROLL %dg", rollCost);
                int rollW = MeasureText(rollText, 14);
                DrawText(rollText, (int)(rollBtn.x + (80 - rollW) / 2),
                        (int)(rollBtn.y + (30 - 14) / 2), 14, WHITE);

                // Shop ability cards (3 slots, centered)
                int shopCardW = 120;
                int shopCardH = 34;
                int shopCardGap = 10;
                int totalShopW = MAX_SHOP_SLOTS * shopCardW + (MAX_SHOP_SLOTS - 1) * shopCardGap;
                int shopCardsX = (hudSw - totalShopW) / 2;
                for (int s = 0; s < MAX_SHOP_SLOTS; s++)
                {
                    int scx = shopCardsX + s * (shopCardW + shopCardGap);
                    int scy = shopY + 8;
                    if (shopSlots[s].abilityId >= 0 && shopSlots[s].abilityId < ABILITY_COUNT) {
                        const AbilityDef *sdef = &ABILITY_DEFS[shopSlots[s].abilityId];
                        bool canAfford = (playerGold >= sdef->goldCost);
                        Color cardBg = canAfford ? ABILITY_DEFS[shopSlots[s].abilityId].color : (Color){50,50,65,255};
                        bool shopHovered = CheckCollisionPointRec(GetMousePosition(),
                            (Rectangle){(float)scx,(float)scy,(float)shopCardW,(float)shopCardH});
                        if (shopHovered) { hoverAbilityId = shopSlots[s].abilityId; hoverAbilityLevel = 0; }
                        if (canAfford && shopHovered)
                            cardBg = (Color){ cardBg.r + 30, cardBg.g + 30, cardBg.b + 30, 255 };
                        DrawRectangle(scx, scy, shopCardW, shopCardH, cardBg);
                        DrawRectangleLines(scx, scy, shopCardW, shopCardH, (Color){90,90,110,255});
                        const char *sname = TextFormat("%s %dg", sdef->name, sdef->goldCost);
                        int snw = MeasureText(sname, 12);
                        DrawText(sname, scx + (shopCardW - snw)/2, scy + (shopCardH - 12)/2, 12,
                                canAfford ? WHITE : (Color){100,100,120,255});
                    } else {
                        DrawRectangle(scx, scy, shopCardW, shopCardH, (Color){35,35,45,255});
                        DrawRectangleLines(scx, scy, shopCardW, shopCardH, (Color){60,60,80,255});
                        DrawText("SOLD", scx + (shopCardW - MeasureText("SOLD",12))/2,
                                scy + (shopCardH - 12)/2, 12, (Color){60,60,80,255});
                    }
                }

                // Gold display (right side)
                const char *goldText = TextFormat("Gold: %d", playerGold);
                int gw = MeasureText(goldText, 18);
                DrawText(goldText, hudSw - gw - 20, shopY + 16, 18, (Color){ 240, 200, 60, 255 });
            }
        }

        // --- Confirm removal popup (drawn on top of everything) ---
        if (removeConfirmUnit >= 0 && phase == PHASE_PREP) {
            int sw2 = GetScreenWidth(), sh2 = GetScreenHeight();
            DrawRectangle(0, 0, sw2, sh2, (Color){ 0, 0, 0, 120 }); // dim overlay
            int popW = 220, popH = 80;
            int popX = sw2 / 2 - popW / 2;
            int popY = sh2 / 2 - popH / 2;
            DrawRectangle(popX, popY, popW, popH, (Color){ 40, 40, 55, 240 });
            DrawRectangleLinesEx((Rectangle){ (float)popX, (float)popY, (float)popW, (float)popH },
                                2, (Color){ 180, 60, 60, 255 });
            const char *confirmText = "Remove this unit?";
            int ctw = MeasureText(confirmText, 16);
            DrawText(confirmText, popX + (popW - ctw) / 2, popY + 12, 16, WHITE);
            // Abilities returned note
            DrawText("(abilities return to inventory)", popX + 14, popY + 32, 9, (Color){160,160,180,255});
            // Yes / No buttons
            Rectangle yesBtn = { (float)(popX + 20), (float)(popY + popH - 32), 80, 24 };
            Rectangle noBtn  = { (float)(popX + popW - 100), (float)(popY + popH - 32), 80, 24 };
            Color yesBg = (Color){ 180, 50, 50, 255 };
            Color noBg  = (Color){ 60, 60, 80, 255 };
            if (CheckCollisionPointRec(GetMousePosition(), yesBtn)) yesBg = (Color){ 230, 70, 70, 255 };
            if (CheckCollisionPointRec(GetMousePosition(), noBtn))  noBg  = (Color){ 80, 80, 110, 255 };
            DrawRectangleRec(yesBtn, yesBg);
            DrawRectangleRec(noBtn, noBg);
            DrawRectangleLinesEx(yesBtn, 1, (Color){120,40,40,255});
            DrawRectangleLinesEx(noBtn, 1, (Color){80,80,100,255});
            int yw = MeasureText("YES", 14), nw = MeasureText("NO", 14);
            DrawText("YES", (int)(yesBtn.x + (80 - yw) / 2), (int)(yesBtn.y + 5), 14, WHITE);
            DrawText("NO",  (int)(noBtn.x + (80 - nw) / 2), (int)(noBtn.y + 5), 14, WHITE);
        }

        // --- Hover tooltip timer + drawing ---
        if (hoverAbilityId >= 0 && hoverAbilityId == prevHoverAbilityId)
            hoverTimer += dt;
        else if (hoverAbilityId >= 0)
            hoverTimer = dt;
        else
            hoverTimer = 0.0f;

        if (hoverAbilityId >= 0 && hoverTimer >= tooltipDelay) {
            const AbilityDef *tipDef = &ABILITY_DEFS[hoverAbilityId];
            Vector2 mpos = GetMousePosition();

            // Build stat lines for this ability
            typedef struct { const char *label; int valueIndex; bool isPercent; } StatLine;
            StatLine statLines[8];
            int numStatLines = 0;

            switch (hoverAbilityId) {
            case ABILITY_MAGIC_MISSILE:
                statLines[numStatLines++] = (StatLine){ "Damage", AV_MM_DAMAGE, true };
                statLines[numStatLines++] = (StatLine){ "Stun", AV_MM_STUN_DUR, false };
                break;
            case ABILITY_DIG:
                statLines[numStatLines++] = (StatLine){ "HP Thresh", AV_DIG_HP_THRESH, true };
                statLines[numStatLines++] = (StatLine){ "Heal Dur", AV_DIG_HEAL_DUR, false };
                break;
            case ABILITY_VACUUM:
                statLines[numStatLines++] = (StatLine){ "Radius", AV_VAC_RADIUS, false };
                statLines[numStatLines++] = (StatLine){ "Stun", AV_VAC_STUN_DUR, false };
                break;
            case ABILITY_CHAIN_FROST:
                statLines[numStatLines++] = (StatLine){ "Damage", AV_CF_DAMAGE, false };
                statLines[numStatLines++] = (StatLine){ "Bounces", AV_CF_BOUNCES, false };
                break;
            case ABILITY_BLOOD_RAGE:
                statLines[numStatLines++] = (StatLine){ "Lifesteal", AV_BR_LIFESTEAL, true };
                statLines[numStatLines++] = (StatLine){ "Duration", AV_BR_DURATION, false };
                break;
            case ABILITY_EARTHQUAKE:
                statLines[numStatLines++] = (StatLine){ "Damage", AV_EQ_DAMAGE, false };
                statLines[numStatLines++] = (StatLine){ "Radius", AV_EQ_RADIUS, false };
                break;
            case ABILITY_SPELL_PROTECT:
                statLines[numStatLines++] = (StatLine){ "Duration", AV_SP_DURATION, false };
                break;
            case ABILITY_CRAGGY_ARMOR:
                statLines[numStatLines++] = (StatLine){ "Armor", AV_CA_ARMOR, false };
                statLines[numStatLines++] = (StatLine){ "Stun %", AV_CA_STUN_CHANCE, true };
                statLines[numStatLines++] = (StatLine){ "Duration", AV_CA_DURATION, false };
                break;
            case ABILITY_STONE_GAZE:
                statLines[numStatLines++] = (StatLine){ "Gaze Time", AV_SG_GAZE_THRESH, false };
                statLines[numStatLines++] = (StatLine){ "Stun", AV_SG_STUN_DUR, false };
                statLines[numStatLines++] = (StatLine){ "Duration", AV_SG_DURATION, false };
                break;
            case ABILITY_SUNDER:
                statLines[numStatLines++] = (StatLine){ "HP Thresh", AV_SU_HP_THRESH, true };
                break;
            case ABILITY_FISSURE:
                statLines[numStatLines++] = (StatLine){ "Damage", AV_FI_DAMAGE, false };
                statLines[numStatLines++] = (StatLine){ "Length", AV_FI_LENGTH, false };
                statLines[numStatLines++] = (StatLine){ "Duration", AV_FI_DURATION, false };
                break;
            case ABILITY_VLAD_AURA:
                statLines[numStatLines++] = (StatLine){ "Lifesteal", AV_VA_LIFESTEAL, true };
                statLines[numStatLines++] = (StatLine){ "Duration", AV_VA_DURATION, false };
                break;
            case ABILITY_MAELSTROM:
                statLines[numStatLines++] = (StatLine){ "Proc %", AV_ML_PROC_CHANCE, true };
                statLines[numStatLines++] = (StatLine){ "Damage", AV_ML_DAMAGE, false };
                statLines[numStatLines++] = (StatLine){ "Duration", AV_ML_DURATION, false };
                break;
            case ABILITY_SWAP:
                statLines[numStatLines++] = (StatLine){ "Shield HP", AV_SW_SHIELD, false };
                statLines[numStatLines++] = (StatLine){ "Shield Dur", AV_SW_SHIELD_DUR, false };
                break;
            case ABILITY_APHOTIC_SHIELD:
                statLines[numStatLines++] = (StatLine){ "Shield HP", AV_AS_SHIELD, false };
                statLines[numStatLines++] = (StatLine){ "Duration", AV_AS_DURATION, false };
                break;
            case ABILITY_HOOK:
                statLines[numStatLines++] = (StatLine){ "Dmg/Dist", AV_HK_DMG_PER_DIST, false };
                statLines[numStatLines++] = (StatLine){ "Range", AV_HK_RANGE, false };
                break;
            case ABILITY_PRIMAL_CHARGE:
                statLines[numStatLines++] = (StatLine){ "Damage", AV_PC_DAMAGE, false };
                statLines[numStatLines++] = (StatLine){ "Knockback", AV_PC_KNOCKBACK, false };
                break;
            default: break;
            }
            // Always add cooldown as last line
            int cdLineIdx = numStatLines; // special: cooldown uses cooldown[] not values[]
            numStatLines++; // reserve a line for cooldown

            int tipW = 200;
            int tipH = 44 + numStatLines * 14;
            int tipX = (int)mpos.x + 14;
            int tipY = (int)mpos.y - tipH - 4;
            if (tipX + tipW > GetScreenWidth()) tipX = (int)mpos.x - tipW - 4;
            if (tipY < 0) tipY = (int)mpos.y + 20;
            DrawRectangle(tipX, tipY, tipW, tipH, (Color){20, 20, 30, 230});
            DrawRectangleLines(tipX, tipY, tipW, tipH, (Color){100, 100, 130, 255});
            DrawText(tipDef->name, tipX + 6, tipY + 4, 14, WHITE);
            DrawText(tipDef->description, tipX + 6, tipY + 22, 10, (Color){180, 180, 200, 255});

            Color dimStatColor = { 100, 100, 120, 255 };
            int lineY = tipY + 38;
            for (int sl = 0; sl < numStatLines; sl++) {
                int lx = tipX + 6;
                if (sl == cdLineIdx) {
                    // Cooldown line
                    const char *cdLabel = "CD: ";
                    DrawText(cdLabel, lx, lineY, 10, (Color){180,180,200,255});
                    lx += MeasureText(cdLabel, 10);
                    for (int lv = 0; lv < ABILITY_MAX_LEVELS; lv++) {
                        const char *val = TextFormat("%.1fs", tipDef->cooldown[lv]);
                        Color vc = (lv == hoverAbilityLevel) ? WHITE : dimStatColor;
                        DrawText(val, lx, lineY, 10, vc);
                        lx += MeasureText(val, 10);
                        if (lv < ABILITY_MAX_LEVELS - 1) {
                            DrawText(" / ", lx, lineY, 10, dimStatColor);
                            lx += MeasureText(" / ", 10);
                        }
                    }
                } else {
                    // Stat value line
                    char labelBuf[32];
                    snprintf(labelBuf, sizeof(labelBuf), "%s: ", statLines[sl].label);
                    DrawText(labelBuf, lx, lineY, 10, (Color){180,180,200,255});
                    lx += MeasureText(labelBuf, 10);
                    for (int lv = 0; lv < ABILITY_MAX_LEVELS; lv++) {
                        float v = tipDef->values[lv][statLines[sl].valueIndex];
                        const char *val;
                        if (statLines[sl].isPercent)
                            val = TextFormat("%.0f%%", v * 100.0f);
                        else if (v == (int)v)
                            val = TextFormat("%.0f", v);
                        else
                            val = TextFormat("%.1f", v);
                        Color vc = (lv == hoverAbilityLevel) ? WHITE : dimStatColor;
                        DrawText(val, lx, lineY, 10, vc);
                        lx += MeasureText(val, 10);
                        if (lv < ABILITY_MAX_LEVELS - 1) {
                            DrawText(" / ", lx, lineY, 10, dimStatColor);
                            lx += MeasureText(" / ", 10);
                        }
                    }
                }
                lineY += 14;
            }
        }

        //==============================================================================
        // PHASE_MENU DRAWING
        //==============================================================================
        if (phase == PHASE_MENU)
        {
            int msw = GetScreenWidth();
            int msh = GetScreenHeight();

            // Dark background
            DrawRectangle(0, 0, msw, msh, (Color){ 20, 20, 30, 255 });

            // Title
            const char *title = "AUTOCHESS";
            int titleSize = 48;
            int tw = MeasureText(title, titleSize);
            DrawText(title, msw/2 - tw/2, msh/4 - 40, titleSize, (Color){200, 180, 255, 255});

            const char *subtitle = "Set in Stone";
            int subSize = 24;
            int sw2 = MeasureText(subtitle, subSize);
            DrawText(subtitle, msw/2 - sw2/2, msh/4 + 20, subSize, (Color){160,140,200,200});

            // Name input field
            {
                const char *nameLabel = "Player Name:";
                int nlw = MeasureText(nameLabel, 16);
                DrawText(nameLabel, msw/2 - nlw/2, msh/2 - 65, 16, (Color){180,180,200,255});

                Rectangle nameField = { (float)(msw/2 - 120), (float)(msh/2 - 40), 240, 36 };
                Color nameBg = nameInputActive ? (Color){50,50,70,255} : (Color){35,35,50,255};
                DrawRectangleRec(nameField, nameBg);
                Color nameBorder = nameInputActive ? (Color){150,140,200,255} : (Color){80,80,100,255};
                DrawRectangleLinesEx(nameField, 2, nameBorder);

                // Draw player name with cursor
                int nameTextW = MeasureText(playerName, 18);
                DrawText(playerName, msw/2 - nameTextW/2, msh/2 - 31, 18, WHITE);
                if (nameInputActive) {
                    // Blinking cursor
                    float blinkTime = (float)GetTime();
                    if ((int)(blinkTime * 2) % 2 == 0) {
                        int cursorX = msw/2 - nameTextW/2 + nameTextW;
                        DrawRectangle(cursorX + 2, msh/2 - 31, 2, 18, WHITE);
                    }
                }
            }

            // PLAY button
            {
                Rectangle playMenuBtn = { (float)(msw/2 - 80), (float)(msh/2 + 20), 160, 50 };
                Color playBg = (Color){50,180,80,255};
                if (CheckCollisionPointRec(GetMousePosition(), playMenuBtn))
                    playBg = (Color){30,220,60,255};
                DrawRectangleRec(playMenuBtn, playBg);
                DrawRectangleLinesEx(playMenuBtn, 2, DARKGREEN);
                const char *playText = "PLAY";
                int ptw2 = MeasureText(playText, 24);
                DrawText(playText, (int)(playMenuBtn.x + 80 - ptw2/2), (int)(playMenuBtn.y + 13), 24, WHITE);
            }

            // LEADERBOARD button
            {
                Rectangle lbBtn = { (float)(msw/2 - 80), (float)(msh/2 + 80), 160, 40 };
                Color lbBg = (Color){60,60,80,255};
                if (CheckCollisionPointRec(GetMousePosition(), lbBtn))
                    lbBg = (Color){80,80,110,255};
                DrawRectangleRec(lbBtn, lbBg);
                DrawRectangleLinesEx(lbBtn, 2, (Color){100,100,130,255});
                const char *lbText = "LEADERBOARD";
                int lbw = MeasureText(lbText, 16);
                DrawText(lbText, (int)(lbBtn.x + 80 - lbw/2), (int)(lbBtn.y + 12), 16, WHITE);
            }

            // --- Multiplayer buttons ---
            // CREATE LOBBY button
            {
                Rectangle createBtn = { (float)(msw/2 - 80), (float)(msh/2 + 130), 160, 40 };
                Color cBg = (Color){40,130,60,255};
                if (CheckCollisionPointRec(GetMousePosition(), createBtn))
                    cBg = (Color){50,170,70,255};
                DrawRectangleRec(createBtn, cBg);
                DrawRectangleLinesEx(createBtn, 2, (Color){30,100,40,255});
                const char *cText = "CREATE LOBBY";
                int cw = MeasureText(cText, 14);
                DrawText(cText, (int)(createBtn.x + 80 - cw/2), (int)(createBtn.y + 13), 14, WHITE);
            }

            // JOIN LOBBY button
            {
                bool codeReady = (joinCodeLen == LOBBY_CODE_LEN);
                Rectangle joinBtn = { (float)(msw/2 - 80), (float)(msh/2 + 180), 160, 40 };
                Color jBg = codeReady ? (Color){160,100,30,255} : (Color){80,80,80,255};
                if (codeReady && CheckCollisionPointRec(GetMousePosition(), joinBtn))
                    jBg = (Color){200,130,40,255};
                DrawRectangleRec(joinBtn, jBg);
                DrawRectangleLinesEx(joinBtn, 2, (Color){100,70,20,255});
                const char *jText = "JOIN LOBBY";
                int jw = MeasureText(jText, 14);
                DrawText(jText, (int)(joinBtn.x + 80 - jw/2), (int)(joinBtn.y + 13), 14, WHITE);
            }

            // Join code input field
            {
                DrawText("Lobby Code:", msw/2 - MeasureText("Lobby Code:", 12)/2, msh/2 + 225, 12, (Color){150,150,170,255});
                Rectangle codeBox = { (float)(msw/2 - 60), (float)(msh/2 + 240), 120, 30 };
                DrawRectangleRec(codeBox, (Color){35,35,50,255});
                DrawRectangleLinesEx(codeBox, 2, (Color){80,80,100,255});
                char codeBuf[8];
                snprintf(codeBuf, sizeof(codeBuf), "%s_", joinCodeInput);
                int ccw = MeasureText(codeBuf, 18);
                DrawText(codeBuf, msw/2 - ccw/2, msh/2 + 246, 18, WHITE);
            }

            // Menu error message
            if (menuError[0]) {
                int ew = MeasureText(menuError, 14);
                DrawText(menuError, msw/2 - ew/2, msh/2 + 280, 14, RED);
            }

            // Leaderboard overlay
            if (showLeaderboard)
            {
                // Dim overlay
                DrawRectangle(0, 0, msw, msh, (Color){0,0,0,180});

                int panelW = 600, panelH = 500;
                int panelX = msw/2 - panelW/2;
                int panelY = msh/2 - panelH/2;
                DrawRectangle(panelX, panelY, panelW, panelH, (Color){24,24,32,240});
                DrawRectangleLinesEx((Rectangle){(float)panelX,(float)panelY,(float)panelW,(float)panelH}, 2, (Color){100,100,130,255});

                // Title
                const char *lbTitle = "LEADERBOARD";
                int ltw = MeasureText(lbTitle, 24);
                DrawText(lbTitle, panelX + panelW/2 - ltw/2, panelY + 10, 24, GOLD);

                // Close button
                Rectangle closeBtn = { (float)(panelX + panelW - 40), (float)panelY, 40, 40 };
                Color closeBg = (Color){180,50,50,200};
                if (CheckCollisionPointRec(GetMousePosition(), closeBtn))
                    closeBg = (Color){230,70,70,255};
                DrawRectangleRec(closeBtn, closeBg);
                int xw = MeasureText("X", 18);
                DrawText("X", (int)(closeBtn.x + 20 - xw/2), (int)(closeBtn.y + 11), 18, WHITE);

                // Entries
                int listTop = panelY + 50;
                int listH = panelH - 60;
                int rowH = 70;
                BeginScissorMode(panelX + 4, listTop, panelW - 8, listH);
                for (int e = 0; e < leaderboard.entryCount; e++) {
                    int rowY = listTop + e * rowH - leaderboardScroll;
                    if (rowY + rowH < listTop || rowY > listTop + listH) continue;

                    LeaderboardEntry *le = &leaderboard.entries[e];
                    Color rowBg = (e % 2 == 0) ? (Color){30,30,42,255} : (Color){36,36,48,255};
                    DrawRectangle(panelX + 4, rowY, panelW - 8, rowH - 2, rowBg);

                    // Rank
                    const char *rankText = TextFormat("#%d", e + 1);
                    DrawText(rankText, panelX + 12, rowY + 8, 20, GOLD);

                    // Round
                    const char *roundText = TextFormat("Wave %d", le->highestRound);
                    DrawText(roundText, panelX + 60, rowY + 8, 18, WHITE);

                    // Player name
                    DrawText(le->playerName, panelX + 180, rowY + 8, 16, (Color){180,180,200,255});

                    // Unit info
                    int ux = panelX + 180;
                    int uy = rowY + 32;
                    for (int u = 0; u < le->unitCount && u < BLUE_TEAM_MAX_SIZE; u++) {
                        SavedUnit *su = &le->units[u];
                        // Unit type name
                        const char *uname = (su->typeIndex < unitTypeCount) ? unitTypes[su->typeIndex].name : "???";
                        DrawText(uname, ux, uy, 12, (Color){150,180,255,255});
                        int nameW = MeasureText(uname, 12);

                        // 2x2 ability mini-grid
                        int gridX = ux + nameW + 6;
                        int miniSize = 14;
                        int miniGap = 2;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            int col = a % 2, row = a / 2;
                            int ax = gridX + col * (miniSize + miniGap);
                            int ay = uy + row * (miniSize + miniGap) - 4;
                            if (su->abilities[a].abilityId >= 0 && su->abilities[a].abilityId < ABILITY_COUNT) {
                                DrawRectangle(ax, ay, miniSize, miniSize, ABILITY_DEFS[su->abilities[a].abilityId].color);
                                const char *abbr = ABILITY_DEFS[su->abilities[a].abilityId].abbrev;
                                DrawText(abbr, ax + 1, ay + 2, 7, WHITE);
                            } else {
                                DrawRectangle(ax, ay, miniSize, miniSize, (Color){40,40,55,255});
                            }
                        }
                        // Unit code string below grid
                        {
                            char ucBuf[16];
                            AbilitySlot ucSlots[MAX_ABILITIES_PER_UNIT];
                            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                ucSlots[a] = (AbilitySlot){
                                    .abilityId = su->abilities[a].abilityId,
                                    .level = su->abilities[a].level,
                                    .cooldownRemaining = 0, .triggered = false
                                };
                            }
                            FormatUnitCode(su->typeIndex, ucSlots, ucBuf, sizeof(ucBuf));
                            DrawText(ucBuf, ux, uy + 2 * (miniSize + miniGap) + 2, 8, (Color){120,120,140,255});
                        }
                        ux += nameW + 6 + 2 * (miniSize + miniGap) + 12;
                    }
                }
                EndScissorMode();

                if (leaderboard.entryCount == 0) {
                    const char *emptyText = "No entries yet - play and Set in Stone!";
                    int etw = MeasureText(emptyText, 16);
                    DrawText(emptyText, panelX + panelW/2 - etw/2, panelY + panelH/2, 16, (Color){100,100,120,255});
                }
            }
        }

        //==============================================================================
        // PHASE_LOBBY DRAWING
        //==============================================================================
        if (phase == PHASE_LOBBY)
        {
            int lsw = GetScreenWidth();
            int lsh = GetScreenHeight();
            DrawRectangle(0, 0, lsw, lsh, (Color){ 20, 20, 30, 255 });

            const char *waitText = "WAITING FOR OPPONENT";
            int wtw = MeasureText(waitText, 30);
            DrawText(waitText, lsw/2 - wtw/2, lsh/2 - 60, 30, (Color){200, 180, 255, 255});

            // Show lobby code
            if (netClient.lobbyCode[0]) {
                const char *codeLabel = "Share this code:";
                int clw = MeasureText(codeLabel, 16);
                DrawText(codeLabel, lsw/2 - clw/2, lsh/2, 16, (Color){150,150,170,255});
                int ccw = MeasureText(netClient.lobbyCode, 40);
                DrawText(netClient.lobbyCode, lsw/2 - ccw/2, lsh/2 + 25, 40, WHITE);
            }

            // Animated dots
            int dots = (int)(GetTime() * 2) % 4;
            char dotBuf[8] = "";
            for (int d = 0; d < dots; d++) strcat(dotBuf, ".");
            DrawText(dotBuf, lsw/2 + wtw/2 + 5, lsh/2 - 60, 30, WHITE);

            const char *escText = "Press ESC to cancel";
            int ew = MeasureText(escText, 14);
            DrawText(escText, lsw/2 - ew/2, lsh/2 + 90, 14, (Color){100,100,120,255});
        }

        //==============================================================================
        // PHASE_MILESTONE DRAWING
        //==============================================================================
        if (phase == PHASE_MILESTONE)
        {
            int msw = GetScreenWidth();
            int msh = GetScreenHeight();

            // Dim overlay on top of 3D scene
            DrawRectangle(0, 0, msw, msh, (Color){0,0,0,160});

            // Title
            const char *msTitle = TextFormat("MILESTONE REACHED - Wave %d", currentRound);
            int mstw = MeasureText(msTitle, 32);
            DrawText(msTitle, msw/2 - mstw/2, 40, 32, GOLD);

            const char *msSubtitle = "Set your party in stone, or risk it all and continue";
            int mssw = MeasureText(msSubtitle, 16);
            DrawText(msSubtitle, msw/2 - mssw/2, 80, 16, (Color){200,200,220,200});

            // Collect active blue units
            int msBlue[BLUE_TEAM_MAX_SIZE]; int msCount = 0;
            for (int i = 0; i < unitCount && msCount < BLUE_TEAM_MAX_SIZE; i++)
                if (units[i].active && units[i].team == TEAM_BLUE) msBlue[msCount++] = i;

            // Unit cards (display only)
            int cardW = 200, cardH = 140, cardGap = 20;
            int totalW = msCount * cardW + (msCount > 1 ? (msCount - 1) * cardGap : 0);
            int startX = (msw - totalW) / 2;
            int cardY = msh / 2 - cardH / 2 - 20;

            for (int h = 0; h < msCount; h++) {
                int cx = startX + h * (cardW + cardGap);
                int ui = msBlue[h];
                UnitType *type = &unitTypes[units[ui].typeIndex];

                // Card background
                DrawRectangle(cx, cardY, cardW, cardH, (Color){35,35,50,240});
                DrawRectangleLinesEx((Rectangle){(float)cx,(float)cardY,(float)cardW,(float)cardH}, 2, (Color){60,60,80,255});

                // Portrait
                if (h < blueHudCount) {
                    int portSize = 80;
                    Rectangle srcRect = { 0, 0, (float)HUD_PORTRAIT_SIZE, -(float)HUD_PORTRAIT_SIZE };
                    Rectangle dstRect = { (float)(cx + 10), (float)(cardY + 10), (float)portSize, (float)portSize };
                    DrawTexturePro(portraits[h].texture, srcRect, dstRect, (Vector2){0,0}, 0.0f, WHITE);
                    DrawRectangleLines(cx + 10, cardY + 10, portSize, portSize, (Color){60,60,80,255});
                }

                // Unit name
                DrawText(type->name, cx + 10, cardY + 96, 14, (Color){200,200,220,255});

                // 2x2 ability grid
                int abilX = cx + 100;
                int abilY2 = cardY + 14;
                int slotSize = 28;
                int slotGap = 4;
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                    int col = a % 2, row = a / 2;
                    int ax = abilX + col * (slotSize + slotGap);
                    int ay = abilY2 + row * (slotSize + slotGap);
                    AbilitySlot *aslot = &units[ui].abilities[a];
                    if (aslot->abilityId >= 0 && aslot->abilityId < ABILITY_COUNT) {
                        DrawRectangle(ax, ay, slotSize, slotSize, ABILITY_DEFS[aslot->abilityId].color);
                        const char *abbr = ABILITY_DEFS[aslot->abilityId].abbrev;
                        int aw = MeasureText(abbr, 10);
                        DrawText(abbr, ax + (slotSize - aw)/2, ay + (slotSize - 10)/2, 10, WHITE);
                        const char *lvl = TextFormat("L%d", aslot->level + 1);
                        DrawText(lvl, ax + 2, ay + slotSize - 8, 7, (Color){220,220,220,200});
                    } else {
                        DrawRectangle(ax, ay, slotSize, slotSize, (Color){40,40,55,255});
                    }
                    DrawRectangleLines(ax, ay, slotSize, slotSize, (Color){90,90,110,255});
                }
            }

            // Buttons (two: SET IN STONE, CONTINUE)
            int btnW2 = 180, btnH2 = 44;
            int btnY2 = cardY + cardH + 40;
            int btnGap2 = 30;
            int totalBtnW2 = 2 * btnW2 + btnGap2;
            int btnStartX2 = (msw - totalBtnW2) / 2;

            // SET IN STONE button
            {
                Rectangle setBtn = { (float)btnStartX2, (float)btnY2, (float)btnW2, (float)btnH2 };
                Color setBg = (Color){200,170,40,255};
                if (CheckCollisionPointRec(GetMousePosition(), setBtn))
                    setBg = (Color){240,200,60,255};
                DrawRectangleRec(setBtn, setBg);
                DrawRectangleLinesEx(setBtn, 2, (Color){140,120,30,255});
                const char *setText = "SET IN STONE";
                int setW = MeasureText(setText, 16);
                DrawText(setText, (int)(setBtn.x + btnW2/2 - setW/2), (int)(setBtn.y + 14), 16, WHITE);
            }

            // CONTINUE button
            {
                Rectangle contBtn = { (float)(btnStartX2 + btnW2 + btnGap2), (float)btnY2, (float)btnW2, (float)btnH2 };
                Color contBg = (Color){50,160,70,255};
                if (CheckCollisionPointRec(GetMousePosition(), contBtn))
                    contBg = (Color){30,200,50,255};
                DrawRectangleRec(contBtn, contBg);
                DrawRectangleLinesEx(contBtn, 2, DARKGREEN);
                const char *contText = "CONTINUE";
                int contW = MeasureText(contText, 16);
                DrawText(contText, (int)(contBtn.x + btnW2/2 - contW/2), (int)(contBtn.y + 14), 16, WHITE);
            }

            // Risk warning
            const char *warnText = "SET IN STONE saves your party to the leaderboard and ends the run.";
            int warnW = MeasureText(warnText, 12);
            DrawText(warnText, msw/2 - warnW/2, btnY2 + btnH2 + 10, 12, (Color){255,180,80,200});
            const char *riskText = "CONTINUE risks everything - losing past this point means permanent death!";
            int riskW = MeasureText(riskText, 12);
            DrawText(riskText, msw/2 - riskW/2, btnY2 + btnH2 + 26, 12, (Color){255,100,80,200});
        }

        //==============================================================================
        // PHASE_GAME_OVER DRAWING — multiplayer
        //==============================================================================
        if (phase == PHASE_GAME_OVER && isMultiplayer)
        {
            int gosw = GetScreenWidth();
            int gosh = GetScreenHeight();
            DrawRectangle(0, 0, gosw, gosh, (Color){20,20,30,240});

            const char *goTitle = roundResultText;
            int gotw = MeasureText(goTitle, 36);
            DrawText(goTitle, gosw/2 - gotw/2, gosh/2 - 60, 36, GOLD);

            const char *goScore = TextFormat("Score: %d - %d", blueWins, redWins);
            int gsw = MeasureText(goScore, 20);
            DrawText(goScore, gosw/2 - gsw/2, gosh/2, 20, WHITE);

            const char *goRestart = "Press R to return to menu";
            int grw = MeasureText(goRestart, 16);
            DrawText(goRestart, gosw/2 - grw/2, gosh/2 + 40, 16, (Color){150,150,170,255});
        }

        //==============================================================================
        // PHASE_GAME_OVER DRAWING — non-death: withdraw units + reset (solo only)
        //==============================================================================
        if (phase == PHASE_GAME_OVER && !isMultiplayer && !deathPenalty)
        {
            int gosw = GetScreenWidth();
            int gosh = GetScreenHeight();

            // Full dark overlay
            DrawRectangle(0, 0, gosw, gosh, (Color){20,20,30,240});

            // Title
            const char *goTitle = "SET IN STONE";
            int gotw = MeasureText(goTitle, 36);
            DrawText(goTitle, gosw/2 - gotw/2, 40, 36, GOLD);

            const char *goRound = TextFormat("Reached Wave %d  |  Score: %d - %d", currentRound, blueWins, redWins);
            int gorw = MeasureText(goRound, 18);
            DrawText(goRound, gosw/2 - gorw/2, 85, 18, WHITE);

            // Collect surviving blue units
            int goBlue[BLUE_TEAM_MAX_SIZE]; int goCount = 0;
            for (int i = 0; i < unitCount && goCount < BLUE_TEAM_MAX_SIZE; i++)
                if (units[i].active && units[i].team == TEAM_BLUE) goBlue[goCount++] = i;

            // Subtitle
            if (goCount > 0) {
                const char *goSub = "Withdraw units before resetting (placeholder for NFC export)";
                int gosub = MeasureText(goSub, 14);
                DrawText(goSub, gosw/2 - gosub/2, 115, 14, (Color){180,180,200,180});
            } else {
                const char *goSub = "All units have been set in stone!";
                int gosub = MeasureText(goSub, 14);
                DrawText(goSub, gosw/2 - gosub/2, 115, 14, (Color){180,180,200,180});
            }

            // Unit cards with WITHDRAW button
            int goCardW = 200, goCardH = 140, goCardGap = 20;
            int goTotalW = goCount * goCardW + (goCount > 1 ? (goCount - 1) * goCardGap : 0);
            int goStartX = (gosw - goTotalW) / 2;
            int goCardY = gosh / 2 - 40;

            // Re-render portraits for game-over screen
            for (int h = 0; h < goCount; h++) {
                int ui = goBlue[h];
                UnitType *type = &unitTypes[units[ui].typeIndex];
                if (!type->loaded) continue;
                BoundingBox bb = type->baseBounds;
                float centerY = (bb.min.y + bb.max.y) / 2.0f * type->scale;
                float extent = (bb.max.y - bb.min.y) * type->scale;
                portraitCam.target = (Vector3){ 0.0f, centerY, 0.0f };
                portraitCam.position = (Vector3){ 0.0f, centerY, extent * 2.5f };
                BeginTextureMode(portraits[h]);
                    ClearBackground((Color){ 30, 30, 40, 255 });
                    BeginMode3D(portraitCam);
                        if (type->hasAnimations && type->animIndex[ANIM_IDLE] >= 0)
                            UpdateModelAnimation(type->model, type->idleAnims[type->animIndex[ANIM_IDLE]], 0);
                        DrawModel(type->model, (Vector3){ 0, 0, 0 }, type->scale, GetTeamTint(TEAM_BLUE));
                    EndMode3D();
                EndTextureMode();
            }

            for (int h = 0; h < goCount; h++) {
                int cx = goStartX + h * (goCardW + goCardGap);
                int ui = goBlue[h];
                UnitType *type = &unitTypes[units[ui].typeIndex];

                DrawRectangle(cx, goCardY, goCardW, goCardH, (Color){35,35,50,240});
                DrawRectangleLinesEx((Rectangle){(float)cx,(float)goCardY,(float)goCardW,(float)goCardH}, 2, (Color){60,60,80,255});

                // Portrait
                if (h < BLUE_TEAM_MAX_SIZE) {
                    int portSize = 80;
                    Rectangle srcRect = { 0, 0, (float)HUD_PORTRAIT_SIZE, -(float)HUD_PORTRAIT_SIZE };
                    Rectangle dstRect = { (float)(cx + 10), (float)(goCardY + 6), (float)portSize, (float)portSize };
                    DrawTexturePro(portraits[h].texture, srcRect, dstRect, (Vector2){0,0}, 0.0f, WHITE);
                    DrawRectangleLines(cx + 10, goCardY + 6, portSize, portSize, (Color){60,60,80,255});
                }

                // Unit name
                DrawText(type->name, cx + 10, goCardY + 90, 14, (Color){200,200,220,255});

                // 2x2 ability grid
                int goAbilX = cx + 100;
                int goAbilY = goCardY + 10;
                int goSlotSize = 28;
                int goSlotGap = 4;
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                    int col = a % 2, row = a / 2;
                    int ax = goAbilX + col * (goSlotSize + goSlotGap);
                    int ay = goAbilY + row * (goSlotSize + goSlotGap);
                    AbilitySlot *aslot = &units[ui].abilities[a];
                    if (aslot->abilityId >= 0 && aslot->abilityId < ABILITY_COUNT) {
                        DrawRectangle(ax, ay, goSlotSize, goSlotSize, ABILITY_DEFS[aslot->abilityId].color);
                        const char *abbr = ABILITY_DEFS[aslot->abilityId].abbrev;
                        int aw = MeasureText(abbr, 10);
                        DrawText(abbr, ax + (goSlotSize - aw)/2, ay + (goSlotSize - 10)/2, 10, WHITE);
                        const char *lvl = TextFormat("L%d", aslot->level + 1);
                        DrawText(lvl, ax + 2, ay + goSlotSize - 8, 7, (Color){220,220,220,200});
                    } else {
                        DrawRectangle(ax, ay, goSlotSize, goSlotSize, (Color){40,40,55,255});
                    }
                    DrawRectangleLines(ax, ay, goSlotSize, goSlotSize, (Color){90,90,110,255});
                }

                // WITHDRAW button
                Rectangle wdBtn = { (float)(cx + 10), (float)(goCardY + goCardH - 34), (float)(goCardW - 20), 28 };
                Color wdBg = (Color){60,50,120,255};
                if (CheckCollisionPointRec(GetMousePosition(), wdBtn))
                    wdBg = (Color){90,70,180,255};
                DrawRectangleRec(wdBtn, wdBg);
                DrawRectangleLinesEx(wdBtn, 1, (Color){100,80,160,255});
                const char *wdText = "WITHDRAW";
                int wdw = MeasureText(wdText, 12);
                DrawText(wdText, (int)(wdBtn.x + (goCardW - 20)/2 - wdw/2), (int)(wdBtn.y + 8), 12, WHITE);
            }

            // RESET button
            int resetBtnW = 180, resetBtnH = 44;
            int resetBtnY = goCardY + goCardH + 30;
            Rectangle resetBtn = { (float)(gosw/2 - resetBtnW/2), (float)resetBtnY, (float)resetBtnW, (float)resetBtnH };
            Color resetBg = (Color){180,50,50,255};
            if (CheckCollisionPointRec(GetMousePosition(), resetBtn))
                resetBg = (Color){220,70,70,255};
            DrawRectangleRec(resetBtn, resetBg);
            DrawRectangleLinesEx(resetBtn, 2, (Color){120,40,40,255});
            const char *resetText = "RESET";
            int rstw = MeasureText(resetText, 18);
            DrawText(resetText, (int)(resetBtn.x + resetBtnW/2 - rstw/2), (int)(resetBtn.y + 13), 18, WHITE);
        }

        //==============================================================================
        // UNIT INTRO SCREEN ("New Challenger" splash)
        //==============================================================================
        if (intro.active)
        {
            int isw = GetScreenWidth();
            int ish = GetScreenHeight();
            float t = intro.timer;

            // Animation progress values
            float wipeProgress = (t < INTRO_WIPE_IN) ? (t / INTRO_WIPE_IN) : 1.0f;
            float fadeAlpha = 1.0f;
            if (t >= INTRO_FADE_OUT_START) {
                fadeAlpha = 1.0f - (t - INTRO_FADE_OUT_START) / (INTRO_FADE_OUT_END - INTRO_FADE_OUT_START);
                if (fadeAlpha < 0.0f) fadeAlpha = 0.0f;
            }
            unsigned char alpha = (unsigned char)(255.0f * fadeAlpha);

            // --- Render 3D model into offscreen texture ---
            UnitType *itype = &unitTypes[intro.typeIndex];
            if (itype->loaded) {
                BoundingBox ib = itype->baseBounds;
                float icenterY = (ib.min.y + ib.max.y) / 2.0f * itype->scale;
                float iextent  = (ib.max.y - ib.min.y) * itype->scale;

                Camera introCam = { 0 };
                introCam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
                introCam.fovy = 30.0f;
                introCam.projection = CAMERA_PERSPECTIVE;
                introCam.target   = (Vector3){ 0.0f, icenterY, 0.0f };
                introCam.position = (Vector3){ 0.0f, icenterY, iextent * 2.0f };

                BeginTextureMode(introModelRT);
                    ClearBackground(BLANK);
                    BeginMode3D(introCam);
                        if (itype->hasAnimations && itype->animIndex[ANIM_IDLE] >= 0)
                            UpdateModelAnimation(itype->model,
                                itype->idleAnims[itype->animIndex[ANIM_IDLE]], intro.animFrame);
                        DrawModel(itype->model, (Vector3){0,0,0}, itype->scale,
                                  GetTeamTint(TEAM_BLUE));
                    EndMode3D();
                EndTextureMode();
            }

            // --- Procedural background (clipped to wipe) ---
            int wipeW = (int)(isw * wipeProgress);
            if (intro.typeIndex == 0) {
                // Mushroom: dark forest green
                DrawRectangle(0, 0, wipeW, ish, (Color){ 30, 45, 25, alpha });
                for (int ring = 0; ring < 8; ring++) {
                    float radius = 100.0f + ring * 80.0f;
                    unsigned char ra = (unsigned char)(alpha * 0.3f);
                    DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                        (Color){ (unsigned char)(50 + ring*8), (unsigned char)(70 + ring*5), 30, ra });
                }
                for (int ln = 0; ln < 12; ln++) {
                    int y = (ish / 12) * ln;
                    DrawLine(0, y, wipeW, y - 40,
                        (Color){ 80, 120, 50, (unsigned char)(alpha * 0.2f) });
                }
            } else {
                // Goblin: dark crimson
                DrawRectangle(0, 0, wipeW, ish, (Color){ 45, 20, 20, alpha });
                for (int ring = 0; ring < 8; ring++) {
                    float radius = 100.0f + ring * 80.0f;
                    unsigned char ra = (unsigned char)(alpha * 0.3f);
                    DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                        (Color){ (unsigned char)(120 + ring*10), 40, 30, ra });
                }
                for (int ln = 0; ln < 15; ln++) {
                    int y = (ish / 15) * ln;
                    DrawLine(0, y + 60, wipeW, y - 60,
                        (Color){ 180, 60, 30, (unsigned char)(alpha * 0.15f) });
                }
            }

            // --- Slash wipe edge ---
            if (wipeProgress < 1.0f) {
                int wipeX = wipeW;
                DrawLine(wipeX, -20, wipeX - 80, ish + 20,
                    (Color){ 255, 255, 255, alpha });
                DrawLine(wipeX + 3, -20, wipeX - 77, ish + 20,
                    (Color){ 255, 255, 200, (unsigned char)(alpha * 0.5f) });
                // Thicker glow
                DrawLine(wipeX - 1, -20, wipeX - 81, ish + 20,
                    (Color){ 255, 255, 255, (unsigned char)(alpha * 0.4f) });
            }

            // --- White flash at wipe completion ---
            if (t >= INTRO_WIPE_IN && t < INTRO_WIPE_IN + 0.15f) {
                float flashAlpha = 1.0f - (t - INTRO_WIPE_IN) / 0.15f;
                DrawRectangle(0, 0, isw, ish,
                    (Color){ 255, 255, 255, (unsigned char)(200.0f * flashAlpha * fadeAlpha) });
            }

            // --- 3D model composited (slide in from right) ---
            float modelSlide = 0.0f;
            if (t >= INTRO_HOLD_START) {
                float slideT = (t - INTRO_HOLD_START) / 0.3f;
                if (slideT > 1.0f) slideT = 1.0f;
                modelSlide = 1.0f - (1.0f - slideT) * (1.0f - slideT); // ease-out
            }
            float modelSize = ish * 0.85f;
            float modelFinalX = isw * 0.45f;
            float modelStartX = isw * 1.2f;
            float modelX = modelStartX + (modelFinalX - modelStartX) * modelSlide;
            float modelY = (ish - modelSize) / 2.0f;

            Rectangle introSrc = { 0, 0, 512.0f, -512.0f };
            Rectangle introDst = { modelX, modelY, modelSize, modelSize };
            DrawTexturePro(introModelRT.texture, introSrc, introDst,
                (Vector2){ 0, 0 }, 0.0f, (Color){ 255, 255, 255, alpha });

            // --- Unit name (slide in from left) ---
            float textSlide = 0.0f;
            if (t >= INTRO_HOLD_START + 0.1f) {
                float textT = (t - INTRO_HOLD_START - 0.1f) / 0.25f;
                if (textT > 1.0f) textT = 1.0f;
                textSlide = 1.0f - (1.0f - textT) * (1.0f - textT);
            }
            const char *introName = unitTypes[intro.typeIndex].name;
            int nameFontSize = ish / 8;
            int nameW = MeasureText(introName, nameFontSize);
            float nameFinalX = isw * 0.08f;
            float nameStartX = (float)(-nameW - 20);
            float nameX = nameStartX + (nameFinalX - nameStartX) * textSlide;
            float nameY = ish * 0.2f;

            // Shadow
            DrawText(introName, (int)nameX + 3, (int)nameY + 3, nameFontSize,
                (Color){ 0, 0, 0, (unsigned char)(alpha * 0.6f) });
            // Main text
            Color nameColor = GetTeamTint(TEAM_BLUE);
            nameColor.a = alpha;
            DrawText(introName, (int)nameX, (int)nameY, nameFontSize, nameColor);

            // Subtitle
            int subSize = nameFontSize / 3;
            if (subSize < 12) subSize = 12;
            const char *subText = "joins the battle!";
            DrawText(subText, (int)nameX + 4, (int)nameY + nameFontSize + 4, subSize,
                (Color){ 200, 200, 220, (unsigned char)(alpha * 0.8f) });

            // --- Decorative line under name ---
            if (textSlide > 0.0f) {
                int lineW = (int)((nameW + 40) * textSlide);
                int lineY2 = (int)nameY + nameFontSize + subSize + 12;
                DrawRectangle((int)nameFinalX, lineY2, lineW, 3,
                    (Color){ nameColor.r, nameColor.g, nameColor.b, (unsigned char)(alpha * 0.7f) });
            }

            // --- Ability slots (fade in with delay) ---
            if (t >= INTRO_HOLD_START + 0.4f) {
                float abilAlpha = (t - INTRO_HOLD_START - 0.4f) / 0.2f;
                if (abilAlpha > 1.0f) abilAlpha = 1.0f;
                abilAlpha *= fadeAlpha;
                unsigned char aa = (unsigned char)(255.0f * abilAlpha);

                int slotSize = 48;
                int slotGap = 8;
                int abilX = (int)nameFinalX;
                int abilY = (int)nameY + nameFontSize + subSize + 24;

                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                    int ax = abilX + a * (slotSize + slotGap);
                    AbilitySlot *slot = &units[intro.unitIndex].abilities[a];

                    if (slot->abilityId >= 0 && slot->abilityId < ABILITY_COUNT) {
                        Color abilCol = ABILITY_DEFS[slot->abilityId].color;
                        abilCol.a = aa;
                        DrawRectangle(ax, abilY, slotSize, slotSize, abilCol);
                        const char *abbr = ABILITY_DEFS[slot->abilityId].abbrev;
                        int aw = MeasureText(abbr, 16);
                        DrawText(abbr, ax + (slotSize - aw)/2,
                            abilY + (slotSize - 16)/2, 16, (Color){ 255, 255, 255, aa });
                        // Level
                        const char *lvl = TextFormat("L%d", slot->level + 1);
                        DrawText(lvl, ax + 2, abilY + slotSize - 10, 8, (Color){ 220, 220, 220, aa });
                    } else {
                        DrawRectangle(ax, abilY, slotSize, slotSize, (Color){ 40, 40, 55, aa });
                        const char *q = "?";
                        int qw = MeasureText(q, 22);
                        DrawText(q, ax + (slotSize - qw)/2,
                            abilY + (slotSize - 22)/2, 22, (Color){ 80, 80, 100, aa });
                    }
                    DrawRectangleLines(ax, abilY, slotSize, slotSize,
                        (Color){ 120, 120, 150, aa });
                }
            }
        }

        DrawFPS(10, 10);
        EndDrawing();
    }

    // Cleanup
    if (isMultiplayer) net_client_disconnect(&netClient);
    if (nfcPipe) {
        pclose(nfcPipe);
        printf("[NFC] Bridge closed\n");
    }
    for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++) UnloadRenderTexture(portraits[i]);
    UnloadRenderTexture(introModelRT);
    UnloadShader(lightShader);
    UnloadShader(borderShader);
    UnloadMesh(borderMesh);
    for (int i = 0; i < unitTypeCount; i++) {
        if (unitTypes[i].anims)
            UnloadModelAnimations(unitTypes[i].anims, unitTypes[i].animCount);
        if (unitTypes[i].idleAnims)
            UnloadModelAnimations(unitTypes[i].idleAnims, unitTypes[i].idleAnimCount);
        UnloadModel(unitTypes[i].model);
    }
    for (int i = 0; i < TILE_VARIANTS; i++) UnloadModel(tileModels[i]);
    UnloadTexture(tileDiffuse);
    UnloadSound(sfxWin);
    UnloadSound(sfxLoss);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
