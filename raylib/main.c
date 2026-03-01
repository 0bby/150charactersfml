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

// --- Color grading tweakable defaults (bright & bubbly) ---
static float cgExposure      = 0.89f;
static float cgContrast      = 1.20f;
static float cgSaturation    = 0.85f;
static float cgTemperature   = 0.10f;
static float cgVignetteStr   = 0.46f;
static float cgVignetteSoft  = 0.94f;
static float cgLift[3]       = { 0.05f, 0.04f, 0.02f };
static float cgGain[3]       = { 1.08f, 1.06f, 1.02f };
static bool  cgDebugOverlay  = false;

#include "game.h"
#include "synergies.h"
#include "helpers.h"
#include "leaderboard.h"
#include "net_client.h"

// Global font — loaded in main(), used by GameDrawText/GameMeasureText
static Font g_gameFont = { 0 };

static inline void GameDrawText(const char *text, int x, int y, int fontSize, Color color)
{
    if (g_gameFont.glyphCount > 0) {
        float spacing = (float)fontSize / 10.0f;
        DrawTextEx(g_gameFont, text, (Vector2){ (float)x, (float)y }, (float)fontSize, spacing, color);
    } else {
        DrawText(text, x, y, fontSize, color);
    }
}

static inline int GameMeasureText(const char *text, int fontSize)
{
    if (g_gameFont.glyphCount > 0) {
        float spacing = (float)fontSize / 10.0f;
        return (int)MeasureTextEx(g_gameFont, text, (float)fontSize, spacing).x;
    }
    return MeasureText(text, fontSize);
}

// Returns WHITE or BLACK depending on background luminance for readable text
static inline Color TextColorForBg(Color bg)
{
    float lum = 0.299f * bg.r + 0.587f * bg.g + 0.114f * bg.b;
    return (lum > 150.0f) ? BLACK : WHITE;
}

// Draw text with auto contrast + shadow on colored backgrounds
static inline void GameDrawTextOnColor(const char *text, int x, int y, int fontSize, Color bg)
{
    Color fg = TextColorForBg(bg);
    Color shadow = (fg.r == 0) ? (Color){255,255,255,80} : (Color){0,0,0,150};
    GameDrawText(text, x + 1, y + 1, fontSize, shadow);
    GameDrawText(text, x, y, fontSize, fg);
}
#include "pve_waves.h"
#include "plaza.h"

// --- UI Scale (720p base) ---
static float uiScale = 1.0f;
#define S(x) ((int)((x) * uiScale))

// --- Hit flash ---
#define HIT_FLASH_DURATION 0.12f

// --- Projectile polish ---
#define PROJ_CHARGE_TIME    0.2f
#define CAST_PAUSE_TIME     0.25f
#define PROJ_TRAIL_LIFE     0.4f
#define PROJ_TRAIL_SIZE     1.0f
#define PROJ_EXPLODE_COUNT  30

// --- Win/loss sound split point (seconds) — tweak & re-split with ffmpeg if needed ---
#define ENDGAME_SFX_VOL  0.5f
#define COMBAT_SFX_VOL   0.5f
#define VOICE_SFX_VOL    0.5f
#define SPAWN_SFX_VOL    0.5f
#define UI_SFX_VOL       0.7f
#define BGM_VOL          0.3f

