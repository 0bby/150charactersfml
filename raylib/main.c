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
    unitTypes[0].scale = 0.07f;
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

    // Dedicated camera for portrait rendering
    Camera portraitCam = { 0 };
    portraitCam.up = (Vector3){ 0.0f, 1.0f, 0.0f };
    portraitCam.fovy = 30.0f;
    portraitCam.projection = CAMERA_PERSPECTIVE;

    // --- Lighting setup ---
    Shader lightShader = LoadShader(
        TextFormat("resources/shaders/glsl%i/lighting.vs", GLSL_VERSION),
        TextFormat("resources/shaders/glsl%i/lighting.fs", GLSL_VERSION));
    lightShader.locs[SHADER_LOC_VECTOR_VIEW] = GetShaderLocation(lightShader, "viewPos");

    int ambientLoc = GetShaderLocation(lightShader, "ambient");
    SetShaderValue(lightShader, ambientLoc, (float[4]){ 0.15f, 0.15f, 0.18f, 1.0f }, SHADER_UNIFORM_VEC4);

    Light lights[MAX_LIGHTS] = { 0 };
    lights[0] = CreateLight(LIGHT_DIRECTIONAL, (Vector3){ 50, 80, 50 }, Vector3Zero(), (Color){255, 245, 220, 255}, lightShader);
    lights[1] = CreateLight(LIGHT_POINT, (Vector3){ 0, 40, 0 }, Vector3Zero(), (Color){240, 240, 255, 255}, lightShader);

    // Assign lighting shader to all loaded models
    for (int i = 0; i < unitTypeCount; i++)
    {
        if (!unitTypes[i].loaded) continue;
        for (int m = 0; m < unitTypes[i].model.materialCount; m++)
            unitTypes[i].model.materials[m].shader = lightShader;
    }

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
    int playerGold = 10;
    int goldPerRound = 5;
    int rollCost = 2;
    ShopSlot shopSlots[MAX_SHOP_SLOTS];
    for (int i = 0; i < MAX_SHOP_SLOTS; i++) shopSlots[i].abilityId = -1;
    InventorySlot inventory[MAX_INVENTORY_SLOTS];
    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
    DragState dragState = { 0 };
    int removeConfirmUnit = -1;  // unit index awaiting removal confirmation (-1 = none)

    // Initial free shop roll
    RollShop(shopSlots, &playerGold, 0);

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

        // Update lighting shader with camera position
        float cameraPos[3] = { camera.position.x, camera.position.y, camera.position.z };
        SetShaderValue(lightShader, lightShader.locs[SHADER_LOC_VECTOR_VIEW], cameraPos, SHADER_UNIFORM_VEC3);

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
                        ClearAllModifiers(modifiers);
                        ClearAllProjectiles(projectiles);
                        ClearAllParticles(particles);
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
                // --- Shop: ROLL button click ---
                if (!clickedButton) {
                    int shopY = hudTop + 2;
                    Rectangle rollBtn = { 20, (float)(shopY + 10), 80, 30 };
                    if (CheckCollisionPointRec(mouse, rollBtn)) {
                        RollShop(shopSlots, &playerGold, rollCost);
                        clickedButton = true;
                    }
                }
                // --- Shop: Buy ability card click ---
                if (!clickedButton) {
                    int shopY = hudTop + 2;
                    int shopCardW = 100, shopCardH = 34, shopCardGap = 10;
                    int totalShopW = MAX_SHOP_SLOTS * shopCardW + (MAX_SHOP_SLOTS - 1) * shopCardGap;
                    int shopCardsX = (sw - totalShopW) / 2;
                    // Collect blue units for auto-combine
                    for (int s = 0; s < MAX_SHOP_SLOTS; s++) {
                        int scx = shopCardsX + s * (shopCardW + shopCardGap);
                        Rectangle r = { (float)scx, (float)(shopY + 8), (float)shopCardW, (float)shopCardH };
                        if (CheckCollisionPointRec(mouse, r) && shopSlots[s].abilityId >= 0) {
                            BuyAbility(&shopSlots[s], inventory, units, unitCount, &playerGold);
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
            if (dragState.dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))
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
                    if (modifiers[m].duration <= 0) { modifiers[m].active = false; continue; }
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

            // === STEP 2: Update projectiles ===
            for (int p = 0; p < MAX_PROJECTILES; p++) {
                if (!projectiles[p].active) continue;
                int ti = projectiles[p].targetIndex;
                // Target gone?
                if (ti < 0 || ti >= unitCount || !units[ti].active) {
                    if (projectiles[p].type == PROJ_CHAIN_FROST && projectiles[p].bouncesRemaining > 0) {
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
                    // HIT
                    if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                        float hitDmg = projectiles[p].damage;
                        // Magic Missile: damage is a fraction of target max HP
                        if (projectiles[p].type == PROJ_MAGIC_MISSILE)
                            hitDmg *= UNIT_STATS[units[ti].typeIndex].health;
                        units[ti].currentHealth -= hitDmg;
                        if (projectiles[p].stunDuration > 0)
                            AddModifier(modifiers, ti, MOD_STUN, projectiles[p].stunDuration, 0);
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
                } else {
                    projectiles[p].position.x += (pdx / pdist) * pstep;
                    projectiles[p].position.y += (pdy / pdist) * pstep;
                    projectiles[p].position.z += (pdz / pdist) * pstep;
                }
            }

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

                // Passive triggers (Dig) — blocked by stun
                if (!stunned) {
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        AbilitySlot *slot = &units[i].abilities[a];
                        if (slot->abilityId != ABILITY_DIG) continue;
                        if (slot->triggered || slot->cooldownRemaining > 0) continue;
                        const AbilityDef *def = &ABILITY_DEFS[ABILITY_DIG];
                        float threshold = def->values[slot->level][AV_DIG_HP_THRESH];
                        if (units[i].currentHealth > 0 && units[i].currentHealth <= stats->health * threshold) {
                            slot->triggered = true;
                            slot->cooldownRemaining = def->cooldown[slot->level];
                            float healDur = def->values[slot->level][AV_DIG_HEAL_DUR];
                            float healPerSec = stats->health / healDur;
                            AddModifier(modifiers, i, MOD_INVULNERABLE, healDur, 0);
                            AddModifier(modifiers, i, MOD_DIG_HEAL, healDur, healPerSec);
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

                // Active ability casting — one per frame, clockwise rotation
                bool castThisFrame = false;
                for (int attempt = 0; attempt < MAX_ABILITIES_PER_UNIT && !castThisFrame; attempt++) {
                    int slotIdx = ACTIVATION_ORDER[units[i].nextAbilitySlot];
                    units[i].nextAbilitySlot = (units[i].nextAbilitySlot + 1) % MAX_ABILITIES_PER_UNIT;

                    AbilitySlot *slot = &units[i].abilities[slotIdx];
                    if (slot->abilityId < 0 || slot->cooldownRemaining > 0) continue;
                    if (slot->abilityId == ABILITY_DIG) continue; // passive

                    const AbilityDef *def = &ABILITY_DEFS[slot->abilityId];

                    switch (slot->abilityId) {
                    case ABILITY_MAGIC_MISSILE: {
                        if (target < 0) break;
                        float d = DistXZ(units[i].position, units[target].position);
                        if (d > def->range[slot->level]) break;
                        SpawnProjectile(projectiles, PROJ_MAGIC_MISSILE,
                            units[i].position, target, i, units[i].team, slot->level,
                            def->values[slot->level][AV_MM_PROJ_SPEED],
                            def->values[slot->level][AV_MM_DAMAGE],
                            def->values[slot->level][AV_MM_STUN_DUR],
                            (Color){120, 80, 255, 255});
                        slot->cooldownRemaining = def->cooldown[slot->level];
                        castThisFrame = true;
                    } break;
                    case ABILITY_VACUUM: {
                        float radius = def->values[slot->level][AV_VAC_RADIUS];
                        float stunDur = def->values[slot->level][AV_VAC_STUN_DUR];
                        bool hitAny = false;
                        for (int j = 0; j < unitCount; j++) {
                            if (!units[j].active || units[j].team == units[i].team) continue;
                            if (UnitHasModifier(modifiers, j, MOD_INVULNERABLE)) continue;
                            float d = DistXZ(units[i].position, units[j].position);
                            if (d <= radius) {
                                units[j].position.x = units[i].position.x;
                                units[j].position.z = units[i].position.z;
                                AddModifier(modifiers, j, MOD_STUN, stunDur, 0);
                                hitAny = true;
                            }
                        }
                        slot->cooldownRemaining = def->cooldown[slot->level];
                        castThisFrame = true;
                    } break;
                    case ABILITY_CHAIN_FROST: {
                        if (target < 0) break;
                        float d = DistXZ(units[i].position, units[target].position);
                        if (d > def->range[slot->level]) break;
                        SpawnChainFrostProjectile(projectiles,
                            units[i].position, target, i, units[i].team, slot->level,
                            def->values[slot->level][AV_CF_PROJ_SPEED],
                            def->values[slot->level][AV_CF_DAMAGE],
                            (int)def->values[slot->level][AV_CF_BOUNCES],
                            def->values[slot->level][AV_CF_BOUNCE_RANGE]);
                        slot->cooldownRemaining = def->cooldown[slot->level];
                        castThisFrame = true;
                    } break;
                    case ABILITY_BLOOD_RAGE: {
                        float dur = def->values[slot->level][AV_BR_DURATION];
                        float ls = def->values[slot->level][AV_BR_LIFESTEAL];
                        AddModifier(modifiers, i, MOD_LIFESTEAL, dur, ls);
                        slot->cooldownRemaining = def->cooldown[slot->level];
                        castThisFrame = true;
                    } break;
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
                    float dx = units[target].position.x - units[i].position.x;
                    float dz = units[target].position.z - units[i].position.z;
                    float len = sqrtf(dx*dx + dz*dz);
                    if (len > 0.001f) {
                        units[i].position.x += (dx/len) * moveSpeed * dt;
                        units[i].position.z += (dz/len) * moveSpeed * dt;
                    }
                }
                else
                {
                    units[i].attackCooldown -= dt;
                    if (units[i].attackCooldown <= 0.0f)
                    {
                        if (!UnitHasModifier(modifiers, target, MOD_INVULNERABLE)) {
                            float dmg = stats->attackDamage;
                            float armor = GetModifierValue(modifiers, target, MOD_ARMOR);
                            dmg -= armor;
                            if (dmg < 0) dmg = 0;
                            units[target].currentHealth -= dmg;
                            // Lifesteal
                            float ls = GetModifierValue(modifiers, i, MOD_LIFESTEAL);
                            if (ls > 0) {
                                units[i].currentHealth += dmg * ls;
                                if (units[i].currentHealth > stats->health)
                                    units[i].currentHealth = stats->health;
                            }
                            if (units[target].currentHealth <= 0) units[target].active = false;
                        }
                        units[i].attackCooldown = stats->attackSpeed;
                    }
                }
            }

            // Smooth Y toward ground during combat
            for (int i = 0; i < unitCount; i++) {
                if (!units[i].active) continue;
                units[i].position.y += (0.0f - units[i].position.y) * 0.1f;
            }

            // Check round end
            int ba, ra;
            CountTeams(units, unitCount, &ba, &ra);
            if (ba == 0 || ra == 0) {
                if (ba > 0) { blueWins++; roundResultText = "BLUE WINS THE ROUND!"; }
                else if (ra > 0) { redWins++; roundResultText = "RED WINS THE ROUND!"; }
                else { roundResultText = "DRAW — NO SURVIVORS!"; }
                currentRound++;
                phase = PHASE_ROUND_OVER;
                roundOverTimer = 2.5f;
                ClearAllParticles(particles);
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
                    // Reset transient ability state
                    for (int i = 0; i < unitCount; i++) {
                        units[i].nextAbilitySlot = 0;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            units[i].abilities[a].cooldownRemaining = 0;
                            units[i].abilities[a].triggered = false;
                        }
                    }
                    ClearAllModifiers(modifiers);
                    ClearAllProjectiles(projectiles);
                    playerGold += goldPerRound;
                    RollShop(shopSlots, &playerGold, 0); // free roll
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
                ClearAllModifiers(modifiers);
                ClearAllProjectiles(projectiles);
                ClearAllParticles(particles);
                playerGold = 10;
                for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                RollShop(shopSlots, &playerGold, 0);
                dragState.dragging = false;
                phase = PHASE_PREP;
            }
        }

        //==============================================================================
        // ANIMATION UPDATE
        //==============================================================================
        for (int i = 0; i < unitCount; i++) {
            if (!units[i].active) continue;
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
                    if (type->hasAnimations && type->animIndex[ANIM_IDLE] >= 0)
                        UpdateModelAnimation(type->model, type->idleAnims[type->animIndex[ANIM_IDLE]], 0);
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
                if (type->hasAnimations) {
                    int idx = type->animIndex[units[i].currentAnim];
                    if (idx >= 0) {
                        ModelAnimation *arr = (units[i].currentAnim == ANIM_IDLE) ? type->idleAnims : type->anims;
                        UpdateModelAnimation(type->model, arr[idx], units[i].animFrame);
                    }
                }
                DrawModelEx(type->model, units[i].position, (Vector3){0,1,0}, units[i].facingAngle,
                    (Vector3){type->scale, type->scale, type->scale}, tint);

                if (units[i].selected)
                {
                    BoundingBox sb = GetUnitBounds(&units[i], type);
                    DrawBoundingBox(sb, GREEN);
                }
            }

            // Draw stun indicator (yellow ring at feet)
            for (int i = 0; i < unitCount; i++) {
                if (!units[i].active) continue;
                if (UnitHasModifier(modifiers, i, MOD_STUN)) {
                    Vector3 ringPos = { units[i].position.x, units[i].position.y + 0.3f, units[i].position.z };
                    DrawCircle3D(ringPos, 4.0f, (Vector3){1,0,0}, 90.0f, YELLOW);
                    DrawCircle3D(ringPos, 3.5f, (Vector3){1,0,0}, 90.0f, YELLOW);
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

            // Modifier labels
            int modY = by + bh + 14;
            for (int m = 0; m < MAX_MODIFIERS; m++) {
                if (!modifiers[m].active || modifiers[m].unitIndex != i) continue;
                const char *modLabel = NULL;
                Color modColor = WHITE;
                switch (modifiers[m].type) {
                    case MOD_STUN:         modLabel = "STUNNED";   modColor = YELLOW;  break;
                    case MOD_INVULNERABLE: modLabel = "INVULN";    modColor = SKYBLUE; break;
                    case MOD_LIFESTEAL:    modLabel = "LIFESTEAL"; modColor = RED;     break;
                    case MOD_SPEED_MULT:   modLabel = "SPEED";     modColor = GREEN;   break;
                    case MOD_ARMOR:        modLabel = "ARMOR";     modColor = GRAY;    break;
                    case MOD_DIG_HEAL:     modLabel = "DIGGING";   modColor = BROWN;   break;
                }
                if (modLabel) {
                    int mlw = MeasureText(modLabel, 9);
                    DrawText(modLabel, (int)sp.x - mlw/2, modY, 9, modColor);
                    modY += 10;
                }
            }
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

                        AbilitySlot *aslot = &units[ui].abilities[a];
                        if (aslot->abilityId >= 0 && aslot->abilityId < ABILITY_COUNT) {
                            // Filled slot — colored background
                            DrawRectangle(ax, ay, HUD_ABILITY_SLOT_SIZE, HUD_ABILITY_SLOT_SIZE,
                                         ABILITY_COLORS[aslot->abilityId]);
                            // Abbreviation
                            const char *abbr = ABILITY_ABBREV[aslot->abilityId];
                            int aw2 = MeasureText(abbr, 11);
                            DrawText(abbr, ax + (HUD_ABILITY_SLOT_SIZE - aw2) / 2,
                                    ay + (HUD_ABILITY_SLOT_SIZE - 11) / 2, 11, WHITE);
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
                                      ABILITY_COLORS[inventory[inv].abilityId]);
                        const char *iabbr = ABILITY_ABBREV[inventory[inv].abilityId];
                        int iaw = MeasureText(iabbr, 11);
                        DrawText(iabbr, ix + (HUD_ABILITY_SLOT_SIZE-iaw)/2,
                                 iy + (HUD_ABILITY_SLOT_SIZE-11)/2, 11, WHITE);
                        const char *ilvl = TextFormat("L%d", inventory[inv].level + 1);
                        DrawText(ilvl, ix + 2, iy + HUD_ABILITY_SLOT_SIZE - 9, 8, (Color){220,220,220,200});
                    }
                }
            }

            // --- Drag ghost ---
            if (dragState.dragging && dragState.abilityId >= 0 && dragState.abilityId < ABILITY_COUNT) {
                Vector2 dmouse = GetMousePosition();
                DrawRectangle((int)dmouse.x - 16, (int)dmouse.y - 16, 32, 32,
                              ABILITY_COLORS[dragState.abilityId]);
                DrawRectangleLines((int)dmouse.x - 16, (int)dmouse.y - 16, 32, 32, WHITE);
                const char *dabbr = ABILITY_ABBREV[dragState.abilityId];
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
                        Color cardBg = canAfford ? ABILITY_COLORS[shopSlots[s].abilityId] : (Color){50,50,65,255};
                        if (canAfford && CheckCollisionPointRec(GetMousePosition(),
                            (Rectangle){(float)scx,(float)scy,(float)shopCardW,(float)shopCardH}))
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

        DrawFPS(10, 10);
        EndDrawing();
    }

    // Cleanup
    if (nfcPipe) {
        pclose(nfcPipe);
        printf("[NFC] Bridge closed\n");
    }
    for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++) UnloadRenderTexture(portraits[i]);
    UnloadShader(lightShader);
    for (int i = 0; i < unitTypeCount; i++) {
        if (unitTypes[i].anims)
            UnloadModelAnimations(unitTypes[i].anims, unitTypes[i].animCount);
        if (unitTypes[i].idleAnims)
            UnloadModelAnimations(unitTypes[i].idleAnims, unitTypes[i].idleAnimCount);
        UnloadModel(unitTypes[i].model);
    }
    CloseWindow();
    return 0;
}