//------------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------------
int main(void)
{
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 720, "Relic Rivals");
    SetWindowMinSize(640, 360);
    InitAudioDevice();

    // Load font at large size — bilinear filter handles downscaling
    g_gameFont = LoadFontEx("fonts/game_font.ttf", 128, NULL, 0);
    if (g_gameFont.glyphCount > 0) {
        GenTextureMipmaps(&g_gameFont.texture);
        SetTextureFilter(g_gameFont.texture, TEXTURE_FILTER_TRILINEAR);
        printf("[FONT] Loaded game font (%d glyphs)\n", g_gameFont.glyphCount);
    } else {
        printf("[FONT] Failed to load fonts/game_font.ttf, using default\n");
    }

    // Win/loss sounds — pre-split into separate files
    Sound sfxWin  = LoadSound("music/match_win.ogg");
    Sound sfxLoss = LoadSound("music/match_loss.ogg");
    SetSoundVolume(sfxWin,  ENDGAME_SFX_VOL);
    SetSoundVolume(sfxLoss, ENDGAME_SFX_VOL);
    bool lastOutcomeWin = false;

    // Combat SFX
    Sound sfxMeleeHit        = LoadSound("sfx/melee_hit.ogg");
    Sound sfxProjectileWhoosh= LoadSound("sfx/projectile_whoosh.ogg");
    Sound sfxProjectileHit   = LoadSound("sfx/projectile_hit.ogg");
    Sound sfxMagicHit        = LoadSound("sfx/magic_hit.ogg");
    SetSoundVolume(sfxMeleeHit, COMBAT_SFX_VOL);
    SetSoundVolume(sfxProjectileWhoosh, COMBAT_SFX_VOL);
    SetSoundVolume(sfxProjectileHit, COMBAT_SFX_VOL);
    SetSoundVolume(sfxMagicHit, COMBAT_SFX_VOL);
    // Unit voice SFX
    Sound sfxToadShout   = LoadSound("sfx/toad_shout.ogg");
    Sound sfxToadDie     = LoadSound("sfx/toad_die.ogg");
    Sound sfxGoblinShout = LoadSound("sfx/goblin_shout.ogg");
    Sound sfxGoblinDie   = LoadSound("sfx/goblin_die.ogg");
    SetSoundVolume(sfxToadShout, VOICE_SFX_VOL);
    SetSoundVolume(sfxToadDie, VOICE_SFX_VOL);
    SetSoundVolume(sfxGoblinShout, VOICE_SFX_VOL);
    SetSoundVolume(sfxGoblinDie, VOICE_SFX_VOL);
    // Spawn SFX
    Sound sfxCharacterFall = LoadSound("sfx/character_fall.ogg");
    Sound sfxCharacterLand = LoadSound("sfx/character_land.ogg");
    Sound sfxNewCharacter  = LoadSound("sfx/new_character.ogg");
    SetSoundVolume(sfxCharacterFall, SPAWN_SFX_VOL);
    SetSoundVolume(sfxCharacterLand, SPAWN_SFX_VOL);
    SetSoundVolume(sfxNewCharacter, SPAWN_SFX_VOL);
    // UI SFX
    Sound sfxUiClick  = LoadSound("sfx/ui_click.ogg");
    Sound sfxUiBuy    = LoadSound("sfx/ui_buy.ogg");
    Sound sfxUiDrag   = LoadSound("sfx/ui_drag.ogg");
    Sound sfxUiDrop   = LoadSound("sfx/ui_drop.ogg");
    Sound sfxUiReroll = LoadSound("sfx/ui_reroll.ogg");
    SetSoundVolume(sfxUiClick, UI_SFX_VOL);
    SetSoundVolume(sfxUiBuy, UI_SFX_VOL);
    SetSoundVolume(sfxUiDrag, UI_SFX_VOL);
    SetSoundVolume(sfxUiDrop, UI_SFX_VOL);
    SetSoundVolume(sfxUiReroll, UI_SFX_VOL);

    // Generate radial gradient texture for particle billboards (white center → transparent edge)
    Texture2D particleTex;
    #define PARTICLE_TEX_SIZE 32
    {
        Image img = GenImageColor(PARTICLE_TEX_SIZE, PARTICLE_TEX_SIZE, BLANK);
        float half = PARTICLE_TEX_SIZE / 2.0f;
        for (int y = 0; y < PARTICLE_TEX_SIZE; y++) {
            for (int x = 0; x < PARTICLE_TEX_SIZE; x++) {
                float dx = (x + 0.5f - half) / half;
                float dy = (y + 0.5f - half) / half;
                float dist = sqrtf(dx*dx + dy*dy);
                if (dist > 1.0f) dist = 1.0f;
                // Additive-friendly: full white center, smooth falloff to 0
                // Brightness stays high so stacked particles blow out to white
                float t = 1.0f - dist;
                float intensity = t * t * t;  // cubic falloff - tight bright core
                unsigned char v = (unsigned char)(255.0f * intensity);
                ImageDrawPixel(&img, x, y, (Color){ 255, 255, 255, v });
            }
        }
        particleTex = LoadTextureFromImage(img);
        UnloadImage(img);
    }

    // Default 1x1 ORM texture for models without ORM files.
    // (R=255,G=128,B=0) = AO=1.0, Roughness~0.5, Metallic=0.0 — preserves current look.
    Texture2D defaultORM;
    {
        Image ormImg = GenImageColor(1, 1, (Color){ 255, 128, 0, 255 });
        defaultORM = LoadTextureFromImage(ormImg);
        UnloadImage(ormImg);
    }

    // Background music
    Music bgm = LoadMusicStream("music/bgm.ogg");
    SetMusicVolume(bgm, BGM_VOL);
    PlayMusicStream(bgm);

    // Camera presets — prep (top-down auto-chess) vs combat (diagonal MOBA) vs plaza (cinematic)
    const float prepHeight = 200.0f, prepDistance = 150.0f, prepFOV = 48.0f, prepX = 0.0f;
    const float combatHeight = 135.0f, combatDistance = 165.0f, combatFOV = 55.0f, combatX = 37.0f;
    const float plazaHeight = 120.0f, plazaDistance = 180.0f, plazaFOV = 55.0f, plazaX = 25.0f;
    const float camLerpSpeed = 2.5f;

    float camHeight = prepHeight;
    float camDistance = prepDistance;
    float camFOV = prepFOV;
    float camX = prepX;
    bool camOverride = false;
    Camera camera = { 0 };
    camera.position = (Vector3){ camX, camHeight, camDistance };
    camera.target   = (Vector3){ 0.0f, 0.0f, 35.0f };
    camera.up       = (Vector3){ 0.0f, 1.0f, 0.0f };
    camera.fovy     = camFOV;
    camera.projection = CAMERA_PERSPECTIVE;

    // Unit types
    int unitTypeCount = 6;
    UnitType unitTypes[MAX_UNIT_TYPES] = { 0 };
    unitTypes[0].name = "Mushroom";
    unitTypes[0].modelPath = "assets/classes/mushroom/MushroomTest.obj";
    unitTypes[0].scale = 0.10f;
    unitTypes[0].yOffset = 1.5f;
    unitTypes[1].name = "Goblin";
    unitTypes[1].modelPath = "assets/goblin/animations/PluginGoblinWalk.glb";
    unitTypes[1].scale = 9.0f;
    unitTypes[2].name = "Devil";
    unitTypes[2].modelPath = "assets/classes/devil/DevilIdle.glb";
    unitTypes[2].scale = 9.0f;
    unitTypes[2].yOffset = 0.0f;
    // slots 3 and 4 (Puppycat, Siren) descoped
    unitTypes[5].name = "Reptile";
    unitTypes[5].modelPath = "assets/classes/reptile/ReptileIdle.glb";
    unitTypes[5].scale = 9.0f;
    unitTypes[5].yOffset = 0.0f;

    for (int i = 0; i < unitTypeCount; i++)
    {
        if (!unitTypes[i].modelPath) { unitTypes[i].loaded = false; continue; }
        unitTypes[i].model = LoadModel(unitTypes[i].modelPath);
        if (unitTypes[i].model.meshCount > 0)
        {
            unitTypes[i].baseBounds = GetMeshBoundingBox(unitTypes[i].model.meshes[0]);
            unitTypes[i].loaded = true;
        }
        else unitTypes[i].loaded = false;

        // Fix GLB alpha: force all material diffuse maps to full opacity
        for (int m = 0; m < unitTypes[i].model.materialCount; m++) {
            unitTypes[i].model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            unitTypes[i].model.materials[m].maps[MATERIAL_MAP_METALNESS].texture = defaultORM;
        }
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
    // ANIM_SCARED: use dedicated scared GLB if available, else fallback to walk
    unitTypes[1].scaredAnims = NULL;
    unitTypes[1].scaredAnimCount = 0;
    if (walkAnimCount > 0) unitTypes[1].animIndex[ANIM_SCARED] = 0;  // fallback to walk
    unitTypes[1].hasAnimations = (walkAnimCount > 0 || idleAnimCount > 0);
    unitTypes[1].attackAnims = NULL; unitTypes[1].attackAnimCount = 0;
    unitTypes[1].castAnims = NULL;   unitTypes[1].castAnimCount = 0;

    // Reptile animations
    {
        int cnt = 0;
        ModelAnimation *walk = LoadModelAnimations("assets/classes/reptile/ReptileWalking.glb", &cnt);
        unitTypes[5].anims = walk; unitTypes[5].animCount = cnt;

        cnt = 0;
        ModelAnimation *idle = LoadModelAnimations("assets/classes/reptile/ReptileIdle.glb", &cnt);
        unitTypes[5].idleAnims = idle; unitTypes[5].idleAnimCount = cnt;

        cnt = 0;
        ModelAnimation *atk = LoadModelAnimations("assets/classes/reptile/ReptileAttack.glb", &cnt);
        unitTypes[5].attackAnims = atk; unitTypes[5].attackAnimCount = cnt;

        unitTypes[5].scaredAnims = NULL; unitTypes[5].scaredAnimCount = 0;
        unitTypes[5].castAnims = NULL;   unitTypes[5].castAnimCount = 0;

        for (int s = 0; s < ANIM_COUNT; s++) unitTypes[5].animIndex[s] = -1;
        if (unitTypes[5].idleAnimCount > 0)   unitTypes[5].animIndex[ANIM_IDLE] = 0;
        if (unitTypes[5].animCount > 0)       unitTypes[5].animIndex[ANIM_WALK] = 0;
        if (unitTypes[5].animCount > 0)       unitTypes[5].animIndex[ANIM_SCARED] = 0;
        if (unitTypes[5].attackAnimCount > 0) unitTypes[5].animIndex[ANIM_ATTACK] = 0;
        unitTypes[5].hasAnimations = true;
    }

    // Devil animations
    {
        int cnt = 0;
        ModelAnimation *walk = LoadModelAnimations("assets/classes/devil/DevilWalk.glb", &cnt);
        unitTypes[2].anims = walk; unitTypes[2].animCount = cnt;

        cnt = 0;
        ModelAnimation *idle = LoadModelAnimations("assets/classes/devil/DevilIdle.glb", &cnt);
        unitTypes[2].idleAnims = idle; unitTypes[2].idleAnimCount = cnt;

        cnt = 0;
        ModelAnimation *atk = LoadModelAnimations("assets/classes/devil/DevilPunch.glb", &cnt);
        unitTypes[2].attackAnims = atk; unitTypes[2].attackAnimCount = cnt;

        cnt = 0;
        ModelAnimation *cast = LoadModelAnimations("assets/classes/devil/DevilMagic.glb", &cnt);
        unitTypes[2].castAnims = cast; unitTypes[2].castAnimCount = cnt;

        cnt = 0;
        ModelAnimation *scared = LoadModelAnimations("assets/classes/devil/DevilScared.glb", &cnt);
        unitTypes[2].scaredAnims = scared; unitTypes[2].scaredAnimCount = cnt;

        for (int s = 0; s < ANIM_COUNT; s++) unitTypes[2].animIndex[s] = -1;
        if (unitTypes[2].idleAnimCount > 0)    unitTypes[2].animIndex[ANIM_IDLE] = 0;
        if (unitTypes[2].animCount > 0)        unitTypes[2].animIndex[ANIM_WALK] = 0;
        if (unitTypes[2].scaredAnimCount > 0)  unitTypes[2].animIndex[ANIM_SCARED] = 0;
        if (unitTypes[2].attackAnimCount > 0)  unitTypes[2].animIndex[ANIM_ATTACK] = 0;
        if (unitTypes[2].castAnimCount > 0)    unitTypes[2].animIndex[ANIM_CAST] = 0;
        unitTypes[2].hasAnimations = true;
    }

    // Portrait render textures for HUD (one per max blue unit)
    RenderTexture2D portraits[BLUE_TEAM_MAX_SIZE];
    for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++)
        portraits[i] = LoadRenderTexture(HUD_PORTRAIT_SIZE_BASE, HUD_PORTRAIT_SIZE_BASE);

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

    // --- SSAO post-process ---
    Shader ssaoShader = LoadShader(NULL,
        TextFormat("resources/shaders/glsl%i/ssao.fs", GLSL_VERSION));
    int ssaoResLoc  = GetShaderLocation(ssaoShader, "resolution");
    int ssaoNearLoc = GetShaderLocation(ssaoShader, "near");
    int ssaoFarLoc  = GetShaderLocation(ssaoShader, "far");
    int ssaoDepthLoc = GetShaderLocation(ssaoShader, "texture1");

    // --- FXAA post-process ---
    Shader fxaaShader = LoadShader(NULL,
        TextFormat("resources/shaders/glsl%i/fxaa.fs", GLSL_VERSION));
    int fxaaResLoc = GetShaderLocation(fxaaShader, "resolution");
    // Scene render texture with samplable depth texture (not renderbuffer)
    int sceneRTWidth = GetScreenWidth();
    int sceneRTHeight = GetScreenHeight();
    RenderTexture2D sceneRT = { 0 };
    sceneRT.id = rlLoadFramebuffer();
    sceneRT.texture.id = rlLoadTexture(NULL, sceneRTWidth, sceneRTHeight, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    sceneRT.texture.width = sceneRTWidth;
    sceneRT.texture.height = sceneRTHeight;
    sceneRT.texture.format = RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    sceneRT.texture.mipmaps = 1;
    sceneRT.depth.id = rlLoadTextureDepth(sceneRTWidth, sceneRTHeight, false);
    sceneRT.depth.width = sceneRTWidth;
    sceneRT.depth.height = sceneRTHeight;
    rlFramebufferAttach(sceneRT.id, sceneRT.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(sceneRT.id, sceneRT.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);

    // FXAA render target (fullscreen, color only)
    int fxaaRTWidth = sceneRTWidth;
    int fxaaRTHeight = sceneRTHeight;
    RenderTexture2D fxaaRT = LoadRenderTexture(fxaaRTWidth, fxaaRTHeight);

    // --- Color grading post-process ---
    Shader colorGradeShader = LoadShader(NULL,
        TextFormat("resources/shaders/glsl%i/color_grade.fs", GLSL_VERSION));
    int cgExposureLoc   = GetShaderLocation(colorGradeShader, "exposure");
    int cgContrastLoc   = GetShaderLocation(colorGradeShader, "contrast");
    int cgSaturationLoc = GetShaderLocation(colorGradeShader, "saturation");
    int cgTemperatureLoc= GetShaderLocation(colorGradeShader, "temperature");
    int cgVigStrLoc     = GetShaderLocation(colorGradeShader, "vignetteStrength");
    int cgVigSoftLoc    = GetShaderLocation(colorGradeShader, "vignetteSoftness");
    int cgLiftLoc       = GetShaderLocation(colorGradeShader, "lift");
    int cgGainLoc       = GetShaderLocation(colorGradeShader, "gain");
    RenderTexture2D colorGradeRT = LoadRenderTexture(fxaaRTWidth, fxaaRTHeight);

    // --- Shadow map setup (color+depth FBO for guaranteed completeness) ---
    #define SHADOW_MAP_SIZE 2048
    RenderTexture2D shadowRT = { 0 };
    shadowRT.id = rlLoadFramebuffer();
    shadowRT.texture.id = rlLoadTexture(NULL, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
    shadowRT.texture.width = SHADOW_MAP_SIZE;
    shadowRT.texture.height = SHADOW_MAP_SIZE;
    shadowRT.texture.format = RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    shadowRT.texture.mipmaps = 1;
    shadowRT.depth.id = rlLoadTextureDepth(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, false);
    shadowRT.depth.width = SHADOW_MAP_SIZE;
    shadowRT.depth.height = SHADOW_MAP_SIZE;
    rlFramebufferAttach(shadowRT.id, shadowRT.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    rlFramebufferAttach(shadowRT.id, shadowRT.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(shadowRT.id)) TraceLog(LOG_ERROR, "Shadow map FBO is not complete!");

    Shader shadowDepthShader = LoadShader(
        TextFormat("resources/shaders/glsl%i/shadow_depth.vs", GLSL_VERSION),
        TextFormat("resources/shaders/glsl%i/shadow_depth.fs", GLSL_VERSION));

    // Light-space matrix (static directional light)
    Vector3 shadowLightPos = { 40.0f, 60.0f, -30.0f };
    Vector3 shadowLightTarget = { 0.0f, 0.0f, 0.0f };
    Matrix lightView = MatrixLookAt(shadowLightPos, shadowLightTarget, (Vector3){ 0.0f, 1.0f, 0.0f });
    Matrix lightProj = MatrixOrtho(-160.0, 160.0, -160.0, 160.0, 1.0, 350.0);
    Matrix lightVP = MatrixMultiply(lightView, lightProj);

    // Uniform locations for shadow mapping in lighting shader
    int lightVPLoc = GetShaderLocation(lightShader, "lightVP");
    int shadowMapLoc = GetShaderLocation(lightShader, "shadowMap");
    int shadowDebugLoc = GetShaderLocation(lightShader, "shadowDebug");
    int noShadowLoc = GetShaderLocation(lightShader, "noShadow");
    int normalMapLoc = GetShaderLocation(lightShader, "normalMap");
    int useNormalMapLoc = GetShaderLocation(lightShader, "useNormalMap");

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
    Texture2D tileORM = LoadTexture("assets/goblin/environment/tiles/T_Tiles_ORM.png");
    Texture2D tileNormal = LoadTexture("assets/goblin/environment/tiles/T_Tiles_N.png");

    for (int i = 0; i < TILE_VARIANTS; i++) {
        tileModels[i] = LoadModel(tilePaths[i]);
        for (int mi = 0; mi < tileModels[i].meshCount; mi++) GenMeshTangents(&tileModels[i].meshes[mi]);
        // Compute OBJ-space center from bounding box
        BoundingBox bb = GetMeshBoundingBox(tileModels[i].meshes[0]);
        tileCenters[i] = (Vector3){
            (bb.min.x + bb.max.x) * 0.5f,
            (bb.min.y + bb.max.y) * 0.5f,
            (bb.min.z + bb.max.z) * 0.5f,
        };
        // Assign diffuse + ORM textures and lighting shader to all materials
        for (int m = 0; m < tileModels[i].materialCount; m++) {
            tileModels[i].materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = tileDiffuse;
            tileModels[i].materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            tileModels[i].materials[m].maps[MATERIAL_MAP_METALNESS].texture = tileORM;
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
    int playerGold = 25;
    int goldPerRound = 15;
    int rollCost = 1;
    const int rollCostBase = 1;
    const int rollCostIncrement = 1;
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
    bool usedShopHotkey = false;  // hides hotkey hint after first use
    bool usedRollHotkey = false;  // hides roll hint after first use

    // Synergy hover tooltip state
    int hoverSynergyIdx = -1;
    float hoverSynergyTimer = 0.0f;
    const float synergyTooltipDelay = 0.3f;

    // --- Visual juice state ---
    float fightBannerTimer = -1.0f;  // <0 = inactive
    float slowmoTimer = 0.0f;        // >0 = slow motion active
    float slowmoScale = 1.0f;
    // Kill feed
    int killCount = 0;               // total kills this round
    int multiKillCount = 0;          // rapid consecutive kills by same team
    float multiKillTimer = 0.0f;     // window for multi-kill
    Team lastKillTeam = TEAM_BLUE;   // which team scored the last kill
    float killFeedTimer = -1.0f;     // <0 = inactive
    char killFeedText[32] = {0};
    float killFeedScale = 1.0f;      // punch-in scale

    // Battle log
    BattleLog battleLog = {0};
    float combatElapsedTime = 0.0f;

    // Plaza state
    PlazaSubState plazaState = PLAZA_ROAMING;
    float plazaTimer = 0.0f;
    PlazaUnitData plazaData[MAX_UNITS] = {0};
    bool showMultiplayerPanel = false;
    Model doorModel = LoadModel("assets/goblin/environment/door/Door.obj");
    for (int m = 0; m < doorModel.materialCount; m++) {
        doorModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        doorModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = defaultORM;
        doorModel.materials[m].shader = lightShader;
    }
    // Re-center and scale Door (Maya cm export, verts in 300-1000 range)
    if (doorModel.meshCount > 0) {
        BoundingBox dbb = GetMeshBoundingBox(doorModel.meshes[0]);
        float dCenterX = (dbb.min.x + dbb.max.x) * 0.5f;
        float dBaseY   = dbb.min.y;
        float dCenterZ = (dbb.min.z + dbb.max.z) * 0.5f;
        float dHeight  = dbb.max.y - dbb.min.y;
        float dScale   = 15.0f / dHeight;  // ~15 game units tall
        doorModel.transform = MatrixMultiply(
            MatrixTranslate(-dCenterX, -dBaseY, -dCenterZ),
            MatrixScale(dScale, dScale, dScale));
    }
    Model trophyModel = LoadModel("assets/goblin/environment/trophy/Trophy.obj");
    for (int m = 0; m < trophyModel.materialCount; m++) {
        trophyModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        trophyModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = defaultORM;
        trophyModel.materials[m].shader = lightShader;
    }
    // Re-center and scale Trophy (Maya cm export, verts around -6000 range)
    if (trophyModel.meshCount > 0) {
        BoundingBox tbb = GetMeshBoundingBox(trophyModel.meshes[0]);
        float tCenterX = (tbb.min.x + tbb.max.x) * 0.5f;
        float tBaseY   = tbb.min.y;
        float tCenterZ = (tbb.min.z + tbb.max.z) * 0.5f;
        float tHeight  = tbb.max.y - tbb.min.y;
        float tScale   = 10.0f / tHeight;  // ~10 game units tall
        trophyModel.transform = MatrixMultiply(
            MatrixTranslate(-tCenterX, -tBaseY, -tCenterZ),
            MatrixScale(tScale, tScale, tScale));
    }
    // --- Environment models: ground (replaces old platform), stairs, circle ---
    Texture2D groundDiffuse = LoadTexture("assets/goblin/environment/ground/T_Ground_BC.png");
    Texture2D groundORM = LoadTexture("assets/goblin/environment/ground/T_Ground_ORM.png");
    Texture2D groundNormal = LoadTexture("assets/goblin/environment/ground/T_Ground_N.png");
    Model platformModel = LoadModel("assets/goblin/environment/ground/ground.obj");
    for (int mi = 0; mi < platformModel.meshCount; mi++) GenMeshTangents(&platformModel.meshes[mi]);
    for (int m = 0; m < platformModel.materialCount; m++) {
        platformModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = groundDiffuse;
        platformModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        platformModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = groundORM;
        platformModel.materials[m].shader = lightShader;
    }
    if (platformModel.meshCount > 0) {
        BoundingBox pbb = GetMeshBoundingBox(platformModel.meshes[0]);
        float pCenterX = (pbb.min.x + pbb.max.x) * 0.5f;
        float pTopY    = pbb.max.y;  // anchor top surface at Y=0
        float pCenterZ = (pbb.min.z + pbb.max.z) * 0.5f;
        float pWidth   = pbb.max.x - pbb.min.x;
        float pScale   = 750.0f / pWidth;
        platformModel.transform = MatrixMultiply(
            MatrixTranslate(-pCenterX, -pTopY, -pCenterZ),
            MatrixScale(pScale, pScale, pScale));
    }

    Texture2D stairsDiffuse = LoadTexture("assets/goblin/environment/stairs/T_Stairs_BC.png");
    Texture2D stairsORM = LoadTexture("assets/goblin/environment/stairs/T_Stairs_ORM.png");
    Texture2D stairsNormal = LoadTexture("assets/goblin/environment/stairs/T_Stairs_N.png");
    Model stairsModel = LoadModel("assets/goblin/environment/stairs/Stairs_LP.obj");
    for (int mi = 0; mi < stairsModel.meshCount; mi++) GenMeshTangents(&stairsModel.meshes[mi]);
    for (int m = 0; m < stairsModel.materialCount; m++) {
        stairsModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = stairsDiffuse;
        stairsModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        stairsModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = stairsORM;
        stairsModel.materials[m].shader = lightShader;
    }
    if (stairsModel.meshCount > 0) {
        BoundingBox sbb = GetMeshBoundingBox(stairsModel.meshes[0]);
        float sCenterX = (sbb.min.x + sbb.max.x) * 0.5f;
        float sBaseY   = sbb.min.y;
        float sCenterZ = (sbb.min.z + sbb.max.z) * 0.5f;
        float sHeight  = sbb.max.y - sbb.min.y;
        float sScale   = 10.0f / sHeight;
        stairsModel.transform = MatrixMultiply(
            MatrixTranslate(-sCenterX, -sBaseY, -sCenterZ),
            MatrixScale(sScale, sScale, sScale));
    }

    Texture2D circleDiffuse = LoadTexture("assets/goblin/environment/circle/T_Circle_BC.png");
    Texture2D circleORM = LoadTexture("assets/goblin/environment/circle/T_Circle_ORM.png");
    Texture2D circleNormal = LoadTexture("assets/goblin/environment/circle/T_Circle_N.png");
    Model circleModel = LoadModel("assets/goblin/environment/circle/circle.obj");
    for (int mi = 0; mi < circleModel.meshCount; mi++) GenMeshTangents(&circleModel.meshes[mi]);
    for (int m = 0; m < circleModel.materialCount; m++) {
        circleModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = circleDiffuse;
        circleModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
        circleModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = circleORM;
        circleModel.materials[m].shader = lightShader;
    }
    if (circleModel.meshCount > 0) {
        BoundingBox cbb = GetMeshBoundingBox(circleModel.meshes[0]);
        float cCenterX = (cbb.min.x + cbb.max.x) * 0.5f;
        float cCenterY = (cbb.min.y + cbb.max.y) * 0.5f;
        float cCenterZ = (cbb.min.z + cbb.max.z) * 0.5f;
        float cWidth   = cbb.max.x - cbb.min.x;
        float cScale   = 80.0f / cWidth;
        // Center, scale, then tilt upright (-90° around X so far face points toward arena)
        circleModel.transform = MatrixMultiply(
            MatrixMultiply(
                MatrixTranslate(-cCenterX, -cCenterY, -cCenterZ),
                MatrixScale(cScale, cScale, cScale)),
            MatrixRotateX(-90.0f * DEG2RAD));
    }

    // platformPos, stairsFarPos, stairsLPos, stairsRPos, circlePos now live in envPieces[]

    Vector3 doorPos = { 120.0f, 0.0f, 80.0f };
    Vector3 trophyPos = { -120.0f, 0.0f, 80.0f };

    // --- Environment model catalog (for debug piece editor) ---
    EnvModelDef envModels[MAX_ENV_MODELS] = {0};
    int envModelCount = 0;

    // 0: Arches
    {
        EnvModelDef *em = &envModels[envModelCount];
        em->name = "Arches";
        em->modelPath = "assets/goblin/environment/arches/Arches.obj";
        em->texturePath = "assets/goblin/environment/arches/T_Arches_BC.png";
        em->ormTexturePath = "assets/goblin/environment/arches/T_Arches_ORM.png";
        em->normalTexturePath = "assets/goblin/environment/arches/T_Arches_N.png";
        em->texture = LoadTexture(em->texturePath);
        em->ormTexture = LoadTexture(em->ormTexturePath);
        em->normalTexture = LoadTexture(em->normalTexturePath);
        em->model = LoadModel(em->modelPath);
        for (int mi = 0; mi < em->model.meshCount; mi++) GenMeshTangents(&em->model.meshes[mi]);
        for (int m = 0; m < em->model.materialCount; m++) {
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture = em->ormTexture;
            em->model.materials[m].shader = lightShader;
        }
        if (em->model.meshCount > 0) {
            BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
            float cx = (bb.min.x + bb.max.x) * 0.5f;
            float by = bb.min.y;
            float cz = (bb.min.z + bb.max.z) * 0.5f;
            float h  = bb.max.y - bb.min.y;
            float s  = 15.0f / h;
            em->model.transform = MatrixMultiply(
                MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
        }
        em->loaded = true;
        envModelCount++;
    }
    // 1: Wall
    {
        EnvModelDef *em = &envModels[envModelCount];
        em->name = "Wall";
        em->modelPath = "assets/goblin/environment/wall/Wall_LP.obj";
        em->texturePath = "assets/goblin/environment/wall/T_Wall_BC.png";
        em->ormTexturePath = "assets/goblin/environment/wall/T_Wall_ORM.png";
        em->normalTexturePath = "assets/goblin/environment/wall/T_Wall_N.png";
        em->texture = LoadTexture(em->texturePath);
        em->ormTexture = LoadTexture(em->ormTexturePath);
        em->normalTexture = LoadTexture(em->normalTexturePath);
        em->model = LoadModel(em->modelPath);
        for (int mi = 0; mi < em->model.meshCount; mi++) GenMeshTangents(&em->model.meshes[mi]);
        for (int m = 0; m < em->model.materialCount; m++) {
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture = em->ormTexture;
            em->model.materials[m].shader = lightShader;
        }
        if (em->model.meshCount > 0) {
            BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
            float cx = (bb.min.x + bb.max.x) * 0.5f;
            float by = bb.min.y;
            float cz = (bb.min.z + bb.max.z) * 0.5f;
            float h  = bb.max.y - bb.min.y;
            float s  = 15.0f / h;
            em->model.transform = MatrixMultiply(
                MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
        }
        em->loaded = true;
        envModelCount++;
    }
    // 2: Stairs (reuse already-loaded stairsModel)
    {
        EnvModelDef *em = &envModels[envModelCount];
        em->name = "Stairs";
        em->modelPath = "assets/goblin/environment/stairs/Stairs_LP.obj";
        em->texturePath = NULL;
        em->model = stairsModel;  // reuse — do NOT unload separately
        em->texture = (Texture2D){0};
        em->normalTexture = stairsNormal;
        em->loaded = true;
        envModelCount++;
    }
    // 3: Circle (reuse already-loaded circleModel)
    {
        EnvModelDef *em = &envModels[envModelCount];
        em->name = "Circle";
        em->modelPath = "assets/goblin/environment/circle/circle.obj";
        em->texturePath = NULL;
        em->model = circleModel;  // reuse — do NOT unload separately
        em->texture = (Texture2D){0};
        em->normalTexture = circleNormal;
        em->loaded = true;
        envModelCount++;
    }
    // 4: FloorTiles
    {
        EnvModelDef *em = &envModels[envModelCount];
        em->name = "FloorTiles";
        em->modelPath = "assets/goblin/environment/floor_tiles/FloorTiles_LP.obj";
        em->texturePath = NULL;
        em->model = LoadModel(em->modelPath);
        for (int mi = 0; mi < em->model.meshCount; mi++) GenMeshTangents(&em->model.meshes[mi]);
        em->texture = (Texture2D){0};
        em->normalTexture = tileNormal;
        for (int m = 0; m < em->model.materialCount; m++) {
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = tileDiffuse;
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture = tileORM;
            em->model.materials[m].shader = lightShader;
        }
        if (em->model.meshCount > 0) {
            BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
            float cx = (bb.min.x + bb.max.x) * 0.5f;
            float by = bb.min.y;
            float cz = (bb.min.z + bb.max.z) * 0.5f;
            float h  = bb.max.y - bb.min.y;
            float s  = 10.0f / h;
            em->model.transform = MatrixMultiply(
                MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
        }
        em->loaded = true;
        envModelCount++;
    }
    // 5: Ground (reuse already-loaded platformModel)
    {
        EnvModelDef *em = &envModels[envModelCount];
        em->name = "Ground";
        em->modelPath = "assets/goblin/environment/ground/ground.obj";
        em->texturePath = NULL;
        em->model = platformModel;  // reuse — do NOT unload separately
        em->texture = (Texture2D){0};
        em->normalTexture = groundNormal;
        em->loaded = true;
        envModelCount++;
    }
    // 6: PillarBig
    {
        EnvModelDef *em = &envModels[envModelCount];
        em->name = "PillarBig";
        em->modelPath = "assets/goblin/environment/pillars/PillarBig_LP.obj";
        em->texturePath = "assets/goblin/environment/pillars/T_Pillars_BC.png";
        em->ormTexturePath = "assets/goblin/environment/pillars/T_Pillars_ORM.png";
        em->normalTexturePath = "assets/goblin/environment/pillars/T_Pillars_N.png";
        em->texture = LoadTexture(em->texturePath);
        em->ormTexture = LoadTexture(em->ormTexturePath);
        em->normalTexture = LoadTexture(em->normalTexturePath);
        em->model = LoadModel(em->modelPath);
        for (int mi = 0; mi < em->model.meshCount; mi++) GenMeshTangents(&em->model.meshes[mi]);
        for (int m = 0; m < em->model.materialCount; m++) {
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture = em->ormTexture;
            em->model.materials[m].shader = lightShader;
        }
        if (em->model.meshCount > 0) {
            BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
            float cx = (bb.min.x + bb.max.x) * 0.5f;
            float by = bb.min.y;
            float cz = (bb.min.z + bb.max.z) * 0.5f;
            float h  = bb.max.y - bb.min.y;
            float s  = 15.0f / h;
            em->model.transform = MatrixMultiply(
                MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
        }
        em->loaded = true;
        envModelCount++;
    }
    // 7: PillarSmall (shares textures with PillarBig)
    {
        EnvModelDef *em = &envModels[envModelCount];
        EnvModelDef *pillarBig = &envModels[envModelCount - 1];
        em->name = "PillarSmall";
        em->modelPath = "assets/goblin/environment/pillars/PillarSmall_LP.obj";
        em->texturePath = pillarBig->texturePath;
        em->ormTexturePath = pillarBig->ormTexturePath;
        em->normalTexturePath = pillarBig->normalTexturePath;
        em->texture = pillarBig->texture;         // shared — do NOT unload separately
        em->ormTexture = pillarBig->ormTexture;   // shared
        em->normalTexture = pillarBig->normalTexture; // shared
        em->model = LoadModel(em->modelPath);
        for (int mi = 0; mi < em->model.meshCount; mi++) GenMeshTangents(&em->model.meshes[mi]);
        for (int m = 0; m < em->model.materialCount; m++) {
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
            em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
            em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture = em->ormTexture;
            em->model.materials[m].shader = lightShader;
        }
        if (em->model.meshCount > 0) {
            BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
            float cx = (bb.min.x + bb.max.x) * 0.5f;
            float by = bb.min.y;
            float cz = (bb.min.z + bb.max.z) * 0.5f;
            float h  = bb.max.y - bb.min.y;
            float s  = 15.0f / h;
            em->model.transform = MatrixMultiply(
                MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
        }
        em->loaded = true;
        envModelCount++;
    }

    // --- Env pieces array (populated from save file) ---
    EnvPiece envPieces[MAX_ENV_PIECES] = {0};
    int envPieceCount = 0;
    int envSelectedPiece = -1;
    bool envDragging = false;
    float envSaveFlashTimer = 0.0f;  // flash "SAVED" text

    // Load env layout from file
    {
        FILE *fp = fopen("env_layout.txt", "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp) && envPieceCount < MAX_ENV_PIECES) {
                if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
                int mi;
                float x, y, z, rot, sc;
                if (sscanf(line, "%d %f %f %f %f %f", &mi, &x, &y, &z, &rot, &sc) == 6) {
                    if (mi >= 0 && mi < envModelCount) {
                        envPieces[envPieceCount] = (EnvPiece){
                            .modelIndex = mi, .position = {x, y, z},
                            .rotationY = rot, .scale = sc, .active = true
                        };
                        envPieceCount++;
                    }
                }
            }
            fclose(fp);
        }
    }
    // Populate default env pieces if no layout was loaded
    if (envPieceCount == 0) {
        // Ground
        envPieces[envPieceCount++] = (EnvPiece){ .modelIndex = 5, .position = {0, -10, 0},
            .rotationY = 0, .scale = 1.0f, .active = true };
        // Stairs far
        envPieces[envPieceCount++] = (EnvPiece){ .modelIndex = 2, .position = {0, -1, -120},
            .rotationY = 0, .scale = 1.0f, .active = true };
        // Stairs left
        envPieces[envPieceCount++] = (EnvPiece){ .modelIndex = 2, .position = {-120, -1, 0},
            .rotationY = 90, .scale = 1.0f, .active = true };
        // Stairs right
        envPieces[envPieceCount++] = (EnvPiece){ .modelIndex = 2, .position = {120, -1, 0},
            .rotationY = -90, .scale = 1.0f, .active = true };
        // Circle
        envPieces[envPieceCount++] = (EnvPiece){ .modelIndex = 3, .position = {0, 0, -140},
            .rotationY = 0, .scale = 1.0f, .active = true };
    }
    int plazaHoverObject = 0;  // 0=none, 1=trophy, 2=door
    float plazaSparkleTimer = 0.0f;  // for sparkle effect on objects

    // Round / score state
    GamePhase phase = PHASE_PLAZA;
    int currentRound = 0;          // 0-indexed, displayed as 1-indexed
    int blueWins = 0;
    int redWins  = 0;
    float roundOverTimer = 0.0f;   // brief pause after a round ends
    const char *roundResultText = "";
    bool debugMode = false;
    int shadowDebugMode = 0;

    // Leaderboard & prestige state
    Leaderboard leaderboard = {0};
    LoadLeaderboard(&leaderboard, LEADERBOARD_FILE);
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
    int playBtnW = 120;
    int playBtnH = 40;

    // Spawn initial plaza enemies
    PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);

    SetTargetFPS(60);

    // --- NFC Bridge Subprocess ---
    FILE *nfcPipe = popen("../nfc/build/bridge", "r");
    int nfcFd = -1;
    char nfcLineBuf[128];
    int nfcLinePos = 0;
    float easterEggTimer = 0.0f;
    if (nfcPipe) {
        nfcFd = fileno(nfcPipe);
        int flags = fcntl(nfcFd, F_GETFL, 0);
        fcntl(nfcFd, F_SETFL, flags | O_NONBLOCK);
        printf("[NFC] Bridge launched\n");
    } else {
        printf("[NFC] Failed to launch bridge\n");
    }

    // NFC UID is now stored directly in Unit.nfcUid / Unit.nfcUidLen
    // Naming state for first-time scans
    int namingUnitIndex = -1;  // >= 0 = unit awaiting name input
    char namingBuf[32] = {0};
    int namingPos = 0;
    char nfcNameBuf[32] = {0};  // temp buffer for lookup response

    // Prefetch known NFC UIDs from server (local authority for existence checks)
    NfcUidCache nfcCache = {0};
    net_nfc_prefetch(serverHost, NET_PORT, &nfcCache);

    //==================================================================================
    // MAIN LOOP
    //==================================================================================
    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        float rawDt = dt; // unscaled dt for UI timers
        uiScale = (float)GetScreenHeight() / 720.0f;
        if (uiScale < 1.0f) uiScale = 1.0f;
        // Scaled HUD dimensions
        int hudBarH = S(HUD_UNIT_BAR_HEIGHT_BASE);
        int hudShopH = S(HUD_SHOP_HEIGHT_BASE);
        int hudTotalH = hudBarH + hudShopH;
        int hudCardW = S(HUD_CARD_WIDTH_BASE);
        int hudCardH = S(HUD_CARD_HEIGHT_BASE);
        int hudCardSpacing = S(HUD_CARD_SPACING_BASE);
        int hudPortraitSize = S(HUD_PORTRAIT_SIZE_BASE);
        int hudAbilSlotSize = S(HUD_ABILITY_SLOT_SIZE_BASE);
        int hudAbilSlotGap = S(HUD_ABILITY_SLOT_GAP_BASE);
        playBtnW = S(160);
        playBtnH = S(44);
        UpdateMusicStream(bgm);
        if (IsMusicStreamPlaying(bgm) && GetMusicTimePlayed(bgm) >= GetMusicTimeLength(bgm) - 0.05f) {
            SeekMusicStream(bgm, 29.091f);
        }
        // Slow-motion time scaling
        if (slowmoTimer > 0.0f) {
            slowmoTimer -= rawDt;
            if (slowmoTimer <= 0.0f) { slowmoTimer = 0.0f; slowmoScale = 1.0f; }
            dt *= slowmoScale;
        }
        // Fight banner timer
        if (fightBannerTimer >= 0.0f) fightBannerTimer += rawDt;
        // Kill feed timer
        if (killFeedTimer >= 0.0f) killFeedTimer += rawDt;
        // Multi-kill window decay
        if (multiKillTimer > 0.0f) {
            multiKillTimer -= rawDt;
            if (multiKillTimer <= 0.0f) multiKillCount = 0;
        }
        GamePhase prevPhase = phase;
        UpdateShake(&shake, dt);
        if (IsKeyPressed(KEY_F1)) debugMode = !debugMode;
        if (IsKeyPressed(KEY_F6)) cgDebugOverlay = !cgDebugOverlay;
        if (cgDebugOverlay) {
            float step = 0.01f;
            if (IsKeyDown(KEY_ONE))   cgExposure    += step;
            if (IsKeyDown(KEY_TWO))   cgExposure    -= step;
            if (IsKeyDown(KEY_THREE)) cgContrast    += step;
            if (IsKeyDown(KEY_FOUR))  cgContrast    -= step;
            if (IsKeyDown(KEY_FIVE))  cgSaturation  += step;
            if (IsKeyDown(KEY_SIX))   cgSaturation  -= step;
            if (IsKeyDown(KEY_SEVEN)) cgTemperature += step;
            if (IsKeyDown(KEY_EIGHT)) cgTemperature -= step;
            if (IsKeyDown(KEY_NINE))  cgVignetteStr += step;
            if (IsKeyDown(KEY_ZERO))  cgVignetteStr -= step;
            if (IsKeyDown(KEY_MINUS)) cgVignetteSoft += step;
            if (IsKeyDown(KEY_EQUAL)) cgVignetteSoft -= step;
        }
        if (IsKeyPressed(KEY_F10)) {
            shadowDebugMode = (shadowDebugMode + 1) % 5;
            SetShaderValue(lightShader, shadowDebugLoc, &shadowDebugMode, SHADER_UNIFORM_INT);
        }

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

            // Env piece keyboard controls (selected piece)
            if (envSelectedPiece >= 0 && envSelectedPiece < envPieceCount && envPieces[envSelectedPiece].active) {
                if (IsKeyPressed(KEY_Q)) envPieces[envSelectedPiece].rotationY -= 15.0f;
                if (IsKeyPressed(KEY_E)) envPieces[envSelectedPiece].rotationY += 15.0f;
                if (IsKeyPressed(KEY_R)) envPieces[envSelectedPiece].position.y += 1.0f;
                if (IsKeyPressed(KEY_F)) envPieces[envSelectedPiece].position.y -= 1.0f;
                if (IsKeyPressed(KEY_RIGHT_BRACKET)) envPieces[envSelectedPiece].scale += 0.1f;
                if (IsKeyPressed(KEY_LEFT_BRACKET)) {
                    envPieces[envSelectedPiece].scale -= 0.1f;
                    if (envPieces[envSelectedPiece].scale < 0.1f) envPieces[envSelectedPiece].scale = 0.1f;
                }
                if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE)) {
                    envPieces[envSelectedPiece].active = false;
                    // Compact: shift remaining pieces down
                    for (int j = envSelectedPiece; j < envPieceCount - 1; j++)
                        envPieces[j] = envPieces[j + 1];
                    envPieceCount--;
                    envPieces[envPieceCount] = (EnvPiece){0};
                    envSelectedPiece = -1;
                    envDragging = false;
                }
            }

            // Env piece dragging (XZ plane)
            if (envDragging && envSelectedPiece >= 0 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
                RayCollision hit = GetRayCollisionQuad(ray,
                    (Vector3){ -500, 0, -500 }, (Vector3){ -500, 0, 500 },
                    (Vector3){  500, 0,  500 }, (Vector3){  500, 0, -500 });
                if (hit.hit) {
                    envPieces[envSelectedPiece].position.x = hit.point.x;
                    envPieces[envSelectedPiece].position.z = hit.point.z;
                }
            }
            if (envDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                envDragging = false;
            }

            // Env save flash timer
            if (envSaveFlashTimer > 0.0f) envSaveFlashTimer -= dt;
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
                int phaseBefore = statueSpawn.phase;
                UpdateStatueSpawn(&statueSpawn, particles, &shake, units[si].position, dt);
                if (phaseBefore != SSPAWN_FALLING && statueSpawn.phase == SSPAWN_FALLING)
                    PlaySound(sfxCharacterFall);
                if (statueSpawn.phase == SSPAWN_DONE) {
                    PlaySound(sfxCharacterLand);
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
                    // Trigger plaza scared on impact
                    if (phase == PHASE_PLAZA && plazaState == PLAZA_ROAMING) {
                        PlazaTriggerScared(units, unitCount, plazaData, &plazaState, &plazaTimer);
                    }
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
        int prevHoverSynergyIdx = hoverSynergyIdx;
        hoverSynergyIdx = -1;

        // Lerp camera toward phase preset (skip when debug override active)
        if (!camOverride) {
            bool combat = (phase == PHASE_COMBAT);
            bool plaza = (phase == PHASE_PLAZA);
            // Scale camera to compensate for larger HUD at higher resolutions
            float hudFrac = (float)hudTotalH / (float)GetScreenHeight();
            float camScale = 1.0f / (1.0f - hudFrac * 0.5f);  // pull back more as HUD grows
            float tgtH = (plaza ? plazaHeight : (combat ? combatHeight : prepHeight)) * camScale;
            float tgtD = (plaza ? plazaDistance : (combat ? combatDistance : prepDistance)) * camScale;
            float tgtF = plaza ? plazaFOV : (combat ? combatFOV : prepFOV);
            float tgtX = plaza ? plazaX : (combat ? combatX : prepX);
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

        // Poll NFC bridge for tag scans (raw read to avoid stdio buffering issues)
        if (nfcFd >= 0) {
            // Drain bytes from pipe into line buffer
            char rdBuf[64];
            int n = read(nfcFd, rdBuf, sizeof(rdBuf));
            if (n > 0) {
                for (int bi = 0; bi < n; bi++) {
                    char c = rdBuf[bi];
                    if (c == '\n' || c == '\r') {
                        if (nfcLinePos > 0) {
                            nfcLineBuf[nfcLinePos] = '\0';
                            // Process complete line (only when in valid phase)
                            if ((phase == PHASE_PLAZA || phase == PHASE_PREP) && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE) {
                                // Parse reader prefix and hex UID: "N:<hex_uid>"
                                int nfcReader = 0;
                                const char *nfcHex = nfcLineBuf;
                                if (nfcLineBuf[0] >= '1' && nfcLineBuf[0] <= '9' && nfcLineBuf[1] == ':') {
                                    nfcReader = nfcLineBuf[0] - '0';
                                    nfcHex = nfcLineBuf + 2;
                                }

                                int hexLen = (int)strlen(nfcHex);
                                int nfcUidLen = hexLen / 2;
                                uint8_t nfcUid[NFC_UID_MAX_LEN] = {0};
                                if (nfcUidLen >= 4 && nfcUidLen <= NFC_UID_MAX_LEN) {
                                    for (int i = 0; i < nfcUidLen; i++) {
                                        unsigned int byte;
                                        sscanf(nfcHex + i * 2, "%2x", &byte);
                                        nfcUid[i] = (uint8_t)byte;
                                    }

                                    uint8_t nfcStatus, nfcTypeIdx, nfcRarity;
                                    AbilitySlot nfcAbilities[MAX_ABILITIES_PER_UNIT];
                                    // Dedup: skip if this UID is already on the blue team
                                    // Check ALL units (not just active) — dead units still count
                                    bool uidAlreadySpawned = false;
                                    for (int u = 0; u < unitCount; u++) {
                                        if (units[u].team == TEAM_BLUE &&
                                            units[u].nfcUidLen == nfcUidLen &&
                                            memcmp(units[u].nfcUid, nfcUid, nfcUidLen) == 0) {
                                            uidAlreadySpawned = true;
                                            break;
                                        }
                                    }
                                    if (uidAlreadySpawned) {
                                        // Tag still on scanner — ignore
                                    } else if (strcmp(nfcHex, "CA31A80C") == 0 || strcmp(nfcHex, "644477EE") == 0) {
                                        easterEggTimer = 4.0f;
                                    } else if (!nfc_cache_contains(&nfcCache, nfcHex)) {
                                        printf("[NFC] Reader %d: UID %s -> unknown (not in local cache)\n", nfcReader, nfcHex);
                                    } else if (namingUnitIndex < 0 && net_nfc_lookup(serverHost, NET_PORT, nfcUid, nfcUidLen,
                                                       &nfcStatus, &nfcTypeIdx, &nfcRarity, nfcAbilities,
                                                       nfcNameBuf, sizeof(nfcNameBuf)) == 0) {
                                        if (nfcStatus == NFC_STATUS_OK && nfcTypeIdx < unitTypeCount) {
                                            if (SpawnUnit(units, &unitCount, nfcTypeIdx, TEAM_BLUE)) {
                                                PlaySound(sfxNewCharacter);
                                                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
                                                    units[unitCount - 1].abilities[a] = nfcAbilities[a];
                                                memcpy(units[unitCount - 1].nfcUid, nfcUid, nfcUidLen);
                                                units[unitCount - 1].nfcUidLen = nfcUidLen;
                                                units[unitCount - 1].rarity = nfcRarity;
                                                strncpy(units[unitCount - 1].nfcName, nfcNameBuf, sizeof(units[unitCount - 1].nfcName) - 1);
                                                ApplyUnitRarity(&units[unitCount - 1]);
                                                printf("[NFC] Reader %d: UID %s -> Spawning %s name=\"%s\" (rarity=%d)\n",
                                                    nfcReader, nfcHex, unitTypes[nfcTypeIdx].name, nfcNameBuf, nfcRarity);
                                                // If unnamed, prompt for name first (intro plays after)
                                                if (nfcNameBuf[0] == '\0') {
                                                    namingUnitIndex = unitCount - 1;
                                                    namingBuf[0] = '\0';
                                                    namingPos = 0;
                                                } else {
                                                    intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                                        .typeIndex = nfcTypeIdx, .unitIndex = unitCount - 1, .animFrame = 0 };
                                                }
                                            } else {
                                                printf("[NFC] Reader %d: UID %s -> Blue team full\n", nfcReader, nfcHex);
                                            }
                                        } else if (nfcStatus == NFC_STATUS_NOT_FOUND) {
                                            printf("[NFC] Reader %d: UID %s -> not registered on server\n", nfcReader, nfcHex);
                                        }
                                    } else {
                                        printf("[NFC] Reader %d: UID %s -> server connection failed\n", nfcReader, nfcHex);
                                    }
                                } else {
                                    printf("[NFC] Invalid hex UID: '%s'\n", nfcLineBuf);
                                }
                            }
                            nfcLinePos = 0;
                        }
                    } else if (nfcLinePos < (int)sizeof(nfcLineBuf) - 1) {
                        nfcLineBuf[nfcLinePos++] = c;
                    }
                }
            }
        }

        // NFC debug input handling (shared for plaza + prep)
        if (debugMode && (phase == PHASE_PLAZA || phase == PHASE_PREP)) {
            // NFC input error timer countdown
            if (nfcInputErrorTimer > 0.0f) {
                nfcInputErrorTimer -= dt;
                if (nfcInputErrorTimer <= 0.0f) nfcInputError[0] = '\0';
            }

            // NFC emulation text input handling
            if (nfcInputActive && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE) {
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
                            PlaySound(sfxNewCharacter);
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
        }

        //------------------------------------------------------------------------------
        // PHASE: PLAZA — 3D plaza with roaming enemies, interactive objects
        //------------------------------------------------------------------------------
        if (phase == PHASE_PLAZA)
        {
            // Update plaza sub-states
            if (plazaState == PLAZA_ROAMING) {
                PlazaUpdateRoaming(units, unitCount, plazaData, dt);
            } else if (plazaState == PLAZA_SCARED) {
                plazaTimer -= dt;
                if (plazaTimer <= 0.0f) {
                    plazaState = PLAZA_FLEEING;
                }
            } else if (plazaState == PLAZA_FLEEING) {
                bool allGone = PlazaUpdateFlee(units, unitCount, plazaData, particles, dt);
                if (allGone) {
                    // All enemies fled — initialize game state and transition to prep
                    ClearRedUnits(units, &unitCount);
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
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    statueSpawn.phase = SSPAWN_INACTIVE;
                    playerGold = 25;
                    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                    RollShop(shopSlots, &playerGold, 0);
                    rollCost = rollCostBase;
                    dragState.dragging = false;
                    SpawnWave(units, &unitCount, 0, unitTypeCount);
                    phase = PHASE_PREP;
                }
            }

            // Check 3D object hover
            if (!showLeaderboard && !showMultiplayerPanel) {
                plazaHoverObject = PlazaCheckObjectHover(camera, trophyPos, doorPos);
            } else {
                plazaHoverObject = 0;
            }

            // Click handling for 3D objects and overlays
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                if (showLeaderboard) {
                    // Close button for leaderboard overlay
                    int sw = GetScreenWidth();
                    int sh = GetScreenHeight();
                    Rectangle closeBtn = { (float)(sw/2 + 280), (float)(sh/2 - 250), 40, 40 };
                    if (CheckCollisionPointRec(GetMousePosition(), closeBtn))
                        showLeaderboard = false;
                } else if (showMultiplayerPanel) {
                    // Multiplayer panel click handling
                    int sw = GetScreenWidth();
                    int sh = GetScreenHeight();
                    int panelW = 400, panelH = 300;
                    int panelX = sw/2 - panelW/2;
                    int panelY = sh/2 - panelH/2;
                    Vector2 mouse = GetMousePosition();

                    // Close button
                    Rectangle closeBtn = { (float)(panelX + panelW - 36), (float)(panelY + 4), 32, 32 };
                    if (CheckCollisionPointRec(mouse, closeBtn)) {
                        showMultiplayerPanel = false;
                    }

                    // Name input field
                    Rectangle nameField = { (float)(panelX + 50), (float)(panelY + 60), (float)(panelW - 100), 36 };
                    if (CheckCollisionPointRec(mouse, nameField))
                        nameInputActive = true;
                    else
                        nameInputActive = false;

                    // CREATE LOBBY button
                    Rectangle createBtn = { (float)(panelX + 50), (float)(panelY + 120), (float)(panelW - 100), 40 };
                    if (CheckCollisionPointRec(mouse, createBtn)) {
                        PlaySound(sfxUiClick);
                        menuError[0] = '\0';
                        isMultiplayer = true;
                        playerReady = false;
                        if (net_client_connect(&netClient, serverHost, NET_PORT, NULL, playerName) == 0) {
                            showMultiplayerPanel = false;
                            phase = PHASE_LOBBY;
                        } else {
                            strncpy(menuError, netClient.errorMsg, sizeof(menuError) - 1);
                            isMultiplayer = false;
                        }
                    }

                    // JOIN LOBBY button
                    Rectangle joinBtn = { (float)(panelX + 50), (float)(panelY + 180), (float)(panelW - 100), 40 };
                    if (joinCodeLen == LOBBY_CODE_LEN && CheckCollisionPointRec(mouse, joinBtn)) {
                        PlaySound(sfxUiClick);
                        menuError[0] = '\0';
                        isMultiplayer = true;
                        playerReady = false;
                        if (net_client_connect(&netClient, serverHost, NET_PORT, joinCodeInput, playerName) == 0) {
                            showMultiplayerPanel = false;
                            phase = PHASE_LOBBY;
                        } else {
                            strncpy(menuError, netClient.errorMsg, sizeof(menuError) - 1);
                            isMultiplayer = false;
                        }
                    }
                } else {
                    // 3D object clicks
                    if (plazaHoverObject == 1) {
                        PlaySound(sfxUiClick);
                        // Try fetching global leaderboard, fall back to local
                        Leaderboard serverLb = {0};
                        if (net_leaderboard_fetch(serverHost, NET_PORT, &serverLb) == 0) {
                            leaderboard = serverLb;
                        }
                        showLeaderboard = true;
                        leaderboardScroll = 0;
                    } else if (plazaHoverObject == 2) {
                        PlaySound(sfxUiClick);
                        showMultiplayerPanel = true;
                    }
                }
            }

            // Name input handling (shared for multiplayer panel)
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

            // Multiplayer join code text input
            if (showMultiplayerPanel && !nameInputActive) {
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
                if (IsKeyPressed(KEY_BACKSPACE) && joinCodeLen > 0) {
                    joinCodeLen--;
                    joinCodeInput[joinCodeLen] = '\0';
                }
            }

            // ESC closes overlays
            if (showLeaderboard && IsKeyPressed(KEY_ESCAPE))
                showLeaderboard = false;
            if (showMultiplayerPanel && IsKeyPressed(KEY_ESCAPE))
                showMultiplayerPanel = false;

            // Leaderboard scroll
            if (showLeaderboard) {
                int wheel = (int)GetMouseWheelMove();
                leaderboardScroll -= wheel * 40;
                if (leaderboardScroll < 0) leaderboardScroll = 0;
                int maxScroll = leaderboard.entryCount * 80 - 400;
                if (maxScroll < 0) maxScroll = 0;
                if (leaderboardScroll > maxScroll) leaderboardScroll = maxScroll;
            }

            // Debug spawn buttons click handling during plaza
            if (debugMode && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                !showLeaderboard && !showMultiplayerPanel) {
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();
                int dHudTop = sh - hudTotalH;
                int plazaValidCount = 0;
                for (int i = 0; i < unitTypeCount; i++) if (unitTypes[i].name) plazaValidCount++;
                int btnYStart = dHudTop - (plazaValidCount * (btnHeight + btnMargin)) - btnMargin;

                // NFC input box click check
                {
                    int nfcBoxW = 200, nfcBoxH = 28;
                    int nfcBoxX = sw/2 - nfcBoxW/2;
                    int nfcBoxY = btnYStart - 55;
                    Rectangle nfcRect = { (float)nfcBoxX, (float)nfcBoxY, (float)nfcBoxW, (float)nfcBoxH };
                    if (CheckCollisionPointRec(mouse, nfcRect)) {
                        nfcInputActive = true;
                    } else if (nfcInputActive) {
                        nfcInputActive = false;
                    }
                }

                bool plazaClickedBtn = false;
                int btnXBlue = btnMargin;
                int clickIdx = 0;
                for (int i = 0; i < unitTypeCount; i++) {
                    if (!unitTypes[i].name) continue;
                    Rectangle r = { (float)btnXBlue, (float)(btnYStart + clickIdx*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                    clickIdx++;
                    if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded) {
                        if (SpawnUnit(units, &unitCount, i, TEAM_BLUE)) {
                            PlaySound(sfxNewCharacter);
                            // Place on blue side
                            units[unitCount-1].position.x = (float)GetRandomValue(-50, 50);
                            units[unitCount-1].position.z = (float)GetRandomValue(10, 80);
                            intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                .typeIndex = i, .unitIndex = unitCount - 1, .animFrame = 0 };
                        }
                        plazaClickedBtn = true;
                        break;
                    }
                }
                // Rarity debug buttons (rare + legendary mushroom)
                {
                    int rY = btnYStart + clickIdx * (btnHeight + btnMargin);
                    Rectangle rr = { (float)btnXBlue, (float)rY, (float)btnWidth, (float)btnHeight };
                    if (CheckCollisionPointRec(mouse, rr) && unitTypes[0].loaded) {
                        if (SpawnUnit(units, &unitCount, 0, TEAM_BLUE)) {
                            PlaySound(sfxNewCharacter);
                            units[unitCount-1].rarity = RARITY_RARE;
                            ApplyUnitRarity(&units[unitCount-1]);
                            units[unitCount-1].position.x = (float)GetRandomValue(-50, 50);
                            units[unitCount-1].position.z = (float)GetRandomValue(10, 80);
                            intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                .typeIndex = 0, .unitIndex = unitCount - 1, .animFrame = 0 };
                        }
                    }
                    rY += btnHeight + btnMargin;
                    Rectangle lr = { (float)btnXBlue, (float)rY, (float)btnWidth, (float)btnHeight };
                    if (CheckCollisionPointRec(mouse, lr) && unitTypes[0].loaded) {
                        if (SpawnUnit(units, &unitCount, 0, TEAM_BLUE)) {
                            PlaySound(sfxNewCharacter);
                            units[unitCount-1].rarity = RARITY_LEGENDARY;
                            ApplyUnitRarity(&units[unitCount-1]);
                            units[unitCount-1].position.x = (float)GetRandomValue(-50, 50);
                            units[unitCount-1].position.z = (float)GetRandomValue(10, 80);
                            intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                .typeIndex = 0, .unitIndex = unitCount - 1, .animFrame = 0 };
                        }
                    }
                }

                // Env piece spawn + save buttons (plaza debug)
                if (!plazaClickedBtn) {
                    int envBtnW = 110, envBtnH = 24, envBtnGap = 4;
                    int envColX = sw / 2 - envBtnW / 2;
                    int envStartY = btnYStart;
                    for (int ei = 0; ei < envModelCount; ei++) {
                        if (!envModels[ei].loaded) continue;
                        Rectangle er = { (float)envColX, (float)(envStartY + ei * (envBtnH + envBtnGap)),
                                         (float)envBtnW, (float)envBtnH };
                        if (CheckCollisionPointRec(mouse, er) && envPieceCount < MAX_ENV_PIECES) {
                            envPieces[envPieceCount] = (EnvPiece){
                                .modelIndex = ei, .position = {0, 0, 0},
                                .rotationY = 0, .scale = 1.0f, .active = true
                            };
                            envSelectedPiece = envPieceCount;
                            envPieceCount++;
                            plazaClickedBtn = true;
                            break;
                        }
                    }
                    if (!plazaClickedBtn) {
                        int saveY = envStartY + envModelCount * (envBtnH + envBtnGap) + 4;
                        Rectangle saveBtn = { (float)envColX, (float)saveY, (float)envBtnW, (float)envBtnH };
                        if (CheckCollisionPointRec(mouse, saveBtn)) {
                            FILE *fp = fopen("env_layout.txt", "w");
                            if (fp) {
                                fprintf(fp, "# modelIndex x y z rotationY scale\n");
                                for (int pi = 0; pi < envPieceCount; pi++) {
                                    if (!envPieces[pi].active) continue;
                                    fprintf(fp, "%d %.1f %.1f %.1f %.1f %.1f\n",
                                            envPieces[pi].modelIndex,
                                            envPieces[pi].position.x, envPieces[pi].position.y,
                                            envPieces[pi].position.z,
                                            envPieces[pi].rotationY, envPieces[pi].scale);
                                }
                                fclose(fp);
                                envSaveFlashTimer = 2.0f;
                            }
                        }
                    }
                }

                // Env piece 3D picking (plaza, debug mode)
                if (!plazaClickedBtn) {
                    int dHudTop2 = sh - hudTotalH;
                    if (mouse.y < dHudTop2) {
                        Ray envRay = GetScreenToWorldRay(mouse, camera);
                        float closestDist = 1e9f;
                        int closestIdx = -1;
                        for (int ep = 0; ep < envPieceCount; ep++) {
                            if (!envPieces[ep].active) continue;
                            EnvModelDef *emd = &envModels[envPieces[ep].modelIndex];
                            if (!emd->loaded || emd->model.meshCount == 0) continue;
                            BoundingBox mbb = GetMeshBoundingBox(emd->model.meshes[0]);
                            Matrix mt = emd->model.transform;
                            Vector3 corners[8] = {
                                {mbb.min.x, mbb.min.y, mbb.min.z}, {mbb.max.x, mbb.min.y, mbb.min.z},
                                {mbb.min.x, mbb.max.y, mbb.min.z}, {mbb.max.x, mbb.max.y, mbb.min.z},
                                {mbb.min.x, mbb.min.y, mbb.max.z}, {mbb.max.x, mbb.min.y, mbb.max.z},
                                {mbb.min.x, mbb.max.y, mbb.max.z}, {mbb.max.x, mbb.max.y, mbb.max.z},
                            };
                            BoundingBox tbb = { .min = {1e9f, 1e9f, 1e9f}, .max = {-1e9f, -1e9f, -1e9f} };
                            for (int ci = 0; ci < 8; ci++) {
                                Vector3 tc = Vector3Transform(corners[ci], mt);
                                if (tc.x < tbb.min.x) tbb.min.x = tc.x;
                                if (tc.y < tbb.min.y) tbb.min.y = tc.y;
                                if (tc.z < tbb.min.z) tbb.min.z = tc.z;
                                if (tc.x > tbb.max.x) tbb.max.x = tc.x;
                                if (tc.y > tbb.max.y) tbb.max.y = tc.y;
                                if (tc.z > tbb.max.z) tbb.max.z = tc.z;
                            }
                            float ps = envPieces[ep].scale;
                            BoundingBox wbb = {
                                .min = { tbb.min.x * ps + envPieces[ep].position.x,
                                         tbb.min.y * ps + envPieces[ep].position.y,
                                         tbb.min.z * ps + envPieces[ep].position.z },
                                .max = { tbb.max.x * ps + envPieces[ep].position.x,
                                         tbb.max.y * ps + envPieces[ep].position.y,
                                         tbb.max.z * ps + envPieces[ep].position.z }
                            };
                            RayCollision rc = GetRayCollisionBox(envRay, wbb);
                            if (rc.hit && rc.distance < closestDist) {
                                closestDist = rc.distance;
                                closestIdx = ep;
                            }
                        }
                        envSelectedPiece = closestIdx;
                        envDragging = (closestIdx >= 0);
                    }
                }
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
                unitCount = 0;
                memset(plazaData, 0, sizeof(plazaData));
                PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);
                plazaState = PLAZA_ROAMING;
                phase = PHASE_PLAZA;
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
                // Reset multiplayer game state — keep blue units from plaza
                ClearRedUnits(units, &unitCount);
                snapshotCount = 0;
                blueWins = 0;
                redWins = 0;
                roundResultText = "";
                ClearAllModifiers(modifiers);
                ClearAllProjectiles(projectiles);
                ClearAllParticles(particles);
                ClearAllFloatingTexts(floatingTexts);
                ClearAllFissures(fissures);
                dragState.dragging = false;
                playerReady = false;
                waitingForOpponent = false;
                phase = PHASE_PREP;
            }

            if (IsKeyPressed(KEY_ESCAPE)) {
                net_client_disconnect(&netClient);
                isMultiplayer = false;
                unitCount = 0;
                memset(plazaData, 0, sizeof(plazaData));
                PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);
                plazaState = PLAZA_ROAMING;
                phase = PHASE_PLAZA;
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
                    unitCount = 0;
                    memset(plazaData, 0, sizeof(plazaData));
                    PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);
                    plazaState = PLAZA_ROAMING;
                    phase = PHASE_PLAZA;
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
                    // Save NFC UID data from blue units before server overwrite
                    // (NetUnit doesn't carry NFC fields, so deserialize loses them)
                    // Saved in order — server preserves blue unit ordering.
                    struct { uint8_t uid[7]; int uidLen; char name[32]; } nfcSave[MAX_UNITS];
                    int nfcSaveCount = 0;
                    for (int i = 0; i < unitCount; i++) {
                        if (units[i].active && units[i].team == TEAM_BLUE) {
                            if (units[i].nfcUidLen > 0) {
                                memcpy(nfcSave[nfcSaveCount].uid, units[i].nfcUid, sizeof(units[i].nfcUid));
                                nfcSave[nfcSaveCount].uidLen = units[i].nfcUidLen;
                                memcpy(nfcSave[nfcSaveCount].name, units[i].nfcName, sizeof(units[i].nfcName));
                            } else {
                                nfcSave[nfcSaveCount].uidLen = 0;
                            }
                            nfcSaveCount++;
                        }
                    }
                    unitCount = deserialize_units(netClient.combatNetUnits,
                        netClient.combatNetUnitCount, units, MAX_UNITS);
                    // Re-apply NFC UIDs to blue units by order (N-th blue = N-th saved)
                    int blueIdx = 0;
                    for (int i = 0; i < unitCount && blueIdx < nfcSaveCount; i++) {
                        if (units[i].team != TEAM_BLUE) continue;
                        if (nfcSave[blueIdx].uidLen > 0) {
                            memcpy(units[i].nfcUid, nfcSave[blueIdx].uid, sizeof(units[i].nfcUid));
                            units[i].nfcUidLen = nfcSave[blueIdx].uidLen;
                            memcpy(units[i].nfcName, nfcSave[blueIdx].name, sizeof(units[i].nfcName));
                        }
                        blueIdx++;
                    }
                    ApplyRarityBuffs(units, unitCount);
                    SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
                    // Sync NFC-tagged units' abilities to server before combat
                    for (int u2 = 0; u2 < unitCount; u2++) {
                        if (units[u2].active && units[u2].team == TEAM_BLUE && units[u2].nfcUidLen > 0) {
                            net_nfc_update_abilities(serverHost, NET_PORT,
                                units[u2].nfcUid, units[u2].nfcUidLen,
                                units[u2].abilities, MAX_ABILITIES_PER_UNIT);
                        }
                    }
                    ApplySynergies(units, unitCount);
                    phase = PHASE_COMBAT;
                    fightBannerTimer = 0.0f;
                    killCount = 0; multiKillCount = 0; multiKillTimer = 0.0f; killFeedTimer = -1.0f;
                    slowmoTimer = 0.0f; slowmoScale = 1.0f;
                    BattleLogClear(&battleLog); combatElapsedTime = 0.0f;
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
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) { PlaySound(sfxUiDrop); units[i].dragging = false; }
            }

            // Quick-buy: keys 1, 2, 3 for shop slots
            if (!(isMultiplayer && playerReady) && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE && !nfcInputActive) {
                int quickBuyKeys[3] = { KEY_ONE, KEY_TWO, KEY_THREE };
                for (int s = 0; s < MAX_SHOP_SLOTS; s++) {
                    if (IsKeyPressed(quickBuyKeys[s]) && shopSlots[s].abilityId >= 0) {
                        usedShopHotkey = true;
                        if (isMultiplayer) {
                            net_client_send_buy(&netClient, s);
                            // Also process locally so ability appears immediately
                            BuyAbility(&shopSlots[s], inventory, units, unitCount, &playerGold);
                        } else {
                            // Check if a selected blue unit has an empty ability slot
                            int selUnit = -1;
                            for (int i = 0; i < unitCount; i++) {
                                if (units[i].active && units[i].team == TEAM_BLUE && units[i].selected) {
                                    selUnit = i;
                                    break;
                                }
                            }
                            if (selUnit >= 0) {
                                int cost = ABILITY_DEFS[shopSlots[s].abilityId].goldCost;
                                if (playerGold >= cost) {
                                    bool placed = false;
                                    // Check for upgrade first (same ability on unit)
                                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                        if (units[selUnit].abilities[a].abilityId == shopSlots[s].abilityId &&
                                            units[selUnit].abilities[a].level < ABILITY_MAX_LEVELS - 1) {
                                            units[selUnit].abilities[a].level++;
                                            playerGold -= cost;
                                            shopSlots[s].abilityId = -1;
                                            placed = true;
                                            break;
                                        }
                                    }
                                    // Otherwise empty slot on selected unit
                                    if (!placed) {
                                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                                            if (units[selUnit].abilities[a].abilityId < 0) {
                                                units[selUnit].abilities[a].abilityId = shopSlots[s].abilityId;
                                                units[selUnit].abilities[a].level = shopSlots[s].level;
                                                playerGold -= cost;
                                                shopSlots[s].abilityId = -1;
                                                placed = true;
                                                break;
                                            }
                                        }
                                    }
                                    // Unit full — fall back to inventory
                                    if (!placed) {
                                        BuyAbility(&shopSlots[s], inventory, units, unitCount, &playerGold);
                                    }
                                    if (placed || shopSlots[s].abilityId < 0) PlaySound(sfxUiBuy);
                                }
                            } else {
                                // No unit selected — normal buy (auto-combine / inventory)
                                BuyAbility(&shopSlots[s], inventory, units, unitCount, &playerGold);
                            }
                        }
                        break;
                    }
                }
            }

            // Quick-roll: R key
            if (!(isMultiplayer && playerReady) && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE && !nfcInputActive) {
                if (IsKeyPressed(KEY_R) && playerGold >= rollCost) {
                    usedRollHotkey = true;
                    PlaySound(sfxUiReroll);
                    if (isMultiplayer) {
                        net_client_send_roll(&netClient);
                    } else {
                        RollShop(shopSlots, &playerGold, rollCost);
                    }
                    rollCost += rollCostIncrement;
                    TriggerShake(&shake, 2.0f, 0.15f);
                }
            }

            // Clicks (blocked during intro)
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE)
            {
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();
                int hudTop = sh - hudTotalH;
                int btnXBlue = btnMargin;
                int btnXRed  = sw - btnWidth - btnMargin;
                int prepValidCount = 0;
                for (int i = 0; i < unitTypeCount; i++) if (unitTypes[i].name) prepValidCount++;
                int btnYStart = hudTop - (prepValidCount * (btnHeight + btnMargin)) - btnMargin;
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
                    int popW = 280, popH = 110;
                    int popX = sw / 2 - popW / 2;
                    int popY = sh / 2 - popH / 2;
                    int rmBtnW = 100, rmBtnH = 30;
                    Rectangle yesBtn = { (float)(popX + 24), (float)(popY + popH - rmBtnH - 12), (float)rmBtnW, (float)rmBtnH };
                    Rectangle noBtn  = { (float)(popX + popW - rmBtnW - 24), (float)(popY + popH - rmBtnH - 12), (float)rmBtnW, (float)rmBtnH };
                    if (CheckCollisionPointRec(mouse, yesBtn)) {
                        // Remove the unit: sync NFC abilities to server, deactivate
                        // Abilities stay on the figurine (server-side), NOT returned to inventory
                        // (returning to inventory would be a duplication glitch)
                        int ri = removeConfirmUnit;
                        if (units[ri].nfcUidLen > 0) {
                            net_nfc_update_abilities(serverHost, NET_PORT,
                                units[ri].nfcUid, units[ri].nfcUidLen,
                                units[ri].abilities, MAX_ABILITIES_PER_UNIT);
                            units[ri].nfcUidLen = 0;
                        }
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
                            units[ri].abilities[a].abilityId = -1;
                        units[ri].active = false;
                        removeConfirmUnit = -1;
                        clickedButton = true;
                        // Check if all blue units removed → return to plaza
                        int blueLeft = CountTeamUnits(units, unitCount, TEAM_BLUE);
                        if (blueLeft == 0) {
                            ClearRedUnits(units, &unitCount);
                            CompactBlueUnits(units, &unitCount);
                            memset(plazaData, 0, sizeof(plazaData));
                            PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);
                            plazaState = PLAZA_ROAMING;
                            phase = PHASE_PLAZA;
                        }
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
                    PlaySound(sfxUiClick);
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
                            CompactBlueUnits(units, &unitCount);
                            SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
                            // Sync NFC-tagged units' abilities to server before combat
                            for (int u2 = 0; u2 < unitCount; u2++) {
                                if (units[u2].active && units[u2].team == TEAM_BLUE && units[u2].nfcUidLen > 0) {
                                    net_nfc_update_abilities(serverHost, NET_PORT,
                                        units[u2].nfcUid, units[u2].nfcUidLen,
                                        units[u2].abilities, MAX_ABILITIES_PER_UNIT);
                                }
                            }
                            ApplySynergies(units, unitCount);
                            phase = PHASE_COMBAT;
                            fightBannerTimer = 0.0f;
                            killCount = 0; multiKillCount = 0; multiKillTimer = 0.0f; killFeedTimer = -1.0f;
                            slowmoTimer = 0.0f; slowmoScale = 1.0f;
                            BattleLogClear(&battleLog); combatElapsedTime = 0.0f;
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
                    int ci = 0;
                    for (int i = 0; i < unitTypeCount; i++)
                    {
                        if (!unitTypes[i].name) continue;
                        Rectangle r = { (float)btnXBlue, (float)(btnYStart + ci*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                        ci++;
                        if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded)
                        {
                            if (SpawnUnit(units, &unitCount, i, TEAM_BLUE)) {
                                PlaySound(sfxNewCharacter);
                                intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                    .typeIndex = i, .unitIndex = unitCount - 1, .animFrame = 0 };
                            }
                            clickedButton = true; break;
                        }
                    }
                    // Rarity debug buttons (rare + legendary mushroom)
                    if (!clickedButton) {
                        int rY = btnYStart + ci * (btnHeight + btnMargin);
                        Rectangle rr = { (float)btnXBlue, (float)rY, (float)btnWidth, (float)btnHeight };
                        if (CheckCollisionPointRec(mouse, rr) && unitTypes[0].loaded) {
                            if (SpawnUnit(units, &unitCount, 0, TEAM_BLUE)) {
                                PlaySound(sfxNewCharacter);
                                units[unitCount-1].rarity = RARITY_RARE;
                                ApplyUnitRarity(&units[unitCount-1]);
                                intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                    .typeIndex = 0, .unitIndex = unitCount - 1, .animFrame = 0 };
                            }
                            clickedButton = true;
                        }
                        rY += btnHeight + btnMargin;
                        Rectangle lr = { (float)btnXBlue, (float)rY, (float)btnWidth, (float)btnHeight };
                        if (!clickedButton && CheckCollisionPointRec(mouse, lr) && unitTypes[0].loaded) {
                            if (SpawnUnit(units, &unitCount, 0, TEAM_BLUE)) {
                                PlaySound(sfxNewCharacter);
                                units[unitCount-1].rarity = RARITY_LEGENDARY;
                                ApplyUnitRarity(&units[unitCount-1]);
                                intro = (UnitIntro){ .active = true, .timer = 0.0f,
                                    .typeIndex = 0, .unitIndex = unitCount - 1, .animFrame = 0 };
                            }
                            clickedButton = true;
                        }
                    }
                }
                // Red spawn buttons (debug only)
                if (!clickedButton && debugMode)
                {
                    int ci = 0;
                    for (int i = 0; i < unitTypeCount; i++)
                    {
                        if (!unitTypes[i].name) continue;
                        Rectangle r = { (float)btnXRed, (float)(btnYStart + ci*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                        ci++;
                        if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded) {
                            if (SpawnUnit(units, &unitCount, i, TEAM_RED))
                                AssignRandomAbilities(&units[unitCount-1], GetRandomValue(1, 2));
                            clickedButton = true; break;
                        }
                    }
                }
                // Env piece spawn + save buttons (debug only)
                if (!clickedButton && debugMode)
                {
                    int envBtnW = 110, envBtnH = 24, envBtnGap = 4;
                    int envColX = sw / 2 - envBtnW / 2;
                    int envStartY = btnYStart;
                    for (int ei = 0; ei < envModelCount; ei++) {
                        if (!envModels[ei].loaded) continue;
                        Rectangle er = { (float)envColX, (float)(envStartY + ei * (envBtnH + envBtnGap)),
                                         (float)envBtnW, (float)envBtnH };
                        if (CheckCollisionPointRec(mouse, er) && envPieceCount < MAX_ENV_PIECES) {
                            envPieces[envPieceCount] = (EnvPiece){
                                .modelIndex = ei, .position = {0, 0, 0},
                                .rotationY = 0, .scale = 1.0f, .active = true
                            };
                            envSelectedPiece = envPieceCount;
                            envPieceCount++;
                            clickedButton = true;
                            break;
                        }
                    }
                    if (!clickedButton) {
                        int saveY = envStartY + envModelCount * (envBtnH + envBtnGap) + 4;
                        Rectangle saveBtn = { (float)envColX, (float)saveY, (float)envBtnW, (float)envBtnH };
                        if (CheckCollisionPointRec(mouse, saveBtn)) {
                            FILE *fp = fopen("env_layout.txt", "w");
                            if (fp) {
                                fprintf(fp, "# modelIndex x y z rotationY scale\n");
                                for (int pi = 0; pi < envPieceCount; pi++) {
                                    if (!envPieces[pi].active) continue;
                                    fprintf(fp, "%d %.1f %.1f %.1f %.1f %.1f\n",
                                            envPieces[pi].modelIndex,
                                            envPieces[pi].position.x, envPieces[pi].position.y,
                                            envPieces[pi].position.z,
                                            envPieces[pi].rotationY, envPieces[pi].scale);
                                }
                                fclose(fp);
                                envSaveFlashTimer = 2.0f;
                            }
                            clickedButton = true;
                        }
                    }
                }
                // --- Shop: ROLL button click ---
                if (!clickedButton && !(isMultiplayer && playerReady)) {
                    int shopY = hudTop + 2;
                    Rectangle rollBtn = { 20, (float)(shopY + 10), S(90), S(34) };
                    if (CheckCollisionPointRec(mouse, rollBtn) && playerGold >= rollCost) {
                        PlaySound(sfxUiReroll);
                        if (isMultiplayer) {
                            net_client_send_roll(&netClient);
                        } else {
                            RollShop(shopSlots, &playerGold, rollCost);
                        }
                        rollCost += rollCostIncrement;
                        TriggerShake(&shake, 2.0f, 0.15f);
                        clickedButton = true;
                    }
                }
                // --- Shop: Buy ability card click ---
                if (!clickedButton && !(isMultiplayer && playerReady)) {
                    int shopY = hudTop + 2;
                    int shopCardW = S(160), shopCardH = S(38), shopCardGap = 10;
                    int totalShopW = MAX_SHOP_SLOTS * shopCardW + (MAX_SHOP_SLOTS - 1) * shopCardGap;
                    int shopCardsX = (sw - totalShopW) / 2;
                    for (int s = 0; s < MAX_SHOP_SLOTS; s++) {
                        int scx = shopCardsX + s * (shopCardW + shopCardGap);
                        Rectangle r = { (float)scx, (float)(shopY + 8), (float)shopCardW, (float)shopCardH };
                        if (CheckCollisionPointRec(mouse, r) && shopSlots[s].abilityId >= 0) {
                            PlaySound(sfxUiBuy);
                            if (isMultiplayer) {
                                net_client_send_buy(&netClient, s);
                                // Also process locally so ability appears in inventory immediately
                                BuyAbility(&shopSlots[s], inventory, units, unitCount, &playerGold);
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
                    int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW + (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
                    int cardsStartX = (sw - totalCardsW) / 2;
                    int invStartX = cardsStartX - (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
                    int invStartY = hudTop + hudShopH + 15;
                    for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
                        int col = inv % HUD_INVENTORY_COLS;
                        int row = inv / HUD_INVENTORY_COLS;
                        int ix = invStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
                        int iy = invStartY + row * (hudAbilSlotSize + hudAbilSlotGap);
                        Rectangle r = { (float)ix, (float)iy, (float)hudAbilSlotSize, (float)hudAbilSlotSize };
                        if (CheckCollisionPointRec(mouse, r) && inventory[inv].abilityId >= 0) {
                            PlaySound(sfxUiDrag);
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
                    int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW + (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
                    int cardsStartX = (sw - totalCardsW) / 2;
                    int cardsY = hudTop + hudShopH + 5;
                    for (int h = 0; h < tmpCount && !clickedButton; h++) {
                        int cardX = cardsStartX + h * (hudCardW + hudCardSpacing);
                        int abilStartX = cardX + hudPortraitSize + 12;
                        int abilStartY = cardsY + 8;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            int col = a % 2, row = a / 2;
                            int ax = abilStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
                            int ay = abilStartY + row * (hudAbilSlotSize + hudAbilSlotGap);
                            Rectangle r = { (float)ax, (float)ay, (float)hudAbilSlotSize, (float)hudAbilSlotSize };
                            int ui = tmpBlue[h];
                            if (CheckCollisionPointRec(mouse, r) && units[ui].abilities[a].abilityId >= 0) {
                                PlaySound(sfxUiDrag);
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
                    int totalCardsW2 = BLUE_TEAM_MAX_SIZE * hudCardW + (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
                    int cardsStartX2 = (sw - totalCardsW2) / 2;
                    int cardsY2 = hudTop + hudShopH + 5;
                    for (int h = 0; h < tmpCount2; h++) {
                        int cardX = cardsStartX2 + h * (hudCardW + hudCardSpacing);
                        int xBtnSize = S(18);
                        Rectangle xBtn = { (float)(cardX + hudCardW - xBtnSize - 2),
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
                            PlaySound(sfxUiDrag);
                            units[i].selected = true;
                            units[i].dragging = true;
                            hitAny = true;
                            for (int j = 0; j < unitCount; j++) if (j != i) units[j].selected = false;
                            break;
                        }
                    }
                    if (!hitAny) for (int j = 0; j < unitCount; j++) units[j].selected = false;

                    // Env piece 3D picking (debug mode, only if no unit was hit)
                    if (!hitAny && debugMode) {
                        Ray envRay = GetScreenToWorldRay(mouse, camera);
                        float closestDist = 1e9f;
                        int closestIdx = -1;
                        for (int ep = 0; ep < envPieceCount; ep++) {
                            if (!envPieces[ep].active) continue;
                            EnvModelDef *emd = &envModels[envPieces[ep].modelIndex];
                            if (!emd->loaded || emd->model.meshCount == 0) continue;
                            // Compute AABB by transforming all 8 corners through model transform
                            BoundingBox mbb = GetMeshBoundingBox(emd->model.meshes[0]);
                            Matrix mt = emd->model.transform;
                            Vector3 corners[8] = {
                                {mbb.min.x, mbb.min.y, mbb.min.z}, {mbb.max.x, mbb.min.y, mbb.min.z},
                                {mbb.min.x, mbb.max.y, mbb.min.z}, {mbb.max.x, mbb.max.y, mbb.min.z},
                                {mbb.min.x, mbb.min.y, mbb.max.z}, {mbb.max.x, mbb.min.y, mbb.max.z},
                                {mbb.min.x, mbb.max.y, mbb.max.z}, {mbb.max.x, mbb.max.y, mbb.max.z},
                            };
                            BoundingBox tbb = { .min = {1e9f, 1e9f, 1e9f}, .max = {-1e9f, -1e9f, -1e9f} };
                            for (int ci = 0; ci < 8; ci++) {
                                Vector3 tc = Vector3Transform(corners[ci], mt);
                                if (tc.x < tbb.min.x) tbb.min.x = tc.x;
                                if (tc.y < tbb.min.y) tbb.min.y = tc.y;
                                if (tc.z < tbb.min.z) tbb.min.z = tc.z;
                                if (tc.x > tbb.max.x) tbb.max.x = tc.x;
                                if (tc.y > tbb.max.y) tbb.max.y = tc.y;
                                if (tc.z > tbb.max.z) tbb.max.z = tc.z;
                            }
                            float ps = envPieces[ep].scale;
                            BoundingBox wbb = {
                                .min = { tbb.min.x * ps + envPieces[ep].position.x,
                                         tbb.min.y * ps + envPieces[ep].position.y,
                                         tbb.min.z * ps + envPieces[ep].position.z },
                                .max = { tbb.max.x * ps + envPieces[ep].position.x,
                                         tbb.max.y * ps + envPieces[ep].position.y,
                                         tbb.max.z * ps + envPieces[ep].position.z }
                            };
                            RayCollision rc = GetRayCollisionBox(envRay, wbb);
                            if (rc.hit && rc.distance < closestDist) {
                                closestDist = rc.distance;
                                closestIdx = ep;
                            }
                        }
                        envSelectedPiece = closestIdx;
                        envDragging = (closestIdx >= 0);
                    }
                }
            }

            // --- Drag-and-drop release handling ---
            if (dragState.dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && !intro.active && statueSpawn.phase == SSPAWN_INACTIVE)
            {
                PlaySound(sfxUiDrop);
                Vector2 mouse = GetMousePosition();
                int sw = GetScreenWidth();
                int sh = GetScreenHeight();
                int hudTop2 = sh - hudTotalH;
                bool placed = false;

                // Collect blue units
                int dropBlue[BLUE_TEAM_MAX_SIZE]; int dropCount = 0;
                for (int i2 = 0; i2 < unitCount && dropCount < BLUE_TEAM_MAX_SIZE; i2++)
                    if (units[i2].active && units[i2].team == TEAM_BLUE) dropBlue[dropCount++] = i2;

                int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW + (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
                int cardsStartX = (sw - totalCardsW) / 2;
                int cardsY = hudTop2 + hudShopH + 5;

                // Check drop on unit ability slot
                for (int h = 0; h < dropCount && !placed; h++) {
                    int cardX = cardsStartX + h * (hudCardW + hudCardSpacing);
                    int abilStartX = cardX + hudPortraitSize + 12;
                    int abilStartY = cardsY + 8;
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        int col = a % 2, row = a / 2;
                        int ax = abilStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
                        int ay = abilStartY + row * (hudAbilSlotSize + hudAbilSlotGap);
                        Rectangle r = { (float)ax, (float)ay, (float)hudAbilSlotSize, (float)hudAbilSlotSize };
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
                    int invStartX = cardsStartX - (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
                    int invStartY = hudTop2 + hudShopH + 15;
                    for (int inv = 0; inv < MAX_INVENTORY_SLOTS && !placed; inv++) {
                        int col = inv % HUD_INVENTORY_COLS;
                        int row = inv / HUD_INVENTORY_COLS;
                        int ix = invStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
                        int iy = invStartY + row * (hudAbilSlotSize + hudAbilSlotGap);
                        Rectangle r = { (float)ix, (float)iy, (float)hudAbilSlotSize, (float)hudAbilSlotSize };
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
                // Check drop on sell zone
                if (!placed && dragState.abilityId >= 0 && dragState.abilityId < ABILITY_COUNT) {
                    int invGridW = HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap);
                    int sellInvX = cardsStartX - invGridW - 20;
                    int szSize = 2 * hudAbilSlotSize + hudAbilSlotGap;
                    int szX = sellInvX - szSize - S(10);
                    int szY = cardsY + S(18);
                    Rectangle sellRect = { (float)szX, (float)szY, (float)szSize, (float)szSize };
                    if (CheckCollisionPointRec(mouse, sellRect)) {
                        int sellValue = ABILITY_DEFS[dragState.abilityId].goldCost / 2;
                        if (sellValue < 1) sellValue = 1;
                        playerGold += sellValue;
                        PlaySound(sfxUiBuy);
                        placed = true;
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
            combatElapsedTime += dt;

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
                // Charge-up phase: stay in place and grow
                if (projectiles[p].chargeTimer > 0) {
                    projectiles[p].chargeTimer -= dt;
                    if (projectiles[p].chargeTimer > 0) continue;
                    PlaySound(sfxProjectileWhoosh);
                }
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
                    PlaySound(sfxProjectileHit);
                    // Impact explosion particles + tile shake
                    {
                        Vector3 impactPos = projectiles[p].position;
                        for (int ep = 0; ep < PROJ_EXPLODE_COUNT; ep++) {
                            float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
                            float spd = (float)GetRandomValue(100, 250) / 10.0f;
                            Vector3 ev = {
                                cosf(angle) * spd,
                                (float)GetRandomValue(40, 150) / 10.0f,
                                sinf(angle) * spd,
                            };
                            SpawnParticle(particles, impactPos, ev, 0.7f,
                                (float)GetRandomValue(70, 130) / 10.0f, projectiles[p].color);
                        }
                        TriggerShake(&shake, 4.0f, 0.2f);
                        // Tile wobble ripple from impact
                        float gridOriginImp = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                        for (int tr = 0; tr < TILE_GRID_SIZE; tr++) {
                            for (int tc = 0; tc < TILE_GRID_SIZE; tc++) {
                                float cx = gridOriginImp + (tc + 0.5f) * TILE_WORLD_SIZE;
                                float cz = gridOriginImp + (tr + 0.5f) * TILE_WORLD_SIZE;
                                float dxw = cx - impactPos.x, dzw = cz - impactPos.z;
                                float dist = sqrtf(dxw*dxw + dzw*dzw);
                                float wobbleR = 50.0f;
                                if (dist < wobbleR) {
                                    float strength = expf(-2.0f * dist / wobbleR);
                                    if (tileWobble[tr][tc] < TILE_WOBBLE_MAX * 0.5f * strength) {
                                        tileWobble[tr][tc] = TILE_WOBBLE_MAX * 0.5f * strength;
                                        tileWobbleTime[tr][tc] = -(dist * 0.008f);
                                        float len = dist > 0.1f ? dist : 1.0f;
                                        tileWobbleDirX[tr][tc] = dzw / len;
                                        tileWobbleDirZ[tr][tc] = -dxw / len;
                                    }
                                }
                            }
                        }
                    }
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
                            SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg, true);

                            // Teleport target to caster
                            units[ti].position.x = units[projectiles[p].sourceIndex].position.x;
                            units[ti].position.z = units[projectiles[p].sourceIndex].position.z;
                            TriggerShake(&shake, 6.0f, 0.3f);
                            if (units[ti].currentHealth <= 0) {
                                PlaySound(units[ti].typeIndex == 0 ? sfxToadDie : sfxGoblinDie);
                                SpawnDeathExplosion(particles, units[ti].position, units[ti].team);
                                TriggerShake(&shake, 6.0f, 0.3f);

                                // Kill feed
                                { Team killerTeam = (units[ti].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                                if (killerTeam != lastKillTeam) multiKillCount = 0;
                                lastKillTeam = killerTeam; }
                                killCount++; multiKillCount++; multiKillTimer = 2.0f;
                                if (killCount == 1) { snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 2) { snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 3) { snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount >= 4) { snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!"); killFeedTimer = 0.0f; killFeedScale = 2.5f; }
                                // Slow-mo check: is this the last unit on a team?
                                int ba2, ra2; CountTeams(units, unitCount, &ba2, &ra2);
                                if (ba2 == 0 || ra2 == 0) { slowmoTimer = 0.5f; slowmoScale = 0.3f; }
                                BattleLogAddKill(&battleLog, combatElapsedTime, units[projectiles[p].sourceIndex].team, units[projectiles[p].sourceIndex].typeIndex, units[ti].team, units[ti].typeIndex, ABILITY_HOOK);
                                units[ti].active = false;
                            }
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
                            SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg, true);

                            if (units[ti].currentHealth <= 0) {
                                PlaySound(units[ti].typeIndex == 0 ? sfxToadDie : sfxGoblinDie);
                                SpawnDeathExplosion(particles, units[ti].position, units[ti].team);
                                TriggerShake(&shake, 6.0f, 0.3f);

                                { Team killerTeam = (units[ti].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                                if (killerTeam != lastKillTeam) multiKillCount = 0;
                                lastKillTeam = killerTeam; }
                                killCount++; multiKillCount++; multiKillTimer = 2.0f;
                                if (killCount == 1) { snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 2) { snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 3) { snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount >= 4) { snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!"); killFeedTimer = 0.0f; killFeedScale = 2.5f; }
                                int ba2, ra2; CountTeams(units, unitCount, &ba2, &ra2);
                                if (ba2 == 0 || ra2 == 0) { slowmoTimer = 0.5f; slowmoScale = 0.3f; }
                                BattleLogAddKill(&battleLog, combatElapsedTime, units[projectiles[p].sourceIndex].team, units[projectiles[p].sourceIndex].typeIndex, units[ti].team, units[ti].typeIndex, ABILITY_MAELSTROM);
                                units[ti].active = false;
                            }
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
                    // HIT — Devil Bolt: flat damage ranged auto-attack
                    else if (projectiles[p].type == PROJ_DEVIL_BOLT) {
                        int si = projectiles[p].sourceIndex;
                        if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                            float hitDmg = projectiles[p].damage;
                            float armor = GetModifierValue(modifiers, ti, MOD_ARMOR);
                            hitDmg -= armor;
                            if (hitDmg < 0) hitDmg = 0;
                            if (units[ti].shieldHP > 0) {
                                if (hitDmg <= units[ti].shieldHP) { units[ti].shieldHP -= hitDmg; hitDmg = 0; }
                                else { hitDmg -= units[ti].shieldHP; units[ti].shieldHP = 0; }
                            }
                            units[ti].currentHealth -= hitDmg;
                            PlaySound(sfxProjectileHit);
                            units[ti].hitFlash = HIT_FLASH_DURATION;
                            SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg, false);
                            // Lifesteal from devil bolt
                            if (si >= 0 && si < unitCount && units[si].active) {
                                float ls = GetModifierValue(modifiers, si, MOD_LIFESTEAL);
                                if (ls > 0) {
                                    float maxHP = UNIT_STATS[units[si].typeIndex].health * units[si].hpMultiplier;
                                    units[si].currentHealth += hitDmg * ls;
                                    if (units[si].currentHealth > maxHP) units[si].currentHealth = maxHP;
                                }
                            }
                            if (units[ti].currentHealth <= 0) {
                                PlaySound(units[ti].typeIndex == 0 ? sfxToadDie : sfxGoblinDie);
                                SpawnDeathExplosion(particles, units[ti].position, units[ti].team);
                                TriggerShake(&shake, 4.0f, 0.2f);
                                { Team killerTeam = (units[ti].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                                if (killerTeam != lastKillTeam) multiKillCount = 0;
                                lastKillTeam = killerTeam; }
                                killCount++; multiKillCount++; multiKillTimer = 2.0f;
                                if (killCount == 1) { snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 2) { snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 3) { snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount >= 4) { snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!"); killFeedTimer = 0.0f; killFeedScale = 2.5f; }
                                int ba2, ra2; CountTeams(units, unitCount, &ba2, &ra2);
                                if (ba2 == 0 || ra2 == 0) { slowmoTimer = 0.5f; slowmoScale = 0.3f; }
                                BattleLogAddKill(&battleLog, combatElapsedTime, units[si].team, units[si].typeIndex, units[ti].team, units[ti].typeIndex, -1);
                                units[ti].active = false;
                            }
                        }
                        projectiles[p].active = false;
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
                        SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg, true);
                        if (projectiles[p].stunDuration > 0) {
                            AddModifier(modifiers, ti, MOD_STUN, projectiles[p].stunDuration, 0);
                            TriggerShake(&shake, 5.0f, 0.25f);
                        }
                        if (units[ti].currentHealth <= 0) {
                            PlaySound(units[ti].typeIndex == 0 ? sfxToadDie : sfxGoblinDie);
                            SpawnDeathExplosion(particles, units[ti].position, units[ti].team);
                            TriggerShake(&shake, 6.0f, 0.3f);
                            killCount++; multiKillCount++; multiKillTimer = 2.0f;
                            if (killCount == 1) { snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                            else if (multiKillCount == 2) { snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                            else if (multiKillCount == 3) { snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                            else if (multiKillCount >= 4) { snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!"); killFeedTimer = 0.0f; killFeedScale = 2.5f; }
                            int ba2, ra2; CountTeams(units, unitCount, &ba2, &ra2);
                            if (ba2 == 0 || ra2 == 0) { slowmoTimer = 0.5f; slowmoScale = 0.3f; }
                            { int abilId = (projectiles[p].type == PROJ_MAGIC_MISSILE) ? ABILITY_MAGIC_MISSILE : ABILITY_CHAIN_FROST;
                            BattleLogAddKill(&battleLog, combatElapsedTime, units[projectiles[p].sourceIndex].team, units[projectiles[p].sourceIndex].typeIndex, units[ti].team, units[ti].typeIndex, abilId); }
                            units[ti].active = false;
                        }
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
                    // Particle trail
                    Color tc = projectiles[p].color;
                    Vector3 tv = {
                        ((GetRandomValue(0, 200) - 100) / 100.0f) * 3.0f,
                        ((GetRandomValue(0, 100)) / 100.0f) * 4.0f + 3.0f,  // upward bias to fight gravity
                        ((GetRandomValue(0, 200) - 100) / 100.0f) * 3.0f,
                    };
                    SpawnParticle(particles, projectiles[p].position, tv,
                        PROJ_TRAIL_LIFE, PROJ_TRAIL_SIZE, tc);
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
                .battleLog = &battleLog, .combatTime = combatElapsedTime,
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
                        PlaySound(sfxMagicHit);
                        PlaySound(units[i].typeIndex == 0 ? sfxToadShout : sfxGoblinShout);
                        SpawnFloatingText(floatingTexts, units[i].position,
                            def->name, def->color, 1.0f);
                        BattleLogAddCast(&battleLog, combatElapsedTime, units[i].team, units[i].typeIndex, slot->abilityId);
                        units[i].abilityCastDelay = 0.75f;
                        // Pause caster briefly for projectile abilities
                        if (slot->abilityId == ABILITY_MAGIC_MISSILE ||
                            slot->abilityId == ABILITY_CHAIN_FROST ||
                            slot->abilityId == ABILITY_HOOK)
                            units[i].castPause = CAST_PAUSE_TIME;
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
                                    SpawnDamageNumber(floatingTexts, units[j].position, dmgHit, true);
        
                                    if (units[j].currentHealth <= 0) {
                                        PlaySound(units[j].typeIndex == 0 ? sfxToadDie : sfxGoblinDie);
                                        SpawnDeathExplosion(particles, units[j].position, units[j].team);
                                        TriggerShake(&shake, 6.0f, 0.3f);
        
                                        { Team killerTeam = (units[j].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                                if (killerTeam != lastKillTeam) multiKillCount = 0;
                                lastKillTeam = killerTeam; }
                                killCount++; multiKillCount++; multiKillTimer = 2.0f;
                                        if (killCount == 1) { snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                        else if (multiKillCount == 2) { snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                        else if (multiKillCount == 3) { snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                        else if (multiKillCount >= 4) { snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!"); killFeedTimer = 0.0f; killFeedScale = 2.5f; }
                                        int ba2, ra2; CountTeams(units, unitCount, &ba2, &ra2);
                                        if (ba2 == 0 || ra2 == 0) { slowmoTimer = 0.5f; slowmoScale = 0.3f; }
                                        BattleLogAddKill(&battleLog, combatElapsedTime, units[i].team, units[i].typeIndex, units[j].team, units[j].typeIndex, ABILITY_PRIMAL_CHARGE);
                                        units[j].active = false;
                                    }
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

                // Cast pause — brief freeze after projectile cast
                if (units[i].castPause > 0) {
                    units[i].castPause -= dt;
                    continue;
                }

                // Movement + basic attack
                if (target < 0) continue;
                float moveSpeed = stats->movementSpeed * units[i].speedMultiplier;
                float speedMult = GetModifierValue(modifiers, i, MOD_SPEED_MULT);
                if (speedMult > 0) moveSpeed *= speedMult;

                bool isDevil = (units[i].typeIndex == DEVIL_TYPE_INDEX);
                float unitAttackRange = isDevil ? DEVIL_RANGED_RANGE : ATTACK_RANGE;

                float dist = DistXZ(units[i].position, units[target].position);
                if (dist > unitAttackRange)
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
                        if (isDevil) {
                            // Devil ranged attack — spawn a bolt projectile
                            float dmg = stats->attackDamage * units[i].dmgMultiplier;
                            SpawnProjectile(projectiles, PROJ_DEVIL_BOLT,
                                units[i].position, target, i, units[i].team, 0,
                                50.0f, dmg, 0,
                                (Color){200, 50, 50, 255});
                            PlaySound(sfxProjectileWhoosh);
                            units[i].attackCooldown = stats->attackSpeed;
                            units[i].castPause = CAST_PAUSE_TIME;
                        } else {
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
                            PlaySound(sfxMeleeHit);
                            units[target].hitFlash = HIT_FLASH_DURATION;
                            SpawnDamageNumber(floatingTexts, units[target].position, dmg, false);
                            SpawnMeleeImpact(particles, units[target].position);
                            // Minor tile wobble on melee hit
                            {
                                float gridOriginMH = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                                for (int tr = 0; tr < TILE_GRID_SIZE; tr++) {
                                    for (int tc = 0; tc < TILE_GRID_SIZE; tc++) {
                                        float cx = gridOriginMH + (tc + 0.5f) * TILE_WORLD_SIZE;
                                        float cz = gridOriginMH + (tr + 0.5f) * TILE_WORLD_SIZE;
                                        float dxw = cx - units[target].position.x, dzw = cz - units[target].position.z;
                                        float dist = sqrtf(dxw*dxw + dzw*dzw);
                                        float wobbleR = 25.0f;
                                        if (dist < wobbleR) {
                                            float strength = expf(-2.0f * dist / wobbleR) * 0.2f;
                                            if (tileWobble[tr][tc] < TILE_WOBBLE_MAX * strength) {
                                                tileWobble[tr][tc] = TILE_WOBBLE_MAX * strength;
                                                tileWobbleTime[tr][tc] = -(dist * 0.008f);
                                                float len = dist > 0.1f ? dist : 1.0f;
                                                tileWobbleDirX[tr][tc] = dzw / len;
                                                tileWobbleDirZ[tr][tc] = -dxw / len;
                                            }
                                        }
                                    }
                                }
                            }
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
                            if (units[target].currentHealth <= 0) {
                                PlaySound(units[target].typeIndex == 0 ? sfxToadDie : sfxGoblinDie);
                                SpawnDeathExplosion(particles, units[target].position, units[target].team);
                                TriggerShake(&shake, 6.0f, 0.3f);

                                { Team killerTeam = (units[target].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                                if (killerTeam != lastKillTeam) multiKillCount = 0;
                                lastKillTeam = killerTeam; }
                                killCount++; multiKillCount++; multiKillTimer = 2.0f;
                                if (killCount == 1) { snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 2) { snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount == 3) { snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!"); killFeedTimer = 0.0f; killFeedScale = 2.0f; }
                                else if (multiKillCount >= 4) { snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!"); killFeedTimer = 0.0f; killFeedScale = 2.5f; }
                                int ba2, ra2; CountTeams(units, unitCount, &ba2, &ra2);
                                if (ba2 == 0 || ra2 == 0) { slowmoTimer = 0.5f; slowmoScale = 0.3f; }
                                BattleLogAddKill(&battleLog, combatElapsedTime, units[i].team, units[i].typeIndex, units[target].team, units[target].typeIndex, -1);
                                units[target].active = false;
                            }
                        }
                        units[i].attackCooldown = stats->attackSpeed;
                        units[i].attackAnimTimer = 0.4f;
                        } // end else (non-devil melee)
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
                    fightBannerTimer = -1.0f;
                    ClearAllParticles(particles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    // Victory celebration confetti
                    if (lastOutcomeWin) {
                        TriggerShake(&shake, 4.0f, 0.3f);
                        for (int ci = 0; ci < 40; ci++) {
                            Vector3 cpos = { (float)GetRandomValue(-80, 80), (float)GetRandomValue(30, 60), (float)GetRandomValue(-80, 80) };
                            Vector3 cvel = { (float)GetRandomValue(-20, 20)/10.0f, (float)GetRandomValue(-10, -2)/10.0f, (float)GetRandomValue(-20, 20)/10.0f };
                            Color cc = (Color){ (unsigned char)GetRandomValue(100, 255), (unsigned char)GetRandomValue(100, 255), (unsigned char)GetRandomValue(100, 255), 255 };
                            SpawnParticle(particles, cpos, cvel, 2.0f + (float)GetRandomValue(0, 10)/10.0f, (float)GetRandomValue(3, 8)/10.0f, cc);
                        }
                    }
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
                    fightBannerTimer = -1.0f;
                    ClearAllParticles(particles);
                    ClearAllFloatingTexts(floatingTexts);
                    ClearAllFissures(fissures);
                    statueSpawn.phase = SSPAWN_INACTIVE;
                    // Victory celebration confetti
                    if (lastOutcomeWin) {
                        TriggerShake(&shake, 4.0f, 0.3f);
                        for (int ci = 0; ci < 40; ci++) {
                            Vector3 cpos = { (float)GetRandomValue(-80, 80), (float)GetRandomValue(30, 60), (float)GetRandomValue(-80, 80) };
                            Vector3 cvel = { (float)GetRandomValue(-20, 20)/10.0f, (float)GetRandomValue(-10, -2)/10.0f, (float)GetRandomValue(-20, 20)/10.0f };
                            Color cc = (Color){ (unsigned char)GetRandomValue(100, 255), (unsigned char)GetRandomValue(100, 255), (unsigned char)GetRandomValue(100, 255), 255 };
                            SpawnParticle(particles, cpos, cvel, 2.0f + (float)GetRandomValue(0, 10)/10.0f, (float)GetRandomValue(3, 8)/10.0f, cc);
                        }
                    }
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
                    rollCost = rollCostBase;
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
                int btnW = 240, btnH = 54;
                int btnY = cardY + cardH + 30;
                int btnGap = 40;
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
                    SaveLeaderboard(&leaderboard, LEADERBOARD_FILE);

                    // Submit to global leaderboard server (best-effort, non-fatal)
                    net_leaderboard_submit(serverHost, NET_PORT, &entry);

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
                    rollCost = rollCostBase;
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
                for (int u2 = 0; u2 < MAX_UNITS; u2++) { units[u2].nfcUidLen = 0; units[u2].active = false; }
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
                playerGold = 25;
                for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                dragState.dragging = false;
                joinCodeLen = 0;
                joinCodeInput[0] = '\0';
                unitCount = 0;
                memset(plazaData, 0, sizeof(plazaData));
                PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);
                plazaState = PLAZA_ROAMING;
                phase = PHASE_PLAZA;
                PlayMusicStream(bgm);
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
                        // Withdraw — sync NFC abilities before removing
                        int wi = goBlue[h];
                        if (units[wi].nfcUidLen > 0) {
                            net_nfc_update_abilities(serverHost, NET_PORT,
                                units[wi].nfcUid, units[wi].nfcUidLen,
                                units[wi].abilities, MAX_ABILITIES_PER_UNIT);
                            units[wi].nfcUidLen = 0;
                        }
                        printf("[WITHDRAW] Unit %d (%s) withdrawn\n",
                               wi, unitTypes[units[wi].typeIndex].name);
                        units[wi].active = false;
                        CompactBlueUnits(units, &unitCount);
                        break; // re-layout next frame
                    }
                }

                // RESET button (only clickable when no NFC units remain)
                bool hasNfcUnits = false;
                for (int h = 0; h < goCount; h++)
                    if (units[goBlue[h]].nfcUidLen > 0) { hasNfcUnits = true; break; }
                int resetBtnW = 180, resetBtnH = 44;
                int resetBtnY = cardY + cardH + 30;
                Rectangle resetBtn = { (float)(sw/2 - resetBtnW/2), (float)resetBtnY, (float)resetBtnW, (float)resetBtnH };
                if (!hasNfcUnits && CheckCollisionPointRec(mouse, resetBtn)) {
                    PlaySound(sfxUiClick);
                    // Full reset — go to menu
                    for (int u2 = 0; u2 < MAX_UNITS; u2++) { units[u2].nfcUidLen = 0; units[u2].active = false; }
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
                    playerGold = 25;
                    for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                    dragState.dragging = false;
                    unitCount = 0;
                    memset(plazaData, 0, sizeof(plazaData));
                    PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);
                    plazaState = PLAZA_ROAMING;
                    phase = PHASE_PLAZA;
                    PlayMusicStream(bgm);
                }
            }

            // Death penalty: just press R (no withdraw possible)
            if (deathPenalty && IsKeyPressed(KEY_R)) {
                // Reset NFC-tagged units' abilities on server (include dead units)
                for (int u2 = 0; u2 < unitCount; u2++) {
                    if (units[u2].team == TEAM_BLUE && units[u2].nfcUidLen > 0) {
                        net_nfc_reset_abilities(serverHost, NET_PORT, units[u2].nfcUid, units[u2].nfcUidLen);
                    }
                }
                // Full reset — clear all units
                for (int u2 = 0; u2 < MAX_UNITS; u2++) { units[u2].nfcUidLen = 0; units[u2].active = false; }
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
                intro.active = false;
                statueSpawn.phase = SSPAWN_INACTIVE;
                playerGold = 25;
                for (int i = 0; i < MAX_INVENTORY_SLOTS; i++) inventory[i].abilityId = -1;
                dragState.dragging = false;
                memset(plazaData, 0, sizeof(plazaData));
                PlazaSpawnEnemies(units, &unitCount, unitTypeCount, plazaData);
                plazaState = PLAZA_ROAMING;
                phase = PHASE_PLAZA;
                PlayMusicStream(bgm);
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

            if (units[i].castPause > 0 && type->animIndex[ANIM_CAST] >= 0) {
                desired = ANIM_CAST;
            } else if (units[i].attackAnimTimer > 0 && type->animIndex[ANIM_ATTACK] >= 0) {
                units[i].attackAnimTimer -= dt;
                desired = ANIM_ATTACK;
            } else if (phase == PHASE_COMBAT && units[i].targetIndex >= 0) {
                float animRange = (units[i].typeIndex == DEVIL_TYPE_INDEX) ? DEVIL_RANGED_RANGE : ATTACK_RANGE;
                float dist = DistXZ(units[i].position, units[units[i].targetIndex].position);
                if (dist > animRange) desired = ANIM_WALK;
            } else if (phase == PHASE_PLAZA) {
                desired = units[i].currentAnim;  // set by plaza roam/flee logic
            }

            // Reset frame on anim change
            if (desired != units[i].currentAnim) {
                units[i].currentAnim = desired;
                units[i].animFrame = 0;
            }

            // Advance frame — pick anim array based on current state
            int idx = type->animIndex[units[i].currentAnim];
            if (idx >= 0) {
                ModelAnimation *arr = GetAnimArray(type, units[i].currentAnim);
                if (arr) {
                    int frameCount = arr[idx].frameCount;
                    if (frameCount > 0)
                        units[i].animFrame = (units[i].animFrame + 1) % frameCount;
                }
            }
        }

        //==============================================================================
        // WIN/LOSS SFX
        //==============================================================================
        if (phase != prevPhase && phase == PHASE_GAME_OVER) {
            StopMusicStream(bgm);
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

        // Recreate scene RT and FXAA RT if window was resized
        {
            int curW = GetScreenWidth(), curH = GetScreenHeight();
            if (curW != sceneRTWidth || curH != sceneRTHeight) {
                rlUnloadFramebuffer(sceneRT.id);
                rlUnloadTexture(sceneRT.texture.id);
                rlUnloadTexture(sceneRT.depth.id);
                sceneRTWidth = curW;
                sceneRTHeight = curH;
                sceneRT.id = rlLoadFramebuffer();
                sceneRT.texture.id = rlLoadTexture(NULL, sceneRTWidth, sceneRTHeight, RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
                sceneRT.texture.width = sceneRTWidth;
                sceneRT.texture.height = sceneRTHeight;
                sceneRT.depth.id = rlLoadTextureDepth(sceneRTWidth, sceneRTHeight, false);
                sceneRT.depth.width = sceneRTWidth;
                sceneRT.depth.height = sceneRTHeight;
                rlFramebufferAttach(sceneRT.id, sceneRT.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
                rlFramebufferAttach(sceneRT.id, sceneRT.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);

                UnloadRenderTexture(fxaaRT);
                fxaaRTWidth = curW;
                fxaaRTHeight = curH;
                fxaaRT = LoadRenderTexture(fxaaRTWidth, fxaaRTHeight);

                UnloadRenderTexture(colorGradeRT);
                colorGradeRT = LoadRenderTexture(fxaaRTWidth, fxaaRTHeight);
            }
        }

        // --- Shadow map pass ---
        {
            rlDrawRenderBatchActive();
            rlEnableFramebuffer(shadowRT.id);
            rlViewport(0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
            rlClearScreenBuffers();
            rlEnableDepthTest();
            rlDisableColorBlend();

            // Set light-space matrices for rlgl
            rlSetMatrixProjection(lightProj);
            rlSetMatrixModelview(lightView);

            // Swap all model materials to shadow depth shader
            for (int i = 0; i < unitTypeCount; i++) {
                if (!unitTypes[i].loaded) continue;
                for (int m = 0; m < unitTypes[i].model.materialCount; m++)
                    unitTypes[i].model.materials[m].shader = shadowDepthShader;
            }
            for (int i = 0; i < TILE_VARIANTS; i++)
                for (int m = 0; m < tileModels[i].materialCount; m++)
                    tileModels[i].materials[m].shader = shadowDepthShader;
            // Swap env model materials to shadow depth shader (covers ground, stairs, circle, etc.)
            for (int ei = 0; ei < envModelCount; ei++) {
                if (!envModels[ei].loaded) continue;
                for (int m = 0; m < envModels[ei].model.materialCount; m++)
                    envModels[ei].model.materials[m].shader = shadowDepthShader;
            }

            // Draw shadow-casting geometry
            {
                float gridOrigin = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                for (int r = 0; r < TILE_GRID_SIZE; r++) {
                    for (int c = 0; c < TILE_GRID_SIZE; c++) {
                        int vi = tileVariantGrid[r][c];
                        float cellX = gridOrigin + (c + 0.5f) * TILE_WORLD_SIZE + tileJitterX[r][c];
                        float cellZ = gridOrigin + (r + 0.5f) * TILE_WORLD_SIZE + tileJitterZ[r][c];
                        float totalRot = tileRotationGrid[r][c] + tileJitterAngle[r][c];
                        float angle = totalRot * DEG2RAD;
                        float cosA = cosf(angle);
                        float sinA = sinf(angle);
                        float sxo = tileCenters[vi].x * tileScale;
                        float szo = tileCenters[vi].z * tileScale;
                        float rxo = sxo * cosA + szo * sinA;
                        float rzo = -sxo * sinA + szo * cosA;
                        Vector3 pos = {
                            cellX - rxo,
                            -tileCenters[vi].y * tileScale - 0.5f,
                            cellZ - rzo,
                        };
                        DrawModelEx(tileModels[vi], pos,
                            (Vector3){ 0.0f, 1.0f, 0.0f }, totalRot,
                            (Vector3){ tileScale, tileScale, tileScale }, WHITE);
                    }
                }
            }
            // Draw env pieces (shadow pass — includes ground, stairs, circle)
            for (int ep = 0; ep < envPieceCount; ep++) {
                if (!envPieces[ep].active) continue;
                EnvModelDef *emd = &envModels[envPieces[ep].modelIndex];
                if (!emd->loaded) continue;
                float es = envPieces[ep].scale;
                DrawModelEx(emd->model, envPieces[ep].position, (Vector3){0,1,0},
                            envPieces[ep].rotationY, (Vector3){es,es,es}, WHITE);
            }
            for (int i = 0; i < unitCount; i++) {
                if (!units[i].active) continue;
                UnitType *type = &unitTypes[units[i].typeIndex];
                if (!type->loaded) continue;
                // Update animation pose so shadow matches current frame
                if (type->hasAnimations) {
                    int idx = type->animIndex[units[i].currentAnim];
                    if (idx >= 0) {
                        ModelAnimation *arr = GetAnimArray(type, units[i].currentAnim);
                        if (arr) UpdateModelAnimation(type->model, arr[idx], units[i].animFrame);
                    }
                }
                float s = type->scale * units[i].scaleOverride;
                Vector3 drawPos = units[i].position;
                drawPos.y += type->yOffset;
                DrawModelEx(type->model, drawPos, (Vector3){0,1,0}, units[i].facingAngle,
                    (Vector3){s, s, s}, WHITE);
            }

            // Restore lighting shader on all materials
            for (int i = 0; i < unitTypeCount; i++) {
                if (!unitTypes[i].loaded) continue;
                for (int m = 0; m < unitTypes[i].model.materialCount; m++)
                    unitTypes[i].model.materials[m].shader = lightShader;
            }
            for (int i = 0; i < TILE_VARIANTS; i++)
                for (int m = 0; m < tileModels[i].materialCount; m++)
                    tileModels[i].materials[m].shader = lightShader;
            // Restore lighting shader on env model materials
            for (int ei = 0; ei < envModelCount; ei++) {
                if (!envModels[ei].loaded) continue;
                for (int m = 0; m < envModels[ei].model.materialCount; m++)
                    envModels[ei].model.materials[m].shader = lightShader;
            }

            rlDrawRenderBatchActive();
            rlEnableColorBlend();
            rlDisableFramebuffer();
            rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
        }

        // Bind shadow map depth to texture slot 2 for lighting shader
        rlActiveTextureSlot(2);
        rlEnableTexture(shadowRT.depth.id);
        SetShaderValue(lightShader, shadowMapLoc, (int[]){2}, SHADER_UNIFORM_INT);
        SetShaderValueMatrix(lightShader, lightVPLoc, lightVP);

        // Render 3D scene into offscreen texture (for SSAO post-process)
        BeginTextureMode(sceneRT);
        ClearBackground((Color){ 45, 40, 35, 255 });
        BeginMode3D(camera);
            // Draw tiled floor (bind normal map for tiles)
            rlActiveTextureSlot(3);
            rlEnableTexture(tileNormal.id);
            SetShaderValue(lightShader, normalMapLoc, (int[]){3}, SHADER_UNIFORM_INT);
            SetShaderValue(lightShader, useNormalMapLoc, (int[]){1}, SHADER_UNIFORM_INT);
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
            SetShaderValue(lightShader, useNormalMapLoc, (int[]){0}, SHADER_UNIFORM_INT);

            // Draw env pieces (main render pass — includes ground, stairs, circle)
            for (int ep = 0; ep < envPieceCount; ep++) {
                if (!envPieces[ep].active) continue;
                EnvModelDef *emd = &envModels[envPieces[ep].modelIndex];
                if (!emd->loaded) continue;
                float es = envPieces[ep].scale;
                Color eTint = WHITE;
                if (debugMode && ep == envSelectedPiece) eTint = (Color){150, 255, 150, 255};
                if (emd->normalTexture.id > 0) {
                    rlActiveTextureSlot(3);
                    rlEnableTexture(emd->normalTexture.id);
                    SetShaderValue(lightShader, normalMapLoc, (int[]){3}, SHADER_UNIFORM_INT);
                    SetShaderValue(lightShader, useNormalMapLoc, (int[]){1}, SHADER_UNIFORM_INT);
                } else {
                    SetShaderValue(lightShader, useNormalMapLoc, (int[]){0}, SHADER_UNIFORM_INT);
                }
                DrawModelEx(emd->model, envPieces[ep].position, (Vector3){0,1,0},
                            envPieces[ep].rotationY, (Vector3){es,es,es}, eTint);
            }
            // Reset normal map after env pieces so other models don't use it
            SetShaderValue(lightShader, useNormalMapLoc, (int[]){0}, SHADER_UNIFORM_INT);

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
                if (units[i].rarity == RARITY_LEGENDARY) {
                    float t = (float)GetTime() + (float)i * 1.7f;
                    float shimmer = sinf(t * 4.0f);
                    if (shimmer > 0.3f) {
                        float f = (shimmer - 0.3f) / 0.7f * 0.5f;
                        tint.r = (unsigned char)(tint.r + (255 - tint.r) * f);
                        tint.g = (unsigned char)(tint.g + (255 - (int)tint.g) * f);
                        tint.b = (unsigned char)(tint.b + (128 - (int)tint.b) * f);
                    }
                }
                if (type->hasAnimations) {
                    int idx = type->animIndex[units[i].currentAnim];
                    if (idx >= 0) {
                        ModelAnimation *arr = GetAnimArray(type, units[i].currentAnim);
                        if (arr) UpdateModelAnimation(type->model, arr[idx], units[i].animFrame);
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
                float pr = 1.5f;
                if (projectiles[p].chargeTimer > 0 && projectiles[p].chargeMax > 0) {
                    float t = 1.0f - projectiles[p].chargeTimer / projectiles[p].chargeMax;
                    pr *= t;
                }
                DrawSphere(projectiles[p].position, pr, projectiles[p].color);
            }

            // Draw particles as camera-facing billboards
            {
                // Compute camera right and up vectors for billboarding
                Vector3 camFwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                Vector3 camRight = Vector3Normalize(Vector3CrossProduct(camFwd, camera.up));
                Vector3 camUp = Vector3CrossProduct(camRight, camFwd);

                rlDisableDepthMask();
                rlDrawRenderBatchActive();
                rlSetBlendFactors(RL_SRC_ALPHA, RL_ONE, RL_FUNC_ADD);  // additive blending
                rlSetBlendMode(BLEND_CUSTOM);
                rlSetTexture(particleTex.id);
                rlBegin(RL_QUADS);
                for (int p = 0; p < MAX_PARTICLES; p++) {
                    if (!particles[p].active) continue;
                    float sz = particles[p].size;
                    Vector3 pos = particles[p].position;
                    Color c = particles[p].color;

                    // 4 corners: pos ± right*sz ± up*sz
                    Vector3 r = { camRight.x * sz, camRight.y * sz, camRight.z * sz };
                    Vector3 u = { camUp.x * sz, camUp.y * sz, camUp.z * sz };

                    // Bottom-left
                    rlColor4ub(c.r, c.g, c.b, c.a);
                    rlTexCoord2f(0.0f, 1.0f);
                    rlVertex3f(pos.x - r.x - u.x, pos.y - r.y - u.y, pos.z - r.z - u.z);
                    // Bottom-right
                    rlTexCoord2f(1.0f, 1.0f);
                    rlVertex3f(pos.x + r.x - u.x, pos.y + r.y - u.y, pos.z + r.z - u.z);
                    // Top-right
                    rlTexCoord2f(1.0f, 0.0f);
                    rlVertex3f(pos.x + r.x + u.x, pos.y + r.y + u.y, pos.z + r.z + u.z);
                    // Top-left
                    rlTexCoord2f(0.0f, 0.0f);
                    rlVertex3f(pos.x - r.x + u.x, pos.y - r.y + u.y, pos.z - r.z + u.z);
                }
                rlEnd();
                rlSetTexture(0);
                rlDrawRenderBatchActive();
                rlSetBlendMode(BLEND_ALPHA);  // restore normal blending
                rlEnableDepthMask();
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

            // Draw plaza 3D objects (door, trophy) during PHASE_PLAZA
            if (phase == PHASE_PLAZA) {
                plazaSparkleTimer += dt;
                PlazaDrawObjects(doorModel, trophyModel, doorPos, trophyPos, camera,
                    plazaHoverObject == 2, plazaHoverObject == 1, plazaSparkleTimer);
            }
        EndMode3D();
        EndTextureMode();

        // Render offscreen textures before fxaaRT to avoid nested render targets
        // (raylib's EndTextureMode always restores to FBO 0, breaking nesting)

        // Game-over portraits
        if (phase == PHASE_GAME_OVER && !isMultiplayer && !deathPenalty) {
            int noShadowOn = 1, noShadowOff = 0;
            SetShaderValue(lightShader, noShadowLoc, &noShadowOn, SHADER_UNIFORM_INT);
            int goBlueRT[BLUE_TEAM_MAX_SIZE]; int goCountRT = 0;
            for (int i = 0; i < unitCount && goCountRT < BLUE_TEAM_MAX_SIZE; i++)
                if (units[i].active && units[i].team == TEAM_BLUE) goBlueRT[goCountRT++] = i;
            for (int h = 0; h < goCountRT; h++) {
                int ui = goBlueRT[h];
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
            SetShaderValue(lightShader, noShadowLoc, &noShadowOff, SHADER_UNIFORM_INT);
        }

        // Intro model
        if (intro.active) {
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

                int noShadowOn = 1, noShadowOff = 0;
                SetShaderValue(lightShader, noShadowLoc, &noShadowOn, SHADER_UNIFORM_INT);
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
                SetShaderValue(lightShader, noShadowLoc, &noShadowOff, SHADER_UNIFORM_INT);
            }
        }

        // Composite scene + post-process into FXAA RT (avoid nesting render targets)
        BeginTextureMode(fxaaRT);
        ClearBackground((Color){ 45, 40, 35, 255 });

        // Draw scene with SSAO post-process
        {
            float res[2] = { (float)sceneRTWidth, (float)sceneRTHeight };
            float nearPlane = 0.1f, farPlane = 1000.0f;
            SetShaderValue(ssaoShader, ssaoResLoc, res, SHADER_UNIFORM_VEC2);
            SetShaderValue(ssaoShader, ssaoNearLoc, &nearPlane, SHADER_UNIFORM_FLOAT);
            SetShaderValue(ssaoShader, ssaoFarLoc, &farPlane, SHADER_UNIFORM_FLOAT);
            // Bind depth texture to texture unit 1
            rlActiveTextureSlot(1);
            rlEnableTexture(sceneRT.depth.id);
            SetShaderValue(ssaoShader, ssaoDepthLoc, (int[]){1}, SHADER_UNIFORM_INT);
            BeginShaderMode(ssaoShader);
                DrawTextureRec(sceneRT.texture,
                    (Rectangle){ 0, 0, (float)sceneRTWidth, -(float)sceneRTHeight },
                    (Vector2){ 0, 0 }, WHITE);
            EndShaderMode();
            rlActiveTextureSlot(0);
        }

        // Restore camera position after shake
        camera.position = camSaved;

        // End fxaaRT here so 3D scene gets FXAA but HUD text does not.
        // (FXAA smears text glyphs, making them blurry/jagged)
        EndTextureMode();

        // FXAA pass → colorGradeRT
        BeginTextureMode(colorGradeRT);
        ClearBackground(BLACK);
        {
            float fxaaRes[2] = { (float)fxaaRTWidth, (float)fxaaRTHeight };
            SetShaderValue(fxaaShader, fxaaResLoc, fxaaRes, SHADER_UNIFORM_VEC2);
            BeginShaderMode(fxaaShader);
            DrawTextureRec(fxaaRT.texture,
                (Rectangle){ 0, 0, (float)fxaaRTWidth, -(float)fxaaRTHeight },
                (Vector2){ 0, 0 }, WHITE);
            EndShaderMode();
        }
        EndTextureMode();

        // Color grading pass → screen
        {
            SetShaderValue(colorGradeShader, cgExposureLoc,   &cgExposure,    SHADER_UNIFORM_FLOAT);
            SetShaderValue(colorGradeShader, cgContrastLoc,   &cgContrast,    SHADER_UNIFORM_FLOAT);
            SetShaderValue(colorGradeShader, cgSaturationLoc, &cgSaturation,  SHADER_UNIFORM_FLOAT);
            SetShaderValue(colorGradeShader, cgTemperatureLoc,&cgTemperature, SHADER_UNIFORM_FLOAT);
            SetShaderValue(colorGradeShader, cgVigStrLoc,     &cgVignetteStr, SHADER_UNIFORM_FLOAT);
            SetShaderValue(colorGradeShader, cgVigSoftLoc,    &cgVignetteSoft,SHADER_UNIFORM_FLOAT);
            SetShaderValue(colorGradeShader, cgLiftLoc,        cgLift,        SHADER_UNIFORM_VEC3);
            SetShaderValue(colorGradeShader, cgGainLoc,        cgGain,        SHADER_UNIFORM_VEC3);
            BeginShaderMode(colorGradeShader);
            DrawTextureRec(colorGradeRT.texture,
                (Rectangle){ 0, 0, (float)fxaaRTWidth, -(float)fxaaRTHeight },
                (Vector2){ 0, 0 }, WHITE);
            EndShaderMode();
        }

        // 2D overlay: labels + health bars (drawn directly to screen, no FXAA)
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

            if (units[i].rarity > 0) {
                const char *stars = (units[i].rarity == RARITY_LEGENDARY) ? "* *" : "*";
                int starsW = GameMeasureText(stars, S(14));
                Color starColor = (units[i].rarity == RARITY_LEGENDARY)
                    ? (Color){ 255, 60, 60, 255 }
                    : (Color){ 180, 100, 255, 255 };
                GameDrawText(stars, (int)sp.x - starsW/2, (int)sp.y - S(26), S(14), starColor);
            }

            const char *label = (units[i].nfcUidLen > 0 && units[i].nfcName[0])
                ? units[i].nfcName : type->name;
            int nameFontSize = S(16);
            int tw = GameMeasureText(label, nameFontSize);
            // Drop shadow for readability
            GameDrawText(label, (int)sp.x - tw/2 + 1, (int)sp.y - S(14) + 1, nameFontSize, (Color){0,0,0,180});
            GameDrawText(label, (int)sp.x - tw/2, (int)sp.y - S(14), nameFontSize,
                     (units[i].team == TEAM_BLUE) ? WHITE : (Color){255, 200, 200, 255});

            // Health bar
            float maxHP = stats->health * units[i].hpMultiplier;
            float hpRatio = units[i].currentHealth / maxHP;
            if (hpRatio < 0) hpRatio = 0;
            if (hpRatio > 1) hpRatio = 1;
            int bw = S(44), bh = S(6);
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
            int htw = GameMeasureText(hpT, S(12));
            GameDrawText(hpT, (int)sp.x - htw/2 + 1, by + bh + 2 + 1, S(12), (Color){0,0,0,180});
            GameDrawText(hpT, (int)sp.x - htw/2, by + bh + 2, S(12), WHITE);

            // Enemy ability grid (prep phase only, red team)
            if (phase == PHASE_PREP && units[i].team == TEAM_RED) {
                int eSlotSz = S(22);
                int eSlotGap = S(3);
                int eGridW = 2 * eSlotSz + eSlotGap;
                int eGridH = 2 * eSlotSz + eSlotGap;
                int egx = (int)sp.x - eGridW / 2;
                int egy = by + bh + S(18);
                // Fade in as mouse approaches the grid
                Vector2 mpos = GetMousePosition();
                float eCenterX = egx + eGridW * 0.5f;
                float eCenterY = egy + eGridH * 0.5f;
                float eDx = mpos.x - eCenterX, eDy = mpos.y - eCenterY;
                float eMouseDist = sqrtf(eDx * eDx + eDy * eDy);
                float eFadeNear = 40.0f, eFadeFar = 160.0f;
                float eAlphaFrac = 1.0f - (eMouseDist - eFadeNear) / (eFadeFar - eFadeNear);
                if (eAlphaFrac < 0.25f) eAlphaFrac = 0.25f;
                if (eAlphaFrac > 1.0f) eAlphaFrac = 1.0f;
                unsigned char eAlpha = (unsigned char)(eAlphaFrac * 255);
                unsigned char eAlphaLow = (unsigned char)(eAlphaFrac * 200);
                // Background panel
                DrawRectangle(egx - 3, egy - 3, eGridW + 6, eGridH + 6, (Color){20, 20, 30, eAlphaLow});
                DrawRectangleLinesEx((Rectangle){(float)(egx - 3), (float)(egy - 3),
                    (float)(eGridW + 6), (float)(eGridH + 6)}, 1, (Color){80, 60, 60, eAlphaLow});
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                    int col = a % 2, row = a / 2;
                    int eax = egx + col * (eSlotSz + eSlotGap);
                    int eay = egy + row * (eSlotSz + eSlotGap);
                    AbilitySlot *eslot = &units[i].abilities[a];
                    if (eslot->abilityId >= 0 && eslot->abilityId < ABILITY_COUNT) {
                        Color slotCol = ABILITY_DEFS[eslot->abilityId].color;
                        slotCol.a = eAlpha;
                        DrawRectangle(eax, eay, eSlotSz, eSlotSz, slotCol);
                        // Hover for tooltip
                        bool eHovered = CheckCollisionPointRec(mpos,
                            (Rectangle){(float)eax, (float)eay, (float)eSlotSz, (float)eSlotSz});
                        if (eHovered) { hoverAbilityId = eslot->abilityId; hoverAbilityLevel = eslot->level; }
                        const char *eabbr = ABILITY_DEFS[eslot->abilityId].abbrev;
                        int eaw = GameMeasureText(eabbr, S(10));
                        Color etxtCol = {255, 255, 255, eAlpha};
                        GameDrawText(eabbr, eax + (eSlotSz - eaw) / 2,
                                eay + (eSlotSz - S(10)) / 2, S(10), etxtCol);
                        const char *elvl = TextFormat("L%d", eslot->level + 1);
                        GameDrawText(elvl, eax + 2, eay + eSlotSz - S(8), S(8), etxtCol);
                    } else {
                        DrawRectangle(eax, eay, eSlotSz, eSlotSz, (Color){40, 40, 55, eAlphaLow});
                    }
                    DrawRectangleLines(eax, eay, eSlotSz, eSlotSz, (Color){80, 80, 100, eAlphaLow});
                }
            }

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
                    int mlw = GameMeasureText(modLabel, S(11));
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
                        GameDrawText(tmp, cx, modY, S(11), charCol);
                        cx += GameMeasureText(tmp, S(11));
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

        // 2D overlay: floating texts (spell shouts + damage numbers)
        for (int i = 0; i < MAX_FLOATING_TEXTS; i++) {
            if (!floatingTexts[i].active) continue;
            Vector2 fsp = GetWorldToScreen(floatingTexts[i].position, camera);
            float alpha = floatingTexts[i].life / floatingTexts[i].maxLife;
            int fsize = floatingTexts[i].fontSize > 0 ? floatingTexts[i].fontSize : 16;
            // Apply horizontal drift
            float elapsed = floatingTexts[i].maxLife - floatingTexts[i].life;
            float driftOffset = floatingTexts[i].driftX * elapsed;
            int ftw = GameMeasureText(floatingTexts[i].text, fsize);
            Color ftc = floatingTexts[i].color;
            ftc.a = (unsigned char)(255.0f * alpha);
            GameDrawText(floatingTexts[i].text, (int)(fsp.x + driftOffset) - ftw/2, (int)fsp.y, fsize, ftc);
        }

        // ── Spawn buttons + Play — during prep and plaza ──
        if (phase == PHASE_PREP || phase == PHASE_PLAZA)
        {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            int dHudTop = sh - hudTotalH;
            int dBtnXBlue = btnMargin;
            int dBtnXRed  = sw - btnWidth - btnMargin;
            int validTypeCount = 0;
            for (int i = 0; i < unitTypeCount; i++) if (unitTypes[i].name) validTypeCount++;
            int dBtnYStart = dHudTop - (validTypeCount * (btnHeight + btnMargin)) - btnMargin;

            // Spawn buttons (debug mode only — F1 to toggle)
            if (debugMode) {
                int drawIdx = 0;
                for (int i = 0; i < unitTypeCount; i++)
                {
                    if (!unitTypes[i].name) continue;
                    Rectangle r = { (float)dBtnXBlue, (float)(dBtnYStart + drawIdx*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                    Color c = unitTypes[i].loaded ? (Color){100,140,230,255} : LIGHTGRAY;
                    if (CheckCollisionPointRec(GetMousePosition(), r) && unitTypes[i].loaded) c = BLUE;
                    DrawRectangleRec(r, c);
                    DrawRectangleLinesEx(r, 2, unitTypes[i].loaded ? DARKBLUE : GRAY);
                    const char *l = TextFormat("BLUE %s", unitTypes[i].name);
                    int lw = GameMeasureText(l, 14);
                    GameDrawText(l, r.x + (btnWidth-lw)/2, r.y + (btnHeight-14)/2, 14, WHITE);
                    drawIdx++;
                }

                drawIdx = 0;
                for (int i = 0; i < unitTypeCount; i++)
                {
                    if (!unitTypes[i].name) continue;
                    Rectangle r = { (float)dBtnXRed, (float)(dBtnYStart + drawIdx*(btnHeight+btnMargin)), (float)btnWidth, (float)btnHeight };
                    Color c = unitTypes[i].loaded ? (Color){230,100,100,255} : LIGHTGRAY;
                    if (CheckCollisionPointRec(GetMousePosition(), r) && unitTypes[i].loaded) c = RED;
                    DrawRectangleRec(r, c);
                    DrawRectangleLinesEx(r, 2, unitTypes[i].loaded ? MAROON : GRAY);
                    const char *l = TextFormat("RED %s", unitTypes[i].name);
                    int lw = GameMeasureText(l, 14);
                    GameDrawText(l, r.x + (btnWidth-lw)/2, r.y + (btnHeight-14)/2, 14, WHITE);
                    drawIdx++;
                }

                // Rarity debug spawn buttons (below blue column)
                {
                    int rY = dBtnYStart + drawIdx * (btnHeight + btnMargin);
                    Rectangle rr = { (float)dBtnXBlue, (float)rY, (float)btnWidth, (float)btnHeight };
                    Color rc = (Color){100,160,255,255};
                    if (CheckCollisionPointRec(GetMousePosition(), rr)) rc = (Color){130,180,255,255};
                    DrawRectangleRec(rr, rc);
                    DrawRectangleLinesEx(rr, 2, (Color){180,200,255,255});
                    const char *rl = "RARE Mushroom";
                    int rlw = GameMeasureText(rl, 14);
                    GameDrawText(rl, rr.x + (btnWidth-rlw)/2, rr.y + (btnHeight-14)/2, 14, (Color){180,200,255,255});

                    rY += btnHeight + btnMargin;
                    Rectangle lr = { (float)dBtnXBlue, (float)rY, (float)btnWidth, (float)btnHeight };
                    Color lc = (Color){200,170,50,255};
                    if (CheckCollisionPointRec(GetMousePosition(), lr)) lc = (Color){230,200,80,255};
                    DrawRectangleRec(lr, lc);
                    DrawRectangleLinesEx(lr, 2, (Color){255,215,0,255});
                    const char *ll = "LEGEND Mushroom";
                    int llw = GameMeasureText(ll, 14);
                    GameDrawText(ll, lr.x + (btnWidth-llw)/2, lr.y + (btnHeight-14)/2, 14, (Color){255,215,0,255});
                }

                GameDrawText("[F1] DEBUG MODE", dBtnXBlue, dBtnYStart - 20, 12, YELLOW);
                GameDrawText(TextFormat("[</>] Tiles: %s", tileLayoutNames[tileLayout]), dBtnXBlue, dBtnYStart - 36, 12, YELLOW);

                // --- ENV PIECE spawn buttons (centered column) ---
                {
                    int envBtnW = 110, envBtnH = 24, envBtnGap = 4;
                    int envColX = sw / 2 - envBtnW / 2;
                    int envStartY = dBtnYStart;
                    GameDrawText("[ENV PIECES]", envColX, envStartY - 16, 12, YELLOW);
                    for (int ei = 0; ei < envModelCount; ei++) {
                        if (!envModels[ei].loaded) continue;
                        Rectangle er = { (float)envColX, (float)(envStartY + ei * (envBtnH + envBtnGap)),
                                         (float)envBtnW, (float)envBtnH };
                        Color ec = (Color){80, 160, 80, 255};
                        if (CheckCollisionPointRec(GetMousePosition(), er)) ec = GREEN;
                        DrawRectangleRec(er, ec);
                        DrawRectangleLinesEx(er, 1, DARKGREEN);
                        const char *el = TextFormat("+ %s", envModels[ei].name);
                        int elw = GameMeasureText(el, 12);
                        GameDrawText(el, (int)(er.x + (envBtnW - elw) / 2), (int)(er.y + 6), 12, WHITE);
                    }
                    // SAVE LAYOUT button
                    int saveY = envStartY + envModelCount * (envBtnH + envBtnGap) + 4;
                    Rectangle saveBtn = { (float)envColX, (float)saveY, (float)envBtnW, (float)envBtnH };
                    Color savCol = (Color){160, 120, 40, 255};
                    if (CheckCollisionPointRec(GetMousePosition(), saveBtn)) savCol = GOLD;
                    DrawRectangleRec(saveBtn, savCol);
                    DrawRectangleLinesEx(saveBtn, 1, DARKBROWN);
                    const char *savLbl = TextFormat("SAVE (%d pcs)", envPieceCount);
                    int savLblW = GameMeasureText(savLbl, 12);
                    GameDrawText(savLbl, (int)(saveBtn.x + (envBtnW - savLblW) / 2), (int)(saveBtn.y + 6), 12, WHITE);

                    // Flash "SAVED!" text
                    if (envSaveFlashTimer > 0.0f) {
                        float alpha = envSaveFlashTimer > 1.0f ? 1.0f : envSaveFlashTimer;
                        GameDrawText("SAVED!", envColX + envBtnW + 8, saveY + 4, 14,
                                 (Color){50, 255, 50, (unsigned char)(255 * alpha)});
                    }

                    // Selected piece info overlay
                    if (envSelectedPiece >= 0 && envSelectedPiece < envPieceCount && envPieces[envSelectedPiece].active) {
                        EnvPiece *sp = &envPieces[envSelectedPiece];
                        const char *infoName = envModels[sp->modelIndex].name;
                        int infoY = saveY + envBtnH + 12;
                        GameDrawText(TextFormat("%s  [X:%.1f Y:%.1f Z:%.1f]", infoName,
                                 sp->position.x, sp->position.y, sp->position.z),
                                 envColX, infoY, 12, WHITE);
                        GameDrawText(TextFormat("Rot: %.0f deg  Scale: %.1fx", sp->rotationY, sp->scale),
                                 envColX, infoY + 14, 12, WHITE);
                        GameDrawText("[Q/E] Rot  [R/F] Y  [[ / ]] Scale  [DEL] Remove", envColX, infoY + 28, 10, (Color){180,180,180,200});
                    }
                }
            }

            // Round info label (prep phase only)
            if (phase == PHASE_PREP) {
                const char *waveLabel;
                if (isMultiplayer) {
                    const char *roundType = currentRoundIsPve ? "PVE" : "PVP";
                    waveLabel = TextFormat("Round %d - %s", currentRound + 1, roundType);
                } else {
                    waveLabel = TextFormat("Wave %d", currentRound + 1);
                }
                int wlw = GameMeasureText(waveLabel, S(20));
                GameDrawText(waveLabel, sw/2 - wlw/2, dBtnYStart - 25, S(20), WHITE);
            }

            // NFC emulation input box (debug only)
            if (debugMode) {
                int nfcBoxW = 200, nfcBoxH = 28;
                int nfcBoxX = sw/2 - nfcBoxW/2;
                int nfcBoxY = dBtnYStart - 55;
                int labelW = GameMeasureText("NFC Code:", 14);

                // Label
                GameDrawText("NFC Code:", nfcBoxX - labelW - 8, nfcBoxY + 6, 14, (Color){180,180,200,255});

                // Input field background
                Color boxBg = nfcInputActive ? (Color){50,50,70,255} : (Color){30,30,45,255};
                Color boxBorder = nfcInputActive ? (Color){100,140,255,255} : (Color){70,70,90,255};
                DrawRectangle(nfcBoxX, nfcBoxY, nfcBoxW, nfcBoxH, boxBg);
                DrawRectangleLinesEx((Rectangle){(float)nfcBoxX,(float)nfcBoxY,(float)nfcBoxW,(float)nfcBoxH}, 1, boxBorder);

                // Text content or placeholder
                if (nfcInputLen > 0) {
                    GameDrawText(nfcInputBuf, nfcBoxX + 6, nfcBoxY + 6, 14, WHITE);
                    // Blinking cursor when active
                    if (nfcInputActive && ((int)(GetTime() * 2.0) % 2 == 0)) {
                        int tw = GameMeasureText(nfcInputBuf, 14);
                        GameDrawText("|", nfcBoxX + 6 + tw, nfcBoxY + 5, 14, (Color){200,200,255,255});
                    }
                } else {
                    if (nfcInputActive) {
                        // Blinking cursor
                        if ((int)(GetTime() * 2.0) % 2 == 0)
                            GameDrawText("|", nfcBoxX + 6, nfcBoxY + 5, 14, (Color){200,200,255,255});
                    } else {
                        GameDrawText("e.g. 1MM1DG2XXCF3", nfcBoxX + 6, nfcBoxY + 6, 12, (Color){100,100,120,255});
                    }
                }

                // Error message below
                if (nfcInputErrorTimer > 0.0f) {
                    float alpha = nfcInputErrorTimer > 1.0f ? 1.0f : nfcInputErrorTimer;
                    Color errColor = { 255, 80, 80, (unsigned char)(255 * alpha) };
                    GameDrawText(nfcInputError, nfcBoxX, nfcBoxY + nfcBoxH + 4, 12, errColor);
                }
            }

            // Danger zone indicator (pushing past a milestone)
            if (lastMilestoneRound > 0) {
                const char *dangerText = "DANGER ZONE - Losing means permanent death!";
                int dtw = GameMeasureText(dangerText, 18);
                GameDrawText(dangerText, sw/2 - dtw/2, 60, 18, RED);
                int nextMilestone = ((currentRound / 5) + 1) * 5;
                const char *nextText = TextFormat("Next milestone: Wave %d", nextMilestone);
                int ntw = GameMeasureText(nextText, 14);
                GameDrawText(nextText, sw/2 - ntw/2, 82, 14, ORANGE);
            }

            // PLAY / READY button (centre-bottom, prep phase only)
            if (phase == PHASE_PREP) {
                Rectangle dPlayBtn = { (float)(sw/2 - playBtnW/2), (float)(dHudTop - playBtnH - btnMargin), (float)playBtnW, (float)playBtnH };
                int ba, ra;
                CountTeams(units, unitCount, &ba, &ra);
                bool canPlay = isMultiplayer ? (ba > 0) : (ba > 0 && ra > 0);
                bool alreadyReady = isMultiplayer && playerReady;
                Color pc;
                if (alreadyReady)
                    pc = (Color){80,80,80,255};
                else if (canPlay)
                    pc = (Color){50,180,80,255};
                else
                    pc = LIGHTGRAY;
                if (canPlay && !alreadyReady && CheckCollisionPointRec(GetMousePosition(), dPlayBtn))
                    pc = (Color){30,220,60,255};
                DrawRectangleRec(dPlayBtn, pc);
                DrawRectangleLinesEx(dPlayBtn, 2, canPlay && !alreadyReady ? DARKGREEN : GRAY);
                const char *pt;
                if (isMultiplayer) {
                    if (alreadyReady)
                        pt = waitingForOpponent ? "WAITING FOR OPPONENT..." : "I'M READY!";
                    else
                        pt = TextFormat("I'M READY - Round %d", currentRound + 1);
                } else {
                    pt = TextFormat("PLAY Round %d", currentRound + 1);
                }
                int playFontSz = S(20);
                int ptw = GameMeasureText(pt, playFontSz);
                GameDrawText(pt, dPlayBtn.x + (playBtnW - ptw)/2, dPlayBtn.y + (playBtnH - playFontSz)/2, playFontSz, WHITE);
            }
        }

        // ── HUD: round + score info ──
        {
            int sw = GetScreenWidth();
            int sh = GetScreenHeight();
            if (phase != PHASE_PLAZA) {
                GameDrawText(TextFormat("Round: %d / %d", currentRound < TOTAL_ROUNDS ? currentRound + 1 : TOTAL_ROUNDS, TOTAL_ROUNDS),
                         sw/2 - 60, 10, 20, BLACK);
                GameDrawText(TextFormat("Units: %d / %d", unitCount, MAX_UNITS), 10, 30, 10, DARKGRAY);
            }
            if (isMultiplayer) {
                const char *youLabel = TextFormat("YOU (%s): %d", playerName, blueWins);
                const char *oppLabel = TextFormat("OPP (%s): %d", netClient.opponentName[0] ? netClient.opponentName : "???", redWins);
                int youW = GameMeasureText(youLabel, 18);
                int oppW = GameMeasureText(oppLabel, 18);
                GameDrawText(youLabel, sw/2 - youW - 10, 35, 18, DARKBLUE);
                GameDrawText(oppLabel, sw/2 + 10, 35, 18, MAROON);
                (void)oppW;
            }

            // Phase label
            if (phase == PHASE_COMBAT)
            {
                // Animated "FIGHT!" banner
                if (fightBannerTimer >= 0.0f && fightBannerTimer < 1.5f) {
                    const char *fightText = "FIGHT!";
                    int baseFontSize = S(56);
                    float t = fightBannerTimer;
                    float scale;
                    if (t < 0.15f) scale = t / 0.15f * 1.5f;           // 0→1.5
                    else if (t < 0.5f) scale = 1.5f - (t - 0.15f) / 0.35f * 0.5f;  // 1.5→1.0
                    else scale = 1.0f;
                    float alpha = t < 1.0f ? 1.0f : 1.0f - (t - 1.0f) / 0.5f;
                    if (alpha < 0) alpha = 0;
                    int drawSize = (int)(baseFontSize * scale);
                    if (drawSize < 1) drawSize = 1;
                    int ftw = GameMeasureText(fightText, drawSize);
                    // Shake during punch-in
                    int shakeX = 0, shakeY = 0;
                    if (t < 0.5f) { shakeX = GetRandomValue(-3, 3); shakeY = GetRandomValue(-2, 2); }
                    Color fc = RED;
                    fc.a = (unsigned char)(255.0f * alpha);
                    GameDrawText(fightText, sw/2 - ftw/2 + shakeX, sh/2 - 60 + shakeY, drawSize, fc);
                }
                // Kill feed announcement
                if (killFeedTimer >= 0.0f && killFeedTimer < 3.0f) {
                    float kft = killFeedTimer;
                    int kfFontSize = 36;
                    float kfScale;
                    if (kft < 0.15f) kfScale = killFeedScale * (kft / 0.15f);
                    else if (kft < 0.4f) kfScale = killFeedScale - (killFeedScale - 1.0f) * ((kft - 0.15f) / 0.25f);
                    else kfScale = 1.0f;
                    float kfAlpha = kft < 2.0f ? 1.0f : 1.0f - (kft - 2.0f) / 1.0f;
                    if (kfAlpha < 0) kfAlpha = 0;
                    int kfDrawSize = (int)(kfFontSize * kfScale);
                    if (kfDrawSize < 1) kfDrawSize = 1;
                    int kfw = GameMeasureText(killFeedText, kfDrawSize);
                    Color kfc = (Color){255, 200, 50, (unsigned char)(255.0f * kfAlpha)};
                    GameDrawText(killFeedText, sw/2 - kfw/2, sh/2 - 20, kfDrawSize, kfc);
                }
            }
            else if (phase == PHASE_ROUND_OVER)
            {
                // Animated round result text with scale punch-in
                float rot = roundOverTimer; // counts down from 2.5
                float elapsed = 2.5f - rot;
                float rtScale;
                if (elapsed < 0.15f) rtScale = elapsed / 0.15f * 1.3f;
                else if (elapsed < 0.4f) rtScale = 1.3f - (elapsed - 0.15f) / 0.25f * 0.3f;
                else rtScale = 1.0f;
                int rtFontSize = (int)(S(30) * rtScale);
                if (rtFontSize < 1) rtFontSize = 1;
                Color rtColor = lastOutcomeWin ? (Color){50, 200, 50, 255} : DARKPURPLE;
                // Color pulse for win
                if (lastOutcomeWin) {
                    float pulse = 0.5f + 0.5f * sinf(elapsed * 6.0f);
                    rtColor.r = (unsigned char)(50 + pulse * 100);
                    rtColor.g = (unsigned char)(200 + pulse * 55);
                }
                int rtw = GameMeasureText(roundResultText, rtFontSize);
                int rtY = sh/2 - rtFontSize - S(5);
                GameDrawText(roundResultText, sw/2 - rtw/2, rtY, rtFontSize, rtColor);

                const char *scoreText = TextFormat("Score: %d - %d", blueWins, redWins);
                int stFontSize = S(22);
                int stw = GameMeasureText(scoreText, stFontSize);
                GameDrawText(scoreText, sw/2 - stw/2, rtY + rtFontSize + S(8), stFontSize, WHITE);
            }

            // Battle Log panel (during combat, round over, and next prep)
            if ((phase == PHASE_COMBAT || phase == PHASE_ROUND_OVER || phase == PHASE_PREP) && battleLog.count > 0)
            {
                int blogW = S(240);
                int blogX = sw - blogW;
                int blogY = 60;
                int blogH = sh - hudTotalH - blogY;
                // Background
                DrawRectangle(blogX, blogY, blogW, blogH, (Color){16, 16, 24, 160});
                DrawRectangleLines(blogX, blogY, blogW, blogH, (Color){80, 80, 100, 120});
                // Title
                const char *blogTitle = "BATTLE LOG";
                int btw = GameMeasureText(blogTitle, S(14));
                GameDrawText(blogTitle, blogX + blogW/2 - btw/2, blogY + S(4), S(14), (Color){200, 200, 220, 255});
                // Entry area
                int entryY = blogY + S(20);
                int entryH = blogH - S(24);
                int lineH = S(18);
                int maxVisible = entryH / lineH;
                // Mouse wheel scroll when not in active combat
                if (phase != PHASE_COMBAT) {
                    int wheel = (int)GetMouseWheelMove();
                    if (wheel != 0) {
                        battleLog.scroll -= wheel;
                        if (battleLog.scroll < 0) battleLog.scroll = 0;
                        int maxScroll = battleLog.count - maxVisible;
                        if (maxScroll < 0) maxScroll = 0;
                        if (battleLog.scroll > maxScroll) battleLog.scroll = maxScroll;
                    }
                } else {
                    // Auto-scroll to bottom during combat
                    int maxScroll = battleLog.count - maxVisible;
                    if (maxScroll < 0) maxScroll = 0;
                    battleLog.scroll = maxScroll;
                }
                // Scissor clip
                BeginScissorMode(blogX, entryY, blogW, entryH);
                int startIdx = battleLog.scroll;
                for (int ei = startIdx; ei < battleLog.count && (ei - startIdx) < maxVisible; ei++) {
                    BattleLogEntry *e = &battleLog.entries[ei];
                    int drawY = entryY + (ei - startIdx) * lineH;
                    // Timestamp
                    const char *ts = TextFormat("%d:%02d", (int)e->timestamp / 60, (int)e->timestamp % 60);
                    GameDrawText(ts, blogX + S(4), drawY, S(12), (Color){140, 140, 140, 200});
                    // Icon
                    const char *icon = (e->type == BLOG_KILL) ? "X" : "*";
                    Color iconColor = (e->type == BLOG_KILL) ? (Color){255, 80, 80, 255} : (Color){80, 200, 255, 255};
                    GameDrawText(icon, blogX + S(34), drawY, S(12), iconColor);
                    // Text (truncated to fit)
                    GameDrawText(e->text, blogX + S(44), drawY, S(12), e->color);
                }
                EndScissorMode();
            }

            else if (phase == PHASE_GAME_OVER)
            {
                if (deathPenalty) {
                    const char *deathMsg = TextFormat("YOUR UNITS HAVE FALLEN - Wave %d", currentRound);
                    int dw = GameMeasureText(deathMsg, S(34));
                    GameDrawText(deathMsg, sw/2 - dw/2, sh/2 - 50, S(34), RED);

                    const char *deathSub = "Defeated! Your units are lost forever!";
                    int dsw2 = GameMeasureText(deathSub, S(22));
                    GameDrawText(deathSub, sw/2 - dsw2/2, sh/2 - 10, S(22), (Color){255,100,100,255});

                    const char *restartMsg = "Press R to return to menu";
                    int rw2 = GameMeasureText(restartMsg, S(24));
                    GameDrawText(restartMsg, sw/2 - rw2/2, sh/2 + 30, S(24), GRAY);
                }
                // Non-death game over is drawn as a full overlay below
            }

        }

        // F1 debug hint (always visible, top-right)
        {
            const char *dbgHint = "[F1] Debug";
            int dbgW = GameMeasureText(dbgHint, 14);
            Color dbgCol = debugMode ? YELLOW : (Color){180,180,180,120};
            GameDrawText(dbgHint, GetScreenWidth() - dbgW - 10, 10, 14, dbgCol);
        }

        // Camera debug sliders (debug mode only)
        if (debugMode) {
            // Override toggle button
            Rectangle overrideBtn = { 10, 60, 80, 20 };
            DrawRectangleRec(overrideBtn, camOverride ? GREEN : GRAY);
            GameDrawText(camOverride ? "Override ON" : "Override OFF", 14, 64, 10, WHITE);
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), overrideBtn))
                camOverride = !camOverride;

            Color sliderBg = camOverride ? LIGHTGRAY : (Color){100,100,100,255};
            Color sliderFill = camOverride ? SKYBLUE : (Color){80,80,120,255};

            // Height slider: range -50 to 500
            Rectangle hBar = { 10, 85, 200, 20 };
            float hPerc = (camHeight - (-50.0f)) / (500.0f - (-50.0f));
            if (hPerc > 1) hPerc = 1;
            if (hPerc < 0) hPerc = 0;
            DrawRectangleRec(hBar, sliderBg);
            DrawRectangle(10, 85, (int)(200*hPerc), 20, sliderFill);
            GameDrawText(TextFormat("Height: %.1f", camHeight), 220, 85, 10, BLACK);
            if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), hBar)) {
                float t = (GetMousePosition().x - 10.0f) / 200.0f;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                camHeight = -50.0f + t * 550.0f;
            }

            // Distance slider: range -300 to 500
            Rectangle dBar = { 10, 110, 200, 20 };
            float dPerc = (camDistance - (-300.0f)) / (500.0f - (-300.0f));
            if (dPerc > 1) dPerc = 1;
            if (dPerc < 0) dPerc = 0;
            DrawRectangleRec(dBar, sliderBg);
            DrawRectangle(10, 110, (int)(200*dPerc), 20, sliderFill);
            GameDrawText(TextFormat("Distance: %.1f", camDistance), 220, 110, 10, BLACK);
            if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), dBar)) {
                float t = (GetMousePosition().x - 10.0f) / 200.0f;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                camDistance = -300.0f + t * 800.0f;
            }

            // FOV slider: range 5 to 160
            Rectangle fBar = { 10, 135, 200, 20 };
            float fPerc = (camFOV - 5.0f) / (160.0f - 5.0f);
            if (fPerc > 1) fPerc = 1;
            if (fPerc < 0) fPerc = 0;
            DrawRectangleRec(fBar, sliderBg);
            DrawRectangle(10, 135, (int)(200*fPerc), 20, sliderFill);
            GameDrawText(TextFormat("FOV: %.1f", camFOV), 220, 135, 10, BLACK);
            if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), fBar)) {
                float t = (GetMousePosition().x - 10.0f) / 200.0f;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                camFOV = 5.0f + t * 155.0f;
            }

            // X Offset slider: range -200 to 200
            Rectangle xBar = { 10, 160, 200, 20 };
            float xPerc = (camX - (-200.0f)) / (200.0f - (-200.0f));
            if (xPerc > 1) xPerc = 1;
            if (xPerc < 0) xPerc = 0;
            DrawRectangleRec(xBar, sliderBg);
            DrawRectangle(10, 160, (int)(200*xPerc), 20, sliderFill);
            GameDrawText(TextFormat("X Offset: %.1f", camX), 220, 160, 10, BLACK);
            if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), xBar)) {
                float t = (GetMousePosition().x - 10.0f) / 200.0f;
                if (t < 0) t = 0;
                if (t > 1) t = 1;
                camX = -200.0f + t * 400.0f;
            }

            // Save button
            Rectangle saveBtn = { 10, 185, 50, 20 };
            DrawRectangleRec(saveBtn, (Color){60,60,200,255});
            GameDrawText("Save", 18, 189, 10, WHITE);
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), saveBtn)) {
                FILE *f = fopen("cam_debug.txt", "w");
                if (f) { fprintf(f, "%f %f %f %f\n", camHeight, camDistance, camFOV, camX); fclose(f); }
            }

            // Load button
            Rectangle loadBtn = { 65, 185, 50, 20 };
            DrawRectangleRec(loadBtn, (Color){60,150,60,255});
            GameDrawText("Load", 73, 189, 10, WHITE);
            if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), loadBtn)) {
                FILE *f = fopen("cam_debug.txt", "r");
                if (f) {
                    if (fscanf(f, "%f %f %f %f", &camHeight, &camDistance, &camFOV, &camX) == 4)
                        camOverride = true;
                    fclose(f);
                }
            }
        }

        // ── UNIT HUD BAR + SHOP ── (visible during prep, combat, round_over only)
        if (phase != PHASE_GAME_OVER && phase != PHASE_PLAZA && phase != PHASE_MILESTONE)
        {
            int hudSw = GetScreenWidth();
            int hudSh = GetScreenHeight();
            int hudTop = hudSh - hudTotalH;

            // --- Dark background panel (full width, bottom) ---
            DrawRectangle(0, hudTop, hudSw, hudTotalH, (Color){ 24, 24, 32, 230 });
            DrawRectangle(0, hudTop, hudSw, 2, (Color){ 60, 60, 80, 255 });

            // --- Unit cards (centered horizontally in the unit bar) ---
            int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW
                            + (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
            int cardsStartX = (hudSw - totalCardsW) / 2;
            int cardsY = hudTop + hudShopH + 5;

            for (int slot = 0; slot < BLUE_TEAM_MAX_SIZE; slot++)
            {
                int cardX = cardsStartX + slot * (hudCardW + hudCardSpacing);

                // Card background
                DrawRectangle(cardX, cardsY, hudCardW, hudCardH,
                             (Color){ 35, 35, 50, 255 });
                DrawRectangleLines(cardX, cardsY, hudCardW, hudCardH,
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
                                        (float)(hudCardW + 2), (float)(hudCardH + 2) },
                            2, (Color){ 100, 255, 100, 255 });

                    // Rarity border glow
                    if (units[ui].rarity == RARITY_LEGENDARY) {
                        float pulse = (sinf((float)GetTime() * 2.5f + (float)slot * 1.7f) + 1.0f) * 0.5f;
                        unsigned char alpha = (unsigned char)(120 + pulse * 80);
                        DrawRectangleLinesEx(
                            (Rectangle){ (float)(cardX-1), (float)(cardsY-1),
                                        (float)(hudCardW+2), (float)(hudCardH+2) },
                            2, (Color){ 255, 60, 60, alpha });
                    } else if (units[ui].rarity == RARITY_RARE) {
                        DrawRectangleLinesEx(
                            (Rectangle){ (float)(cardX-1), (float)(cardsY-1),
                                        (float)(hudCardW+2), (float)(hudCardH+2) },
                            1, (Color){ 180, 100, 255, 160 });
                    }

                    // X button (remove unit) — prep phase only
                    if (phase == PHASE_PREP) {
                        int xBtnSize = S(18);
                        int xBtnX = cardX + hudCardW - xBtnSize - 2;
                        int xBtnY = cardsY + 2;
                        Color xBg = (Color){ 180, 50, 50, 200 };
                        if (CheckCollisionPointRec(GetMousePosition(),
                            (Rectangle){ (float)xBtnX, (float)xBtnY, (float)xBtnSize, (float)xBtnSize }))
                            xBg = (Color){ 230, 70, 70, 255 };
                        DrawRectangle(xBtnX, xBtnY, xBtnSize, xBtnSize, xBg);
                        DrawRectangleLines(xBtnX, xBtnY, xBtnSize, xBtnSize, (Color){100,30,30,255});
                        int xw = GameMeasureText("X", 12);
                        GameDrawText("X", xBtnX + (xBtnSize - xw) / 2, xBtnY + 2, 12, WHITE);
                    }

                    // Portrait (left side of card) — Y-flipped for RenderTexture
                    // srcRect uses base texture size, dstRect uses scaled size
                    Rectangle srcRect = { 0, 0, (float)HUD_PORTRAIT_SIZE_BASE, -(float)HUD_PORTRAIT_SIZE_BASE };
                    Rectangle dstRect = { (float)(cardX + S(4)), (float)(cardsY + S(4)),
                                          (float)hudPortraitSize, (float)hudPortraitSize };
                    DrawTexturePro(portraits[slot].texture, srcRect, dstRect,
                                  (Vector2){ 0, 0 }, 0.0f, WHITE);
                    DrawRectangleLines(cardX + S(4), cardsY + S(4),
                                      hudPortraitSize, hudPortraitSize,
                                      (Color){ 60, 60, 80, 255 });

                    // Unit name below portrait
                    const char *unitName = type->name;
                    int nameW = GameMeasureText(unitName, S(12));
                    GameDrawText(unitName,
                            cardX + S(4) + (hudPortraitSize - nameW) / 2,
                            cardsY + S(4) + hudPortraitSize + S(2),
                            S(12), (Color){ 200, 200, 220, 255 });

                    if (units[ui].rarity > 0) {
                        const char *stars = (units[ui].rarity == RARITY_LEGENDARY) ? "* *" : "*";
                        int starsW = GameMeasureText(stars, S(10));
                        Color starColor = (units[ui].rarity == RARITY_LEGENDARY)
                            ? (Color){ 255, 60, 60, 255 }
                            : (Color){ 180, 100, 255, 255 };
                        GameDrawText(stars, cardX + S(4) + (hudPortraitSize - starsW)/2,
                                 cardsY + S(4) + hudPortraitSize - S(4), S(10), starColor);
                    }

                    // Mini health bar
                    int hbX = cardX + S(4);
                    int hbY = cardsY + S(4) + hudPortraitSize + S(16);
                    int hbW = hudPortraitSize;
                    int hbH = S(6);
                    float cardMaxHP = stats->health * units[ui].hpMultiplier;
                    float hpRatio = units[ui].currentHealth / cardMaxHP;
                    if (hpRatio < 0) hpRatio = 0;
                    if (hpRatio > 1) hpRatio = 1;
                    DrawRectangle(hbX, hbY, hbW, hbH, (Color){ 20, 20, 20, 255 });
                    Color hpCol = (hpRatio > 0.5f) ? GREEN : (hpRatio > 0.25f) ? ORANGE : RED;
                    DrawRectangle(hbX, hbY, (int)(hbW * hpRatio), hbH, hpCol);
                    DrawRectangleLines(hbX, hbY, hbW, hbH, (Color){ 60, 60, 80, 255 });

                    // 2x2 Ability slot grid (right side of card)
                    int abilStartX = cardX + hudPortraitSize + 12;
                    int abilStartY = cardsY + 8;
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
                    {
                        int col = a % 2;
                        int row = a / 2;
                        int ax = abilStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
                        int ay = abilStartY + row * (hudAbilSlotSize + hudAbilSlotGap);

                        AbilitySlot *aslot = &units[ui].abilities[a];
                        if (aslot->abilityId >= 0 && aslot->abilityId < ABILITY_COUNT) {
                            // Filled slot — colored background
                            DrawRectangle(ax, ay, hudAbilSlotSize, hudAbilSlotSize,
                                         ABILITY_DEFS[aslot->abilityId].color);
                            // Hover detection
                            bool slotHovered = CheckCollisionPointRec(GetMousePosition(),
                                (Rectangle){(float)ax,(float)ay,(float)hudAbilSlotSize,(float)hudAbilSlotSize});
                            if (slotHovered) { hoverAbilityId = aslot->abilityId; hoverAbilityLevel = aslot->level; }
                            // Abbreviation (scale up when charging tooltip)
                            int abbrSize = S(13);
                            if (slotHovered && hoverTimer > 0 && hoverTimer < tooltipDelay)
                                abbrSize = S(13) + (int)(3.0f * (hoverTimer / tooltipDelay));
                            const char *abbr = ABILITY_DEFS[aslot->abilityId].abbrev;
                            int aw2 = GameMeasureText(abbr, abbrSize);
                            GameDrawTextOnColor(abbr, ax + (hudAbilSlotSize - aw2) / 2,
                                    ay + (hudAbilSlotSize - abbrSize) / 2, abbrSize,
                                    ABILITY_DEFS[aslot->abilityId].color);
                            // Level indicator (bottom-left)
                            const char *lvl = TextFormat("L%d", aslot->level + 1);
                            int lvlFsz = S(11);
                            GameDrawTextOnColor(lvl, ax + S(2), ay + hudAbilSlotSize - lvlFsz, lvlFsz,
                                    ABILITY_DEFS[aslot->abilityId].color);
                            // Cooldown overlay (combat only)
                            if (aslot->cooldownRemaining > 0 && phase == PHASE_COMBAT) {
                                const AbilityDef *adef = &ABILITY_DEFS[aslot->abilityId];
                                float cdFrac = aslot->cooldownRemaining / adef->cooldown[aslot->level];
                                if (cdFrac > 1) cdFrac = 1;
                                int overlayH = (int)(hudAbilSlotSize * cdFrac);
                                DrawRectangle(ax, ay, hudAbilSlotSize, overlayH, (Color){0,0,0,150});
                                int cdFsz = S(14);
                                const char *cdTxt = TextFormat("%.0f", aslot->cooldownRemaining);
                                int cdw = GameMeasureText(cdTxt, cdFsz);
                                GameDrawText(cdTxt, ax + (hudAbilSlotSize - cdw)/2,
                                        ay + (hudAbilSlotSize - cdFsz)/2, cdFsz, WHITE);
                            }
                        } else {
                            // Empty slot
                            DrawRectangle(ax, ay, hudAbilSlotSize, hudAbilSlotSize,
                                         (Color){ 40, 40, 55, 255 });
                            const char *q = "?";
                            int qFsz = S(18);
                            int qw = GameMeasureText(q, qFsz);
                            GameDrawText(q, ax + (hudAbilSlotSize - qw) / 2,
                                    ay + (hudAbilSlotSize - qFsz) / 2, qFsz, (Color){ 80, 80, 100, 255 });
                        }
                        DrawRectangleLines(ax, ay, hudAbilSlotSize, hudAbilSlotSize,
                                          (Color){ 90, 90, 110, 255 });
                        // Activation order number (top-right corner)
                        // Find which activation position this slot is
                        int orderNum = 0;
                        for (int o = 0; o < MAX_ABILITIES_PER_UNIT; o++)
                            if (ACTIVATION_ORDER[o] == a) { orderNum = o + 1; break; }
                        Color orderCol = (Color){100,100,120,255};
                        if (phase == PHASE_COMBAT && ACTIVATION_ORDER[units[ui].nextAbilitySlot] == a)
                            orderCol = YELLOW;
                        int ordFsz = S(11);
                        const char *ordTxt = TextFormat("%d", orderNum);
                        GameDrawText(ordTxt, ax + hudAbilSlotSize - ordFsz + 1, ay + S(1) + 1, ordFsz, (Color){0,0,0,180});
                        GameDrawText(ordTxt, ax + hudAbilSlotSize - ordFsz, ay + S(1), ordFsz, orderCol);
                    }
                }
                else
                {
                    // Empty slot placeholder
                    const char *emptyText = "EMPTY";
                    int emptyFsz = S(16);
                    int ew = GameMeasureText(emptyText, emptyFsz);
                    GameDrawText(emptyText,
                            cardX + (hudCardW - ew) / 2,
                            cardsY + (hudCardH - emptyFsz) / 2,
                            emptyFsz, (Color){ 60, 60, 80, 255 });
                }
            }

            // --- Sell zone (left of inventory) ---
            int sellZoneSize, sellZoneX, sellZoneY;
            {
                int invGridW = HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap);
                int invStartX = cardsStartX - invGridW - 20;
                sellZoneSize = 2 * hudAbilSlotSize + hudAbilSlotGap;
                sellZoneX = invStartX - sellZoneSize - S(10);
                sellZoneY = cardsY + S(18);
                bool hovering = dragState.dragging && CheckCollisionPointRec(GetMousePosition(),
                    (Rectangle){(float)sellZoneX, (float)sellZoneY, (float)sellZoneSize, (float)sellZoneSize});
                Color sellBg = hovering ? (Color){80, 30, 30, 255} : (Color){45, 35, 35, 255};
                Color sellBorder = hovering ? (Color){255, 80, 80, 255} : (Color){120, 80, 80, 255};
                DrawRectangle(sellZoneX, sellZoneY, sellZoneSize, sellZoneSize, sellBg);
                DrawRectangleLines(sellZoneX, sellZoneY, sellZoneSize, sellZoneSize, sellBorder);
                // Sell label
                const char *sellLabel = "SELL";
                int sellLabelW = GameMeasureText(sellLabel, S(14));
                GameDrawText(sellLabel, sellZoneX + (sellZoneSize - sellLabelW) / 2,
                    sellZoneY + sellZoneSize / 2 - S(16), S(14), sellBorder);
                // Gold indicator
                if (dragState.dragging && dragState.abilityId >= 0 && dragState.abilityId < ABILITY_COUNT) {
                    int sellValue = ABILITY_DEFS[dragState.abilityId].goldCost / 2;
                    if (sellValue < 1) sellValue = 1;
                    const char *sellGold = TextFormat("+%dg", sellValue);
                    int sgW = GameMeasureText(sellGold, S(12));
                    GameDrawText(sellGold, sellZoneX + (sellZoneSize - sgW) / 2,
                        sellZoneY + sellZoneSize / 2 + S(2), S(12),
                        hovering ? (Color){240, 200, 60, 255} : (Color){160, 140, 50, 200});
                }
            }

            // --- Inventory (left of unit cards) ---
            {
                int invStartX = cardsStartX - (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
                int invLabelY = cardsY + S(2);
                GameDrawText("INV", invStartX, invLabelY, S(14), (Color){160,160,180,255});
                int invStartY = invLabelY + S(16);
                for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
                    int icol = inv % HUD_INVENTORY_COLS;
                    int irow = inv / HUD_INVENTORY_COLS;
                    int ix = invStartX + icol * (hudAbilSlotSize + hudAbilSlotGap);
                    int iy = invStartY + irow * (hudAbilSlotSize + hudAbilSlotGap);
                    DrawRectangle(ix, iy, hudAbilSlotSize, hudAbilSlotSize, (Color){40,40,55,255});
                    DrawRectangleLines(ix, iy, hudAbilSlotSize, hudAbilSlotSize, (Color){90,90,110,255});
                    if (inventory[inv].abilityId >= 0 && inventory[inv].abilityId < ABILITY_COUNT) {
                        DrawRectangle(ix+1, iy+1, hudAbilSlotSize-2, hudAbilSlotSize-2,
                                      ABILITY_DEFS[inventory[inv].abilityId].color);
                        // Hover detection
                        bool invHovered = CheckCollisionPointRec(GetMousePosition(),
                            (Rectangle){(float)ix,(float)iy,(float)hudAbilSlotSize,(float)hudAbilSlotSize});
                        if (invHovered) { hoverAbilityId = inventory[inv].abilityId; hoverAbilityLevel = inventory[inv].level; }
                        int invAbbrSize = S(13);
                        if (invHovered && hoverTimer > 0 && hoverTimer < tooltipDelay)
                            invAbbrSize = S(13) + (int)(3.0f * (hoverTimer / tooltipDelay));
                        Color invAbilColor = ABILITY_DEFS[inventory[inv].abilityId].color;
                        const char *iabbr = ABILITY_DEFS[inventory[inv].abilityId].abbrev;
                        int iaw = GameMeasureText(iabbr, invAbbrSize);
                        GameDrawTextOnColor(iabbr, ix + (hudAbilSlotSize-iaw)/2,
                                 iy + (hudAbilSlotSize-invAbbrSize)/2, invAbbrSize, invAbilColor);
                        const char *ilvl = TextFormat("L%d", inventory[inv].level + 1);
                        int ilvlFsz = S(11);
                        GameDrawTextOnColor(ilvl, ix + S(2), iy + hudAbilSlotSize - ilvlFsz, ilvlFsz, invAbilColor);
                    }
                }
            }

            // --- Synergy Panel (right of unit cards) ---
            {
                // Compute synergy tiers for blue team (display only)
                int synTier[SYNERGY_COUNT];
                int synMatchCount[SYNERGY_COUNT];
                bool unitSyn[BLUE_TEAM_MAX_SIZE][SYNERGY_COUNT];
                for (int s = 0; s < (int)SYNERGY_COUNT; s++) synTier[s] = -1;
                for (int s = 0; s < (int)SYNERGY_COUNT; s++) synMatchCount[s] = 0;
                for (int sl = 0; sl < BLUE_TEAM_MAX_SIZE; sl++)
                    for (int s = 0; s < (int)SYNERGY_COUNT; s++)
                        unitSyn[sl][s] = false;

                for (int s = 0; s < (int)SYNERGY_COUNT; s++) {
                    const SynergyDef *syn = &SYNERGY_DEFS[s];
                    int matchCount = 0;
                    if (syn->requireAllTypes) {
                        bool typePresent[4] = {0};
                        for (int sl = 0; sl < blueHudCount; sl++) {
                            int ui = blueHudUnits[sl];
                            for (int r = 0; r < syn->requiredTypeCount; r++)
                                if (units[ui].typeIndex == syn->requiredTypes[r])
                                    typePresent[r] = true;
                        }
                        for (int r = 0; r < syn->requiredTypeCount; r++)
                            if (typePresent[r]) matchCount++;
                    } else {
                        for (int sl = 0; sl < blueHudCount; sl++) {
                            int ui = blueHudUnits[sl];
                            for (int r = 0; r < syn->requiredTypeCount; r++) {
                                if (units[ui].typeIndex == syn->requiredTypes[r]) {
                                    matchCount++;
                                    break;
                                }
                            }
                        }
                    }
                    synMatchCount[s] = matchCount;
                    for (int tier = 0; tier < syn->tierCount; tier++)
                        if (matchCount >= syn->tiers[tier].minUnits)
                            synTier[s] = tier;

                    // Mark which card slots benefit from this synergy
                    if (synTier[s] >= 0) {
                        for (int sl = 0; sl < blueHudCount; sl++) {
                            int ui = blueHudUnits[sl];
                            bool isTarget = false;
                            if (syn->targetType < 0) {
                                for (int r = 0; r < syn->requiredTypeCount; r++)
                                    if (units[ui].typeIndex == syn->requiredTypes[r])
                                        { isTarget = true; break; }
                            } else {
                                isTarget = (units[ui].typeIndex == syn->targetType);
                            }
                            unitSyn[sl][s] = isTarget;
                        }
                    }
                }

                // Draw synergy panel rows (right of the cards)
                int synPanelX = cardsStartX + totalCardsW + S(12);
                int synPanelY = cardsY + S(2);
                int synRowH = S(20);
                int maxSynRows = hudCardH / synRowH;
                int activeSynCount = 0;
                for (int s = 0; s < (int)SYNERGY_COUNT; s++) {
                    if (synTier[s] < 0) continue;
                    if (activeSynCount >= maxSynRows) break;
                    const SynergyDef *syn = &SYNERGY_DEFS[s];
                    int rowY = synPanelY + activeSynCount * synRowH;

                    // Hover detection for tooltip
                    Rectangle synRow = { (float)synPanelX, (float)rowY, (float)S(160), (float)synRowH };
                    bool synHovered = CheckCollisionPointRec(GetMousePosition(), synRow);
                    if (synHovered) hoverSynergyIdx = s;

                    // Colored dot
                    DrawCircle(synPanelX + S(5), rowY + synRowH / 2, S(4), syn->color);
                    // Synergy name
                    GameDrawText(syn->name, synPanelX + S(14), rowY + S(2), S(11), WHITE);
                    // Tier pips
                    int pipX = synPanelX + S(14) + GameMeasureText(syn->name, S(11)) + S(6);
                    for (int t = 0; t < syn->tierCount; t++) {
                        Color pipColor = (t <= synTier[s])
                            ? syn->color
                            : (Color){ 60, 60, 80, 255 };
                        DrawCircle(pipX + t * S(10), rowY + synRowH / 2, S(3), pipColor);
                    }
                    // Buff text
                    if (syn->buffDesc[synTier[s]]) {
                        int buffX = pipX + syn->tierCount * S(10) + S(6);
                        GameDrawText(syn->buffDesc[synTier[s]], buffX, rowY + S(3), S(11),
                                 (Color){ 160, 160, 180, 200 });
                    }
                    activeSynCount++;
                }

                // Per-card synergy badges (inside card, at bottom)
                int badgeFsz = S(9);
                int badgeH = badgeFsz + S(4);
                for (int sl = 0; sl < blueHudCount; sl++) {
                    int cardX = cardsStartX + sl * (hudCardW + hudCardSpacing);
                    int badgeY = cardsY + hudCardH - badgeH - S(2);
                    int badgeX = cardX + S(2);
                    for (int s = 0; s < (int)SYNERGY_COUNT; s++) {
                        if (!unitSyn[sl][s]) continue;
                        const SynergyDef *syn = &SYNERGY_DEFS[s];
                        int abbrW = GameMeasureText(syn->abbrev, badgeFsz) + S(6);
                        if (badgeX + abbrW > cardX + hudCardW - S(2)) break;
                        // Pill background
                        DrawRectangle(badgeX, badgeY, abbrW, badgeH,
                                      (Color){ syn->color.r, syn->color.g, syn->color.b, 180 });
                        DrawRectangleLines(badgeX, badgeY, abbrW, badgeH,
                                           (Color){ syn->color.r, syn->color.g, syn->color.b, 255 });
                        GameDrawText(syn->abbrev, badgeX + S(3), badgeY + S(2), badgeFsz, WHITE);
                        // Badge hover detection
                        Rectangle badgeRect = { (float)badgeX, (float)badgeY, (float)abbrW, (float)badgeH };
                        if (CheckCollisionPointRec(GetMousePosition(), badgeRect))
                            hoverSynergyIdx = s;
                        badgeX += abbrW + S(3);
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
                int daw = GameMeasureText(dabbr, S(13));
                GameDrawText(dabbr, (int)dmouse.x - daw/2, (int)dmouse.y - 5, S(13), WHITE);
            }

            // --- Shop panel (only during PREP, above unit bar) ---
            if (phase == PHASE_PREP)
            {
                int shopY = hudTop + 2;
                int shopH = hudShopH - 2;
                DrawRectangle(0, shopY, hudSw, shopH, (Color){ 20, 20, 28, 240 });
                DrawRectangle(0, shopY + shopH - 1, hudSw, 1, (Color){ 60, 60, 80, 255 });

                // ROLL button (left) — show cost
                Rectangle rollBtn = { 20, (float)(shopY + 10), S(90), S(34) };
                bool canRoll = (playerGold >= rollCost);
                Color rollColor = canRoll ? (Color){ 180, 140, 40, 255 } : (Color){ 80, 70, 40, 255 };
                if (canRoll && CheckCollisionPointRec(GetMousePosition(), rollBtn))
                    rollColor = (Color){ 220, 180, 60, 255 };
                DrawRectangleRec(rollBtn, rollColor);
                DrawRectangleLinesEx(rollBtn, 2, (Color){ 120, 90, 20, 255 });
                const char *rollText = TextFormat("ROLL %dg", rollCost);
                int rollW = GameMeasureText(rollText, S(16));
                GameDrawText(rollText, (int)(rollBtn.x + (S(90) - rollW) / 2),
                        (int)(rollBtn.y + (S(34) - S(16)) / 2), S(16), WHITE);
                GameDrawText("[R]", (int)(rollBtn.x + 2), (int)(rollBtn.y + 2), S(10), (Color){255,255,200,240});

                // Roll hotkey hint (first round only, until player uses it)
                if (currentRound == 0 && !usedRollHotkey) {
                    const char *rhint = "Press [R] to reroll shop!";
                    int rhSz = S(14);
                    int rhW = GameMeasureText(rhint, rhSz);
                    int rhX = (int)(rollBtn.x + rollBtn.width + 10);
                    int rhY = (int)(rollBtn.y + (rollBtn.height - rhSz) / 2);
                    float rpulse = 0.5f + 0.5f * sinf((float)GetTime() * 3.0f);
                    unsigned char rhAlpha = (unsigned char)(160 + (int)(rpulse * 95));
                    DrawRectangle(rhX - 6, rhY - 4, rhW + 12, rhSz + 8, (Color){20, 20, 35, (unsigned char)(rhAlpha * 0.7f)});
                    DrawRectangleLinesEx((Rectangle){(float)(rhX - 6), (float)(rhY - 4),
                        (float)(rhW + 12), (float)(rhSz + 8)}, 1, (Color){255, 220, 100, rhAlpha});
                    GameDrawText(rhint, rhX, rhY, rhSz, (Color){255, 230, 120, rhAlpha});
                }

                // Shop ability cards (3 slots, centered)
                int shopCardW = S(160);
                int shopCardH = S(38);
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
                        int shopFontSz = S(14);
                        int snw = GameMeasureText(sname, shopFontSz);
                        if (canAfford) {
                            GameDrawTextOnColor(sname, scx + (shopCardW - snw)/2, scy + (shopCardH - shopFontSz)/2, shopFontSz, cardBg);
                        } else {
                            GameDrawText(sname, scx + (shopCardW - snw)/2, scy + (shopCardH - shopFontSz)/2, shopFontSz, (Color){100,100,120,255});
                        }
                    } else {
                        int shopFontSz = S(14);
                        DrawRectangle(scx, scy, shopCardW, shopCardH, (Color){35,35,45,255});
                        DrawRectangleLines(scx, scy, shopCardW, shopCardH, (Color){60,60,80,255});
                        GameDrawText("SOLD", scx + (shopCardW - GameMeasureText("SOLD",shopFontSz))/2,
                                scy + (shopCardH - shopFontSz)/2, shopFontSz, (Color){60,60,80,255});
                    }
                    // Keybind indicator
                    const char *keyLabel = TextFormat("[%d]", s + 1);
                    GameDrawText(keyLabel, scx + 2, scy + 2, S(12), (Color){255, 255, 220, 240});
                }

                // Gold display (right side)
                const char *goldText = TextFormat("Gold: %d", playerGold);
                int gw = GameMeasureText(goldText, S(20));
                GameDrawText(goldText, hudSw - gw - 20, shopY + 16, S(20), (Color){ 240, 200, 60, 255 });

                // Hotkey hint (first round only, until player uses a hotkey)
                if (currentRound == 0 && !usedShopHotkey) {
                    const char *hint = "Press [1] [2] [3] to quick-buy!";
                    int hintSz = S(14);
                    int hintW = GameMeasureText(hint, hintSz);
                    int hintX = (hudSw - hintW) / 2;
                    int hintY = shopY - hintSz - S(8);
                    // Pulsing glow
                    float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 3.0f);
                    unsigned char hintAlpha = (unsigned char)(160 + (int)(pulse * 95));
                    DrawRectangle(hintX - 8, hintY - 4, hintW + 16, hintSz + 8, (Color){20, 20, 35, (unsigned char)(hintAlpha * 0.7f)});
                    DrawRectangleLinesEx((Rectangle){(float)(hintX - 8), (float)(hintY - 4),
                        (float)(hintW + 16), (float)(hintSz + 8)}, 1, (Color){255, 220, 100, hintAlpha});
                    GameDrawText(hint, hintX, hintY, hintSz, (Color){255, 230, 120, hintAlpha});
                }
            }
        }

        // --- Confirm removal popup (drawn on top of everything) ---
        if (removeConfirmUnit >= 0 && phase == PHASE_PREP) {
            int sw2 = GetScreenWidth(), sh2 = GetScreenHeight();
            DrawRectangle(0, 0, sw2, sh2, (Color){ 0, 0, 0, 120 }); // dim overlay
            int popW = 280, popH = 110;
            int popX = sw2 / 2 - popW / 2;
            int popY = sh2 / 2 - popH / 2;
            DrawRectangle(popX, popY, popW, popH, (Color){ 40, 40, 55, 240 });
            DrawRectangleLinesEx((Rectangle){ (float)popX, (float)popY, (float)popW, (float)popH },
                                2, (Color){ 180, 60, 60, 255 });
            const char *confirmText = "Remove this unit?";
            int ctw = GameMeasureText(confirmText, 20);
            GameDrawText(confirmText, popX + (popW - ctw) / 2, popY + 14, 20, WHITE);
            // Abilities returned note
            const char *noteText = "(abilities stay on figurine)";
            int ntw = GameMeasureText(noteText, 12);
            GameDrawText(noteText, popX + (popW - ntw) / 2, popY + 40, 12, (Color){160,160,180,255});
            // Yes / No buttons
            int rmBtnW = 100, rmBtnH = 30;
            Rectangle yesBtn = { (float)(popX + 24), (float)(popY + popH - rmBtnH - 12), (float)rmBtnW, (float)rmBtnH };
            Rectangle noBtn  = { (float)(popX + popW - rmBtnW - 24), (float)(popY + popH - rmBtnH - 12), (float)rmBtnW, (float)rmBtnH };
            Color yesBg = (Color){ 180, 50, 50, 255 };
            Color noBg  = (Color){ 60, 60, 80, 255 };
            if (CheckCollisionPointRec(GetMousePosition(), yesBtn)) yesBg = (Color){ 230, 70, 70, 255 };
            if (CheckCollisionPointRec(GetMousePosition(), noBtn))  noBg  = (Color){ 80, 80, 110, 255 };
            DrawRectangleRec(yesBtn, yesBg);
            DrawRectangleRec(noBtn, noBg);
            DrawRectangleLinesEx(yesBtn, 1, (Color){120,40,40,255});
            DrawRectangleLinesEx(noBtn, 1, (Color){80,80,100,255});
            int yw = GameMeasureText("YES", 16), nw = GameMeasureText("NO", 16);
            GameDrawText("YES", (int)(yesBtn.x + (rmBtnW - yw) / 2), (int)(yesBtn.y + 7), 16, WHITE);
            GameDrawText("NO",  (int)(noBtn.x + (rmBtnW - nw) / 2), (int)(noBtn.y + 7), 16, WHITE);
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

            int tipW = S(300);
            int tipH = S(50) + numStatLines * S(18);
            int tipX = (int)mpos.x + 14;
            int tipY = (int)mpos.y - tipH - 4;
            if (tipX + tipW > GetScreenWidth()) tipX = (int)mpos.x - tipW - 4;
            if (tipY < 0) tipY = (int)mpos.y + 20;
            DrawRectangle(tipX, tipY, tipW, tipH, (Color){20, 20, 30, 230});
            DrawRectangleLines(tipX, tipY, tipW, tipH, (Color){100, 100, 130, 255});
            GameDrawText(tipDef->name, tipX + S(6), tipY + S(4), S(16), WHITE);
            const char *lvlText = TextFormat("Lvl:%d/%d", hoverAbilityLevel + 1, ABILITY_MAX_LEVELS);
            int lvlW = GameMeasureText(lvlText, S(12));
            GameDrawText(lvlText, tipX + tipW - lvlW - S(6), tipY + S(6), S(12), (Color){180, 180, 200, 255});
            GameDrawText(tipDef->description, tipX + S(6), tipY + S(22), S(12), (Color){180, 180, 200, 255});

            Color dimStatColor = { 100, 100, 120, 255 };
            // Rolling 3-window: show 3 levels centered on current
            int winStart = hoverAbilityLevel <= 0 ? 0
                         : (hoverAbilityLevel >= ABILITY_MAX_LEVELS - 1 ? ABILITY_MAX_LEVELS - 3
                         : hoverAbilityLevel - 1);
            if (winStart < 0) winStart = 0;
            int winEnd = winStart + 3;
            if (winEnd > ABILITY_MAX_LEVELS) winEnd = ABILITY_MAX_LEVELS;

            int lineY = tipY + S(40);
            for (int sl = 0; sl < numStatLines; sl++) {
                int lx = tipX + S(6);
                if (sl == cdLineIdx) {
                    // Cooldown line
                    const char *cdLabel = "CD: ";
                    GameDrawText(cdLabel, lx, lineY, S(12), (Color){180,180,200,255});
                    lx += GameMeasureText(cdLabel, S(12));
                    for (int lv = winStart; lv < winEnd; lv++) {
                        const char *val = TextFormat("%.1fs", tipDef->cooldown[lv]);
                        Color vc = (lv == hoverAbilityLevel) ? WHITE : dimStatColor;
                        GameDrawText(val, lx, lineY, S(12), vc);
                        lx += GameMeasureText(val, S(12));
                        if (lv < winEnd - 1) {
                            GameDrawText(" / ", lx, lineY, S(12), dimStatColor);
                            lx += GameMeasureText(" / ", S(12));
                        }
                    }
                } else {
                    // Stat value line
                    char labelBuf[32];
                    snprintf(labelBuf, sizeof(labelBuf), "%s: ", statLines[sl].label);
                    GameDrawText(labelBuf, lx, lineY, S(12), (Color){180,180,200,255});
                    lx += GameMeasureText(labelBuf, S(12));
                    for (int lv = winStart; lv < winEnd; lv++) {
                        float v = tipDef->values[lv][statLines[sl].valueIndex];
                        const char *val;
                        if (statLines[sl].isPercent)
                            val = TextFormat("%.0f%%", v * 100.0f);
                        else if (v == (int)v)
                            val = TextFormat("%.0f", v);
                        else
                            val = TextFormat("%.1f", v);
                        Color vc = (lv == hoverAbilityLevel) ? WHITE : dimStatColor;
                        GameDrawText(val, lx, lineY, S(12), vc);
                        lx += GameMeasureText(val, S(12));
                        if (lv < winEnd - 1) {
                            GameDrawText(" / ", lx, lineY, S(12), dimStatColor);
                            lx += GameMeasureText(" / ", S(12));
                        }
                    }
                }
                lineY += S(18);
            }
        }

        // --- Synergy hover tooltip timer + drawing ---
        if (hoverSynergyIdx >= 0 && hoverSynergyIdx == prevHoverSynergyIdx)
            hoverSynergyTimer += dt;
        else if (hoverSynergyIdx >= 0)
            hoverSynergyTimer = dt;
        else
            hoverSynergyTimer = 0.0f;

        if (hoverSynergyIdx >= 0 && hoverSynergyIdx < (int)SYNERGY_COUNT
            && hoverSynergyTimer >= synergyTooltipDelay) {
            const SynergyDef *syn = &SYNERGY_DEFS[hoverSynergyIdx];
            Vector2 mpos = GetMousePosition();

            // Count matching blue units for the tooltip
            int synMatch = 0;
            if (syn->requireAllTypes) {
                bool tp[4] = {0};
                for (int i = 0; i < unitCount; i++) {
                    if (!units[i].active || units[i].team != TEAM_BLUE) continue;
                    for (int r = 0; r < syn->requiredTypeCount; r++)
                        if (units[i].typeIndex == syn->requiredTypes[r])
                            tp[r] = true;
                }
                for (int r = 0; r < syn->requiredTypeCount; r++)
                    if (tp[r]) synMatch++;
            } else {
                for (int i = 0; i < unitCount; i++) {
                    if (!units[i].active || units[i].team != TEAM_BLUE) continue;
                    for (int r = 0; r < syn->requiredTypeCount; r++) {
                        if (units[i].typeIndex == syn->requiredTypes[r]) {
                            synMatch++;
                            break;
                        }
                    }
                }
            }

            // Find current tier
            int curTier = -1;
            for (int t = 0; t < syn->tierCount; t++)
                if (synMatch >= syn->tiers[t].minUnits) curTier = t;

            // Next tier threshold
            int nextThresh = 0;
            if (curTier + 1 < syn->tierCount)
                nextThresh = syn->tiers[curTier + 1].minUnits;

            // Build tooltip content
            const char *tierLabel = (curTier >= 0) ? TextFormat("%s %s", syn->name,
                (curTier == 0) ? "I" : (curTier == 1) ? "II" : "III") : syn->name;
            const char *bonusText = (curTier >= 0 && syn->buffDesc[curTier])
                ? syn->buffDesc[curTier] : "Inactive";
            const char *countText;
            if (syn->requireAllTypes)
                countText = TextFormat("%d/%d types", synMatch, syn->requiredTypeCount);
            else {
                int maxNeeded = syn->tiers[syn->tierCount - 1].minUnits;
                countText = TextFormat("%d/%d %s", synMatch, maxNeeded,
                    (syn->requiredTypeCount == 1) ? GetUnitTypeName(syn->requiredTypes[0]) : "units");
            }

            int tipW = 180;
            int tipH = 52;
            if (nextThresh > 0) tipH += 14;
            int tipX = (int)mpos.x + 14;
            int tipY = (int)mpos.y - tipH - 4;
            if (tipX + tipW > GetScreenWidth()) tipX = (int)mpos.x - tipW - 4;
            if (tipY < 0) tipY = (int)mpos.y + 20;
            DrawRectangle(tipX, tipY, tipW, tipH, (Color){20, 20, 30, 230});
            DrawRectangleLines(tipX, tipY, tipW, tipH, syn->color);
            GameDrawText(tierLabel, tipX + 6, tipY + 4, 12, WHITE);
            GameDrawText(bonusText, tipX + 6, tipY + 20, 10, (Color){200, 200, 220, 220});
            GameDrawText(countText, tipX + 6, tipY + 36, 10, (Color){160, 160, 180, 200});
            if (nextThresh > 0) {
                const char *nextText = TextFormat("Next: %d for tier %s",
                    nextThresh, (curTier + 1 == 1) ? "II" : "III");
                GameDrawText(nextText, tipX + 6, tipY + 50, 9, (Color){120, 120, 140, 180});
            }
        }

        //==============================================================================
        // PHASE_PLAZA DRAWING (2D overlays on top of the 3D world)
        //==============================================================================
        if (phase == PHASE_PLAZA)
        {
            int msw = GetScreenWidth();
            int msh = GetScreenHeight();

            // Title text (floating over the 3D scene)
            const char *title = "Relic Rivals";
            int titleSize = 72;
            int tw = GameMeasureText(title, titleSize);
            GameDrawText(title, msw/2 - tw/2, 60, titleSize, (Color){200, 180, 255, 220});

            const char *subtitle = "Scan a figure to begin";
            int subSize = 32;
            int sw2 = GameMeasureText(subtitle, subSize);
            GameDrawText(subtitle, msw/2 - sw2/2, 140, subSize, (Color){160,140,200,160});

            // Draw floating 2D labels above 3D objects
            {
                Vector2 trophyScreen = GetWorldToScreen((Vector3){trophyPos.x, trophyPos.y + 14.0f, trophyPos.z}, camera);
                const char *tLabel = "LEADERBOARD";
                int tlw = GameMeasureText(tLabel, 14);
                Color tlCol = (plazaHoverObject == 1) ? YELLOW : (Color){200,200,220,200};
                GameDrawText(tLabel, (int)trophyScreen.x - tlw/2, (int)trophyScreen.y, 14, tlCol);

                Vector2 doorScreen = GetWorldToScreen((Vector3){doorPos.x, doorPos.y + 18.0f, doorPos.z}, camera);
                const char *dLabel = "MULTIPLAYER";
                int dlw = GameMeasureText(dLabel, 14);
                Color dlCol = (plazaHoverObject == 2) ? YELLOW : (Color){200,200,220,200};
                GameDrawText(dLabel, (int)doorScreen.x - dlw/2, (int)doorScreen.y, 14, dlCol);
            }

            // Leaderboard overlay (reused from old menu)
            if (showLeaderboard)
            {
                DrawRectangle(0, 0, msw, msh, (Color){0,0,0,180});

                int panelW = 600, panelH = 500;
                int panelX = msw/2 - panelW/2;
                int panelY = msh/2 - panelH/2;
                DrawRectangle(panelX, panelY, panelW, panelH, (Color){24,24,32,240});
                DrawRectangleLinesEx((Rectangle){(float)panelX,(float)panelY,(float)panelW,(float)panelH}, 2, (Color){100,100,130,255});

                const char *lbTitle = "LEADERBOARD";
                int ltw = GameMeasureText(lbTitle, 24);
                GameDrawText(lbTitle, panelX + panelW/2 - ltw/2, panelY + 10, 24, GOLD);

                Rectangle closeBtn = { (float)(panelX + panelW - 40), (float)panelY, 40, 40 };
                Color closeBg = (Color){180,50,50,200};
                if (CheckCollisionPointRec(GetMousePosition(), closeBtn))
                    closeBg = (Color){230,70,70,255};
                DrawRectangleRec(closeBtn, closeBg);
                int xw = GameMeasureText("X", 18);
                GameDrawText("X", (int)(closeBtn.x + 20 - xw/2), (int)(closeBtn.y + 11), 18, WHITE);

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

                    const char *rankText = TextFormat("#%d", e + 1);
                    GameDrawText(rankText, panelX + 12, rowY + 8, 20, GOLD);
                    const char *roundText = TextFormat("Wave %d", le->highestRound);
                    GameDrawText(roundText, panelX + 60, rowY + 8, 18, WHITE);
                    GameDrawText(le->playerName, panelX + 180, rowY + 8, 16, (Color){180,180,200,255});

                    int ux = panelX + 180;
                    int uy = rowY + 32;
                    for (int u = 0; u < le->unitCount && u < BLUE_TEAM_MAX_SIZE; u++) {
                        SavedUnit *su = &le->units[u];
                        const char *uname = (su->typeIndex < unitTypeCount) ? unitTypes[su->typeIndex].name : "???";
                        GameDrawText(uname, ux, uy, 12, (Color){150,180,255,255});
                        int nameW = GameMeasureText(uname, 12);
                        int gridX = ux + nameW + 6;
                        int miniSize = 14, miniGap = 2;
                        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                            int col = a % 2, row = a / 2;
                            int ax = gridX + col * (miniSize + miniGap);
                            int ay = uy + row * (miniSize + miniGap) - 4;
                            if (su->abilities[a].abilityId >= 0 && su->abilities[a].abilityId < ABILITY_COUNT) {
                                DrawRectangle(ax, ay, miniSize, miniSize, ABILITY_DEFS[su->abilities[a].abilityId].color);
                                const char *abbr = ABILITY_DEFS[su->abilities[a].abilityId].abbrev;
                                GameDrawText(abbr, ax + 1, ay + 2, 7, WHITE);
                            } else {
                                DrawRectangle(ax, ay, miniSize, miniSize, (Color){40,40,55,255});
                            }
                        }
                        ux += nameW + 6 + 2 * (14 + 2) + 12;
                    }
                }
                EndScissorMode();

                if (leaderboard.entryCount == 0) {
                    const char *emptyText = "No entries yet - play and Set in Stone!";
                    int etw = GameMeasureText(emptyText, 16);
                    GameDrawText(emptyText, panelX + panelW/2 - etw/2, panelY + panelH/2, 16, (Color){100,100,120,255});
                }
            }

            // Multiplayer panel overlay
            if (showMultiplayerPanel)
            {
                DrawRectangle(0, 0, msw, msh, (Color){0,0,0,140});

                int panelW = 400, panelH = 300;
                int panelX = msw/2 - panelW/2;
                int panelY = msh/2 - panelH/2;
                DrawRectangle(panelX, panelY, panelW, panelH, (Color){24,24,32,240});
                DrawRectangleLinesEx((Rectangle){(float)panelX,(float)panelY,(float)panelW,(float)panelH}, 2, (Color){100,100,130,255});

                const char *mpTitle = "MULTIPLAYER";
                int mptw = GameMeasureText(mpTitle, 24);
                GameDrawText(mpTitle, panelX + panelW/2 - mptw/2, panelY + 10, 24, (Color){200,180,255,255});

                // Close button
                Rectangle closeBtn = { (float)(panelX + panelW - 36), (float)(panelY + 4), 32, 32 };
                Color closeBg = (Color){180,50,50,200};
                if (CheckCollisionPointRec(GetMousePosition(), closeBtn))
                    closeBg = (Color){230,70,70,255};
                DrawRectangleRec(closeBtn, closeBg);
                GameDrawText("X", (int)(closeBtn.x + 10), (int)(closeBtn.y + 7), 18, WHITE);

                // Name input
                GameDrawText("Player Name:", panelX + 50, panelY + 45, 14, (Color){180,180,200,255});
                Rectangle nameField = { (float)(panelX + 50), (float)(panelY + 60), (float)(panelW - 100), 36 };
                Color nameBg = nameInputActive ? (Color){50,50,70,255} : (Color){35,35,50,255};
                DrawRectangleRec(nameField, nameBg);
                DrawRectangleLinesEx(nameField, 2, nameInputActive ? (Color){150,140,200,255} : (Color){80,80,100,255});
                GameDrawText(playerName, panelX + 58, panelY + 69, 18, WHITE);
                if (nameInputActive) {
                    float blinkTime = (float)GetTime();
                    if ((int)(blinkTime * 2) % 2 == 0) {
                        int cw = GameMeasureText(playerName, 18);
                        DrawRectangle(panelX + 58 + cw + 2, panelY + 69, 2, 18, WHITE);
                    }
                }

                // CREATE LOBBY button
                Rectangle createBtn = { (float)(panelX + 50), (float)(panelY + 120), (float)(panelW - 100), 40 };
                Color cBg = (Color){40,130,60,255};
                if (CheckCollisionPointRec(GetMousePosition(), createBtn)) cBg = (Color){50,170,70,255};
                DrawRectangleRec(createBtn, cBg);
                DrawRectangleLinesEx(createBtn, 2, (Color){30,100,40,255});
                const char *cText = "CREATE LOBBY";
                int ctw = GameMeasureText(cText, 16);
                GameDrawText(cText, (int)(createBtn.x + (panelW-100)/2 - ctw/2), (int)(createBtn.y + 12), 16, WHITE);

                // JOIN LOBBY button
                bool codeReady = (joinCodeLen == LOBBY_CODE_LEN);
                Rectangle joinBtn = { (float)(panelX + 50), (float)(panelY + 180), (float)(panelW - 100), 40 };
                Color jBg = codeReady ? (Color){160,100,30,255} : (Color){80,80,80,255};
                if (codeReady && CheckCollisionPointRec(GetMousePosition(), joinBtn)) jBg = (Color){200,130,40,255};
                DrawRectangleRec(joinBtn, jBg);
                DrawRectangleLinesEx(joinBtn, 2, (Color){100,70,20,255});
                const char *jText = "JOIN LOBBY";
                int jtw = GameMeasureText(jText, 16);
                GameDrawText(jText, (int)(joinBtn.x + (panelW-100)/2 - jtw/2), (int)(joinBtn.y + 12), 16, WHITE);

                // Lobby code input
                GameDrawText("Lobby Code:", panelX + 50, panelY + 230, 12, (Color){150,150,170,255});
                Rectangle codeBox = { (float)(panelX + 50), (float)(panelY + 248), 120, 30 };
                DrawRectangleRec(codeBox, (Color){35,35,50,255});
                DrawRectangleLinesEx(codeBox, 2, (Color){80,80,100,255});
                char codeBuf[8];
                snprintf(codeBuf, sizeof(codeBuf), "%s_", joinCodeInput);
                GameDrawText(codeBuf, panelX + 58, panelY + 254, 18, WHITE);

                // Error message
                if (menuError[0]) {
                    int ew = GameMeasureText(menuError, 12);
                    GameDrawText(menuError, panelX + panelW/2 - ew/2, panelY + panelH - 20, 12, RED);
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
            int wtw = GameMeasureText(waitText, 30);
            GameDrawText(waitText, lsw/2 - wtw/2, lsh/2 - 60, 30, (Color){200, 180, 255, 255});

            // Show lobby code
            if (netClient.lobbyCode[0]) {
                const char *codeLabel = "Share this code:";
                int clw = GameMeasureText(codeLabel, 16);
                GameDrawText(codeLabel, lsw/2 - clw/2, lsh/2, 16, (Color){150,150,170,255});
                int ccw = GameMeasureText(netClient.lobbyCode, 40);
                GameDrawText(netClient.lobbyCode, lsw/2 - ccw/2, lsh/2 + 25, 40, WHITE);
            }

            // Animated dots
            int dots = (int)(GetTime() * 2) % 4;
            char dotBuf[8] = "";
            for (int d = 0; d < dots; d++) strcat(dotBuf, ".");
            GameDrawText(dotBuf, lsw/2 + wtw/2 + 5, lsh/2 - 60, 30, WHITE);

            const char *escText = "Press ESC to cancel";
            int ew = GameMeasureText(escText, 14);
            GameDrawText(escText, lsw/2 - ew/2, lsh/2 + 90, 14, (Color){100,100,120,255});
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
            const char *msTitle = TextFormat("MILESTONE - Wave %d", currentRound);
            int mstw = GameMeasureText(msTitle, 40);
            GameDrawText(msTitle, msw/2 - mstw/2, 30, 40, GOLD);

            const char *msSubtitle = "Immortalise your party, or gamble their fate?";
            int mssw = GameMeasureText(msSubtitle, 22);
            GameDrawText(msSubtitle, msw/2 - mssw/2, 78, 22, (Color){220,220,240,220});

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
                    Rectangle srcRect = { 0, 0, (float)hudPortraitSize, -(float)hudPortraitSize };
                    Rectangle dstRect = { (float)(cx + 10), (float)(cardY + 10), (float)portSize, (float)portSize };
                    DrawTexturePro(portraits[h].texture, srcRect, dstRect, (Vector2){0,0}, 0.0f, WHITE);
                    DrawRectangleLines(cx + 10, cardY + 10, portSize, portSize, (Color){60,60,80,255});
                }

                // Unit name
                GameDrawText(type->name, cx + 10, cardY + 96, 14, (Color){200,200,220,255});

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
                        int aw = GameMeasureText(abbr, 10);
                        GameDrawText(abbr, ax + (slotSize - aw)/2, ay + (slotSize - 10)/2, 10, WHITE);
                        const char *lvl = TextFormat("L%d", aslot->level + 1);
                        GameDrawText(lvl, ax + 2, ay + slotSize - 8, 7, (Color){220,220,220,200});
                    } else {
                        DrawRectangle(ax, ay, slotSize, slotSize, (Color){40,40,55,255});
                    }
                    DrawRectangleLines(ax, ay, slotSize, slotSize, (Color){90,90,110,255});
                }
            }

            // Buttons (two: SET IN STONE, CONTINUE)
            int btnW2 = 240, btnH2 = 54;
            int btnY2 = cardY + cardH + 30;
            int btnGap2 = 40;
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
                int setW = GameMeasureText(setText, 22);
                GameDrawText(setText, (int)(setBtn.x + btnW2/2 - setW/2), (int)(setBtn.y + 16), 22, WHITE);

                // Description under SET IN STONE
                const char *setDesc1 = "Save your party to the leaderboard.";
                int sd1w = GameMeasureText(setDesc1, 16);
                GameDrawText(setDesc1, (int)(setBtn.x + btnW2/2 - sd1w/2), (int)(setBtn.y + btnH2 + 8), 16, (Color){255,210,80,230});
                const char *setDesc2 = "Your creatures are imprisoned forever.";
                int sd2w = GameMeasureText(setDesc2, 14);
                GameDrawText(setDesc2, (int)(setBtn.x + btnW2/2 - sd2w/2), (int)(setBtn.y + btnH2 + 28), 14, (Color){255,180,60,180});
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
                int contW = GameMeasureText(contText, 22);
                GameDrawText(contText, (int)(contBtn.x + btnW2/2 - contW/2), (int)(contBtn.y + 16), 22, WHITE);

                // Description under CONTINUE
                const char *contDesc1 = "Keep fighting. Higher risk, higher glory.";
                int cd1w = GameMeasureText(contDesc1, 16);
                GameDrawText(contDesc1, (int)(contBtn.x + btnW2/2 - cd1w/2), (int)(contBtn.y + btnH2 + 8), 16, (Color){100,220,120,230});
                const char *contDesc2 = "If you lose, your party dies for nothing!";
                int cd2w = GameMeasureText(contDesc2, 14);
                GameDrawText(contDesc2, (int)(contBtn.x + btnW2/2 - cd2w/2), (int)(contBtn.y + btnH2 + 28), 14, (Color){255,100,80,200});
            }
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
            int gotw = GameMeasureText(goTitle, 36);
            GameDrawText(goTitle, gosw/2 - gotw/2, gosh/2 - 60, 36, GOLD);

            const char *goScore = TextFormat("Score: %d - %d", blueWins, redWins);
            int gsw = GameMeasureText(goScore, 20);
            GameDrawText(goScore, gosw/2 - gsw/2, gosh/2, 20, WHITE);

            const char *goRestart = "Press R to return to menu";
            int grw = GameMeasureText(goRestart, 16);
            GameDrawText(goRestart, gosw/2 - grw/2, gosh/2 + 40, 16, (Color){150,150,170,255});
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
            int gotw = GameMeasureText(goTitle, 36);
            GameDrawText(goTitle, gosw/2 - gotw/2, 40, 36, GOLD);

            const char *goRound = TextFormat("Reached Wave %d  |  Score: %d - %d", currentRound, blueWins, redWins);
            int gorw = GameMeasureText(goRound, 18);
            GameDrawText(goRound, gosw/2 - gorw/2, 85, 18, WHITE);

            // Collect surviving blue units
            int goBlue[BLUE_TEAM_MAX_SIZE]; int goCount = 0;
            for (int i = 0; i < unitCount && goCount < BLUE_TEAM_MAX_SIZE; i++)
                if (units[i].active && units[i].team == TEAM_BLUE) goBlue[goCount++] = i;

            // Check if any NFC units still need withdrawing
            bool hasNfcUnits = false;
            for (int i = 0; i < goCount; i++)
                if (units[goBlue[i]].nfcUidLen > 0) { hasNfcUnits = true; break; }

            // Subtitle
            if (hasNfcUnits) {
                const char *goSub = "Remove all units from sensors before resetting";
                int gosub = GameMeasureText(goSub, 14);
                GameDrawText(goSub, gosw/2 - gosub/2, 115, 14, (Color){255,120,120,220});
            } else if (goCount > 0) {
                const char *goSub = "Withdraw your units or reset";
                int gosub = GameMeasureText(goSub, 14);
                GameDrawText(goSub, gosw/2 - gosub/2, 115, 14, (Color){180,180,200,180});
            } else {
                const char *goSub = "All units have been set in stone!";
                int gosub = GameMeasureText(goSub, 14);
                GameDrawText(goSub, gosw/2 - gosub/2, 115, 14, (Color){180,180,200,180});
            }

            // Unit cards with WITHDRAW button
            int goCardW = 200, goCardH = 140, goCardGap = 20;
            int goTotalW = goCount * goCardW + (goCount > 1 ? (goCount - 1) * goCardGap : 0);
            int goStartX = (gosw - goTotalW) / 2;
            int goCardY = gosh / 2 - 40;

            for (int h = 0; h < goCount; h++) {
                int cx = goStartX + h * (goCardW + goCardGap);
                int ui = goBlue[h];
                UnitType *type = &unitTypes[units[ui].typeIndex];

                DrawRectangle(cx, goCardY, goCardW, goCardH, (Color){35,35,50,240});
                DrawRectangleLinesEx((Rectangle){(float)cx,(float)goCardY,(float)goCardW,(float)goCardH}, 2, (Color){60,60,80,255});

                // Portrait
                if (h < BLUE_TEAM_MAX_SIZE) {
                    int portSize = 80;
                    Rectangle srcRect = { 0, 0, (float)hudPortraitSize, -(float)hudPortraitSize };
                    Rectangle dstRect = { (float)(cx + 10), (float)(goCardY + 6), (float)portSize, (float)portSize };
                    DrawTexturePro(portraits[h].texture, srcRect, dstRect, (Vector2){0,0}, 0.0f, WHITE);
                    DrawRectangleLines(cx + 10, goCardY + 6, portSize, portSize, (Color){60,60,80,255});
                }

                // Unit name
                GameDrawText(type->name, cx + 10, goCardY + 90, 14, (Color){200,200,220,255});

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
                        int aw = GameMeasureText(abbr, 10);
                        GameDrawText(abbr, ax + (goSlotSize - aw)/2, ay + (goSlotSize - 10)/2, 10, WHITE);
                        const char *lvl = TextFormat("L%d", aslot->level + 1);
                        GameDrawText(lvl, ax + 2, ay + goSlotSize - 8, 7, (Color){220,220,220,200});
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
                int wdw = GameMeasureText(wdText, 12);
                GameDrawText(wdText, (int)(wdBtn.x + (goCardW - 20)/2 - wdw/2), (int)(wdBtn.y + 8), 12, WHITE);
            }

            // RESET button (disabled while NFC units remain)
            int resetBtnW = 180, resetBtnH = 44;
            int resetBtnY = goCardY + goCardH + 30;
            Rectangle resetBtn = { (float)(gosw/2 - resetBtnW/2), (float)resetBtnY, (float)resetBtnW, (float)resetBtnH };
            if (hasNfcUnits) {
                DrawRectangleRec(resetBtn, (Color){60,50,50,255});
                DrawRectangleLinesEx(resetBtn, 2, (Color){80,60,60,255});
                const char *resetText = "RESET";
                int rstw = GameMeasureText(resetText, 18);
                GameDrawText(resetText, (int)(resetBtn.x + resetBtnW/2 - rstw/2), (int)(resetBtn.y + 13), 18, (Color){100,90,90,255});
            } else {
                Color resetBg = (Color){180,50,50,255};
                if (CheckCollisionPointRec(GetMousePosition(), resetBtn))
                    resetBg = (Color){220,70,70,255};
                DrawRectangleRec(resetBtn, resetBg);
                DrawRectangleLinesEx(resetBtn, 2, (Color){120,40,40,255});
                const char *resetText = "RESET";
                int rstw = GameMeasureText(resetText, 18);
                GameDrawText(resetText, (int)(resetBtn.x + resetBtnW/2 - rstw/2), (int)(resetBtn.y + 13), 18, WHITE);
            }
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
            } else if (intro.typeIndex == 3) {
                // Puppycat: warm pink
                DrawRectangle(0, 0, wipeW, ish, (Color){ 50, 25, 40, alpha });
                for (int ring = 0; ring < 8; ring++) {
                    float radius = 100.0f + ring * 80.0f;
                    unsigned char ra = (unsigned char)(alpha * 0.3f);
                    DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                        (Color){ (unsigned char)(180 + ring*6), (unsigned char)(80 + ring*5), (unsigned char)(140 + ring*4), ra });
                }
                for (int ln = 0; ln < 12; ln++) {
                    int y = (ish / 12) * ln;
                    DrawLine(0, y, wipeW, y - 30,
                        (Color){ 200, 100, 160, (unsigned char)(alpha * 0.15f) });
                }
            } else if (intro.typeIndex == 4) {
                // Siren: deep ocean
                DrawRectangle(0, 0, wipeW, ish, (Color){ 15, 25, 50, alpha });
                for (int ring = 0; ring < 8; ring++) {
                    float radius = 100.0f + ring * 80.0f;
                    unsigned char ra = (unsigned char)(alpha * 0.3f);
                    DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                        (Color){ (unsigned char)(40 + ring*5), (unsigned char)(120 + ring*8), (unsigned char)(180 + ring*6), ra });
                }
                for (int ln = 0; ln < 15; ln++) {
                    int y = (ish / 15) * ln;
                    DrawLine(0, y + 80, wipeW, y - 80,
                        (Color){ 60, 140, 200, (unsigned char)(alpha * 0.15f) });
                }
            } else {
                // Default: dark crimson
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
            // Determine display name: custom name if set, class name as fallback
            const char *className = unitTypes[intro.typeIndex].name;
            bool hasCustomName = (intro.unitIndex >= 0 && intro.unitIndex < unitCount &&
                                  units[intro.unitIndex].nfcName[0] != '\0');
            const char *introName = hasCustomName ? units[intro.unitIndex].nfcName : className;
            int nameFontSize = ish / 8;
            int nameW = GameMeasureText(introName, nameFontSize);
            float nameFinalX = isw * 0.08f;
            float nameStartX = (float)(-nameW - 20);
            float nameX = nameStartX + (nameFinalX - nameStartX) * textSlide;
            float nameY = ish * 0.2f;

            // Shadow
            GameDrawText(introName, (int)nameX + 3, (int)nameY + 3, nameFontSize,
                (Color){ 0, 0, 0, (unsigned char)(alpha * 0.6f) });
            // Main text
            Color nameColor = GetTeamTint(TEAM_BLUE);
            nameColor.a = alpha;
            GameDrawText(introName, (int)nameX, (int)nameY, nameFontSize, nameColor);

            // Subtitle: class name if custom named, otherwise "joins the battle!"
            int subSize = nameFontSize / 3;
            if (subSize < 12) subSize = 12;
            const char *subText = hasCustomName
                ? TextFormat("%s joins the battle!", className)
                : "joins the battle!";
            GameDrawText(subText, (int)nameX + 4, (int)nameY + nameFontSize + 4, subSize,
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
                        int aw = GameMeasureText(abbr, 16);
                        GameDrawText(abbr, ax + (slotSize - aw)/2,
                            abilY + (slotSize - 16)/2, 16, (Color){ 255, 255, 255, aa });
                        // Level
                        const char *lvl = TextFormat("L%d", slot->level + 1);
                        GameDrawText(lvl, ax + 2, abilY + slotSize - 10, 8, (Color){ 220, 220, 220, aa });
                    } else {
                        DrawRectangle(ax, abilY, slotSize, slotSize, (Color){ 40, 40, 55, aa });
                        const char *q = "?";
                        int qw = GameMeasureText(q, 22);
                        GameDrawText(q, ax + (slotSize - qw)/2,
                            abilY + (slotSize - 22)/2, 22, (Color){ 80, 80, 100, aa });
                    }
                    DrawRectangleLines(ax, abilY, slotSize, slotSize,
                        (Color){ 120, 120, 150, aa });
                }
            }
        }

        // Shadow debug overlay
        if (shadowDebugMode > 0) {
            const char *modeNames[] = { "", "Shadow Factor", "Light Depth", "Light UV", "Sampled Depth" };
            GameDrawText(TextFormat("[F10] Shadow Debug: %d - %s", shadowDebugMode, modeNames[shadowDebugMode]),
                10, GetScreenHeight() - 30, 20, YELLOW);
            // Draw shadow map depth as small preview in corner
            float previewSize = 256.0f;
            Rectangle srcRec = { 0, 0, (float)SHADOW_MAP_SIZE, -(float)SHADOW_MAP_SIZE };
            Rectangle dstRec = { GetScreenWidth() - previewSize - 10, 10, previewSize, previewSize };
            DrawTexturePro(shadowRT.texture, srcRec, dstRec, (Vector2){0,0}, 0.0f, WHITE);
            DrawRectangleLines((int)dstRec.x, (int)dstRec.y, (int)previewSize, (int)previewSize, YELLOW);
            GameDrawText("Shadow Color RT", (int)dstRec.x, (int)dstRec.y + (int)previewSize + 4, 16, YELLOW);
        }

        // Naming prompt overlay
        if (namingUnitIndex >= 0) {
            // Handle text input
            int key = GetCharPressed();
            while (key > 0) {
                if (key >= 32 && key <= 126 && namingPos < 30) {
                    namingBuf[namingPos++] = (char)key;
                    namingBuf[namingPos] = '\0';
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && namingPos > 0) {
                namingBuf[--namingPos] = '\0';
            }
            if (IsKeyPressed(KEY_ENTER) && namingPos > 0) {
                // Save name to unit (explicit copy + null terminate)
                int ni = namingUnitIndex;
                if (ni >= 0 && ni < unitCount) {
                    memcpy(units[ni].nfcName, namingBuf, namingPos);
                    units[ni].nfcName[namingPos] = '\0';
                    printf("[NFC] Named unit %d: \"%s\" (nfcName set to \"%s\")\n", ni, namingBuf, units[ni].nfcName);
                    // Persist to server
                    if (units[ni].nfcUidLen > 0) {
                        net_nfc_set_name(serverHost, NET_PORT,
                            units[ni].nfcUid, units[ni].nfcUidLen, namingBuf);
                    }
                    // Start intro cutscene now that naming is done
                    intro = (UnitIntro){ .active = true, .timer = 0.0f,
                        .typeIndex = units[ni].typeIndex, .unitIndex = ni, .animFrame = 0 };
                }
                namingUnitIndex = -1;
            }
            // Draw overlay
            int sw = GetScreenWidth(), sh = GetScreenHeight();
            DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, 120});
            int boxW = S(400), boxH = S(80);
            int boxX = (sw - boxW) / 2, boxY = (sh - boxH) / 2;
            DrawRectangle(boxX, boxY, boxW, boxH, (Color){30, 30, 45, 240});
            DrawRectangleLinesEx((Rectangle){(float)boxX, (float)boxY, (float)boxW, (float)boxH}, 2, (Color){100, 200, 100, 255});
            const char *prompt = "Name your creature:";
            int promptW = GameMeasureText(prompt, S(18));
            GameDrawText(prompt, (sw - promptW) / 2, boxY + S(8), S(18), WHITE);
            // Input field
            int fieldW = boxW - S(40), fieldH = S(28);
            int fieldX = boxX + S(20), fieldY = boxY + S(38);
            DrawRectangle(fieldX, fieldY, fieldW, fieldH, (Color){50, 50, 70, 255});
            DrawRectangleLines(fieldX, fieldY, fieldW, fieldH, (Color){100, 200, 100, 255});
            if (namingPos > 0) {
                GameDrawText(namingBuf, fieldX + S(6), fieldY + S(4), S(18), WHITE);
            }
            // Blinking cursor
            if ((int)(GetTime() * 2.0) % 2 == 0) {
                int cursorX = fieldX + S(6) + GameMeasureText(namingBuf, S(18));
                GameDrawText("|", cursorX, fieldY + S(4), S(18), (Color){200, 255, 200, 255});
            }
            GameDrawText("[Enter] Confirm", boxX + S(20), boxY + boxH + S(4), S(12), (Color){160, 160, 180, 200});
        }

        // Easter egg overlay
        if (easterEggTimer > 0.0f) {
            easterEggTimer -= rawDt;
            float alpha = easterEggTimer > 1.0f ? 1.0f : easterEggTimer;
            const char *msg = "hey judges :)";
            int fontSize = 120;
            int w = GameMeasureText(msg, fontSize);
            int x = (GetScreenWidth() - w) / 2;
            int y = (GetScreenHeight() - fontSize) / 2;
            GameDrawText(msg, x + 3, y + 3, fontSize, Fade(BLACK, alpha * 0.5f));
            GameDrawText(msg, x, y, fontSize, Fade(GOLD, alpha));
        }

        // Color grading debug overlay
        if (cgDebugOverlay) {
            int oy = 30;
            DrawRectangle(5, oy - 2, 320, 200, Fade(BLACK, 0.7f));
            DrawText(TextFormat("Color Grade [F6]  1/2:exp 3/4:con 5/6:sat 7/8:temp 9/0:vig"), 10, oy, 10, GREEN);
            oy += 16;
            DrawText(TextFormat("exposure:    %.3f", cgExposure),    10, oy, 10, WHITE); oy += 14;
            DrawText(TextFormat("contrast:    %.3f", cgContrast),    10, oy, 10, WHITE); oy += 14;
            DrawText(TextFormat("saturation:  %.3f", cgSaturation),  10, oy, 10, WHITE); oy += 14;
            DrawText(TextFormat("temperature: %.3f", cgTemperature), 10, oy, 10, WHITE); oy += 14;
            DrawText(TextFormat("vignetteStr: %.3f", cgVignetteStr), 10, oy, 10, WHITE); oy += 14;
            DrawText(TextFormat("vignetteSft: %.3f", cgVignetteSoft),10, oy, 10, WHITE); oy += 14;
            DrawText(TextFormat("lift: %.2f %.2f %.2f", cgLift[0], cgLift[1], cgLift[2]), 10, oy, 10, WHITE); oy += 14;
            DrawText(TextFormat("gain: %.2f %.2f %.2f", cgGain[0], cgGain[1], cgGain[2]), 10, oy, 10, WHITE); oy += 14;
            DrawText("-/=: vignetteSoftness", 10, oy, 10, GRAY);
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
    UnloadRenderTexture(fxaaRT);
    UnloadRenderTexture(colorGradeRT);
    rlUnloadFramebuffer(sceneRT.id);
    rlUnloadTexture(sceneRT.texture.id);
    rlUnloadTexture(sceneRT.depth.id);
    UnloadShader(ssaoShader);
    UnloadShader(fxaaShader);
    UnloadShader(colorGradeShader);
    rlUnloadFramebuffer(shadowRT.id);
    rlUnloadTexture(shadowRT.texture.id);
    rlUnloadTexture(shadowRT.depth.id);
    UnloadShader(shadowDepthShader);
    UnloadTexture(particleTex);
    UnloadShader(lightShader);
    UnloadShader(borderShader);
    UnloadMesh(borderMesh);
    for (int i = 0; i < unitTypeCount; i++) {
        if (unitTypes[i].anims)
            UnloadModelAnimations(unitTypes[i].anims, unitTypes[i].animCount);
        if (unitTypes[i].idleAnims)
            UnloadModelAnimations(unitTypes[i].idleAnims, unitTypes[i].idleAnimCount);
        if (unitTypes[i].scaredAnims)
            UnloadModelAnimations(unitTypes[i].scaredAnims, unitTypes[i].scaredAnimCount);
        if (unitTypes[i].attackAnims)
            UnloadModelAnimations(unitTypes[i].attackAnims, unitTypes[i].attackAnimCount);
        if (unitTypes[i].castAnims)
            UnloadModelAnimations(unitTypes[i].castAnims, unitTypes[i].castAnimCount);
        if (unitTypes[i].loaded) UnloadModel(unitTypes[i].model);
    }
    for (int i = 0; i < TILE_VARIANTS; i++) UnloadModel(tileModels[i]);
    UnloadTexture(tileDiffuse);
    UnloadTexture(tileORM);
    UnloadTexture(tileNormal);
    UnloadModel(doorModel);
    UnloadModel(trophyModel);
    UnloadModel(platformModel);
    UnloadTexture(groundDiffuse);
    UnloadTexture(groundORM);
    UnloadTexture(groundNormal);
    UnloadModel(stairsModel);
    UnloadTexture(stairsDiffuse);
    UnloadTexture(stairsORM);
    UnloadTexture(stairsNormal);
    UnloadModel(circleModel);
    UnloadTexture(circleDiffuse);
    UnloadTexture(circleORM);
    UnloadTexture(circleNormal);
    // Unload env models (skip 2=stairs, 3=circle, 5=ground which alias stairsModel/circleModel/platformModel)
    // Skip textures for 7=PillarSmall which shares textures with 6=PillarBig
    for (int i = 0; i < envModelCount; i++) {
        if (i == 2 || i == 3 || i == 5) continue;  // reused models, already unloaded above
        if (envModels[i].loaded) UnloadModel(envModels[i].model);
        if (i == 4 || i == 7) continue;  // shared textures (FloorTiles=tiles, PillarSmall=PillarBig)
        if (envModels[i].texture.id > 0) UnloadTexture(envModels[i].texture);
        if (envModels[i].ormTexture.id > 0) UnloadTexture(envModels[i].ormTexture);
        if (envModels[i].normalTexture.id > 0) UnloadTexture(envModels[i].normalTexture);
    }
    UnloadTexture(defaultORM);
    UnloadMusicStream(bgm);
    UnloadSound(sfxWin);
    UnloadSound(sfxLoss);
    UnloadSound(sfxMeleeHit);
    UnloadSound(sfxProjectileWhoosh);
    UnloadSound(sfxProjectileHit);
    UnloadSound(sfxMagicHit);
    UnloadSound(sfxToadShout);
    UnloadSound(sfxToadDie);
    UnloadSound(sfxGoblinShout);
    UnloadSound(sfxGoblinDie);
    UnloadSound(sfxCharacterFall);
    UnloadSound(sfxCharacterLand);
    UnloadSound(sfxNewCharacter);
    UnloadSound(sfxUiClick);
    UnloadSound(sfxUiBuy);
    UnloadSound(sfxUiDrag);
    UnloadSound(sfxUiDrop);
    UnloadSound(sfxUiReroll);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
