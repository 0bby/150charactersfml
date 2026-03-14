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
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <libgen.h>
#endif

#define RLIGHTS_IMPLEMENTATION
#include "rlights.h"

#define GLSL_VERSION 330

// --- Color grading tweakable defaults (bright & bubbly) ---
static float cgExposure = 0.95f;
static float cgContrast = 1.20f;
static float cgSaturation = 0.90f;
static float cgTemperature = 0.10f;
static float cgVignetteStr = 0.46f;
static float cgVignetteSoft = 0.94f;
static float cgLift[3] = {0.05f, 0.04f, 0.02f};
static float cgGain[3] = {1.08f, 1.06f, 1.02f};
static bool cgDebugOverlay = false;

#include "combat_sim.h"
#include "game.h"
#include "helpers.h"
#include "items.h"
#include "map.h"
#include "events.h"
#include "synergies.h"

// Must match server's COMBAT_DT exactly (game_session.h)
#define COMBAT_DT (1.0f / 60.0f)
#include "host.h"
#include "leaderboard.h"
#include "net_client.h"

#ifdef USE_EOS
#include "net_eos.h"
#endif

#include "plaza.h"
#include "pve_waves.h"
#include "ui.h"

#include "settings.h"

// --- UI Scale (720p base, computed per-frame in main loop) ---

// --- Hit flash ---
#define HIT_FLASH_DURATION 0.12f

// --- Projectile polish ---
#define PROJ_CHARGE_TIME 0.2f
#define CAST_PAUSE_TIME 0.25f
#define PROJ_TRAIL_LIFE 0.4f
#define PROJ_TRAIL_SIZE 1.0f
#define PROJ_EXPLODE_COUNT 30

// --- Win/loss sound split point (seconds) — tweak & re-split with ffmpeg if
// needed ---
#define ENDGAME_SFX_VOL 0.5f
#define COMBAT_SFX_VOL 0.5f
#define VOICE_SFX_VOL 0.5f
#define SPAWN_SFX_VOL 0.5f
#define UI_SFX_VOL 0.7f
#define BGM_VOL 0.3f

//------------------------------------------------------------------------------------
// Main
//------------------------------------------------------------------------------------
int main(int argc, char *argv[]) {
#ifdef __APPLE__
  // chdir to the executable's directory so relative asset paths work
  // when launched from Finder (which sets cwd to $HOME)
  {
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
      chdir(dirname(path));
    }
  }
#endif
  net_platform_init();
#ifdef USE_EOS
  bool eosAlt = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--eos-alt") == 0)
      eosAlt = true;
  }
  eos_init(eosAlt);
  if (eosAlt) {
    printf("[EOS] Running as ALT instance (fresh device ID)\n");
    fflush(stdout);
  }
#else
  (void)argc;
  (void)argv;
#endif
  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(1280, 720, "Relic Rivals");
  SetExitKey(0); // Disable default ESC-to-close — we handle ESC ourselves
  SetWindowMinSize(640, 360);
  {
    Image icon = LoadImage("assets/ui/game_icon/game_icon.png");
    if (icon.data) {
      SetWindowIcon(icon);
      UnloadImage(icon);
    }
  }
  InitAudioDevice();

  // Load font at large size — bilinear filter handles downscaling
  g_gameFont = LoadFontEx("fonts/old-newspaper-types-font/Oldnewspapertypes-449D.ttf", 128, NULL, 0);
  if (g_gameFont.glyphCount > 0) {
    GenTextureMipmaps(&g_gameFont.texture);
    SetTextureFilter(g_gameFont.texture, TEXTURE_FILTER_TRILINEAR);
    printf("[FONT] Loaded game font (%d glyphs)\n", g_gameFont.glyphCount);
  } else {
    printf("[FONT] Failed to load fonts/game_font.ttf, using default\n");
  }

  // Win/loss sounds — pre-split into separate files
  Sound sfxWin = LoadSound("music/match_win.ogg");
  Sound sfxLoss = LoadSound("music/match_loss.ogg");
  SetSoundVolume(sfxWin, ENDGAME_SFX_VOL);
  SetSoundVolume(sfxLoss, ENDGAME_SFX_VOL);
  bool lastOutcomeWin = false;

  // Combat SFX
  Sound sfxMeleeHit = LoadSound("sfx/melee_hit.ogg");
  Sound sfxProjectileWhoosh = LoadSound("sfx/projectile_whoosh.ogg");
  Sound sfxProjectileHit = LoadSound("sfx/projectile_hit.ogg");
  Sound sfxMagicHit = LoadSound("sfx/magic_hit.ogg");
  SetSoundVolume(sfxMeleeHit, COMBAT_SFX_VOL);
  SetSoundVolume(sfxProjectileWhoosh, COMBAT_SFX_VOL);
  SetSoundVolume(sfxProjectileHit, COMBAT_SFX_VOL);
  SetSoundVolume(sfxMagicHit, COMBAT_SFX_VOL);
  // Unit voice SFX
  Sound sfxToadShout = LoadSound("sfx/toad_shout.ogg");
  Sound sfxToadDie = LoadSound("sfx/toad_die.ogg");
  Sound sfxGoblinShout = LoadSound("sfx/goblin_shout.ogg");
  Sound sfxGoblinDie = LoadSound("sfx/goblin_die.ogg");
  Sound sfxDevilShout = LoadSound("sfx/devil_shout.ogg");
  Sound sfxDevilDie = LoadSound("sfx/devil_die.ogg");
  Sound sfxLizardShout = LoadSound("sfx/lizard_shout.ogg");
  Sound sfxLizardDie = LoadSound("sfx/lizard_die.ogg");
  SetSoundVolume(sfxToadShout, VOICE_SFX_VOL);
  SetSoundVolume(sfxToadDie, VOICE_SFX_VOL);
  SetSoundVolume(sfxGoblinShout, VOICE_SFX_VOL);
  SetSoundVolume(sfxGoblinDie, VOICE_SFX_VOL);
  SetSoundVolume(sfxDevilShout, VOICE_SFX_VOL);
  SetSoundVolume(sfxDevilDie, VOICE_SFX_VOL);
  SetSoundVolume(sfxLizardShout, VOICE_SFX_VOL);
  SetSoundVolume(sfxLizardDie, VOICE_SFX_VOL);
  // Spawn SFX
  Sound sfxCharacterFall = LoadSound("sfx/character_fall.ogg");
  Sound sfxCharacterLand = LoadSound("sfx/character_land.ogg");
  Sound sfxNewCharacter = LoadSound("sfx/new_character.ogg");
  SetSoundVolume(sfxCharacterFall, SPAWN_SFX_VOL);
  SetSoundVolume(sfxCharacterLand, SPAWN_SFX_VOL);
  SetSoundVolume(sfxNewCharacter, SPAWN_SFX_VOL);
  // UI SFX
  Sound sfxUiClick = LoadSound("sfx/ui_click.ogg");
  Sound sfxUiBuy = LoadSound("sfx/ui_buy.ogg");
  Sound sfxUiDrag = LoadSound("sfx/ui_drag.ogg");
  Sound sfxUiDrop = LoadSound("sfx/ui_drop.ogg");
  Sound sfxUiReroll = LoadSound("sfx/ui_reroll.ogg");
  SetSoundVolume(sfxUiClick, UI_SFX_VOL);
  SetSoundVolume(sfxUiBuy, UI_SFX_VOL);
  SetSoundVolume(sfxUiDrag, UI_SFX_VOL);
  SetSoundVolume(sfxUiDrop, UI_SFX_VOL);
  SetSoundVolume(sfxUiReroll, UI_SFX_VOL);

  // SFX arrays for volume control
  Sound allSfx[] = {
      sfxWin,           sfxLoss,      sfxMeleeHit,      sfxProjectileWhoosh,
      sfxProjectileHit, sfxMagicHit,  sfxToadShout,     sfxToadDie,
      sfxGoblinShout,   sfxGoblinDie, sfxDevilShout,    sfxDevilDie,
      sfxLizardShout,   sfxLizardDie, sfxCharacterFall, sfxCharacterLand,
      sfxNewCharacter,  sfxUiClick,   sfxUiBuy,         sfxUiDrag,
      sfxUiDrop,        sfxUiReroll};
  float sfxBaseVol[] = {
      ENDGAME_SFX_VOL, ENDGAME_SFX_VOL, COMBAT_SFX_VOL, COMBAT_SFX_VOL,
      COMBAT_SFX_VOL,  COMBAT_SFX_VOL,  VOICE_SFX_VOL,  VOICE_SFX_VOL,
      VOICE_SFX_VOL,   VOICE_SFX_VOL,   VOICE_SFX_VOL,  VOICE_SFX_VOL,
      VOICE_SFX_VOL,   VOICE_SFX_VOL,   SPAWN_SFX_VOL,  SPAWN_SFX_VOL,
      SPAWN_SFX_VOL,   UI_SFX_VOL,      UI_SFX_VOL,     UI_SFX_VOL,
      UI_SFX_VOL,      UI_SFX_VOL};
  int sfxCount = sizeof(allSfx) / sizeof(allSfx[0]);

  // Voice SFX lookup by unit type index (fallback = toad for types without
  // unique SFX)
  Sound dieSfxByType[MAX_UNIT_TYPES];
  Sound shoutSfxByType[MAX_UNIT_TYPES];
  for (int i = 0; i < MAX_UNIT_TYPES; i++) {
    dieSfxByType[i] = sfxToadDie;
    shoutSfxByType[i] = sfxToadShout;
  }
  dieSfxByType[1] = sfxGoblinDie;
  shoutSfxByType[1] = sfxGoblinShout;
  dieSfxByType[2] = sfxDevilDie;
  shoutSfxByType[2] = sfxDevilShout;
  dieSfxByType[5] = sfxLizardDie;
  shoutSfxByType[5] = sfxLizardShout;

  // Generate radial gradient texture for particle billboards (white center →
  // transparent edge)
  Texture2D particleTex;
#define PARTICLE_TEX_SIZE 32
  {
    Image img = GenImageColor(PARTICLE_TEX_SIZE, PARTICLE_TEX_SIZE, BLANK);
    float half = PARTICLE_TEX_SIZE / 2.0f;
    for (int y = 0; y < PARTICLE_TEX_SIZE; y++) {
      for (int x = 0; x < PARTICLE_TEX_SIZE; x++) {
        float dx = (x + 0.5f - half) / half;
        float dy = (y + 0.5f - half) / half;
        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > 1.0f)
          dist = 1.0f;
        // Additive-friendly: full white center, smooth falloff to 0
        // Brightness stays high so stacked particles blow out to white
        float t = 1.0f - dist;
        float intensity = t * t * t; // cubic falloff - tight bright core
        unsigned char v = (unsigned char)(255.0f * intensity);
        ImageDrawPixel(&img, x, y, (Color){255, 255, 255, v});
      }
    }
    particleTex = LoadTextureFromImage(img);
    UnloadImage(img);
  }

  // Default 1x1 ORM texture for models without ORM files.
  // (R=255,G=128,B=0) = AO=1.0, Roughness~0.5, Metallic=0.0 — preserves current
  // look.
  Texture2D defaultORM;
  {
    Image ormImg = GenImageColor(1, 1, (Color){255, 128, 0, 255});
    defaultORM = LoadTextureFromImage(ormImg);
    UnloadImage(ormImg);
  }

  // Background music
  Music bgm = LoadMusicStream("music/bgm.ogg");
  SetMusicVolume(bgm, BGM_VOL);
  PlayMusicStream(bgm);

  // Camera presets — prep (diagonal side-on) vs combat (top-down auto-chess) vs
  // plaza (cinematic)
  const float prepHeight = 135.0f, prepDistance = 165.0f, prepFOV = 55.0f,
              prepX = 37.0f;
  const float combatHeight = 200.0f, combatDistance = 150.0f, combatFOV = 48.0f,
              combatX = 0.0f;
  const float plazaHeight = 95.0f, plazaDistance = 210.0f, plazaFOV = 52.0f,
              plazaX = 25.0f;
  const float camLerpSpeed = 2.5f;

  float camHeight = prepHeight;
  float camDistance = prepDistance;
  float camFOV = prepFOV;
  float camX = prepX;
  bool camOverride = false;
  Camera camera = {0};
  camera.position = (Vector3){camX, camHeight, camDistance};
  camera.target = (Vector3){0.0f, 0.0f, 35.0f};
  camera.up = (Vector3){0.0f, 1.0f, 0.0f};
  camera.fovy = camFOV;
  camera.projection = CAMERA_PERSPECTIVE;

  // Unit types
  int unitTypeCount = 6;
  UnitType unitTypes[MAX_UNIT_TYPES] = {0};
  unitTypes[0].name = "Mushroom";
  unitTypes[0].modelPath = "assets/classes/mushroom/MushroomTest.obj";
  unitTypes[0].scale = 0.10f;
  unitTypes[0].yOffset = 1.5f;
  unitTypes[1].name = "Goblin";
  unitTypes[1].modelPath = "assets/classes/goblin/UpdatedGoblinWalk.glb";
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

  for (int i = 0; i < unitTypeCount; i++) {
    if (!unitTypes[i].modelPath) {
      unitTypes[i].loaded = false;
      continue;
    }
    unitTypes[i].model = LoadModel(unitTypes[i].modelPath);
    if (unitTypes[i].model.meshCount > 0) {
      unitTypes[i].baseBounds =
          GetMeshBoundingBox(unitTypes[i].model.meshes[0]);
      unitTypes[i].loaded = true;
    } else
      unitTypes[i].loaded = false;

    // Fix GLB alpha: force all material diffuse maps to full opacity
    for (int m = 0; m < unitTypes[i].model.materialCount; m++) {
      unitTypes[i].model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      unitTypes[i].model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          defaultORM;
    }
  }

  // Recompute goblin bounds across all meshes (new model may split geometry)
  if (unitTypes[1].loaded && unitTypes[1].model.meshCount > 0) {
    BoundingBox full = GetMeshBoundingBox(unitTypes[1].model.meshes[0]);
    for (int m = 1; m < unitTypes[1].model.meshCount; m++) {
      BoundingBox mb = GetMeshBoundingBox(unitTypes[1].model.meshes[m]);
      if (mb.min.x < full.min.x) full.min.x = mb.min.x;
      if (mb.min.y < full.min.y) full.min.y = mb.min.y;
      if (mb.min.z < full.min.z) full.min.z = mb.min.z;
      if (mb.max.x > full.max.x) full.max.x = mb.max.x;
      if (mb.max.y > full.max.y) full.max.y = mb.max.y;
      if (mb.max.z > full.max.z) full.max.z = mb.max.z;
    }
    // Shrink X/Z to body width centered on model origin (0,0)
    float xzShrink = 0.5f;
    float hx = (full.max.x - full.min.x) * 0.5f * xzShrink;
    float hz = (full.max.z - full.min.z) * 0.5f * xzShrink;
    full.min.x = -hx;
    full.max.x =  hx;
    full.min.z = -hz;
    full.max.z =  hz;
    unitTypes[1].baseBounds = full;
  }

  // Load and apply goblin texture
  Texture2D goblinTex = LoadTexture("assets/goblin/T_ImprovedGoblin_BC.png");
  for (int m = 0; m < unitTypes[1].model.materialCount; m++) {
    unitTypes[1].model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = goblinTex;
  }

  // Load goblin animations from separate GLBs
  int walkAnimCount = 0, idleAnimCount = 0;
  ModelAnimation *walkAnims = LoadModelAnimations(
      "assets/classes/goblin/UpdatedGoblinWalk.glb", &walkAnimCount);
  ModelAnimation *idleAnims = LoadModelAnimations(
      "assets/classes/goblin/UpdatedGoblinIdle.glb", &idleAnimCount);
  // Store walk anims as the main array, keep idle separate
  unitTypes[1].anims = walkAnims;
  unitTypes[1].animCount = walkAnimCount;
  unitTypes[1].idleAnims = idleAnims;
  unitTypes[1].idleAnimCount = idleAnimCount;
  for (int s = 0; s < ANIM_COUNT; s++)
    unitTypes[1].animIndex[s] = -1;
  if (walkAnimCount > 0)
    unitTypes[1].animIndex[ANIM_WALK] = 0;
  if (idleAnimCount > 0)
    unitTypes[1].animIndex[ANIM_IDLE] = 0;
  // ANIM_SCARED: use dedicated scared GLB if available, else fallback to walk
  unitTypes[1].scaredAnims = NULL;
  unitTypes[1].scaredAnimCount = 0;
  if (walkAnimCount > 0)
    unitTypes[1].animIndex[ANIM_SCARED] = 0; // fallback to walk
  unitTypes[1].hasAnimations = (walkAnimCount > 0 || idleAnimCount > 0);
  unitTypes[1].attackAnims = NULL;
  unitTypes[1].attackAnimCount = 0;
  unitTypes[1].castAnims = NULL;
  unitTypes[1].castAnimCount = 0;

  // Reptile animations
  {
    int cnt = 0;
    ModelAnimation *walk =
        LoadModelAnimations("assets/classes/reptile/ReptileWalking.glb", &cnt);
    unitTypes[5].anims = walk;
    unitTypes[5].animCount = cnt;

    cnt = 0;
    ModelAnimation *idle =
        LoadModelAnimations("assets/classes/reptile/ReptileIdle.glb", &cnt);
    unitTypes[5].idleAnims = idle;
    unitTypes[5].idleAnimCount = cnt;

    cnt = 0;
    ModelAnimation *atk =
        LoadModelAnimations("assets/classes/reptile/ReptileAttack.glb", &cnt);
    unitTypes[5].attackAnims = atk;
    unitTypes[5].attackAnimCount = cnt;

    unitTypes[5].scaredAnims = NULL;
    unitTypes[5].scaredAnimCount = 0;
    unitTypes[5].castAnims = NULL;
    unitTypes[5].castAnimCount = 0;

    for (int s = 0; s < ANIM_COUNT; s++)
      unitTypes[5].animIndex[s] = -1;
    if (unitTypes[5].idleAnimCount > 0)
      unitTypes[5].animIndex[ANIM_IDLE] = 0;
    if (unitTypes[5].animCount > 0)
      unitTypes[5].animIndex[ANIM_WALK] = 0;
    if (unitTypes[5].animCount > 0)
      unitTypes[5].animIndex[ANIM_SCARED] = 0;
    if (unitTypes[5].attackAnimCount > 0)
      unitTypes[5].animIndex[ANIM_ATTACK] = 0;
    unitTypes[5].hasAnimations = true;
  }

  // Devil animations
  {
    int cnt = 0;
    ModelAnimation *walk =
        LoadModelAnimations("assets/classes/devil/DevilWalk.glb", &cnt);
    unitTypes[2].anims = walk;
    unitTypes[2].animCount = cnt;

    cnt = 0;
    ModelAnimation *idle =
        LoadModelAnimations("assets/classes/devil/DevilIdle.glb", &cnt);
    unitTypes[2].idleAnims = idle;
    unitTypes[2].idleAnimCount = cnt;

    cnt = 0;
    ModelAnimation *atk =
        LoadModelAnimations("assets/classes/devil/DevilPunch.glb", &cnt);
    unitTypes[2].attackAnims = atk;
    unitTypes[2].attackAnimCount = cnt;

    cnt = 0;
    ModelAnimation *cast =
        LoadModelAnimations("assets/classes/devil/DevilMagic.glb", &cnt);
    unitTypes[2].castAnims = cast;
    unitTypes[2].castAnimCount = cnt;

    cnt = 0;
    ModelAnimation *scared =
        LoadModelAnimations("assets/classes/devil/DevilScared.glb", &cnt);
    unitTypes[2].scaredAnims = scared;
    unitTypes[2].scaredAnimCount = cnt;

    for (int s = 0; s < ANIM_COUNT; s++)
      unitTypes[2].animIndex[s] = -1;
    if (unitTypes[2].idleAnimCount > 0)
      unitTypes[2].animIndex[ANIM_IDLE] = 0;
    if (unitTypes[2].animCount > 0)
      unitTypes[2].animIndex[ANIM_WALK] = 0;
    if (unitTypes[2].scaredAnimCount > 0)
      unitTypes[2].animIndex[ANIM_SCARED] = 0;
    if (unitTypes[2].attackAnimCount > 0)
      unitTypes[2].animIndex[ANIM_ATTACK] = 0;
    if (unitTypes[2].castAnimCount > 0)
      unitTypes[2].animIndex[ANIM_CAST] = 0;
    unitTypes[2].hasAnimations = true;
  }

  // Portrait render textures for HUD (one per max blue unit)
  RenderTexture2D portraits[BLUE_TEAM_MAX_SIZE];
  bool portraitDirty[BLUE_TEAM_MAX_SIZE];
  int portraitTypeCache[BLUE_TEAM_MAX_SIZE];       // cached typeIndex per slot
  uint8_t portraitRarityCache[BLUE_TEAM_MAX_SIZE]; // cached rarity per slot
  for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++) {
    portraits[i] =
        LoadRenderTexture(HUD_PORTRAIT_SIZE_BASE, HUD_PORTRAIT_SIZE_BASE);
    portraitDirty[i] = true;
    portraitTypeCache[i] = -1;
    portraitRarityCache[i] = 255;
  }

  // Intro screen render texture (larger for cinematic model display)
  RenderTexture2D introModelRT = LoadRenderTexture(512, 512);

  // Dedicated camera for portrait rendering
  Camera portraitCam = {0};
  portraitCam.up = (Vector3){0.0f, 1.0f, 0.0f};
  portraitCam.fovy = 35.0f;
  portraitCam.projection = CAMERA_PERSPECTIVE;

  // --- Lighting setup ---
  Shader lightShader = LoadShader(
      TextFormat("resources/shaders/glsl%i/lighting.vs", GLSL_VERSION),
      TextFormat("resources/shaders/glsl%i/lighting.fs", GLSL_VERSION));
  lightShader.locs[SHADER_LOC_VECTOR_VIEW] =
      GetShaderLocation(lightShader, "viewPos");

  int ambientLoc = GetShaderLocation(lightShader, "ambient");
  SetShaderValue(lightShader, ambientLoc, (float[4]){0.25f, 0.22f, 0.18f, 1.0f},
                 SHADER_UNIFORM_VEC4);

  int fogColorLoc = GetShaderLocation(lightShader, "fogColor");
  int fogDensityLoc = GetShaderLocation(lightShader, "fogDensity");
  SetShaderValue(lightShader, fogColorLoc, (float[3]){0.176f, 0.157f, 0.137f},
                 SHADER_UNIFORM_VEC3);
  float fogDensity = 0.003f;
  SetShaderValue(lightShader, fogDensityLoc, &fogDensity, SHADER_UNIFORM_FLOAT);

  Light lights[MAX_LIGHTS] = {0};
  lights[0] =
      CreateLight(LIGHT_DIRECTIONAL, (Vector3){0, 60, 30}, Vector3Zero(),
                  (Color){245, 230, 200, 255}, lightShader);
  lights[1] = CreateLight(LIGHT_POINT, (Vector3){0, 40, 0}, Vector3Zero(),
                          (Color){220, 200, 170, 255}, lightShader);
  (void)lights;

  // --- SSAO post-process ---
  Shader ssaoShader = LoadShader(
      NULL, TextFormat("resources/shaders/glsl%i/ssao.fs", GLSL_VERSION));
  int ssaoResLoc = GetShaderLocation(ssaoShader, "resolution");
  int ssaoNearLoc = GetShaderLocation(ssaoShader, "near");
  int ssaoFarLoc = GetShaderLocation(ssaoShader, "far");
  int ssaoDepthLoc = GetShaderLocation(ssaoShader, "texture1");

  // --- FXAA post-process ---
  Shader fxaaShader = LoadShader(
      NULL, TextFormat("resources/shaders/glsl%i/fxaa.fs", GLSL_VERSION));
  int fxaaResLoc = GetShaderLocation(fxaaShader, "resolution");
  // Scene render texture with samplable depth texture (not renderbuffer)
  int sceneRTWidth = GetScreenWidth();
  int sceneRTHeight = GetScreenHeight();
  RenderTexture2D sceneRT = {0};
  sceneRT.id = rlLoadFramebuffer();
  sceneRT.texture.id = rlLoadTexture(NULL, sceneRTWidth, sceneRTHeight,
                                     RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
  sceneRT.texture.width = sceneRTWidth;
  sceneRT.texture.height = sceneRTHeight;
  sceneRT.texture.format = RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  sceneRT.texture.mipmaps = 1;
  sceneRT.depth.id = rlLoadTextureDepth(sceneRTWidth, sceneRTHeight, false);
  sceneRT.depth.width = sceneRTWidth;
  sceneRT.depth.height = sceneRTHeight;
  rlFramebufferAttach(sceneRT.id, sceneRT.texture.id,
                      RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
  rlFramebufferAttach(sceneRT.id, sceneRT.depth.id, RL_ATTACHMENT_DEPTH,
                      RL_ATTACHMENT_TEXTURE2D, 0);

  // FXAA render target (fullscreen, color only)
  int fxaaRTWidth = sceneRTWidth;
  int fxaaRTHeight = sceneRTHeight;
  RenderTexture2D fxaaRT = LoadRenderTexture(fxaaRTWidth, fxaaRTHeight);

  // --- Color grading post-process ---
  Shader colorGradeShader =
      LoadShader(NULL, TextFormat("resources/shaders/glsl%i/color_grade.fs",
                                  GLSL_VERSION));
  int cgExposureLoc = GetShaderLocation(colorGradeShader, "exposure");
  int cgContrastLoc = GetShaderLocation(colorGradeShader, "contrast");
  int cgSaturationLoc = GetShaderLocation(colorGradeShader, "saturation");
  int cgTemperatureLoc = GetShaderLocation(colorGradeShader, "temperature");
  int cgVigStrLoc = GetShaderLocation(colorGradeShader, "vignetteStrength");
  int cgVigSoftLoc = GetShaderLocation(colorGradeShader, "vignetteSoftness");
  int cgLiftLoc = GetShaderLocation(colorGradeShader, "lift");
  int cgGainLoc = GetShaderLocation(colorGradeShader, "gain");
  RenderTexture2D colorGradeRT = LoadRenderTexture(fxaaRTWidth, fxaaRTHeight);

// --- Shadow map setup (color+depth FBO for guaranteed completeness) ---
#define SHADOW_MAP_SIZE 2048
  RenderTexture2D shadowRT = {0};
  shadowRT.id = rlLoadFramebuffer();
  shadowRT.texture.id = rlLoadTexture(NULL, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE,
                                      RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
  shadowRT.texture.width = SHADOW_MAP_SIZE;
  shadowRT.texture.height = SHADOW_MAP_SIZE;
  shadowRT.texture.format = RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
  shadowRT.texture.mipmaps = 1;
  shadowRT.depth.id =
      rlLoadTextureDepth(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, false);
  shadowRT.depth.width = SHADOW_MAP_SIZE;
  shadowRT.depth.height = SHADOW_MAP_SIZE;
  rlFramebufferAttach(shadowRT.id, shadowRT.texture.id,
                      RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
  rlFramebufferAttach(shadowRT.id, shadowRT.depth.id, RL_ATTACHMENT_DEPTH,
                      RL_ATTACHMENT_TEXTURE2D, 0);
  if (!rlFramebufferComplete(shadowRT.id))
    TraceLog(LOG_ERROR, "Shadow map FBO is not complete!");

  Shader shadowDepthShader = LoadShader(
      TextFormat("resources/shaders/glsl%i/shadow_depth.vs", GLSL_VERSION),
      TextFormat("resources/shaders/glsl%i/shadow_depth.fs", GLSL_VERSION));

  // Light-space matrix (static directional light)
  Vector3 shadowLightPos = {0.0f, 60.0f, -30.0f};
  Vector3 shadowLightTarget = {0.0f, 0.0f, 0.0f};
  Matrix lightView = MatrixLookAt(shadowLightPos, shadowLightTarget,
                                  (Vector3){0.0f, 1.0f, 0.0f});
  Matrix lightProj = MatrixOrtho(-160.0, 160.0, -160.0, 160.0, 1.0, 350.0);
  Matrix lightVP = MatrixMultiply(lightView, lightProj);

  // Uniform locations for shadow mapping in lighting shader
  int lightVPLoc = GetShaderLocation(lightShader, "lightVP");
  int shadowMapLoc = GetShaderLocation(lightShader, "shadowMap");
  int shadowDebugLoc = GetShaderLocation(lightShader, "shadowDebug");
  int noShadowLoc = GetShaderLocation(lightShader, "noShadow");
  int normalMapLoc = GetShaderLocation(lightShader, "normalMap");
  int useNormalMapLoc = GetShaderLocation(lightShader, "useNormalMap");

  // --- Foliage sway shader (instanced, with wind animation) ---
  Shader foliageShader = LoadShader(
      TextFormat("resources/shaders/glsl%i/foliage.vs", GLSL_VERSION),
      TextFormat("resources/shaders/glsl%i/foliage.fs", GLSL_VERSION));
  foliageShader.locs[SHADER_LOC_VECTOR_VIEW] =
      GetShaderLocation(foliageShader, "viewPos");
  // Sync all shared uniforms with lightShader
  int fAmbientLoc = GetShaderLocation(foliageShader, "ambient");
  SetShaderValue(foliageShader, fAmbientLoc,
                 (float[4]){0.25f, 0.22f, 0.18f, 1.0f}, SHADER_UNIFORM_VEC4);
  int fFogColorLoc = GetShaderLocation(foliageShader, "fogColor");
  int fFogDensityLoc = GetShaderLocation(foliageShader, "fogDensity");
  SetShaderValue(foliageShader, fFogColorLoc,
                 (float[3]){0.176f, 0.157f, 0.137f}, SHADER_UNIFORM_VEC3);
  SetShaderValue(foliageShader, fFogDensityLoc, &fogDensity,
                 SHADER_UNIFORM_FLOAT);
  int fTimeLoc = GetShaderLocation(foliageShader, "time");
  int fLightVPLoc = GetShaderLocation(foliageShader, "lightVP");
  int fShadowMapLoc = GetShaderLocation(foliageShader, "shadowMap");
  int fNormalMapLoc = GetShaderLocation(foliageShader, "normalMap");
  int fUseNormalMapLoc = GetShaderLocation(foliageShader, "useNormalMap");
  // Manually set lights[0] and lights[1] on foliage shader
  // (Can't use CreateLight — its static counter is already at 2 from lightShader)
  {
    Light fLight0 = {0};
    fLight0.enabled = true;
    fLight0.type = LIGHT_DIRECTIONAL;
    fLight0.position = (Vector3){40, 60, 30};
    fLight0.target = Vector3Zero();
    fLight0.color = (Color){245, 230, 200, 255};
    fLight0.enabledLoc = GetShaderLocation(foliageShader, "lights[0].enabled");
    fLight0.typeLoc = GetShaderLocation(foliageShader, "lights[0].type");
    fLight0.positionLoc = GetShaderLocation(foliageShader, "lights[0].position");
    fLight0.targetLoc = GetShaderLocation(foliageShader, "lights[0].target");
    fLight0.colorLoc = GetShaderLocation(foliageShader, "lights[0].color");
    UpdateLightValues(foliageShader, fLight0);

    Light fLight1 = {0};
    fLight1.enabled = true;
    fLight1.type = LIGHT_POINT;
    fLight1.position = (Vector3){0, 40, 0};
    fLight1.target = Vector3Zero();
    fLight1.color = (Color){220, 200, 170, 255};
    fLight1.enabledLoc = GetShaderLocation(foliageShader, "lights[1].enabled");
    fLight1.typeLoc = GetShaderLocation(foliageShader, "lights[1].type");
    fLight1.positionLoc = GetShaderLocation(foliageShader, "lights[1].position");
    fLight1.targetLoc = GetShaderLocation(foliageShader, "lights[1].target");
    fLight1.colorLoc = GetShaderLocation(foliageShader, "lights[1].color");
    UpdateLightValues(foliageShader, fLight1);
  }

  // Foliage shadow depth shader (with sway)
  Shader foliageShadowShader = LoadShader(
      TextFormat("resources/shaders/glsl%i/foliage_shadow.vs", GLSL_VERSION),
      TextFormat("resources/shaders/glsl%i/shadow_depth.fs", GLSL_VERSION));
  int fsTimeLoc = GetShaderLocation(foliageShadowShader, "time");

  // Foliage model indices (envModels 8-12)
#define FOLIAGE_MODEL_FIRST 8
#define FOLIAGE_MODEL_LAST 12

  // Assign lighting shader to all loaded models
  for (int i = 0; i < unitTypeCount; i++) {
    if (!unitTypes[i].loaded)
      continue;
    for (int m = 0; m < unitTypes[i].model.materialCount; m++)
      unitTypes[i].model.materials[m].shader = lightShader;
  }

// --- Tile floor setup ---
#define TILE_VARIANTS 5
#define TILE_GRID_SIZE 10
#define TILE_WORLD_SIZE 20.0f

  Model tileModels[TILE_VARIANTS];
  Vector3 tileCenters[TILE_VARIANTS]; // OBJ-space center offset per variant
  const char *tilePaths[TILE_VARIANTS] = {
      "assets/environment/tiles/Tile1.obj",
      "assets/environment/tiles/Tile2.obj",
      "assets/environment/tiles/Tile3.obj",
      "assets/environment/tiles/Tile4.obj",
      "assets/environment/tiles/Tile5.obj",
  };
  Texture2D tileDiffuse =
      LoadTexture("assets/environment/tiles/T_TilesDark_BC.png");
  Texture2D tileORM =
      LoadTexture("assets/environment/tiles/T_TilesDark_ORM.png");
  Texture2D tileNormal =
      LoadTexture("assets/environment/tiles/T_TilesDark_N.png");

  for (int i = 0; i < TILE_VARIANTS; i++) {
    tileModels[i] = LoadModel(tilePaths[i]);
    for (int mi = 0; mi < tileModels[i].meshCount; mi++)
      GenMeshTangents(&tileModels[i].meshes[mi]);
    // Compute OBJ-space center from bounding box
    BoundingBox bb = GetMeshBoundingBox(tileModels[i].meshes[0]);
    tileCenters[i] = (Vector3){
        (bb.min.x + bb.max.x) * 0.5f,
        (bb.min.y + bb.max.y) * 0.5f,
        (bb.min.z + bb.max.z) * 0.5f,
    };
    // Assign diffuse + ORM textures and lighting shader to all materials
    for (int m = 0; m < tileModels[i].materialCount; m++) {
      tileModels[i].materials[m].maps[MATERIAL_MAP_DIFFUSE].texture =
          tileDiffuse;
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
  float
      tileJitterAngle[TILE_GRID_SIZE]
                     [TILE_GRID_SIZE]; // small random rotation offset (degrees)
  float tileJitterX[TILE_GRID_SIZE]
                   [TILE_GRID_SIZE]; // small random X position offset
  float tileJitterZ[TILE_GRID_SIZE]
                   [TILE_GRID_SIZE]; // small random Z position offset
  float tileWobble[TILE_GRID_SIZE]
                  [TILE_GRID_SIZE]; // current wobble amplitude (degrees)
  float tileWobbleTime[TILE_GRID_SIZE]
                      [TILE_GRID_SIZE]; // elapsed time since wobble started
  float tileWobbleDirX[TILE_GRID_SIZE]
                      [TILE_GRID_SIZE]; // tilt axis direction (from impact)
  float tileWobbleDirZ[TILE_GRID_SIZE][TILE_GRID_SIZE];
  memset(tileWobble, 0, sizeof(tileWobble));
  memset(tileWobbleTime, 0, sizeof(tileWobbleTime));
  memset(tileWobbleDirX, 0, sizeof(tileWobbleDirX));
  memset(tileWobbleDirZ, 0, sizeof(tileWobbleDirZ));
#define TILE_WOBBLE_MAX 25.0f    // max tilt angle at impact center (degrees)
#define TILE_WOBBLE_DECAY 3.0f   // exponential decay rate (per second)
#define TILE_WOBBLE_FREQ 6.0f    // oscillation frequency (Hz)
#define TILE_WOBBLE_RADIUS 90.0f // max radius of effect in game units
#define TILE_WOBBLE_BOUNCE 3.0f  // max Y bounce at impact center (game units)
  const float tileRotations[4] = {0.0f, 90.0f, 180.0f, 270.0f};
  const char *tileLayoutNames[TILE_LAYOUT_COUNT] = {"Random", "Checkerboard",
                                                    "Amongus"};

  // Amongus pixel art: 1 = dark tile (variant 0-1), 0 = light tile (variant
  // 2-4)
  const int amongusPattern[TILE_GRID_SIZE][TILE_GRID_SIZE] = {
      {0, 0, 0, 1, 1, 1, 1, 0, 0, 0}, {0, 0, 1, 1, 1, 1, 1, 1, 0, 0},
      {0, 1, 1, 0, 0, 0, 0, 1, 1, 0}, {0, 1, 1, 0, 0, 0, 0, 1, 1, 0},
      {0, 1, 1, 1, 1, 1, 1, 1, 1, 0}, {0, 1, 1, 1, 1, 1, 1, 1, 1, 0},
      {0, 1, 1, 1, 1, 1, 1, 1, 1, 0}, {0, 0, 1, 1, 1, 0, 1, 1, 0, 0},
      {0, 0, 1, 1, 0, 0, 0, 1, 1, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
  };

// Generate tile grid for current layout
#define TILE_JITTER_ANGLE 3.0f // max rotation jitter in degrees (+/-)
#define TILE_JITTER_POS 0.4f   // max position jitter in game units (+/-)
#define GENERATE_TILE_GRID()                                                   \
  do {                                                                         \
    for (int r = 0; r < TILE_GRID_SIZE; r++) {                                 \
      for (int c = 0; c < TILE_GRID_SIZE; c++) {                               \
        if (tileLayout == 0) {                                                 \
          tileVariantGrid[r][c] = GetRandomValue(0, TILE_VARIANTS - 1);        \
          tileRotationGrid[r][c] = tileRotations[GetRandomValue(0, 3)];        \
        } else if (tileLayout == 1) {                                          \
          int dark = (r + c) % 2;                                              \
          tileVariantGrid[r][c] = dark ? GetRandomValue(0, 1)                  \
                                       : GetRandomValue(2, TILE_VARIANTS - 1); \
          tileRotationGrid[r][c] = tileRotations[GetRandomValue(0, 3)];        \
        } else {                                                               \
          int dark = amongusPattern[r][c];                                     \
          tileVariantGrid[r][c] = dark ? GetRandomValue(0, 1)                  \
                                       : GetRandomValue(2, TILE_VARIANTS - 1); \
          tileRotationGrid[r][c] = tileRotations[GetRandomValue(0, 3)];        \
        }                                                                      \
        tileJitterAngle[r][c] =                                                \
            (GetRandomValue(-100, 100) / 100.0f) * TILE_JITTER_ANGLE;          \
        tileJitterX[r][c] =                                                    \
            (GetRandomValue(-100, 100) / 100.0f) * TILE_JITTER_POS;            \
        tileJitterZ[r][c] =                                                    \
            (GetRandomValue(-100, 100) / 100.0f) * TILE_JITTER_POS;            \
      }                                                                        \
    }                                                                          \
  } while (0)
  GENERATE_TILE_GRID();
  const float tileScale = TILE_WORLD_SIZE / 156.0f * 0.9f;

  // Border barrier shader + mesh
  Shader borderShader = LoadShader("resources/shaders/glsl330/border.vs",
                                   "resources/shaders/glsl330/border.fs");
  int borderTimeLoc = GetShaderLocation(borderShader, "time");
  int borderProximityLoc = GetShaderLocation(borderShader, "proximity");

  Mesh borderMesh = {0};
  borderMesh.vertexCount = 4;
  borderMesh.triangleCount = 2;
  borderMesh.vertices = (float *)MemAlloc(4 * 3 * sizeof(float));
  borderMesh.texcoords = (float *)MemAlloc(4 * 2 * sizeof(float));
  borderMesh.indices = (unsigned short *)MemAlloc(6 * sizeof(unsigned short));
  // Quad: X=-100..100, Y=0..40, Z=ARENA_BOUNDARY_Z
  // Vertex 0: bottom-left
  borderMesh.vertices[0] = -100.0f;
  borderMesh.vertices[1] = 0.0f;
  borderMesh.vertices[2] = ARENA_BOUNDARY_Z;
  borderMesh.texcoords[0] = 0.0f;
  borderMesh.texcoords[1] = 0.0f;
  // Vertex 1: bottom-right
  borderMesh.vertices[3] = 100.0f;
  borderMesh.vertices[4] = 0.0f;
  borderMesh.vertices[5] = ARENA_BOUNDARY_Z;
  borderMesh.texcoords[2] = 1.0f;
  borderMesh.texcoords[3] = 0.0f;
  // Vertex 2: top-right
  borderMesh.vertices[6] = 100.0f;
  borderMesh.vertices[7] = 40.0f;
  borderMesh.vertices[8] = ARENA_BOUNDARY_Z;
  borderMesh.texcoords[4] = 1.0f;
  borderMesh.texcoords[5] = 1.0f;
  // Vertex 3: top-left
  borderMesh.vertices[9] = -100.0f;
  borderMesh.vertices[10] = 40.0f;
  borderMesh.vertices[11] = ARENA_BOUNDARY_Z;
  borderMesh.texcoords[6] = 0.0f;
  borderMesh.texcoords[7] = 1.0f;
  // Two triangles: 0-1-2, 0-2-3
  borderMesh.indices[0] = 0;
  borderMesh.indices[1] = 1;
  borderMesh.indices[2] = 2;
  borderMesh.indices[3] = 0;
  borderMesh.indices[4] = 2;
  borderMesh.indices[5] = 3;
  UploadMesh(&borderMesh, false);

  Material borderMaterial = LoadMaterialDefault();
  borderMaterial.shader = borderShader;

  // Units
  Unit units[MAX_UNITS] = {0};
  int unitCount = 0;

  // Snapshot for round-reset
  UnitSnapshot snapshots[MAX_UNITS] = {0};
  int snapshotCount = 0;

  // Modifiers, projectiles, economy
  Modifier modifiers[MAX_MODIFIERS] = {0};
  Projectile projectiles[MAX_PROJECTILES] = {0};
  Particle particles[MAX_PARTICLES] = {0};
  int playerGold = 20;
  int roundGoldReward = 0;
  int goldFlat = 0, goldKills = 0, goldBoss = 0, goldAlive = 0,
      goldInterest = 0;
  int rollCost = 1;
  const int rollCostBase = 1;
  const int rollCostIncrement = 1;
  ShopSlot shopSlots[MAX_SHOP_SLOTS];
  int activeShopSlots = 3; // can grow up to MAX_SHOP_SLOTS via events
  for (int i = 0; i < MAX_SHOP_SLOTS; i++) {
    shopSlots[i].abilityId = -1;
    shopSlots[i].locked = false;
  }
  InventorySlot inventory[MAX_INVENTORY_SLOTS];
  for (int i = 0; i < MAX_INVENTORY_SLOTS; i++)
    inventory[i].abilityId = -1;
  DragState dragState = {0};
  // Item inventory (unequipped items the player owns)
  int itemInventory[MAX_ITEMS];
  int itemInventoryCount = 0;
  for (int i = 0; i < MAX_ITEMS; i++)
    itemInventory[i] = ITEM_NONE;
  // Item drag state
  typedef struct {
    bool dragging;
    int sourceType; // 0=inventory, 1=unit slot
    int sourceIndex;
    int itemId;
  } ItemDragState;
  ItemDragState itemDrag = {0};
  // Item shop state (for NODE_SHOP overlay)
  int itemShopOffers[3] = {ITEM_NONE, ITEM_NONE, ITEM_NONE};
  bool itemShopGenerated = false;
  int itemShopBuyCount = 0; // limits purchases per shop visit
  bool showingItemShop = false; // overlay on map (like events)
  int removeConfirmUnit =
      -1; // unit index awaiting removal confirmation (-1 = none)
  ScreenShake shake = {0};
  FloatingText floatingTexts[MAX_FLOATING_TEXTS] = {0};
  Fissure fissures[MAX_FISSURES] = {0};
  UnitIntro intro = {.active = false, .timer = 0.0f};
  UnitIntro pendingIntro = {.active = false}; // deferred intro (shown after map pick)
  float postIntroDelay = 0.0f; // pause after intro before enemies flee
  StatueSpawn statueSpawn = {.phase = SSPAWN_INACTIVE};
  int hoverAbilityId = -1;
  int hoverAbilityLevel = 0;
  int hoverAbilityUnitIndex = -1;
  int shopHoverAbilityId = -1;
  float shopHighlightTimer = 0.0f;
  int shopHighlightAbilityId = -1;
  float dragOffsetX = 0, dragOffsetZ = 0;
  float hoverTimer = 0.0f;
  const float tooltipDelay = 0.5f;
  bool usedShopHotkey = false;    // hides hotkey hint after first use
  bool usedRollHotkey = false;    // hides roll hint after first use
  bool usedLockHint = false;      // hides lock hint after first right-click lock
  bool hasDraggedUnit = false;    // hides drag hint after first drag
  char waveUpgradeText[128] = ""; // describes what changed this wave
  int battleCount = 0;            // number of battles fought (for display)

  // Synergy hover tooltip state
  int hoverSynergyIdx = -1;
  float hoverSynergyTimer = 0.0f;
  const float synergyTooltipDelay = 0.3f;

  // --- Visual juice state ---
  float fightBannerTimer = -1.0f; // <0 = inactive
  float slowmoTimer = 0.0f;       // >0 = slow motion active
  float slowmoScale = 1.0f;
  // Kill feed
  int killCount = 0;             // total kills this round
  int multiKillCount = 0;        // rapid consecutive kills by same team
  float multiKillTimer = 0.0f;   // window for multi-kill
  Team lastKillTeam = TEAM_BLUE; // which team scored the last kill
  float killFeedTimer = -1.0f;   // <0 = inactive
  char killFeedText[32] = {0};
  float killFeedScale = 1.0f; // punch-in scale

  // Battle log
  BattleLog battleLog = {0};
  float combatElapsedTime = 0.0f;
  float combatAccum = 0.0f;

  // Plaza state
  PlazaSubState plazaState = PLAZA_ROAMING;
  float plazaTimer = 0.0f;
  PlazaUnitData plazaData[MAX_UNITS] = {0};
  LobbySelection lobbySelection = {
      .poolCount = 0, .selectedSlot = -1, .heroSelected = false};
  MpLobbySelection mpLobby = {.slotTypes = {-1, -1, -1, -1},
                              .activeSlot = 0,
                              .selectionComplete = false,
                              .glowTimer = 0.0f};
  int lastKilledEnemyType = -1;
  AbilitySlot lastKilledAbilities[MAX_ABILITIES_PER_UNIT] = {
      {.abilityId = -1, .level = 0, .cooldownRemaining = 0, .triggered = false},
      {.abilityId = -1, .level = 0, .cooldownRemaining = 0, .triggered = false},
      {.abilityId = -1, .level = 0, .cooldownRemaining = 0, .triggered = false},
      {.abilityId = -1, .level = 0, .cooldownRemaining = 0, .triggered = false},
  };
  uint8_t lastKilledRarity = 0;
  int plazaHoverUnit =
      -1; // index of red unit hovered in plaza for hero selection
  bool showMultiplayerPanel = false;

  // Escape menu state
  bool showEscMenu = false;
  bool showHelp = false;
  float musicVolume = BGM_VOL;
  float sfxVolume = 1.0f;
  bool isFullscreen = false;
  int focusedSlider = -1; // 0=music, 1=sfx, -1=none
  float sliderKeyTimer = 0.0f;
  Model doorModel = LoadModel("assets/environment/door/Door.obj");
  for (int m = 0; m < doorModel.materialCount; m++) {
    doorModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    doorModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = defaultORM;
    doorModel.materials[m].shader = lightShader;
  }
  // Re-center and scale Door (Maya cm export, verts in 300-1000 range)
  if (doorModel.meshCount > 0) {
    BoundingBox dbb = GetMeshBoundingBox(doorModel.meshes[0]);
    float dCenterX = (dbb.min.x + dbb.max.x) * 0.5f;
    float dBaseY = dbb.min.y;
    float dCenterZ = (dbb.min.z + dbb.max.z) * 0.5f;
    float dHeight = dbb.max.y - dbb.min.y;
    float dScale = 15.0f / dHeight; // ~15 game units tall
    doorModel.transform =
        MatrixMultiply(MatrixTranslate(-dCenterX, -dBaseY, -dCenterZ),
                       MatrixScale(dScale, dScale, dScale));
  }
  Model trophyModel = LoadModel("assets/environment/trophy/Trophy.obj");
  for (int m = 0; m < trophyModel.materialCount; m++) {
    trophyModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    trophyModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = defaultORM;
    trophyModel.materials[m].shader = lightShader;
  }
  // Re-center and scale Trophy (Maya cm export, verts around -6000 range)
  if (trophyModel.meshCount > 0) {
    BoundingBox tbb = GetMeshBoundingBox(trophyModel.meshes[0]);
    float tCenterX = (tbb.min.x + tbb.max.x) * 0.5f;
    float tBaseY = tbb.min.y;
    float tCenterZ = (tbb.min.z + tbb.max.z) * 0.5f;
    float tHeight = tbb.max.y - tbb.min.y;
    float tScale = 10.0f / tHeight; // ~10 game units tall
    trophyModel.transform =
        MatrixMultiply(MatrixTranslate(-tCenterX, -tBaseY, -tCenterZ),
                       MatrixScale(tScale, tScale, tScale));
  }
  // --- Environment models: ground (replaces old platform), stairs, circle ---
  Texture2D groundDiffuse =
      LoadTexture("assets/environment/ground/T_Ground_BC.png");
  Texture2D groundORM =
      LoadTexture("assets/environment/ground/T_Ground_ORM.png");
  Texture2D groundNormal =
      LoadTexture("assets/environment/ground/T_Ground_N.png");
  Model platformModel =
      LoadModel("assets/environment/ground/ground.obj");
  for (int mi = 0; mi < platformModel.meshCount; mi++)
    GenMeshTangents(&platformModel.meshes[mi]);
  for (int m = 0; m < platformModel.materialCount; m++) {
    platformModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture =
        groundDiffuse;
    platformModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    platformModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = groundORM;
    platformModel.materials[m].shader = lightShader;
  }
  if (platformModel.meshCount > 0) {
    BoundingBox pbb = GetMeshBoundingBox(platformModel.meshes[0]);
    float pCenterX = (pbb.min.x + pbb.max.x) * 0.5f;
    float pTopY = pbb.max.y; // anchor top surface at Y=0
    float pCenterZ = (pbb.min.z + pbb.max.z) * 0.5f;
    float pWidth = pbb.max.x - pbb.min.x;
    float pScale = 750.0f / pWidth;
    platformModel.transform =
        MatrixMultiply(MatrixTranslate(-pCenterX, -pTopY, -pCenterZ),
                       MatrixScale(pScale, pScale, pScale));
  }

  Texture2D stairsDiffuse =
      LoadTexture("assets/environment/stairs/T_Stairs_BC.png");
  Texture2D stairsORM =
      LoadTexture("assets/environment/stairs/T_Stairs_ORM.png");
  Texture2D stairsNormal =
      LoadTexture("assets/environment/stairs/T_Stairs_N.png");
  Model stairsModel =
      LoadModel("assets/environment/stairs/Stairs_LP.obj");
  for (int mi = 0; mi < stairsModel.meshCount; mi++)
    GenMeshTangents(&stairsModel.meshes[mi]);
  for (int m = 0; m < stairsModel.materialCount; m++) {
    stairsModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = stairsDiffuse;
    stairsModel.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    stairsModel.materials[m].maps[MATERIAL_MAP_METALNESS].texture = stairsORM;
    stairsModel.materials[m].shader = lightShader;
  }
  if (stairsModel.meshCount > 0) {
    BoundingBox sbb = GetMeshBoundingBox(stairsModel.meshes[0]);
    float sCenterX = (sbb.min.x + sbb.max.x) * 0.5f;
    float sBaseY = sbb.min.y;
    float sCenterZ = (sbb.min.z + sbb.max.z) * 0.5f;
    float sHeight = sbb.max.y - sbb.min.y;
    float sScale = 10.0f / sHeight;
    stairsModel.transform =
        MatrixMultiply(MatrixTranslate(-sCenterX, -sBaseY, -sCenterZ),
                       MatrixScale(sScale, sScale, sScale));
  }

  Texture2D circleDiffuse =
      LoadTexture("assets/environment/circle/T_Circle_BC.png");
  Texture2D circleORM =
      LoadTexture("assets/environment/circle/T_Circle_ORM.png");
  Texture2D circleNormal =
      LoadTexture("assets/environment/circle/T_Circle_N.png");
  Model circleModel = LoadModel("assets/environment/circle/circle.obj");
  for (int mi = 0; mi < circleModel.meshCount; mi++)
    GenMeshTangents(&circleModel.meshes[mi]);
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
    float cWidth = cbb.max.x - cbb.min.x;
    float cScale = 80.0f / cWidth;
    // Center, scale, then tilt upright (-90° around X so far face points toward
    // arena)
    circleModel.transform = MatrixMultiply(
        MatrixMultiply(MatrixTranslate(-cCenterX, -cCenterY, -cCenterZ),
                       MatrixScale(cScale, cScale, cScale)),
        MatrixRotateX(-90.0f * DEG2RAD));
  }

  // platformPos, stairsFarPos, stairsLPos, stairsRPos, circlePos now live in
  // envPieces[]

  Vector3 doorPos = {120.0f, 0.0f, 80.0f};
  Vector3 trophyPos = {-120.0f, 0.0f, 80.0f};

  // --- Environment model catalog (for debug piece editor) ---
  EnvModelDef envModels[MAX_ENV_MODELS] = {0};
  int envModelCount = 0;

  // 0: Arches
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "Arches";
    em->modelPath = "assets/environment/arches/Arches.obj";
    em->texturePath = "assets/environment/arches/T_Arches_BC.png";
    em->ormTexturePath = "assets/environment/arches/T_Arches_ORM.png";
    em->normalTexturePath = "assets/environment/arches/T_Arches_N.png";
    em->texture = LoadTexture(em->texturePath);
    em->ormTexture = LoadTexture(em->ormTexturePath);
    em->normalTexture = LoadTexture(em->normalTexturePath);
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = lightShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 1: Wall
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "Wall";
    em->modelPath = "assets/environment/wall/Wall_LP.obj";
    em->texturePath = "assets/environment/wall/T_Wall_BC.png";
    em->ormTexturePath = "assets/environment/wall/T_Wall_ORM.png";
    em->normalTexturePath = "assets/environment/wall/T_Wall_N.png";
    em->texture = LoadTexture(em->texturePath);
    em->ormTexture = LoadTexture(em->ormTexturePath);
    em->normalTexture = LoadTexture(em->normalTexturePath);
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = lightShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 2: Stairs (reuse already-loaded stairsModel)
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "Stairs";
    em->modelPath = "assets/environment/stairs/Stairs_LP.obj";
    em->texturePath = NULL;
    em->model = stairsModel; // reuse — do NOT unload separately
    em->texture = (Texture2D){0};
    em->normalTexture = stairsNormal;
    em->loaded = true;
    envModelCount++;
  }
  // 3: Circle (reuse already-loaded circleModel)
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "Circle";
    em->modelPath = "assets/environment/circle/circle.obj";
    em->texturePath = NULL;
    em->model = circleModel; // reuse — do NOT unload separately
    em->texture = (Texture2D){0};
    em->normalTexture = circleNormal;
    em->loaded = true;
    envModelCount++;
  }
  // 4: FloorTiles
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "FloorTiles";
    em->modelPath = "assets/environment/floor_tiles/FloorTiles_LP.obj";
    em->texturePath = NULL;
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
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
      float h = bb.max.y - bb.min.y;
      float s = 10.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 5: Ground (reuse already-loaded platformModel)
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "Ground";
    em->modelPath = "assets/environment/ground/ground.obj";
    em->texturePath = NULL;
    em->model = platformModel; // reuse — do NOT unload separately
    em->texture = (Texture2D){0};
    em->normalTexture = groundNormal;
    em->loaded = true;
    envModelCount++;
  }
  // 6: PillarBig
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "PillarBig";
    em->modelPath = "assets/environment/pillars/PillarBig_LP.obj";
    em->texturePath = "assets/environment/pillars/T_Pillars_BC.png";
    em->ormTexturePath = "assets/environment/pillars/T_Pillars_ORM.png";
    em->normalTexturePath = "assets/environment/pillars/T_Pillars_N.png";
    em->texture = LoadTexture(em->texturePath);
    em->ormTexture = LoadTexture(em->ormTexturePath);
    em->normalTexture = LoadTexture(em->normalTexturePath);
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = lightShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 7: PillarSmall (shares textures with PillarBig)
  {
    EnvModelDef *em = &envModels[envModelCount];
    EnvModelDef *pillarBig = &envModels[envModelCount - 1];
    em->name = "PillarSmall";
    em->modelPath = "assets/environment/pillars/PillarSmall_LP.obj";
    em->texturePath = pillarBig->texturePath;
    em->ormTexturePath = pillarBig->ormTexturePath;
    em->normalTexturePath = pillarBig->normalTexturePath;
    em->texture = pillarBig->texture;       // shared — do NOT unload separately
    em->ormTexture = pillarBig->ormTexture; // shared
    em->normalTexture = pillarBig->normalTexture; // shared
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = lightShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }

  // 8: GrassBig (loads shared foliage textures)
  {
    EnvModelDef *em = &envModels[envModelCount];
    em->name = "GrassBig";
    em->modelPath = "assets/environment/foliage/GrassBig.obj";
    em->texturePath = "assets/environment/foliage/T_Foliage_BC.png";
    em->ormTexturePath = "assets/environment/foliage/T_Foliage_ORM.png";
    em->normalTexturePath = "assets/environment/foliage/T_Foliage_N.png";
    em->texture = LoadTexture(em->texturePath);
    em->ormTexture = LoadTexture(em->ormTexturePath);
    em->normalTexture = LoadTexture(em->normalTexturePath);
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = foliageShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 9: GrassSmall (shares foliage textures with GrassBig)
  {
    EnvModelDef *em = &envModels[envModelCount];
    EnvModelDef *grassBig = &envModels[envModelCount - 1];
    em->name = "GrassSmall";
    em->modelPath = "assets/environment/foliage/GrassSmall.obj";
    em->texturePath = grassBig->texturePath;
    em->ormTexturePath = grassBig->ormTexturePath;
    em->normalTexturePath = grassBig->normalTexturePath;
    em->texture = grassBig->texture;       // shared — do NOT unload separately
    em->ormTexture = grassBig->ormTexture; // shared
    em->normalTexture = grassBig->normalTexture; // shared
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = foliageShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 10: GroundFoliage (shares foliage textures with GrassBig)
  {
    EnvModelDef *em = &envModels[envModelCount];
    EnvModelDef *grassBig = &envModels[8]; // GrassBig
    em->name = "GroundFoliage";
    em->modelPath = "assets/environment/foliage/GroundFoliage.obj";
    em->texturePath = grassBig->texturePath;
    em->ormTexturePath = grassBig->ormTexturePath;
    em->normalTexturePath = grassBig->normalTexturePath;
    em->texture = grassBig->texture;             // shared
    em->ormTexture = grassBig->ormTexture;       // shared
    em->normalTexture = grassBig->normalTexture; // shared
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = foliageShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 11: TallFoliage (shares foliage textures with GrassBig)
  {
    EnvModelDef *em = &envModels[envModelCount];
    EnvModelDef *grassBig = &envModels[8]; // GrassBig
    em->name = "TallFoliage";
    em->modelPath = "assets/environment/foliage/TallFoliage.obj";
    em->texturePath = grassBig->texturePath;
    em->ormTexturePath = grassBig->ormTexturePath;
    em->normalTexturePath = grassBig->normalTexturePath;
    em->texture = grassBig->texture;             // shared
    em->ormTexture = grassBig->ormTexture;       // shared
    em->normalTexture = grassBig->normalTexture; // shared
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = foliageShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }
  // 12: TallFoliageBigger (shares foliage textures with GrassBig)
  {
    EnvModelDef *em = &envModels[envModelCount];
    EnvModelDef *grassBig = &envModels[8]; // GrassBig
    em->name = "TallFoliageBigger";
    em->modelPath = "assets/environment/foliage/TallFoliageBigger.obj";
    em->texturePath = grassBig->texturePath;
    em->ormTexturePath = grassBig->ormTexturePath;
    em->normalTexturePath = grassBig->normalTexturePath;
    em->texture = grassBig->texture;             // shared
    em->ormTexture = grassBig->ormTexture;       // shared
    em->normalTexture = grassBig->normalTexture; // shared
    em->model = LoadModel(em->modelPath);
    for (int mi = 0; mi < em->model.meshCount; mi++)
      GenMeshTangents(&em->model.meshes[mi]);
    for (int m = 0; m < em->model.materialCount; m++) {
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].texture = em->texture;
      em->model.materials[m].maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
      em->model.materials[m].maps[MATERIAL_MAP_METALNESS].texture =
          em->ormTexture;
      em->model.materials[m].shader = foliageShader;
    }
    if (em->model.meshCount > 0) {
      BoundingBox bb = GetMeshBoundingBox(em->model.meshes[0]);
      float cx = (bb.min.x + bb.max.x) * 0.5f;
      float by = bb.min.y;
      float cz = (bb.min.z + bb.max.z) * 0.5f;
      float h = bb.max.y - bb.min.y;
      float s = 15.0f / h;
      em->model.transform =
          MatrixMultiply(MatrixTranslate(-cx, -by, -cz), MatrixScale(s, s, s));
    }
    em->loaded = true;
    envModelCount++;
  }

  // --- Env pieces array (populated from save file) ---
  EnvPiece envPieces[MAX_ENV_PIECES] = {0};
  int envPieceCount = 0;
  int envSelectedPiece = -1;
  bool envDragging = false;
  Vector3 envDragOffset = {0};
  float envSaveFlashTimer = 0.0f; // flash "SAVED" text
  float envKeyTimers[8] = {0};    // W,A,S,D,R,F,[,]

  // Load env layout from file
  {
    FILE *fp = fopen("env_layout.txt", "r");
    if (fp) {
      char line[256];
      while (fgets(line, sizeof(line), fp) && envPieceCount < MAX_ENV_PIECES) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
          continue;
        int mi;
        float x, y, z, rotX, rotY, rotZ, sc;
        int fields = sscanf(line, "%d %f %f %f %f %f %f %f", &mi, &x, &y, &z,
                            &rotX, &rotY, &rotZ, &sc);
        if (fields == 8) {
          if (mi >= 0 && mi < envModelCount) {
            envPieces[envPieceCount] = (EnvPiece){.modelIndex = mi,
                                                  .position = {x, y, z},
                                                  .rotationX = rotX,
                                                  .rotationY = rotY,
                                                  .rotationZ = rotZ,
                                                  .scale = sc,
                                                  .active = true};
            envPieceCount++;
          }
        } else if (fields == 6) {
          // Backward compat: old 6-field format (no rotX/rotZ)
          float oldRot = rotX, oldSc = rotY; // fields 5,6 map to rotX,rotY vars
          if (mi >= 0 && mi < envModelCount) {
            envPieces[envPieceCount] = (EnvPiece){.modelIndex = mi,
                                                  .position = {x, y, z},
                                                  .rotationX = 0,
                                                  .rotationY = oldRot,
                                                  .rotationZ = 0,
                                                  .scale = oldSc,
                                                  .active = true};
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
    envPieces[envPieceCount++] = (EnvPiece){.modelIndex = 5,
                                            .position = {0, -10, 0},
                                            .rotationX = 0,
                                            .rotationY = 0,
                                            .rotationZ = 0,
                                            .scale = 1.0f,
                                            .active = true};
    // Stairs far
    envPieces[envPieceCount++] = (EnvPiece){.modelIndex = 2,
                                            .position = {0, -1, -120},
                                            .rotationX = 0,
                                            .rotationY = 0,
                                            .rotationZ = 0,
                                            .scale = 1.0f,
                                            .active = true};
    // Stairs left
    envPieces[envPieceCount++] = (EnvPiece){.modelIndex = 2,
                                            .position = {-120, -1, 0},
                                            .rotationX = 0,
                                            .rotationY = 90,
                                            .rotationZ = 0,
                                            .scale = 1.0f,
                                            .active = true};
    // Stairs right
    envPieces[envPieceCount++] = (EnvPiece){.modelIndex = 2,
                                            .position = {120, -1, 0},
                                            .rotationX = 0,
                                            .rotationY = -90,
                                            .rotationZ = 0,
                                            .scale = 1.0f,
                                            .active = true};
    // Circle
    envPieces[envPieceCount++] = (EnvPiece){.modelIndex = 3,
                                            .position = {0, 0, -140},
                                            .rotationX = 0,
                                            .rotationY = 0,
                                            .rotationZ = 0,
                                            .scale = 1.0f,
                                            .active = true};
  }
  int plazaHoverObject = 0;       // 0=none, 1=trophy, 2=door
  float plazaSparkleTimer = 0.0f; // for sparkle effect on objects

  // Round / score state
  GamePhase phase = PHASE_PLAZA;
  int currentRound = 0; // 0-indexed, displayed as 1-indexed
  int blueWins = 0;
  int redWins = 0;
  int mpHealth[2] = {20, 20}; // multiplayer HP (indexed by server slot)
  int lastRoundDamage = 0;
  char mpRoundResultBuf[64] = {0};
  float roundOverTimer = 0.0f; // brief pause after a round ends
  const char *roundResultText = "";
  bool debugMode = false;
  int debugSpawnRarity = RARITY_COMMON;
  int shadowDebugMode = 0;

  // Frame-time profiler (debug mode overlay)
  double profLogicTime = 0.0;  // game logic + combat tick
  double profRenderTime = 0.0; // 3D scene + 2D overlay
  double profTotalTime = 0.0;  // full frame
  // Smoothed values for display (EMA)
  double profLogicSmooth = 0.0;
  double profRenderSmooth = 0.0;
  double profTotalSmooth = 0.0;

  // Leaderboard & prestige state
  Leaderboard leaderboard = {0};
  LoadLeaderboard(&leaderboard, LEADERBOARD_FILE);
  bool showLeaderboard = false;
  int leaderboardScroll = 0;
  int lastMilestoneRound = 0;
  bool blueLostLastRound = false;
  bool deathPenalty = false;

  // Map state (Slay the Spire branching map)
  ActMap actMap = {0};
  bool mapActive = false;       // true when using map system (singleplayer)
  bool firstWaveDone = false;   // set after beating wave 1 (triggers map)
  int mapSelectedNodeType = -1; // last selected node type
  int currentEventIndex = 0;
  bool showingMapEvent = false; // true when displaying event choices
  int mapEventChoice = -1;      // player's event choice (-1 = none yet)
  char playerName[32] = "Player";
  int playerNameLen = 6;
  bool nameInputActive = false;

  // --- Multiplayer state ---
  NetClient netClient;
  net_client_init(&netClient);
  bool isMultiplayer = false;
  bool playerReady = false;
  char joinIpAddress[64] = "127.0.0.1";
  int joinIpLen = 9;
  bool isHosting = false;
  bool waitingForOpponent = false;
  bool opponentIsReady = false;
  float oppReadyCountdown = 0.0f; // countdown timer when opponent is ready
  char menuError[128] = {0};
  bool currentRoundIsPve = false;
#ifdef USE_EOS
  EosClient eosClient;
  eos_client_init(&eosClient);
  bool useEos = false;
  char joinLobbyCode[LOBBY_CODE_LEN + 1] = {0};
  int joinLobbyCodeLen = 0;
#endif

  // UI button sizes (positions computed each frame for resize support)
  const int btnWidth = 150;
  const int btnHeight = 30;
  const int btnMargin = 10;
  int playBtnW = 120;
  int playBtnH = 40;

  // Spawn initial plaza lobby pool (hero selection)
  PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);

  // Load persisted settings
  LoadSettings(&musicVolume, &sfxVolume, &isFullscreen, playerName,
               &playerNameLen);
  SetMusicVolume(bgm, musicVolume);
  for (int si = 0; si < sfxCount; si++)
    SetSoundVolume(allSfx[si], sfxBaseVol[si] * sfxVolume);
  if (isFullscreen)
    ToggleBorderlessWindowed();

  float easterEggTimer = 0.0f;

  //==================================================================================
  // MAIN LOOP
  //==================================================================================
  while (!WindowShouldClose()) {
    double profFrameStart = GetTime();
    float dt = GetFrameTime();
    float rawDt = dt; // unscaled dt for UI timers
    uiScale = (float)GetScreenHeight() / 720.0f;
    if (uiScale < 1.0f)
      uiScale = 1.0f;
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
    if (IsMusicStreamPlaying(bgm) &&
        GetMusicTimePlayed(bgm) >= GetMusicTimeLength(bgm) - 0.05f) {
      SeekMusicStream(bgm, 29.091f);
    }
#ifdef USE_EOS
    eos_tick();
#endif
    // Slow-motion time scaling
    if (slowmoTimer > 0.0f) {
      slowmoTimer -= rawDt;
      if (slowmoTimer <= 0.0f) {
        slowmoTimer = 0.0f;
        slowmoScale = 1.0f;
      }
      dt *= slowmoScale;
    }
    // Fight banner timer
    if (fightBannerTimer >= 0.0f)
      fightBannerTimer += rawDt;
    // Kill feed timer
    if (killFeedTimer >= 0.0f)
      killFeedTimer += rawDt;
    // Multi-kill window decay
    if (multiKillTimer > 0.0f) {
      multiKillTimer -= rawDt;
      if (multiKillTimer <= 0.0f)
        multiKillCount = 0;
    }
    GamePhase prevPhase = phase;
    UpdateShake(&shake, dt);
    if (IsKeyPressed(KEY_F1))
      debugMode = !debugMode;
    if (debugMode && IsKeyPressed(KEY_G))
      debugSpawnRarity = (debugSpawnRarity + 1) % 3;
    if (debugMode && IsKeyPressed(KEY_EQUAL) && activeShopSlots < MAX_SHOP_SLOTS)
      activeShopSlots++;
    if (debugMode && IsKeyPressed(KEY_MINUS) && activeShopSlots > 1)
      activeShopSlots--;
    if (IsKeyPressed(KEY_F6))
      cgDebugOverlay = !cgDebugOverlay;
    if (cgDebugOverlay) {
      float step = 0.01f;
      if (IsKeyDown(KEY_ONE))
        cgExposure += step;
      if (IsKeyDown(KEY_TWO))
        cgExposure -= step;
      if (IsKeyDown(KEY_THREE))
        cgContrast += step;
      if (IsKeyDown(KEY_FOUR))
        cgContrast -= step;
      if (IsKeyDown(KEY_FIVE))
        cgSaturation += step;
      if (IsKeyDown(KEY_SIX))
        cgSaturation -= step;
      if (IsKeyDown(KEY_SEVEN))
        cgTemperature += step;
      if (IsKeyDown(KEY_EIGHT))
        cgTemperature -= step;
      if (IsKeyDown(KEY_NINE))
        cgVignetteStr += step;
      if (IsKeyDown(KEY_ZERO))
        cgVignetteStr -= step;
      if (IsKeyDown(KEY_MINUS))
        cgVignetteSoft += step;
      if (IsKeyDown(KEY_EQUAL))
        cgVignetteSoft -= step;
    }
    if (IsKeyPressed(KEY_F10)) {
      shadowDebugMode = (shadowDebugMode + 1) % 5;
      SetShaderValue(lightShader, shadowDebugLoc, &shadowDebugMode,
                     SHADER_UNIFORM_INT);
    }
    // F11: toggle borderless fullscreen
    if (IsKeyPressed(KEY_F11)) {
      ToggleBorderlessWindowed();
      isFullscreen = !isFullscreen;
    }
    // H: toggle help overlay (when no other overlay is open)
    if (IsKeyPressed(KEY_H) && !showEscMenu && !showLeaderboard &&
        !showMultiplayerPanel && !nameInputActive) {
      showHelp = !showHelp;
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
      if (envSelectedPiece >= 0 && envSelectedPiece < envPieceCount &&
          envPieces[envSelectedPiece].active) {
        if (IsKeyPressed(KEY_C))
          envPieces[envSelectedPiece].rotationX -= 15.0f;
        if (IsKeyPressed(KEY_V))
          envPieces[envSelectedPiece].rotationX += 15.0f;
        if (IsKeyPressed(KEY_Q))
          envPieces[envSelectedPiece].rotationY -= 15.0f;
        if (IsKeyPressed(KEY_E))
          envPieces[envSelectedPiece].rotationY += 15.0f;
        if (IsKeyPressed(KEY_Z))
          envPieces[envSelectedPiece].rotationZ -= 15.0f;
        if (IsKeyPressed(KEY_X))
          envPieces[envSelectedPiece].rotationZ += 15.0f;
        if (KeyRepeat(KEY_R, dt, &envKeyTimers[4]))
          envPieces[envSelectedPiece].position.y += 1.0f;
        if (KeyRepeat(KEY_F, dt, &envKeyTimers[5]))
          envPieces[envSelectedPiece].position.y -= 1.0f;
        if (KeyRepeat(KEY_W, dt, &envKeyTimers[0]))
          envPieces[envSelectedPiece].position.z -= 1.0f;
        if (KeyRepeat(KEY_S, dt, &envKeyTimers[1]))
          envPieces[envSelectedPiece].position.z += 1.0f;
        if (KeyRepeat(KEY_A, dt, &envKeyTimers[2]))
          envPieces[envSelectedPiece].position.x -= 1.0f;
        if (KeyRepeat(KEY_D, dt, &envKeyTimers[3]))
          envPieces[envSelectedPiece].position.x += 1.0f;
        if (KeyRepeat(KEY_RIGHT_BRACKET, dt, &envKeyTimers[6]))
          envPieces[envSelectedPiece].scale += 0.1f;
        if (KeyRepeat(KEY_LEFT_BRACKET, dt, &envKeyTimers[7])) {
          if (envPieces[envSelectedPiece].scale <= 1.0f) {
            envPieces[envSelectedPiece].scale -= 0.05f;
          } else {
            envPieces[envSelectedPiece].scale -= 0.1f;
          }
          if (envPieces[envSelectedPiece].scale < 0.1f)
            envPieces[envSelectedPiece].scale = 0.1f;
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
      if (envDragging && envSelectedPiece >= 0 &&
          IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
        RayCollision hit = GetRayCollisionQuad(
            ray, (Vector3){-500, 0, -500}, (Vector3){-500, 0, 500},
            (Vector3){500, 0, 500}, (Vector3){500, 0, -500});
        if (hit.hit) {
          envPieces[envSelectedPiece].position.x =
              hit.point.x + envDragOffset.x;
          envPieces[envSelectedPiece].position.z =
              hit.point.z + envDragOffset.z;
        }
      }
      if (envDragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        envDragging = false;
      }

      // Env save flash timer
      if (envSaveFlashTimer > 0.0f)
        envSaveFlashTimer -= dt;
    }

    // Update unit intro animation
    if (intro.active) {
      intro.timer += dt;
      UnitType *itype = &unitTypes[intro.typeIndex];
      if (itype->hasAnimations && itype->animIndex[ANIM_IDLE] >= 0) {
        int fc = itype->idleAnims[itype->animIndex[ANIM_IDLE]].frameCount;
        if (fc > 0) {
          float animFps = 30.0f;
          intro.introAnimTimer += dt;
          float frameDur = 1.0f / animFps;
          while (intro.introAnimTimer >= frameDur) {
            intro.introAnimTimer -= frameDur;
            intro.animFrame = (intro.animFrame + 1) % fc;
          }
        }
      }
      if (intro.timer >= INTRO_DURATION) {
        intro.active = false;
        if (lobbySelection.heroSelected) postIntroDelay = 1.5f;
        // Trigger statue spawn for blue units
        if (intro.unitIndex >= 0 && intro.unitIndex < unitCount &&
            units[intro.unitIndex].active &&
            units[intro.unitIndex].team == TEAM_BLUE) {
          // Force-finish any previous spawn anim (snap old unit to ground)
          if (statueSpawn.phase != SSPAWN_INACTIVE) {
            int old = statueSpawn.unitIndex;
            if (old >= 0 && old < unitCount && units[old].active)
              units[old].position.y = 0.0f;
            statueSpawn.phase = SSPAWN_INACTIVE;
          }
          // Randomize landing position on player side of field + random facing
          // angle
          int idx2 = intro.unitIndex;
          float gridLim = ARENA_GRID_HALF - 10.0f; // 90
          units[idx2].position.x =
              (float)GetRandomValue((int)(-gridLim), (int)(gridLim));
          units[idx2].position.z = (float)GetRandomValue(
              (int)(ARENA_BOUNDARY_Z + 5.0f), (int)(gridLim));
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
        UpdateStatueSpawn(&statueSpawn, particles, &shake, units[si].position,
                          dt);
        if (phaseBefore != SSPAWN_FALLING &&
            statueSpawn.phase == SSPAWN_FALLING)
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
                float dist = sqrtf(dx * dx + dz * dz);
                if (dist < TILE_WOBBLE_RADIUS) {
                  float strength = expf(-2.5f * dist / TILE_WOBBLE_RADIUS);
                  tileWobble[tr][tc] = TILE_WOBBLE_MAX * strength;
                  tileWobbleTime[tr][tc] =
                      -(dist * 0.008f); // negative = propagation delay
                  // Tilt direction: away from impact point
                  float len = dist > 0.1f ? dist : 1.0f;
                  tileWobbleDirX[tr][tc] =
                      dz / len; // tilt around X pushes Z edge up
                  tileWobbleDirZ[tr][tc] =
                      -dx / len; // tilt around Z pushes X edge up
                }
              }
            }
          }
          units[si].position.y = 0.0f;
          units[si].currentAnim = ANIM_IDLE;
          units[si].animFrame = 0;
          statueSpawn.phase = SSPAWN_INACTIVE;
          printf("[STATUE LAND] idx=%d type=%d rarity=%d hp=%.1f hpMult=%.2f\n",
                 si, units[si].typeIndex, units[si].rarity,
                 units[si].currentHealth, units[si].hpMultiplier);
          // Trigger plaza scared on impact
          if (phase == PHASE_PLAZA && plazaState == PLAZA_ROAMING) {
            PlazaTriggerScared(units, unitCount, plazaData, &plazaState,
                               &plazaTimer);
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
    int prevHoverAbilityLevel = hoverAbilityLevel;
    int prevHoverAbilityUnitIndex = hoverAbilityUnitIndex;
    hoverAbilityId = -1;
    hoverAbilityLevel = 0;
    hoverAbilityUnitIndex = -1;
    shopHoverAbilityId = -1;
    int prevHoverSynergyIdx = hoverSynergyIdx;
    hoverSynergyIdx = -1;

    // Tick shop highlight linger timer
    if (shopHighlightTimer > 0) {
      shopHighlightTimer -= dt;
      if (shopHighlightTimer <= 0)
        shopHighlightAbilityId = -1;
    }

    // Lerp camera toward phase preset (skip when debug override active)
    if (!camOverride) {
      bool combat = (phase == PHASE_COMBAT);
      bool plaza = (phase == PHASE_PLAZA);
      // Scale camera to compensate for larger HUD at higher resolutions
      float hudFrac = (float)hudTotalH / (float)GetScreenHeight();
      float camScale =
          1.0f / (1.0f - hudFrac * 0.5f); // pull back more as HUD grows
      float tgtH =
          (plaza ? plazaHeight : (combat ? combatHeight : prepHeight)) *
          camScale;
      float tgtD =
          (plaza ? plazaDistance : (combat ? combatDistance : prepDistance)) *
          camScale;
      float tgtF = plaza ? plazaFOV : (combat ? combatFOV : prepFOV);
      float tgtX = plaza ? plazaX : (combat ? combatX : prepX);
      // Mirror camera for player 2 during PVP (prep + combat)
      if (isMultiplayer && netClient.playerSlot == 1 && !currentRoundIsPve &&
          !plaza) {
        tgtX = -tgtX;
        tgtD = -tgtD;
      }
      float t = camLerpSpeed * dt;
      if (t > 1.0f)
        t = 1.0f;
      camHeight += (tgtH - camHeight) * t;
      camDistance += (tgtD - camDistance) * t;
      camFOV += (tgtF - camFOV) * t;
      camX += (tgtX - camX) * t;
    }

    // Update camera
    camera.position.x = camX;
    camera.position.y = camHeight;
    camera.position.z = camDistance;
    camera.fovy = camFOV;

    // Update lighting shader with camera position
    float cameraPos[3] = {camera.position.x, camera.position.y,
                          camera.position.z};
    SetShaderValue(lightShader, lightShader.locs[SHADER_LOC_VECTOR_VIEW],
                   cameraPos, SHADER_UNIFORM_VEC3);
    SetShaderValue(foliageShader, foliageShader.locs[SHADER_LOC_VECTOR_VIEW],
                   cameraPos, SHADER_UNIFORM_VEC3);
    // Update foliage time uniform
    float foliageTime = (float)GetTime();
    SetShaderValue(foliageShader, fTimeLoc, &foliageTime, SHADER_UNIFORM_FLOAT);
    SetShaderValue(foliageShadowShader, fsTimeLoc, &foliageTime,
                   SHADER_UNIFORM_FLOAT);

    //------------------------------------------------------------------------------
    // PHASE: PLAZA — 3D plaza with roaming enemies, interactive objects
    //------------------------------------------------------------------------------
    if (phase == PHASE_PLAZA) {
      // Update plaza sub-states
      if (postIntroDelay > 0.0f) postIntroDelay -= dt;
      if (plazaState == PLAZA_ROAMING) {
        PlazaUpdateRoaming(units, unitCount, plazaData, dt);
      } else if (plazaState == PLAZA_SCARED) {
        plazaTimer -= dt;
        if (plazaTimer <= 0.0f) {
          plazaState = PLAZA_FLEEING;
        }
      } else if (plazaState == PLAZA_FLEEING) {
        bool allGone =
            PlazaUpdateFlee(units, unitCount, plazaData, particles, dt);
        if (allGone) {
          // All enemies fled — initialize game state and spawn first wave
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
          playerGold = 20;
          for (int i = 0; i < MAX_INVENTORY_SLOTS; i++)
            inventory[i].abilityId = -1;
          for (int i = 0; i < MAX_ITEMS; i++)
            itemInventory[i] = ITEM_NONE;
          itemInventoryCount = 0;
          itemShopGenerated = false;
          itemDrag.dragging = false;
          RollShop(shopSlots, &playerGold, 0, currentRound, activeShopSlots);
          rollCost = rollCostBase;
          dragState.dragging = false;
          waveUpgradeText[0] = '\0';
          // First wave: spawn directly, no map choice yet
          mapActive = false;
          firstWaveDone = false;
          SpawnWave(units, &unitCount, 0, unitTypeCount, false);
          battleCount = 1;
          phase = PHASE_PREP;
        }
      }

      // Check 3D object hover
      if (!showLeaderboard && !showMultiplayerPanel && !showEscMenu &&
          !showHelp) {
        plazaHoverObject = PlazaCheckObjectHover(camera, trophyPos, doorPos);
      } else {
        plazaHoverObject = 0;
      }

      // Hero selection: raycast to red units for hover detection
      plazaHoverUnit = -1;
      if (!lobbySelection.heroSelected && !showLeaderboard &&
          !showMultiplayerPanel && !showEscMenu && !showHelp) {
        Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
        float bestDist = 999999.0f;
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active || units[i].team != TEAM_RED)
            continue;
          // Approximate bounding box around unit
          BoundingBox uBox = {{units[i].position.x - 4.0f, units[i].position.y,
                               units[i].position.z - 4.0f},
                              {units[i].position.x + 4.0f,
                               units[i].position.y + 10.0f,
                               units[i].position.z + 4.0f}};
          RayCollision hit = GetRayCollisionBox(ray, uBox);
          if (hit.hit && hit.distance < bestDist) {
            bestDist = hit.distance;
            plazaHoverUnit = i;
          }
        }
      }

      // Hero selection: click to recruit hovered red unit
      if (!lobbySelection.heroSelected && plazaHoverUnit >= 0 &&
          IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !showLeaderboard &&
          !showMultiplayerPanel && !showEscMenu && !showHelp) {
        int ri = plazaHoverUnit;
        int heroType = units[ri].typeIndex;
        uint8_t heroRarity = units[ri].rarity;
        // Remove the red unit
        units[ri].active = false;
        // Spawn as blue unit
        if (SpawnUnit(units, &unitCount, heroType, TEAM_BLUE)) {
          int newIdx = unitCount - 1;
          units[newIdx].rarity = heroRarity;
          if (heroRarity > 0)
            ApplyUnitRarity(&units[newIdx]);
          units[newIdx].position.x = (float)GetRandomValue(-30, 30);
          units[newIdx].position.z = (float)GetRandomValue(20, 60);
          PlaySound(sfxNewCharacter);
          intro = (UnitIntro){.active = true,
                              .timer = 0.0f,
                              .typeIndex = heroType,
                              .unitIndex = newIdx,
                              .animFrame = 0,
                              .rarity = heroRarity};
          lobbySelection.heroSelected = true;
          plazaHoverUnit = -1;
        }
      }

      // Auto-start: after hero selected + intro finished → trigger scared →
      // flee → prep
      if (lobbySelection.heroSelected && !intro.active &&
          postIntroDelay <= 0.0f && plazaState == PLAZA_ROAMING) {
        PlazaTriggerScared(units, unitCount, plazaData, &plazaState,
                           &plazaTimer);
      }

      // Click handling for 3D objects and overlays
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (showLeaderboard) {
          // Close button for leaderboard overlay
          int sw = GetScreenWidth();
          int sh = GetScreenHeight();
          Rectangle closeBtn = {(float)(sw / 2 + 280), (float)(sh / 2 - 250),
                                40, 40};
          if (CheckCollisionPointRec(GetMousePosition(), closeBtn))
            showLeaderboard = false;
        } else if (showMultiplayerPanel) {
          // Multiplayer panel click handling
          int sw = GetScreenWidth();
          int sh = GetScreenHeight();
          int panelW = 400, panelH = 340;
          int panelX = sw / 2 - panelW / 2;
          int panelY = sh / 2 - panelH / 2;
          Vector2 mouse = GetMousePosition();

          // Close button
          Rectangle closeBtn = {(float)(panelX + panelW - 36),
                                (float)(panelY + 4), 32, 32};
          if (CheckCollisionPointRec(mouse, closeBtn)) {
            PlaySound(sfxUiClick);
            showMultiplayerPanel = false;
          }

          // Name input field
          Rectangle nameField = {(float)(panelX + 50), (float)(panelY + 60),
                                 (float)(panelW - 100), 36};
          if (CheckCollisionPointRec(mouse, nameField))
            nameInputActive = true;
          else
            nameInputActive = false;

#ifdef USE_EOS
          // LAN / Online toggle tabs (click handling)
          {
            int tabW = (panelW - 100) / 2;
            Rectangle lanTab = {(float)(panelX + 50), (float)(panelY + 105),
                                (float)tabW, 24};
            Rectangle onlineTab = {(float)(panelX + 50 + tabW),
                                   (float)(panelY + 105), (float)tabW, 24};
            bool hitLan = CheckCollisionPointRec(mouse, lanTab);
            bool hitOnl = CheckCollisionPointRec(mouse, onlineTab);
            if (hitLan || hitOnl) {
              printf("[UI] Tab click: lan=%d onl=%d useEos=%d eosLoggedIn=%d "
                     "mouse=(%.0f,%.0f) onlRect=(%.0f,%.0f,%.0f,%.0f)\n",
                     hitLan, hitOnl, useEos, eos_is_logged_in(), mouse.x,
                     mouse.y, onlineTab.x, onlineTab.y, onlineTab.width,
                     onlineTab.height);
              fflush(stdout);
            }
            if (hitLan && useEos) {
              useEos = false;
              PlaySound(sfxUiClick);
            }
            if (hitOnl && !useEos && eos_is_logged_in()) {
              useEos = true;
              PlaySound(sfxUiClick);
            }
          }
#endif

          // HOST GAME button
          Rectangle hostBtn = {(float)(panelX + 50), (float)(panelY + 140),
                               (float)(panelW - 100), 40};
          if (CheckCollisionPointRec(mouse, hostBtn)) {
            PlaySound(sfxUiClick);
            menuError[0] = '\0';
#ifdef USE_EOS
            if (useEos) {
              isHosting = true;
              isMultiplayer = true;
              playerReady = false;
              if (eos_host_lobby(&eosClient, playerName) == 0) {
                showMultiplayerPanel = false;
                mpLobby = (MpLobbySelection){.slotTypes = {-1, -1, -1, -1},
                                             .activeSlot = 0,
                                             .selectionComplete = false,
                                             .glowTimer = 0.0f};
                for (int u = 0; u < unitCount; u++)
                  if (units[u].team == TEAM_BLUE)
                    units[u].active = false;
                CompactBlueUnits(units, &unitCount);
                phase = PHASE_LOBBY;
              } else {
                snprintf(menuError, sizeof(menuError), "%s",
                         eosClient.errorMsg);
                isHosting = false;
                isMultiplayer = false;
              }
            } else
#endif
                if (host_start(NET_PORT) == 0) {
              isHosting = true;
              isMultiplayer = true;
              playerReady = false;
              if (net_client_connect(&netClient, "127.0.0.1", NET_PORT, NULL,
                                     playerName) == 0) {
                showMultiplayerPanel = false;
                mpLobby = (MpLobbySelection){.slotTypes = {-1, -1, -1, -1},
                                             .activeSlot = 0,
                                             .selectionComplete = false,
                                             .glowTimer = 0.0f};
                for (int u = 0; u < unitCount; u++)
                  if (units[u].team == TEAM_BLUE)
                    units[u].active = false;
                CompactBlueUnits(units, &unitCount);
                phase = PHASE_LOBBY;
              } else {
                snprintf(menuError, sizeof(menuError), "%s",
                         netClient.errorMsg);
                host_stop();
                isHosting = false;
                isMultiplayer = false;
              }
            } else {
              strncpy(menuError, "Failed to start server",
                      sizeof(menuError) - 1);
            }
          }

          // JOIN GAME button
          Rectangle joinBtn = {(float)(panelX + 50), (float)(panelY + 200),
                               (float)(panelW - 100), 40};
#ifdef USE_EOS
          bool joinReady =
              useEos ? (joinLobbyCodeLen == LOBBY_CODE_LEN) : (joinIpLen > 0);
#else
          bool joinReady = (joinIpLen > 0);
#endif
          if (joinReady && CheckCollisionPointRec(mouse, joinBtn)) {
            PlaySound(sfxUiClick);
            menuError[0] = '\0';
            isMultiplayer = true;
            isHosting = false;
            playerReady = false;
#ifdef USE_EOS
            if (useEos) {
              if (eos_join_lobby(&eosClient, joinLobbyCode, playerName) == 0) {
                showMultiplayerPanel = false;
                mpLobby = (MpLobbySelection){.slotTypes = {-1, -1, -1, -1},
                                             .activeSlot = 0,
                                             .selectionComplete = false,
                                             .glowTimer = 0.0f};
                for (int u = 0; u < unitCount; u++)
                  if (units[u].team == TEAM_BLUE)
                    units[u].active = false;
                CompactBlueUnits(units, &unitCount);
                phase = PHASE_LOBBY;
              } else {
                snprintf(menuError, sizeof(menuError), "%s",
                         eosClient.errorMsg);
                isMultiplayer = false;
              }
            } else
#endif
                if (net_client_connect(&netClient, joinIpAddress, NET_PORT,
                                       NULL, playerName) == 0) {
              showMultiplayerPanel = false;
              mpLobby = (MpLobbySelection){.slotTypes = {-1, -1, -1, -1},
                                           .activeSlot = 0,
                                           .selectionComplete = false,
                                           .glowTimer = 0.0f};
              for (int u = 0; u < unitCount; u++)
                if (units[u].team == TEAM_BLUE)
                  units[u].active = false;
              CompactBlueUnits(units, &unitCount);
              phase = PHASE_LOBBY;
            } else {
              snprintf(menuError, sizeof(menuError), "%s", netClient.errorMsg);
              isMultiplayer = false;
            }
          }
        } else {
          // 3D object clicks
          if (plazaHoverObject == 1) {
            PlaySound(sfxUiClick);
            showLeaderboard = true;
            leaderboardScroll = 0;
          } else if (plazaHoverObject == 2) {
            PlaySound(sfxUiClick);
            showMultiplayerPanel = true;
            net_trigger_firewall_prompt(NET_PORT);
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
        if (IsKeyPressed(KEY_ENTER)) {
          nameInputActive = false;
          SaveSettings(musicVolume, sfxVolume, isFullscreen, playerName);
        }
      }

      // IP address / lobby code text input
      if (showMultiplayerPanel && !nameInputActive) {
        int key = GetCharPressed();
        while (key > 0) {
#ifdef USE_EOS
          if (useEos) {
            // Lobby code: uppercase alphanumeric, max 4 chars
            char c = (char)key;
            if (c >= 'a' && c <= 'z')
              c -= 32; // uppercase
            if (joinLobbyCodeLen < LOBBY_CODE_LEN &&
                ((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9'))) {
              joinLobbyCode[joinLobbyCodeLen] = c;
              joinLobbyCodeLen++;
              joinLobbyCode[joinLobbyCodeLen] = '\0';
            }
          } else
#endif
          {
            if (joinIpLen < 63 && ((key >= '0' && key <= '9') || key == '.')) {
              joinIpAddress[joinIpLen] = (char)key;
              joinIpLen++;
              joinIpAddress[joinIpLen] = '\0';
            }
          }
          key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
#ifdef USE_EOS
          if (useEos) {
            if (joinLobbyCodeLen > 0) {
              joinLobbyCodeLen--;
              joinLobbyCode[joinLobbyCodeLen] = '\0';
            }
          } else
#endif
              if (joinIpLen > 0) {
            joinIpLen--;
            joinIpAddress[joinIpLen] = '\0';
          }
        }
      }

      // ESC closes overlays or toggles escape menu
      if (IsKeyPressed(KEY_ESCAPE)) {
        if (showHelp)
          showHelp = false;
        else if (showLeaderboard)
          showLeaderboard = false;
        else if (showMultiplayerPanel)
          showMultiplayerPanel = false;
        else
          showEscMenu = !showEscMenu;
      }

      // Leaderboard scroll
      if (showLeaderboard) {
        int wheel = (int)GetMouseWheelMove();
        leaderboardScroll -= wheel * 40;
        if (leaderboardScroll < 0)
          leaderboardScroll = 0;
        int maxScroll = leaderboard.entryCount * 80 - 400;
        if (maxScroll < 0)
          maxScroll = 0;
        if (leaderboardScroll > maxScroll)
          leaderboardScroll = maxScroll;
      }

      // Debug spawn buttons click handling during plaza
      if (debugMode && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
          !showLeaderboard && !showMultiplayerPanel && !showEscMenu &&
          !showHelp) {
        Vector2 mouse = GetMousePosition();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        int dHudTop = sh - hudTotalH;
        int plazaValidCount = 0;
        for (int i = 0; i < unitTypeCount; i++)
          if (unitTypes[i].name)
            plazaValidCount++;
        int btnYStart =
            dHudTop - (plazaValidCount * (btnHeight + btnMargin)) - btnMargin;

        bool plazaClickedBtn = false;
        int btnXBlue = btnMargin;
        int clickIdx = 0;
        for (int i = 0; i < unitTypeCount; i++) {
          if (!unitTypes[i].name)
            continue;
          Rectangle r = {
              (float)btnXBlue,
              (float)(btnYStart + clickIdx * (btnHeight + btnMargin)),
              (float)btnWidth, (float)btnHeight};
          clickIdx++;
          if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded) {
            if (SpawnUnit(units, &unitCount, i, TEAM_BLUE)) {
              if (debugSpawnRarity > 0) {
                units[unitCount - 1].rarity = debugSpawnRarity;
                ApplyUnitRarity(&units[unitCount - 1]);
              }
              PlaySound(sfxNewCharacter);
              printf("[PLAZA SPAWN] type=%d (%s) idx=%d rarity=%d hp=%.1f "
                     "hpMult=%.2f\n",
                     i, unitTypes[i].name, unitCount - 1,
                     units[unitCount - 1].rarity,
                     units[unitCount - 1].currentHealth,
                     units[unitCount - 1].hpMultiplier);
              // Place on blue side
              units[unitCount - 1].position.x = (float)GetRandomValue(-50, 50);
              units[unitCount - 1].position.z = (float)GetRandomValue(10, 80);
              intro = (UnitIntro){.active = true,
                                  .timer = 0.0f,
                                  .typeIndex = i,
                                  .unitIndex = unitCount - 1,
                                  .animFrame = 0,
                                  .rarity = units[unitCount - 1].rarity};
            }
            plazaClickedBtn = true;
            break;
          }
        }
        // Rarity debug buttons (rare + legendary mushroom)
        {
          int rY = btnYStart + clickIdx * (btnHeight + btnMargin);
          Rectangle rr = {(float)btnXBlue, (float)rY, (float)btnWidth,
                          (float)btnHeight};
          if (CheckCollisionPointRec(mouse, rr) && unitTypes[0].loaded) {
            if (SpawnUnit(units, &unitCount, 0, TEAM_BLUE)) {
              PlaySound(sfxNewCharacter);
              units[unitCount - 1].rarity = RARITY_RARE;
              ApplyUnitRarity(&units[unitCount - 1]);
              printf("[PLAZA SPAWN] Rare Mushroom idx=%d rarity=%d hp=%.1f\n",
                     unitCount - 1, units[unitCount - 1].rarity,
                     units[unitCount - 1].currentHealth);
              units[unitCount - 1].position.x = (float)GetRandomValue(-50, 50);
              units[unitCount - 1].position.z = (float)GetRandomValue(10, 80);
              intro = (UnitIntro){.active = true,
                                  .timer = 0.0f,
                                  .typeIndex = 0,
                                  .unitIndex = unitCount - 1,
                                  .animFrame = 0,
                                  .rarity = RARITY_RARE};
            }
          }
          rY += btnHeight + btnMargin;
          Rectangle lr = {(float)btnXBlue, (float)rY, (float)btnWidth,
                          (float)btnHeight};
          if (CheckCollisionPointRec(mouse, lr) && unitTypes[0].loaded) {
            if (SpawnUnit(units, &unitCount, 0, TEAM_BLUE)) {
              PlaySound(sfxNewCharacter);
              units[unitCount - 1].rarity = RARITY_LEGENDARY;
              ApplyUnitRarity(&units[unitCount - 1]);
              printf(
                  "[PLAZA SPAWN] Legendary Mushroom idx=%d rarity=%d hp=%.1f\n",
                  unitCount - 1, units[unitCount - 1].rarity,
                  units[unitCount - 1].currentHealth);
              units[unitCount - 1].position.x = (float)GetRandomValue(-50, 50);
              units[unitCount - 1].position.z = (float)GetRandomValue(10, 80);
              intro = (UnitIntro){.active = true,
                                  .timer = 0.0f,
                                  .typeIndex = 0,
                                  .unitIndex = unitCount - 1,
                                  .animFrame = 0,
                                  .rarity = RARITY_LEGENDARY};
            }
          }
        }

        // Env piece spawn + save buttons (plaza debug)
        if (!plazaClickedBtn) {
          int envBtnW = 110, envBtnH = 24, envBtnGap = 4;
          int envColX = sw / 2 - envBtnW / 2;
          int envStartY = btnYStart;
          for (int ei = 0; ei < envModelCount; ei++) {
            if (!envModels[ei].loaded)
              continue;
            Rectangle er = {(float)envColX,
                            (float)(envStartY + ei * (envBtnH + envBtnGap)),
                            (float)envBtnW, (float)envBtnH};
            if (CheckCollisionPointRec(mouse, er) &&
                envPieceCount < MAX_ENV_PIECES) {
              envPieces[envPieceCount] = (EnvPiece){.modelIndex = ei,
                                                    .position = {0, 0, 0},
                                                    .rotationX = 0,
                                                    .rotationY = 0,
                                                    .rotationZ = 0,
                                                    .scale = 1.0f,
                                                    .active = true};
              envSelectedPiece = envPieceCount;
              envPieceCount++;
              plazaClickedBtn = true;
              break;
            }
          }
          if (!plazaClickedBtn) {
            int saveY = envStartY + envModelCount * (envBtnH + envBtnGap) + 4;
            Rectangle saveBtn = {(float)envColX, (float)saveY, (float)envBtnW,
                                 (float)envBtnH};
            if (CheckCollisionPointRec(mouse, saveBtn)) {
              FILE *fp = fopen("env_layout.txt", "w");
              if (fp) {
                fprintf(
                    fp,
                    "# modelIndex x y z rotationX rotationY rotationZ scale\n");
                for (int pi = 0; pi < envPieceCount; pi++) {
                  if (!envPieces[pi].active)
                    continue;
                  fprintf(fp, "%d %.1f %.1f %.1f %.1f %.1f %.1f %.2f\n",
                          envPieces[pi].modelIndex, envPieces[pi].position.x,
                          envPieces[pi].position.y, envPieces[pi].position.z,
                          envPieces[pi].rotationX, envPieces[pi].rotationY,
                          envPieces[pi].rotationZ, envPieces[pi].scale);
                }
                fclose(fp);
                envSaveFlashTimer = 2.0f;
              }
            }
          }
        }

        // Env piece 3D picking (plaza, debug mode)
        bool mouseOnDebugSliders = debugMode && mouse.x < 280 && mouse.y < 210;
        if (!plazaClickedBtn && !mouseOnDebugSliders) {
          int dHudTop2 = sh - hudTotalH;
          if (mouse.y < dHudTop2) {
            Ray envRay = GetScreenToWorldRay(mouse, camera);
            float closestDist = 1e9f;
            int closestIdx = -1;
            for (int ep = 0; ep < envPieceCount; ep++) {
              if (!envPieces[ep].active)
                continue;
              EnvModelDef *emd = &envModels[envPieces[ep].modelIndex];
              if (!emd->loaded || emd->model.meshCount == 0)
                continue;
              BoundingBox mbb = GetMeshBoundingBox(emd->model.meshes[0]);
              Matrix mt = emd->model.transform;
              Vector3 corners[8] = {
                  {mbb.min.x, mbb.min.y, mbb.min.z},
                  {mbb.max.x, mbb.min.y, mbb.min.z},
                  {mbb.min.x, mbb.max.y, mbb.min.z},
                  {mbb.max.x, mbb.max.y, mbb.min.z},
                  {mbb.min.x, mbb.min.y, mbb.max.z},
                  {mbb.max.x, mbb.min.y, mbb.max.z},
                  {mbb.min.x, mbb.max.y, mbb.max.z},
                  {mbb.max.x, mbb.max.y, mbb.max.z},
              };
              BoundingBox tbb = {.min = {1e9f, 1e9f, 1e9f},
                                 .max = {-1e9f, -1e9f, -1e9f}};
              for (int ci = 0; ci < 8; ci++) {
                Vector3 tc = Vector3Transform(corners[ci], mt);
                if (tc.x < tbb.min.x)
                  tbb.min.x = tc.x;
                if (tc.y < tbb.min.y)
                  tbb.min.y = tc.y;
                if (tc.z < tbb.min.z)
                  tbb.min.z = tc.z;
                if (tc.x > tbb.max.x)
                  tbb.max.x = tc.x;
                if (tc.y > tbb.max.y)
                  tbb.max.y = tc.y;
                if (tc.z > tbb.max.z)
                  tbb.max.z = tc.z;
              }
              float ps = envPieces[ep].scale;
              BoundingBox wbb = {
                  .min = {tbb.min.x * ps + envPieces[ep].position.x,
                          tbb.min.y * ps + envPieces[ep].position.y,
                          tbb.min.z * ps + envPieces[ep].position.z},
                  .max = {tbb.max.x * ps + envPieces[ep].position.x,
                          tbb.max.y * ps + envPieces[ep].position.y,
                          tbb.max.z * ps + envPieces[ep].position.z}};
              RayCollision rc = GetRayCollisionBox(envRay, wbb);
              if (rc.hit) {
                // Build full world transform (same as draw code)
                EnvPiece p = envPieces[ep];
                float es = p.scale;
                Matrix matW = MatrixScale(es, es, es);
                matW =
                    MatrixMultiply(matW, MatrixRotateX(p.rotationX * DEG2RAD));
                matW =
                    MatrixMultiply(matW, MatrixRotateY(p.rotationY * DEG2RAD));
                matW =
                    MatrixMultiply(matW, MatrixRotateZ(p.rotationZ * DEG2RAD));
                matW = MatrixMultiply(
                    matW,
                    MatrixTranslate(p.position.x, p.position.y, p.position.z));
                Matrix fullTransform =
                    MatrixMultiply(emd->model.transform, matW);
                // Test all meshes in the model
                for (int mi = 0; mi < emd->model.meshCount; mi++) {
                  RayCollision mc = GetRayCollisionMesh(
                      envRay, emd->model.meshes[mi], fullTransform);
                  if (mc.hit && mc.distance < closestDist) {
                    closestDist = mc.distance;
                    closestIdx = ep;
                  }
                }
              }
            }
            envSelectedPiece = closestIdx;
            envDragging = (closestIdx >= 0);
            if (closestIdx >= 0) {
              Ray grabRay = GetScreenToWorldRay(mouse, camera);
              RayCollision grabHit = GetRayCollisionQuad(
                  grabRay, (Vector3){-500, 0, -500}, (Vector3){-500, 0, 500},
                  (Vector3){500, 0, 500}, (Vector3){500, 0, -500});
              if (grabHit.hit) {
                envDragOffset.x =
                    envPieces[closestIdx].position.x - grabHit.point.x;
                envDragOffset.z =
                    envPieces[closestIdx].position.z - grabHit.point.z;
              }
            }
          }
        }
      }
    }
    //------------------------------------------------------------------------------
    // PHASE: LOBBY — waiting for opponent / game start
    //------------------------------------------------------------------------------
    else if (phase == PHASE_LOBBY) {
#ifdef USE_EOS
      if (useEos)
        eos_client_poll(&eosClient);
      else
#endif
        net_client_poll(&netClient);

#ifdef USE_EOS
#define NC_STATE                                                               \
  (useEos ? (eosClient.state == EOS_STATE_ERROR)                               \
          : (netClient.state == NET_ERROR))
#define NC_ERR (useEos ? eosClient.errorMsg : netClient.errorMsg)
#define NC_FLAG(f) (useEos ? eosClient.f : netClient.f)
#define NC_CLEAR(f)                                                            \
  do {                                                                         \
    if (useEos)                                                                \
      eosClient.f = false;                                                     \
    else                                                                       \
      netClient.f = false;                                                     \
  } while (0)
#else
#define NC_STATE (netClient.state == NET_ERROR)
#define NC_ERR (netClient.errorMsg)
#define NC_FLAG(f) (netClient.f)
#define NC_CLEAR(f)                                                            \
  do {                                                                         \
    netClient.f = false;                                                       \
  } while (0)
#endif

      if (NC_STATE) {
        printf("[LOBBY ERROR] %s\n", NC_ERR);
        fflush(stdout);
        snprintf(menuError, sizeof(menuError), "%s", NC_ERR);
#ifdef USE_EOS
        if (useEos) {
          eos_client_disconnect(&eosClient);
        } else
#endif
        {
          if (isHosting) {
            host_stop();
            isHosting = false;
          }
          net_client_disconnect(&netClient);
        }
        isMultiplayer = false;
        unitCount = 0;
        memset(plazaData, 0, sizeof(plazaData));
        PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);
        plazaState = PLAZA_ROAMING;
        phase = PHASE_PLAZA;
      }

      if (NC_FLAG(gameStarted)) {
        NC_CLEAR(gameStarted);
        playerGold = NC_FLAG(currentGold);
      }

      // Wait for both prep data AND unit selection before entering prep
      if (NC_FLAG(prepStarted) && mpLobby.selectionComplete) {
        NC_CLEAR(prepStarted);
        playerGold = NC_FLAG(currentGold);
        currentRound = NC_FLAG(currentRound);
        currentRoundIsPve = NC_FLAG(isPveRound);
        for (int i = 0; i < MAX_SHOP_SLOTS; i++) {
#ifdef USE_EOS
          shopSlots[i] =
              useEos ? eosClient.serverShop[i] : netClient.serverShop[i];
#else
          shopSlots[i] = netClient.serverShop[i];
#endif
        }
        // Reset multiplayer game state — keep blue units from lobby
        ClearRedUnits(units, &unitCount);
        snapshotCount = 0;
        mpHealth[0] = 20;
        mpHealth[1] = 20;
        lastRoundDamage = 0;
        roundResultText = "";
        ClearAllModifiers(modifiers);
        ClearAllProjectiles(projectiles);
        ClearAllParticles(particles);
        ClearAllFloatingTexts(floatingTexts);
        ClearAllFissures(fissures);
        dragState.dragging = false;
        playerReady = false;
        waitingForOpponent = false;
        opponentIsReady = false;
        phase = PHASE_PREP;
      }

      // Multiplayer lobby unit picker
      mpLobby.glowTimer += dt;
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !showEscMenu) {
        int lsw = GetScreenWidth();
        int lsh = GetScreenHeight();
        Vector2 mouse = GetMousePosition();

        // Class picker buttons (below slots)
        int cpBtnW = 140, cpBtnH = 50, cpBtnGap = 16;
        int cpTotalW = VALID_UNIT_TYPE_COUNT * cpBtnW +
                       (VALID_UNIT_TYPE_COUNT - 1) * cpBtnGap;
        int cpStartX = lsw / 2 - cpTotalW / 2;
        int cpY = lsh / 2 + 40;

        for (int c = 0; c < VALID_UNIT_TYPE_COUNT; c++) {
          Rectangle cpBtn = {(float)(cpStartX + c * (cpBtnW + cpBtnGap)),
                             (float)cpY, (float)cpBtnW, (float)cpBtnH};
          if (CheckCollisionPointRec(mouse, cpBtn) &&
              mpLobby.activeSlot < BLUE_TEAM_MAX_SIZE) {
            int type = VALID_UNIT_TYPES[c];
            mpLobby.slotTypes[mpLobby.activeSlot] = type;
            // Spawn the unit
            if (SpawnUnit(units, &unitCount, type, TEAM_BLUE)) {
              units[unitCount - 1].position.x =
                  -40.0f + mpLobby.activeSlot * 25.0f;
              units[unitCount - 1].position.z = 40.0f;
            }
            mpLobby.activeSlot++;
            if (mpLobby.activeSlot >= BLUE_TEAM_MAX_SIZE)
              mpLobby.selectionComplete = true;
            break;
          }
        }

        // Click filled slot to undo
        int slotW = 100, slotH = 60, slotGap = 16;
        int slotTotalW =
            BLUE_TEAM_MAX_SIZE * slotW + (BLUE_TEAM_MAX_SIZE - 1) * slotGap;
        int slotStartX = lsw / 2 - slotTotalW / 2;
        int slotY = lsh / 2 - 80;

        for (int s = 0; s < mpLobby.activeSlot; s++) {
          Rectangle slotRect = {(float)(slotStartX + s * (slotW + slotGap)),
                                (float)slotY, (float)slotW, (float)slotH};
          if (CheckCollisionPointRec(mouse, slotRect)) {
            // Remove the unit at this slot
            // Find and deactivate the blue unit of this type (from the end)
            for (int u = unitCount - 1; u >= 0; u--) {
              if (units[u].active && units[u].team == TEAM_BLUE &&
                  units[u].typeIndex == mpLobby.slotTypes[s]) {
                units[u].active = false;
                break;
              }
            }
            // Shift slots down
            for (int ss = s; ss < mpLobby.activeSlot - 1; ss++)
              mpLobby.slotTypes[ss] = mpLobby.slotTypes[ss + 1];
            mpLobby.slotTypes[mpLobby.activeSlot - 1] = -1;
            mpLobby.activeSlot--;
            mpLobby.selectionComplete = false;
            break;
          }
        }
      }

      if (IsKeyPressed(KEY_ESCAPE)) {
        showEscMenu = !showEscMenu;
      }
    }
    //------------------------------------------------------------------------------
    // PHASE: MAP — Slay the Spire branching map (singleplayer)
    //------------------------------------------------------------------------------
    else if (phase == PHASE_MAP) {
      if (IsKeyPressed(KEY_ESCAPE)) {
        if (showHelp)
          showHelp = false;
        else
          showEscMenu = !showEscMenu;
      }

      if (showingMapEvent && !showEscMenu && !showHelp) {
        // Event screen: handle choice buttons (data-driven)
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
          Vector2 emouse = GetMousePosition();
          int esw = GetScreenWidth(), esh = GetScreenHeight();
          const EventDef *evt = &EVENT_DEFS[currentEventIndex];
          int btnW = 260, btnH = 60, btnGap = 24;
          int totalBtnW = evt->choiceCount * btnW + (evt->choiceCount - 1) * btnGap;
          int btnStartX = esw / 2 - totalBtnW / 2;
          int btnY = esh / 2 + 40;
          for (int c = 0; c < evt->choiceCount; c++) {
            Rectangle btnRect = {(float)(btnStartX + c * (btnW + btnGap)),
                                 (float)btnY, (float)btnW, (float)btnH};
            if (CheckCollisionPointRec(emouse, btnRect)) {
              mapEventChoice = c;
              break;
            }
          }
          if (mapEventChoice >= 0) {
            const EventChoice *choice = &evt->choices[mapEventChoice];
            bool needsPicker = false;
            int pickerType = 0;
            ApplyEventEffect(choice->effect, choice->value, choice->cost,
                             units, unitCount, &playerGold,
                             &needsPicker, &pickerType);
            // Handle shop slot changes (needs local var access)
            if (choice->effect == EVFX_ADD_SHOP_SLOT && activeShopSlots < MAX_SHOP_SLOTS)
              activeShopSlots++;
            else if (choice->effect == EVFX_REMOVE_SHOP_SLOT && activeShopSlots > 1)
              activeShopSlots--;
            // Save snapshot and return to map
            SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
            showingMapEvent = false;
            mapEventChoice = -1;
          }
        }
      } else if (showingItemShop && !showEscMenu && !showHelp) {
        // Item shop overlay: handle clicks
        if (IsKeyPressed(KEY_ESCAPE)) {
          showingItemShop = false;
          itemShopGenerated = false;
          SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
          Vector2 emouse = GetMousePosition();
          int esw = GetScreenWidth(), esh = GetScreenHeight();
          int iCardW = 200, iCardH = 80, iCardGap = 20;
          int iTotalW = 3 * iCardW + 2 * iCardGap;
          int iStartX = esw / 2 - iTotalW / 2;
          int iCardY = esh / 2 - 20;
          for (int io = 0; io < 3; io++) {
            int ix = iStartX + io * (iCardW + iCardGap);
            Rectangle iRect = {(float)ix, (float)iCardY, (float)iCardW, (float)iCardH};
            if (CheckCollisionPointRec(emouse, iRect)) {
              int iid = itemShopOffers[io];
              if (iid >= 0 && iid < ITEM_COUNT && itemShopBuyCount < 1) {
                const ItemDef *idef = &ITEM_DEFS[iid];
                if (playerGold >= idef->cost && itemInventoryCount < MAX_ITEMS) {
                  playerGold -= idef->cost;
                  for (int ii = 0; ii < MAX_ITEMS; ii++) {
                    if (itemInventory[ii] == ITEM_NONE) {
                      itemInventory[ii] = iid;
                      itemInventoryCount++;
                      break;
                    }
                  }
                  itemShopOffers[io] = ITEM_NONE;
                  itemShopBuyCount++;
                }
              }
              break;
            }
          }
          // Continue button
          int contW = 180, contH = 40;
          int contX = esw / 2 - contW / 2;
          int contY = iCardY + iCardH + 30;
          Rectangle contRect = {(float)contX, (float)contY, (float)contW, (float)contH};
          if (CheckCollisionPointRec(emouse, contRect)) {
            showingItemShop = false;
            itemShopGenerated = false;
            SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
          }
        }
      } else if (!showEscMenu && !showHelp) {
        int selected = UpdateMap(&actMap);
        if (selected >= 0) {
          mapSelectedNodeType = selected;
          // Compute wave round based on act and floor
          int mapRound = (actMap.act - 1) * MAP_LAYERS + actMap.currentLayer;
          currentRound = mapRound;

          switch (selected) {
          case NODE_COMBAT: {
            // Spawn normal wave and go to prep
            battleCount++;
            SpawnWave(units, &unitCount, mapRound, unitTypeCount, false);
            // Apply act scaling
            for (int i = 0; i < unitCount; i++) {
              if (units[i].active && units[i].team == TEAM_RED) {
                float actHpMult = 1.0f + 0.3f * (actMap.act - 1);
                float actDmgMult = 1.0f + 0.15f * (actMap.act - 1);
                units[i].hpMultiplier *= actHpMult;
                units[i].dmgMultiplier *= actDmgMult;
                units[i].currentHealth = UNIT_STATS[units[i].typeIndex].health *
                                         units[i].hpMultiplier;
              }
            }
            waveUpgradeText[0] = '\0';
            RollShop(shopSlots, &playerGold, 0, currentRound, activeShopSlots);
            rollCost = rollCostBase;
            phase = PHASE_PREP;
            if (pendingIntro.active) { intro = pendingIntro; pendingIntro.active = false; }
          } break;
          case NODE_ELITE: {
            // Elite fight: enemies are rare/legendary
            battleCount++;
            SpawnWave(units, &unitCount, mapRound, unitTypeCount, false);
            for (int i = 0; i < unitCount; i++) {
              if (units[i].active && units[i].team == TEAM_RED) {
                // Act scaling
                float actHpMult = 1.0f + 0.3f * (actMap.act - 1);
                float actDmgMult = 1.0f + 0.15f * (actMap.act - 1);
                units[i].hpMultiplier *= actHpMult;
                units[i].dmgMultiplier *= actDmgMult;
                // All enemies are rare; 20% chance to be legendary
                units[i].rarity = (GetRandomValue(0, 99) < 20)
                                      ? RARITY_LEGENDARY
                                      : RARITY_RARE;
                ApplyUnitRarity(&units[i]);
                units[i].currentHealth = UNIT_STATS[units[i].typeIndex].health *
                                         units[i].hpMultiplier;
              }
            }
            snprintf(waveUpgradeText, sizeof(waveUpgradeText), "ELITE FIGHT!");
            RollShop(shopSlots, &playerGold, 0, currentRound, activeShopSlots);
            rollCost = rollCostBase;
            phase = PHASE_PREP;
            if (pendingIntro.active) { intro = pendingIntro; pendingIntro.active = false; }
          } break;
          case NODE_BOSS: {
            // Boss wave — spawn a big boss + normal enemies
            battleCount++;
            SpawnWave(units, &unitCount, mapRound, unitTypeCount, true);
            // Find first red unit and make it the boss
            bool bossAssigned = false;
            for (int i = 0; i < unitCount; i++) {
              if (units[i].active && units[i].team == TEAM_RED) {
                float actHpMult = 1.0f + 0.3f * (actMap.act - 1);
                float actDmgMult = 1.0f + 0.15f * (actMap.act - 1);
                if (!bossAssigned) {
                  // Boss: big, tanky, hits hard
                  float bossScale = 2.0f + 0.1f * (float)actMap.act;
                  if (bossScale > 3.5f)
                    bossScale = 3.5f;
                  units[i].scaleOverride = bossScale;
                  units[i].hpMultiplier *= actHpMult * 1.5f;
                  units[i].dmgMultiplier *= actDmgMult * 1.1f;
                  units[i].rarity = RARITY_LEGENDARY;
                  ApplyUnitRarity(&units[i]);
                  bossAssigned = true;
                } else {
                  // Support enemies: slightly smaller
                  units[i].scaleOverride = 0.8f;
                  units[i].hpMultiplier *= actHpMult * 0.8f;
                  units[i].dmgMultiplier *= actDmgMult * 0.8f;
                }
                units[i].currentHealth = UNIT_STATS[units[i].typeIndex].health *
                                         units[i].hpMultiplier;
              }
            }
            snprintf(waveUpgradeText, sizeof(waveUpgradeText), "ACT %d BOSS!",
                     actMap.act);
            RollShop(shopSlots, &playerGold, 0, currentRound, activeShopSlots);
            rollCost = rollCostBase;
            phase = PHASE_PREP;
            if (pendingIntro.active) { intro = pendingIntro; pendingIntro.active = false; }
          } break;
          case NODE_SHOP: {
            // Shop: show item shop overlay on map
            {
              uint32_t iseed = actMap.seed + (uint32_t)actMap.currentNode * 97;
              for (int io = 0; io < 3; io++) {
                iseed ^= iseed << 13;
                iseed ^= iseed >> 17;
                iseed ^= iseed << 5;
                int attempts = 0;
                int pick;
                do {
                  pick = (int)(iseed % (uint32_t)ITEM_COUNT);
                  iseed ^= iseed << 13;
                  iseed ^= iseed >> 17;
                  iseed ^= iseed << 5;
                  attempts++;
                } while ((!ITEM_DEFS[pick].enabled ||
                          pick == itemShopOffers[0] ||
                          pick == itemShopOffers[1]) &&
                         attempts < 20);
                itemShopOffers[io] = pick;
              }
              itemShopGenerated = true;
              itemShopBuyCount = 0;
            }
            showingItemShop = true;
          } break;
          case NODE_REST: {
            // Heal all units by 30% max HP
            for (int i = 0; i < unitCount; i++) {
              if (units[i].active && units[i].team == TEAM_BLUE) {
                float maxHP = UNIT_STATS[units[i].typeIndex].health *
                              units[i].hpMultiplier;
                units[i].currentHealth += maxHP * 0.3f;
                if (units[i].currentHealth > maxHP)
                  units[i].currentHealth = maxHP;
              }
            }
            SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
            // Stay on map (node already visited)
          } break;
          case NODE_EVENT: {
            // Show event screen
            currentEventIndex = GetRandomEventIndex(
                (uint32_t)(actMap.seed + actMap.currentNode * 31));
            showingMapEvent = true;
            mapEventChoice = -1;
          } break;
          default:
            break;
          }
        }
      }
    }
    //------------------------------------------------------------------------------
    // PHASE: PREP — place units, click Play to start
    //------------------------------------------------------------------------------
    else if (phase == PHASE_PREP) {
      if (IsKeyPressed(KEY_ESCAPE)) {
        if (showHelp)
          showHelp = false;
        else
          showEscMenu = !showEscMenu;
      }
      // --- Multiplayer: poll network and handle server messages ---
      if (isMultiplayer && opponentIsReady && oppReadyCountdown > 0.0f) {
        oppReadyCountdown -= dt;
        if (oppReadyCountdown < 0.0f) oppReadyCountdown = 0.0f;
      }
      if (isMultiplayer) {
#ifdef USE_EOS
        if (useEos)
          eos_client_poll(&eosClient);
        else
#endif
          net_client_poll(&netClient);
        if (NC_STATE) {
          printf("[PREP ERROR] %s\n", NC_ERR);
          fflush(stdout);
#ifdef USE_EOS
          if (useEos) {
            eos_client_disconnect(&eosClient);
          } else
#endif
          {
            if (isHosting) {
              host_stop();
              isHosting = false;
            }
            net_client_disconnect(&netClient);
          }
          isMultiplayer = false;
          unitCount = 0;
          memset(plazaData, 0, sizeof(plazaData));
          PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);
          plazaState = PLAZA_ROAMING;
          phase = PHASE_PLAZA;
        }
        // Peer disconnected — treat as game over
        if (NC_FLAG(peerDisconnected)) {
          NC_CLEAR(peerDisconnected);
          lastOutcomeWin = true;
          roundResultText = "Opponent disconnected";
          phase = PHASE_GAME_OVER;
        }
        if (NC_FLAG(shopUpdated)) {
          NC_CLEAR(shopUpdated);
          for (int i = 0; i < MAX_SHOP_SLOTS; i++) {
#ifdef USE_EOS
            shopSlots[i] =
                useEos ? eosClient.serverShop[i] : netClient.serverShop[i];
#else
            shopSlots[i] = netClient.serverShop[i];
#endif
          }
        }
        if (NC_FLAG(goldUpdated)) {
          NC_CLEAR(goldUpdated);
          playerGold = NC_FLAG(currentGold);
        }
        if (NC_FLAG(opponentReady)) {
          NC_CLEAR(opponentReady);
          waitingForOpponent = false;
          opponentIsReady = true;
          oppReadyCountdown = (float)NC_FLAG(prepTimeRemaining);
        }
        // Combat started — server sends serialized units
        if (NC_FLAG(combatStarted)) {
          NC_CLEAR(combatStarted);
#ifdef USE_EOS
          unitCount = deserialize_units(useEos ? eosClient.combatNetUnits
                                               : netClient.combatNetUnits,
                                        useEos ? eosClient.combatNetUnitCount
                                               : netClient.combatNetUnitCount,
                                        units, MAX_UNITS);
#else
          unitCount =
              deserialize_units(netClient.combatNetUnits,
                                netClient.combatNetUnitCount, units, MAX_UNITS);
#endif
          // Server already applied rarity buffs + synergies;
          // multipliers are included in the serialized NetUnit data.
          // Seed combat RNG with server's shared seed
          {
            uint32_t seed;
#ifdef USE_EOS
            seed = useEos ? eosClient.combatSeed : netClient.combatSeed;
#else
            seed = netClient.combatSeed;
#endif
            combat_rng_seed(seed);
          }
          SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
          phase = PHASE_COMBAT;
          fightBannerTimer = 0.0f;
          killCount = 0;
          multiKillCount = 0;
          multiKillTimer = 0.0f;
          killFeedTimer = -1.0f;
          slowmoTimer = 0.0f;
          slowmoScale = 1.0f;
          BattleLogClear(&battleLog);
          combatElapsedTime = 0.0f;
          combatAccum = 0.0f;
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
      for (int i = 0; i < unitCount; i++) {
        if (!units[i].active)
          continue;
        if (IsUnitInStatueSpawn(&statueSpawn, i))
          continue;
        float targetY = units[i].dragging ? 5.0f : 0.0f;
        units[i].position.y += (targetY - units[i].position.y) * 0.1f;
      }

      // Update particles during prep (so impact particles decay)
      UpdateParticles(particles, dt);

      // Dragging
      for (int i = 0; i < unitCount; i++) {
        if (!units[i].active || !units[i].dragging)
          continue;
        Ray ray = GetScreenToWorldRay(GetMousePosition(), camera);
        RayCollision groundHit = GetRayCollisionQuad(
            ray, (Vector3){-500, 0, -500}, (Vector3){-500, 0, 500},
            (Vector3){500, 0, 500}, (Vector3){500, 0, -500});
        // Only allow dragging red units in debug mode
        if (units[i].team == TEAM_RED && !debugMode) {
          units[i].dragging = false;
          continue;
        }
        if (groundHit.hit) {
          units[i].position.x = groundHit.point.x + dragOffsetX;
          units[i].position.z = groundHit.point.z + dragOffsetZ;
          // Clamp blue units to their half (positive Z = blue side)
          if (units[i].team == TEAM_BLUE) {
            if (units[i].position.z < ARENA_BOUNDARY_Z)
              units[i].position.z = ARENA_BOUNDARY_Z;
          }
          // Clamp all units to grid bounds (X and Z)
          float gridLimit = ARENA_GRID_HALF - 5.0f; // 95
          if (units[i].position.x < -gridLimit)
            units[i].position.x = -gridLimit;
          if (units[i].position.x > gridLimit)
            units[i].position.x = gridLimit;
          if (units[i].position.z < -gridLimit)
            units[i].position.z = -gridLimit;
          if (units[i].position.z > gridLimit)
            units[i].position.z = gridLimit;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
          PlaySound(sfxUiDrop);
          units[i].dragging = false;
          dragOffsetX = 0;
          dragOffsetZ = 0;
        }
      }

      // Quick-buy: keys 1, 2, 3 for shop slots
      if (!(isMultiplayer && playerReady) && !intro.active &&
          statueSpawn.phase == SSPAWN_INACTIVE) {
        int quickBuyKeys[6] = {KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR, KEY_FIVE, KEY_SIX};
        for (int s = 0; s < activeShopSlots; s++) {
          if (IsKeyPressed(quickBuyKeys[s]) && shopSlots[s].abilityId >= 0) {
            usedShopHotkey = true;
            shopHighlightAbilityId = shopSlots[s].abilityId;
            shopHighlightTimer = 0.5f;
            if (isMultiplayer) {
#ifdef USE_EOS
              if (useEos)
                eos_client_send_buy(&eosClient, s);
              else
#endif
                net_client_send_buy(&netClient, s);
              // Also process locally so ability appears immediately
              BuyAbility(&shopSlots[s], inventory, units, unitCount,
                         &playerGold);
            } else {
              // Check if a selected blue unit has an empty ability slot
              int selUnit = -1;
              for (int i = 0; i < unitCount; i++) {
                if (units[i].active && units[i].team == TEAM_BLUE &&
                    units[i].selected) {
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
                    if (units[selUnit].abilities[a].abilityId ==
                            shopSlots[s].abilityId &&
                        units[selUnit].abilities[a].level <
                            ABILITY_MAX_LEVELS - 1) {
                      units[selUnit].abilities[a].level++;
                      playerGold -= cost;
                      shopSlots[s].abilityId = -1;
                      shopSlots[s].locked = false;
                      placed = true;
                      break;
                    }
                  }
                  // Otherwise empty slot on selected unit
                  if (!placed) {
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                      if (units[selUnit].abilities[a].abilityId < 0) {
                        units[selUnit].abilities[a].abilityId =
                            shopSlots[s].abilityId;
                        units[selUnit].abilities[a].level = shopSlots[s].level;
                        playerGold -= cost;
                        shopSlots[s].abilityId = -1;
                        shopSlots[s].locked = false;
                        placed = true;
                        break;
                      }
                    }
                  }
                  // Unit full — fall back to inventory
                  if (!placed) {
                    BuyAbility(&shopSlots[s], inventory, units, unitCount,
                               &playerGold);
                  }
                  if (placed || shopSlots[s].abilityId < 0)
                    PlaySound(sfxUiBuy);
                }
              } else {
                // No unit selected — normal buy (auto-combine / inventory)
                BuyAbility(&shopSlots[s], inventory, units, unitCount,
                           &playerGold);
              }
            }
            break;
          }
        }
      }

      // Quick-roll: R key
      if (!(isMultiplayer && playerReady) && !intro.active &&
          statueSpawn.phase == SSPAWN_INACTIVE) {
        if (IsKeyPressed(KEY_R) && playerGold >= rollCost) {
          usedRollHotkey = true;
          PlaySound(sfxUiReroll);
          if (isMultiplayer) {
#ifdef USE_EOS
            if (useEos)
              eos_client_send_roll(&eosClient);
            else
#endif
              net_client_send_roll(&netClient);
          } else {
            RollShop(shopSlots, &playerGold, rollCost, currentRound, activeShopSlots);
          }
          rollCost += rollCostIncrement;
          TriggerShake(&shake, 2.0f, 0.15f);
        }
      }

      // Clicks (blocked during intro)
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !intro.active &&
          statueSpawn.phase == SSPAWN_INACTIVE && !showEscMenu && !showHelp) {
        Vector2 mouse = GetMousePosition();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        int hudTop = sh - hudTotalH;
        int btnXBlue = btnMargin;
        int btnXRed = sw - btnWidth - btnMargin;
        int prepValidCount = 0;
        for (int i = 0; i < unitTypeCount; i++)
          if (unitTypes[i].name)
            prepValidCount++;
        int btnYStart =
            hudTop - (prepValidCount * (btnHeight + btnMargin)) - btnMargin;
        Rectangle playBtn = {(float)(20),
                             (float)(hudTop - playBtnH - btnMargin),
                             (float)playBtnW, (float)playBtnH};
        bool clickedButton = false;

        // Confirm removal popup (takes priority over everything)
        if (removeConfirmUnit >= 0) {
          int popW = 280, popH = 110;
          int popX = sw / 2 - popW / 2;
          int popY = sh / 2 - popH / 2;
          int rmBtnW = 100, rmBtnH = 30;
          Rectangle yesBtn = {(float)(popX + 24),
                              (float)(popY + popH - rmBtnH - 12), (float)rmBtnW,
                              (float)rmBtnH};
          Rectangle noBtn = {(float)(popX + popW - rmBtnW - 24),
                             (float)(popY + popH - rmBtnH - 12), (float)rmBtnW,
                             (float)rmBtnH};
          if (CheckCollisionPointRec(mouse, yesBtn)) {
            // Remove the unit: deactivate
            int ri = removeConfirmUnit;
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
              units[ri].abilities[a].abilityId = -1;
            units[ri].active = false;
            removeConfirmUnit = -1;
            clickedButton = true;
            // Check if all blue units removed → return to plaza (singleplayer
            // only)
            int blueLeft = CountTeamUnits(units, unitCount, TEAM_BLUE);
            if (blueLeft == 0 && !isMultiplayer) {
              ClearRedUnits(units, &unitCount);
              CompactBlueUnits(units, &unitCount);
              memset(plazaData, 0, sizeof(plazaData));
              PlazaSpawnLobbyPool(units, &unitCount, plazaData,
                                  &lobbySelection);
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
        if (CheckCollisionPointRec(mouse, playBtn) && unitCount > 0) {
          PlaySound(sfxUiClick);
          if (isMultiplayer) {
            // Multiplayer: send READY with army
            if (!playerReady) {
              int ba = CountTeamUnits(units, unitCount, TEAM_BLUE);
              if (ba > 0) {
#ifdef USE_EOS
                if (useEos)
                  eos_client_send_ready(&eosClient, units, unitCount);
                else
#endif
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
            if (ba > 0 && ra > 0) {
              CompactBlueUnits(units, &unitCount);
              SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
              ApplySynergies(units, unitCount);
              combat_rng_seed((uint32_t)(GetTime() * 1000.0) ^
                              (uint32_t)currentRound);
              phase = PHASE_COMBAT;
              fightBannerTimer = 0.0f;
              killCount = 0;
              multiKillCount = 0;
              multiKillTimer = 0.0f;
              killFeedTimer = -1.0f;
              slowmoTimer = 0.0f;
              slowmoScale = 1.0f;
              BattleLogClear(&battleLog);
              combatElapsedTime = 0.0f;
              combatAccum = 0.0f;
              ClearAllModifiers(modifiers);
              // Apply item modifier effects for blue units (stat mults already applied during prep)
              for (int ji = 0; ji < unitCount; ji++) {
                if (units[ji].active && units[ji].team == TEAM_BLUE) {
                  int iid = units[ji].itemId;
                  if (iid >= 0 && iid < ITEM_COUNT && ITEM_DEFS[iid].effectType == IEFF_MODIFIER)
                    ApplyItemEffects(&units[ji], ji, modifiers);
                }
              }
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
                units[j].damageTaken = 0;
                units[j].hasSpawnedMushling = false;
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
        if (!clickedButton && debugMode) {
          int ci = 0;
          for (int i = 0; i < unitTypeCount; i++) {
            if (!unitTypes[i].name)
              continue;
            Rectangle r = {(float)btnXBlue,
                           (float)(btnYStart + ci * (btnHeight + btnMargin)),
                           (float)btnWidth, (float)btnHeight};
            ci++;
            if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded) {
              if (SpawnUnit(units, &unitCount, i, TEAM_BLUE)) {
                if (debugSpawnRarity > 0) {
                  units[unitCount - 1].rarity = debugSpawnRarity;
                  ApplyUnitRarity(&units[unitCount - 1]);
                }
                printf("[DEBUG SPAWN] type=%d (%s) idx=%d rarity=%d hp=%.1f "
                       "hpMult=%.2f\n",
                       i, unitTypes[i].name ? unitTypes[i].name : "?",
                       unitCount - 1, units[unitCount - 1].rarity,
                       units[unitCount - 1].currentHealth,
                       units[unitCount - 1].hpMultiplier);
                PlaySound(sfxNewCharacter);
                intro = (UnitIntro){.active = true,
                                    .timer = 0.0f,
                                    .typeIndex = i,
                                    .unitIndex = unitCount - 1,
                                    .animFrame = 0,
                                    .rarity = units[unitCount - 1].rarity};
              }
              clickedButton = true;
              break;
            }
          }
        }
        // Red spawn buttons (debug only)
        if (!clickedButton && debugMode) {
          int ci = 0;
          for (int i = 0; i < unitTypeCount; i++) {
            if (!unitTypes[i].name)
              continue;
            Rectangle r = {(float)btnXRed,
                           (float)(btnYStart + ci * (btnHeight + btnMargin)),
                           (float)btnWidth, (float)btnHeight};
            ci++;
            if (CheckCollisionPointRec(mouse, r) && unitTypes[i].loaded) {
              if (SpawnUnit(units, &unitCount, i, TEAM_RED))
                AssignRandomAbilities(&units[unitCount - 1],
                                      GetRandomValue(1, 2));
              clickedButton = true;
              break;
            }
          }
        }
        // Env piece spawn + save buttons (debug only)
        if (!clickedButton && debugMode) {
          int envBtnW = 110, envBtnH = 24, envBtnGap = 4;
          int envColX = sw / 2 - envBtnW / 2;
          int envStartY = btnYStart;
          for (int ei = 0; ei < envModelCount; ei++) {
            if (!envModels[ei].loaded)
              continue;
            Rectangle er = {(float)envColX,
                            (float)(envStartY + ei * (envBtnH + envBtnGap)),
                            (float)envBtnW, (float)envBtnH};
            if (CheckCollisionPointRec(mouse, er) &&
                envPieceCount < MAX_ENV_PIECES) {
              envPieces[envPieceCount] = (EnvPiece){.modelIndex = ei,
                                                    .position = {0, 0, 0},
                                                    .rotationX = 0,
                                                    .rotationY = 0,
                                                    .rotationZ = 0,
                                                    .scale = 1.0f,
                                                    .active = true};
              envSelectedPiece = envPieceCount;
              envPieceCount++;
              clickedButton = true;
              break;
            }
          }
          if (!clickedButton) {
            int saveY = envStartY + envModelCount * (envBtnH + envBtnGap) + 4;
            Rectangle saveBtn = {(float)envColX, (float)saveY, (float)envBtnW,
                                 (float)envBtnH};
            if (CheckCollisionPointRec(mouse, saveBtn)) {
              FILE *fp = fopen("env_layout.txt", "w");
              if (fp) {
                fprintf(
                    fp,
                    "# modelIndex x y z rotationX rotationY rotationZ scale\n");
                for (int pi = 0; pi < envPieceCount; pi++) {
                  if (!envPieces[pi].active)
                    continue;
                  fprintf(fp, "%d %.1f %.1f %.1f %.1f %.1f %.1f %.2f\n",
                          envPieces[pi].modelIndex, envPieces[pi].position.x,
                          envPieces[pi].position.y, envPieces[pi].position.z,
                          envPieces[pi].rotationX, envPieces[pi].rotationY,
                          envPieces[pi].rotationZ, envPieces[pi].scale);
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
          int shopH = S(HUD_SHOP_HEIGHT_BASE) - 2;
          Rectangle rollBtn = {20, (float)(shopY + (shopH - S(34)) / 2), S(90), S(34)};
          if (CheckCollisionPointRec(mouse, rollBtn) &&
              playerGold >= rollCost) {
            PlaySound(sfxUiReroll);
            if (isMultiplayer) {
#ifdef USE_EOS
              if (useEos)
                eos_client_send_roll(&eosClient);
              else
#endif
                net_client_send_roll(&netClient);
            } else {
              RollShop(shopSlots, &playerGold, rollCost, currentRound, activeShopSlots);
            }
            rollCost += rollCostIncrement;
            TriggerShake(&shake, 2.0f, 0.15f);
            clickedButton = true;
          }
        }
        // --- Shop: Buy ability card click ---
        if (!clickedButton && !(isMultiplayer && playerReady)) {
          int shopY = hudTop + 2;
          int shopH = S(HUD_SHOP_HEIGHT_BASE) - 2;
          int shopCardW = S(160), shopCardH = S(38), shopCardGap = 10;
          int totalShopW =
              activeShopSlots * shopCardW + (activeShopSlots - 1) * shopCardGap;
          int shopCardsX = (sw - totalShopW) / 2;
          for (int s = 0; s < activeShopSlots; s++) {
            int scx = shopCardsX + s * (shopCardW + shopCardGap);
            Rectangle r = {(float)scx, (float)(shopY + (shopH - shopCardH) / 2), (float)shopCardW,
                           (float)shopCardH};
            if (CheckCollisionPointRec(mouse, r) &&
                shopSlots[s].abilityId >= 0) {
              PlaySound(sfxUiBuy);
              shopHighlightAbilityId = shopSlots[s].abilityId;
              shopHighlightTimer = 0.5f;
              if (isMultiplayer) {
#ifdef USE_EOS
                if (useEos)
                  eos_client_send_buy(&eosClient, s);
                else
#endif
                  net_client_send_buy(&netClient, s);
                BuyAbility(&shopSlots[s], inventory, units, unitCount,
                           &playerGold);
              } else {
                // Try selected unit first (same as hotkey logic)
                int selUnit = -1;
                for (int i2 = 0; i2 < unitCount; i2++) {
                  if (units[i2].active && units[i2].team == TEAM_BLUE &&
                      units[i2].selected) {
                    selUnit = i2;
                    break;
                  }
                }
                if (selUnit >= 0) {
                  int cost = ABILITY_DEFS[shopSlots[s].abilityId].goldCost;
                  if (playerGold >= cost) {
                    bool placed = false;
                    for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                      if (units[selUnit].abilities[a].abilityId ==
                              shopSlots[s].abilityId &&
                          units[selUnit].abilities[a].level <
                              ABILITY_MAX_LEVELS - 1) {
                        units[selUnit].abilities[a].level++;
                        playerGold -= cost;
                        shopSlots[s].abilityId = -1;
                        shopSlots[s].locked = false;
                        placed = true;
                        break;
                      }
                    }
                    if (!placed) {
                      for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        if (units[selUnit].abilities[a].abilityId < 0) {
                          units[selUnit].abilities[a].abilityId =
                              shopSlots[s].abilityId;
                          units[selUnit].abilities[a].level =
                              shopSlots[s].level;
                          playerGold -= cost;
                          shopSlots[s].abilityId = -1;
                          shopSlots[s].locked = false;
                          placed = true;
                          break;
                        }
                      }
                    }
                    if (!placed) {
                      BuyAbility(&shopSlots[s], inventory, units, unitCount,
                                 &playerGold);
                    }
                  }
                } else {
                  BuyAbility(&shopSlots[s], inventory, units, unitCount,
                             &playerGold);
                }
              }
              clickedButton = true;
              break;
            }
          }
        }
        // --- X button on unit cards to remove (check before card selection)
        // ---
        if (!clickedButton && !dragState.dragging) {
          int tmpBlue2[BLUE_TEAM_MAX_SIZE];
          int tmpCount2 = 0;
          for (int i2 = 0; i2 < unitCount && tmpCount2 < BLUE_TEAM_MAX_SIZE;
               i2++)
            if (units[i2].active && units[i2].team == TEAM_BLUE)
              tmpBlue2[tmpCount2++] = i2;
          int totalCardsW2 = BLUE_TEAM_MAX_SIZE * hudCardW +
                             (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
          int cardsStartX2 = (sw - totalCardsW2) / 2;
          int cardsY2 = hudTop + hudShopH + 5;
          for (int h = 0; h < tmpCount2; h++) {
            int cardX = cardsStartX2 + h * (hudCardW + hudCardSpacing);
            int xBtnSize = S(18);
            Rectangle xBtn = {(float)(cardX + hudCardW - xBtnSize - 2),
                              (float)(cardsY2 + 2), (float)xBtnSize,
                              (float)xBtnSize};
            if (CheckCollisionPointRec(mouse, xBtn)) {
              removeConfirmUnit = tmpBlue2[h];
              clickedButton = true;
              break;
            }
          }
        }
        // --- Drag start: unit ability slots on HUD (check before card
        // selection) ---
        if (!clickedButton && !dragState.dragging) {
          // Need blueHudUnits — build it here too
          int tmpBlue[BLUE_TEAM_MAX_SIZE];
          int tmpCount = 0;
          for (int i2 = 0; i2 < unitCount && tmpCount < BLUE_TEAM_MAX_SIZE;
               i2++)
            if (units[i2].active && units[i2].team == TEAM_BLUE)
              tmpBlue[tmpCount++] = i2;
          int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW +
                            (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
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
              Rectangle r = {(float)ax, (float)ay, (float)hudAbilSlotSize,
                             (float)hudAbilSlotSize};
              int ui = tmpBlue[h];
              if (CheckCollisionPointRec(mouse, r) &&
                  units[ui].abilities[a].abilityId >= 0) {
                PlaySound(sfxUiDrag);
                dragState =
                    (DragState){.dragging = true,
                                .sourceType = 1,
                                .sourceIndex = a,
                                .sourceUnitIndex = ui,
                                .abilityId = units[ui].abilities[a].abilityId,
                                .level = units[ui].abilities[a].level};
                units[ui].abilities[a].abilityId = -1;
                clickedButton = true;
                break;
              }
            }
          }
        }
        // --- Drag start: inventory slots ---
        if (!clickedButton && !dragState.dragging) {
          int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW +
                            (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
          int cardsStartX = (sw - totalCardsW) / 2;
          int invStartX =
              cardsStartX -
              (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
          int invStartY = hudTop + hudShopH + 15;
          for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
            int col = inv % HUD_INVENTORY_COLS;
            int row = inv / HUD_INVENTORY_COLS;
            int ix = invStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
            int iy = invStartY + row * (hudAbilSlotSize + hudAbilSlotGap);
            Rectangle r = {(float)ix, (float)iy, (float)hudAbilSlotSize,
                           (float)hudAbilSlotSize};
            if (CheckCollisionPointRec(mouse, r) &&
                inventory[inv].abilityId >= 0) {
              PlaySound(sfxUiDrag);
              dragState = (DragState){.dragging = true,
                                      .sourceType = 0,
                                      .sourceIndex = inv,
                                      .sourceUnitIndex = -1,
                                      .abilityId = inventory[inv].abilityId,
                                      .level = inventory[inv].level};
              inventory[inv].abilityId = -1;
              clickedButton = true;
              break;
            }
          }
        }
        // --- Click to select unit from bottom card (catch-all, checked last)
        // ---
        if (!clickedButton) {
          int tmpBlueCards[BLUE_TEAM_MAX_SIZE];
          int tmpCardCount = 0;
          for (int i2 = 0; i2 < unitCount && tmpCardCount < BLUE_TEAM_MAX_SIZE;
               i2++)
            if (units[i2].active && units[i2].team == TEAM_BLUE &&
                !units[i2].isMushling)
              tmpBlueCards[tmpCardCount++] = i2;
          int totalCardsWC = BLUE_TEAM_MAX_SIZE * hudCardW +
                             (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
          int cardsStartXC = (sw - totalCardsWC) / 2;
          int cardsYC = hudTop + hudShopH + 5;
          for (int h = 0; h < tmpCardCount; h++) {
            int cardX = cardsStartXC + h * (hudCardW + hudCardSpacing);
            Rectangle cardRect = {(float)cardX, (float)cardsYC, (float)hudCardW,
                                  (float)hudCardH};
            if (CheckCollisionPointRec(mouse, cardRect)) {
              int ui = tmpBlueCards[h];
              for (int j = 0; j < unitCount; j++)
                units[j].selected = (j == ui);
              PlaySound(sfxUiDrag);
              clickedButton = true;
              break;
            }
          }
        }
        // Unit selection (skip if clicking inside HUD area)
        if (!clickedButton && mouse.y < hudTop) {
          bool hitAny = false;
          for (int i = unitCount - 1; i >= 0; i--) {
            if (!units[i].active)
              continue;
            if (units[i].team != TEAM_BLUE)
              continue;
            BoundingBox sb =
                GetUnitBounds(&units[i], &unitTypes[units[i].typeIndex]);
            if (GetRayCollisionBox(GetScreenToWorldRay(mouse, camera), sb)
                    .hit) {
              PlaySound(sfxUiDrag);
              units[i].selected = true;
              units[i].dragging = true;
              hasDraggedUnit = true;
              hitAny = true;
              // Store cursor-to-unit offset so drag doesn't snap
              {
                Ray oRay = GetScreenToWorldRay(mouse, camera);
                RayCollision oHit = GetRayCollisionQuad(
                    oRay, (Vector3){-500, 0, -500}, (Vector3){-500, 0, 500},
                    (Vector3){500, 0, 500}, (Vector3){500, 0, -500});
                if (oHit.hit) {
                  dragOffsetX = units[i].position.x - oHit.point.x;
                  dragOffsetZ = units[i].position.z - oHit.point.z;
                } else {
                  dragOffsetX = 0;
                  dragOffsetZ = 0;
                }
              }
              for (int j = 0; j < unitCount; j++)
                if (j != i)
                  units[j].selected = false;
              break;
            }
          }
          if (!hitAny)
            for (int j = 0; j < unitCount; j++)
              units[j].selected = false;

          // Env piece 3D picking (debug mode, only if no unit was hit)
          bool mouseOnDebugSliders = mouse.x < 280 && mouse.y < 210;
          if (!hitAny && debugMode && !mouseOnDebugSliders) {
            Ray envRay = GetScreenToWorldRay(mouse, camera);
            float closestDist = 1e9f;
            int closestIdx = -1;
            for (int ep = 0; ep < envPieceCount; ep++) {
              if (!envPieces[ep].active)
                continue;
              EnvModelDef *emd = &envModels[envPieces[ep].modelIndex];
              if (!emd->loaded || emd->model.meshCount == 0)
                continue;
              // Compute AABB by transforming all 8 corners through model
              // transform
              BoundingBox mbb = GetMeshBoundingBox(emd->model.meshes[0]);
              Matrix mt = emd->model.transform;
              Vector3 corners[8] = {
                  {mbb.min.x, mbb.min.y, mbb.min.z},
                  {mbb.max.x, mbb.min.y, mbb.min.z},
                  {mbb.min.x, mbb.max.y, mbb.min.z},
                  {mbb.max.x, mbb.max.y, mbb.min.z},
                  {mbb.min.x, mbb.min.y, mbb.max.z},
                  {mbb.max.x, mbb.min.y, mbb.max.z},
                  {mbb.min.x, mbb.max.y, mbb.max.z},
                  {mbb.max.x, mbb.max.y, mbb.max.z},
              };
              BoundingBox tbb = {.min = {1e9f, 1e9f, 1e9f},
                                 .max = {-1e9f, -1e9f, -1e9f}};
              for (int ci = 0; ci < 8; ci++) {
                Vector3 tc = Vector3Transform(corners[ci], mt);
                if (tc.x < tbb.min.x)
                  tbb.min.x = tc.x;
                if (tc.y < tbb.min.y)
                  tbb.min.y = tc.y;
                if (tc.z < tbb.min.z)
                  tbb.min.z = tc.z;
                if (tc.x > tbb.max.x)
                  tbb.max.x = tc.x;
                if (tc.y > tbb.max.y)
                  tbb.max.y = tc.y;
                if (tc.z > tbb.max.z)
                  tbb.max.z = tc.z;
              }
              float ps = envPieces[ep].scale;
              BoundingBox wbb = {
                  .min = {tbb.min.x * ps + envPieces[ep].position.x,
                          tbb.min.y * ps + envPieces[ep].position.y,
                          tbb.min.z * ps + envPieces[ep].position.z},
                  .max = {tbb.max.x * ps + envPieces[ep].position.x,
                          tbb.max.y * ps + envPieces[ep].position.y,
                          tbb.max.z * ps + envPieces[ep].position.z}};
              RayCollision rc = GetRayCollisionBox(envRay, wbb);
              if (rc.hit) {
                // Build full world transform (same as draw code)
                EnvPiece p = envPieces[ep];
                float es = p.scale;
                Matrix matW = MatrixScale(es, es, es);
                matW =
                    MatrixMultiply(matW, MatrixRotateX(p.rotationX * DEG2RAD));
                matW =
                    MatrixMultiply(matW, MatrixRotateY(p.rotationY * DEG2RAD));
                matW =
                    MatrixMultiply(matW, MatrixRotateZ(p.rotationZ * DEG2RAD));
                matW = MatrixMultiply(
                    matW,
                    MatrixTranslate(p.position.x, p.position.y, p.position.z));
                Matrix fullTransform =
                    MatrixMultiply(emd->model.transform, matW);
                // Test all meshes in the model
                for (int mi = 0; mi < emd->model.meshCount; mi++) {
                  RayCollision mc = GetRayCollisionMesh(
                      envRay, emd->model.meshes[mi], fullTransform);
                  if (mc.hit && mc.distance < closestDist) {
                    closestDist = mc.distance;
                    closestIdx = ep;
                  }
                }
              }
            }
            envSelectedPiece = closestIdx;
            envDragging = (closestIdx >= 0);
            if (closestIdx >= 0) {
              Ray grabRay = GetScreenToWorldRay(mouse, camera);
              RayCollision grabHit = GetRayCollisionQuad(
                  grabRay, (Vector3){-500, 0, -500}, (Vector3){-500, 0, 500},
                  (Vector3){500, 0, 500}, (Vector3){500, 0, -500});
              if (grabHit.hit) {
                envDragOffset.x =
                    envPieces[closestIdx].position.x - grabHit.point.x;
                envDragOffset.z =
                    envPieces[closestIdx].position.z - grabHit.point.z;
              }
            }
          }
        }
      }

      // --- Shop: Right-click to toggle lock ---
      if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) &&
          !(isMultiplayer && playerReady) && !intro.active &&
          statueSpawn.phase == SSPAWN_INACTIVE && !showEscMenu && !showHelp) {
        Vector2 mouse = GetMousePosition();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        int hudTop = sh - hudTotalH;
        int shopY = hudTop + 2;
        int shopH = S(HUD_SHOP_HEIGHT_BASE) - 2;
        int shopCardW = S(160), shopCardH = S(38), shopCardGap = 10;
        int totalShopW =
            activeShopSlots * shopCardW + (activeShopSlots - 1) * shopCardGap;
        int shopCardsX = (sw - totalShopW) / 2;
        for (int s = 0; s < activeShopSlots; s++) {
          int scx = shopCardsX + s * (shopCardW + shopCardGap);
          Rectangle r = {(float)scx, (float)(shopY + (shopH - shopCardH) / 2), (float)shopCardW,
                         (float)shopCardH};
          if (CheckCollisionPointRec(mouse, r) && shopSlots[s].abilityId >= 0) {
            shopSlots[s].locked = !shopSlots[s].locked;
            usedLockHint = true;
            PlaySound(sfxUiBuy);
            break;
          }
        }
        // Right-click on inventory slot to sell
        if (!dragState.dragging) {
          int sw2 = GetScreenWidth(), sh2 = GetScreenHeight();
          int hudTop2 = sh2 - hudTotalH;
          int totalCardsW2 = BLUE_TEAM_MAX_SIZE * hudCardW + (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
          int cardsStartX2 = (sw2 - totalCardsW2) / 2;
          int rcInvStartX = cardsStartX2 - (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
          int rcInvStartY = hudTop2 + hudShopH + 15 + S(16);
          for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
            int col = inv % HUD_INVENTORY_COLS;
            int row = inv / HUD_INVENTORY_COLS;
            int ix = rcInvStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
            int iy = rcInvStartY + row * (hudAbilSlotSize + hudAbilSlotGap);
            Rectangle ir = {(float)ix, (float)iy, (float)hudAbilSlotSize, (float)hudAbilSlotSize};
            if (CheckCollisionPointRec(mouse, ir) && inventory[inv].abilityId >= 0) {
              SellAbility(inventory[inv].abilityId, inventory[inv].level, &playerGold);
              inventory[inv].abilityId = -1;
              inventory[inv].level = 0;
              PlaySound(sfxUiBuy);
              break;
            }
          }
        }
      }

      // --- Drag-and-drop release handling ---
      if (dragState.dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) &&
          !intro.active && statueSpawn.phase == SSPAWN_INACTIVE) {
        PlaySound(sfxUiDrop);
        Vector2 mouse = GetMousePosition();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        int hudTop2 = sh - hudTotalH;
        bool placed = false;

        // Collect blue units
        int dropBlue[BLUE_TEAM_MAX_SIZE];
        int dropCount = 0;
        for (int i2 = 0; i2 < unitCount && dropCount < BLUE_TEAM_MAX_SIZE; i2++)
          if (units[i2].active && units[i2].team == TEAM_BLUE)
            dropBlue[dropCount++] = i2;

        int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW +
                          (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
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
            Rectangle r = {(float)ax, (float)ay, (float)hudAbilSlotSize,
                           (float)hudAbilSlotSize};
            if (CheckCollisionPointRec(mouse, r)) {
              int ui = dropBlue[h];
              // Dropping on the same slot we picked from — just restore it
              if (dragState.sourceType == 1 &&
                  dragState.sourceUnitIndex == ui &&
                  dragState.sourceIndex == a) {
                units[ui].abilities[a].abilityId = dragState.abilityId;
                units[ui].abilities[a].level = dragState.level;
                placed = true;
                break;
              }
              // Combine: same ability => merge levels
              if (units[ui].abilities[a].abilityId == dragState.abilityId &&
                  dragState.abilityId >= 0) {
                int combined =
                    dragState.level + units[ui].abilities[a].level + 1;
                if (combined > ABILITY_MAX_LEVELS - 1)
                  combined = ABILITY_MAX_LEVELS - 1;
                units[ui].abilities[a].level = combined;
                PlaySound(sfxUiBuy);
                placed = true;
                break;
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
                units[dragState.sourceUnitIndex]
                    .abilities[dragState.sourceIndex]
                    .abilityId = oldId;
                units[dragState.sourceUnitIndex]
                    .abilities[dragState.sourceIndex]
                    .level = oldLv;
              }
              placed = true;
              break;
            }
          }
        }
        // Check drop on inventory slot
        if (!placed) {
          int invStartX =
              cardsStartX -
              (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
          int invStartY = hudTop2 + hudShopH + 15;
          for (int inv = 0; inv < MAX_INVENTORY_SLOTS && !placed; inv++) {
            int col = inv % HUD_INVENTORY_COLS;
            int row = inv / HUD_INVENTORY_COLS;
            int ix = invStartX + col * (hudAbilSlotSize + hudAbilSlotGap);
            int iy = invStartY + row * (hudAbilSlotSize + hudAbilSlotGap);
            Rectangle r = {(float)ix, (float)iy, (float)hudAbilSlotSize,
                           (float)hudAbilSlotSize};
            if (CheckCollisionPointRec(mouse, r)) {
              // Dropping on the same inventory slot we picked from — just
              // restore it
              if (dragState.sourceType == 0 && dragState.sourceIndex == inv) {
                inventory[inv].abilityId = dragState.abilityId;
                inventory[inv].level = dragState.level;
                placed = true;
                break;
              }
              // Combine: same ability => merge levels
              if (inventory[inv].abilityId == dragState.abilityId &&
                  dragState.abilityId >= 0) {
                int combined = dragState.level + inventory[inv].level + 1;
                if (combined > ABILITY_MAX_LEVELS - 1)
                  combined = ABILITY_MAX_LEVELS - 1;
                inventory[inv].level = combined;
                PlaySound(sfxUiBuy);
                placed = true;
                break;
              }
              int oldId = inventory[inv].abilityId;
              int oldLv = inventory[inv].level;
              inventory[inv].abilityId = dragState.abilityId;
              inventory[inv].level = dragState.level;
              if (dragState.sourceType == 0) {
                inventory[dragState.sourceIndex].abilityId = oldId;
                inventory[dragState.sourceIndex].level = oldLv;
              } else {
                units[dragState.sourceUnitIndex]
                    .abilities[dragState.sourceIndex]
                    .abilityId = oldId;
                units[dragState.sourceUnitIndex]
                    .abilities[dragState.sourceIndex]
                    .level = oldLv;
              }
              placed = true;
            }
          }
        }
        // Check drop on sell zone (prep phase only, directly below ability inventory)
        if (!placed && dragState.abilityId >= 0) {
          int sellInvStartX =
              cardsStartX -
              (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
          int sellInvStartY = hudTop2 + hudShopH + 15 + S(16);
          int sellZW = HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap);
          int sellZH = S(20);
          int sellZX = sellInvStartX;
          int sellZY = sellInvStartY + HUD_INVENTORY_ROWS * (hudAbilSlotSize + hudAbilSlotGap) + S(2);
          Rectangle sellRect = {(float)sellZX, (float)sellZY, (float)sellZW, (float)sellZH};
          if (CheckCollisionPointRec(mouse, sellRect)) {
            SellAbility(dragState.abilityId, dragState.level, &playerGold);
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
            units[dragState.sourceUnitIndex]
                .abilities[dragState.sourceIndex]
                .abilityId = dragState.abilityId;
            units[dragState.sourceUnitIndex]
                .abilities[dragState.sourceIndex]
                .level = dragState.level;
          }
        }
        dragState.dragging = false;
      }
    }
    //------------------------------------------------------------------------------
    // PHASE: COMBAT — abilities, modifiers, projectiles, movement, attack
    //------------------------------------------------------------------------------
    else if (phase == PHASE_COMBAT) {
      if (IsKeyPressed(KEY_ESCAPE)) {
        if (showHelp)
          showHelp = false;
        else
          showEscMenu = !showEscMenu;
      }

      // Click to select unit during combat (3D raycast + party bar)
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !showEscMenu &&
          !showHelp) {
        Vector2 cmouse = GetMousePosition();
        bool cHitAny = false;

        // Party bar click
        int cBlue[BLUE_TEAM_MAX_SIZE];
        int cBlueCount = 0;
        for (int i = 0; i < unitCount && cBlueCount < BLUE_TEAM_MAX_SIZE; i++)
          if (units[i].active && units[i].team == TEAM_BLUE &&
              !units[i].isMushling)
            cBlue[cBlueCount++] = i;
        int cTotalW = BLUE_TEAM_MAX_SIZE * hudCardW +
                      (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
        int cSh = GetScreenHeight();
        int cStartX = (GetScreenWidth() - cTotalW) / 2;
        int cHudTop = cSh - hudTotalH;
        int cCardsY = cHudTop + hudShopH + 5;
        for (int h = 0; h < cBlueCount; h++) {
          int cx = cStartX + h * (hudCardW + hudCardSpacing);
          Rectangle cr = {(float)cx, (float)cCardsY, (float)hudCardW,
                          (float)hudCardH};
          if (CheckCollisionPointRec(cmouse, cr)) {
            int ui = cBlue[h];
            for (int j = 0; j < unitCount; j++)
              units[j].selected = (j == ui);
            cHitAny = true;
            break;
          }
        }

        // 3D raycast click (only above HUD)
        if (!cHitAny && cmouse.y < cHudTop) {
          for (int i = unitCount - 1; i >= 0; i--) {
            if (!units[i].active)
              continue;
            BoundingBox sb =
                GetUnitBounds(&units[i], &unitTypes[units[i].typeIndex]);
            if (GetRayCollisionBox(GetScreenToWorldRay(cmouse, camera), sb)
                    .hit) {
              for (int j = 0; j < unitCount; j++)
                units[j].selected = (j == i);
              cHitAny = true;
              break;
            }
          }
        }

        if (!cHitAny)
          for (int j = 0; j < unitCount; j++)
            units[j].selected = false;
      }

      // === MULTIPLAYER: deterministic CombatTick with visual feedback ===
      if (isMultiplayer) {
        float realDt = dt; // preserve real dt for visual updates
        combatAccum += realDt;

        // Sub-tick: visual-only updates + poll
        if (combatAccum < COMBAT_DT) {
          UpdateParticles(particles, realDt);
          UpdateFloatingTexts(floatingTexts, realDt);
          combatElapsedTime += realDt;
#ifdef USE_EOS
          if (useEos)
            eos_client_poll(&eosClient);
          else
#endif
            net_client_poll(&netClient);
          if (NC_FLAG(roundResultReady) || NC_FLAG(gameOver))
            goto combat_check_end;
          goto combat_skip;
        }

        // Death spiral prevention: if >1s behind, reset to 1 tick
        if (combatAccum > 1.0f)
          combatAccum = COMBAT_DT;

        // Desync detection only — don't apply corrections, trust local
        // simulation
        if (NC_FLAG(combatSyncReady)) {
          NC_CLEAR(combatSyncReady);
          uint32_t serverHash;
#ifdef USE_EOS
          serverHash =
              useEos ? eosClient.combatSyncHash : netClient.combatSyncHash;
#else
          serverHash = netClient.combatSyncHash;
#endif
          if (serverHash != 0) {
            uint32_t localHash = 0;
            for (int i = 0; i < unitCount; i++) {
              if (!units[i].active)
                continue;
              uint32_t hx, hz, hh;
              memcpy(&hx, &units[i].position.x, 4);
              memcpy(&hz, &units[i].position.z, 4);
              memcpy(&hh, &units[i].currentHealth, 4);
              localHash ^=
                  hx * 2654435761u ^ hz * 2246822519u ^ hh * 0x45d9f3bu;
            }
            if (localHash != serverHash)
              printf("[DESYNC] local=0x%08X server=0x%08X\n", localHash,
                     serverHash);
          }
        }

        // Count ticks to catch up (cap at 4)
        int ticksToRun = 0;
        float tempAccum = combatAccum;
        while (tempAccum >= COMBAT_DT) {
          tempAccum -= COMBAT_DT;
          ticksToRun++;
        }
        if (ticksToRun > 4)
          ticksToRun = 4;

        // Full struct snapshots BEFORE all ticks (for visual effect diffing)
        Unit unitsBefore[MAX_UNITS];
        Projectile projBefore[MAX_PROJECTILES];
        memcpy(unitsBefore, units, sizeof(Unit) * unitCount);
        memcpy(projBefore, projectiles, sizeof(Projectile) * MAX_PROJECTILES);

        // Run all catch-up ticks — accumulate CombatEvents across all ticks
        CombatEvent combatEvents[MAX_COMBAT_EVENTS];
        int combatEventCount = 0;
        for (int tick = 0; tick < ticksToRun; tick++) {
          combatAccum -= COMBAT_DT;
          CombatEvent tickEvents[MAX_COMBAT_EVENTS];
          int tickEventCount = 0;
          CombatTick(units, &unitCount, modifiers, projectiles, fissures,
                     COMBAT_DT, tickEvents, &tickEventCount);
          for (int te = 0; te < tickEventCount && combatEventCount < MAX_COMBAT_EVENTS; te++)
            combatEvents[combatEventCount++] = tickEvents[te];
        }
        if (combatAccum > 4.0f * COMBAT_DT)
          combatAccum = 4.0f * COMBAT_DT;

        // === Process CombatEvents (accumulated from all ticks) ===
        for (int e = 0; e < combatEventCount; e++) {
          switch (combatEvents[e].type) {
          case COMBAT_EVT_SHAKE:
            TriggerShake(&shake, combatEvents[e].value1,
                         combatEvents[e].value2);
            break;
          case COMBAT_EVT_ABILITY_CAST: {
            int ui = combatEvents[e].unitIndex;
            int ai = combatEvents[e].abilityId;
            if (ui >= 0 && ui < unitCount && ai >= 0 && ai < ABILITY_COUNT) {
              BattleLogAddCast(&battleLog, combatElapsedTime, units[ui].team,
                               units[ui].typeIndex, ai);
              PlaySound(shoutSfxByType[units[ui].typeIndex]);
              SpawnFloatingText(floatingTexts, units[ui].position,
                                ABILITY_DEFS[ai].name,
                                (Color){255, 220, 100, 255}, 1.2f);
            }
          } break;
          case COMBAT_EVT_MELEE_HIT:
            PlaySound(sfxMeleeHit);
            if (combatEvents[e].unitIndex >= 0 &&
                combatEvents[e].unitIndex < unitCount)
              SpawnMeleeImpact(particles,
                               units[combatEvents[e].unitIndex].position);
            break;
          case COMBAT_EVT_PROJECTILE_HIT:
            PlaySound(sfxProjectileHit);
            // Spawn explosion particles at impact position
            if (combatEvents[e].unitIndex >= 0 &&
                combatEvents[e].unitIndex < unitCount) {
              Vector3 impactPos = combatEvents[e].position;
              for (int ep = 0; ep < PROJ_EXPLODE_COUNT; ep++) {
                float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
                float spd = (float)GetRandomValue(100, 250) / 10.0f;
                Vector3 ev = {cosf(angle) * spd,
                              (float)GetRandomValue(40, 150) / 10.0f,
                              sinf(angle) * spd};
                // Color based on projectile type (stored in abilityId field)
                Color ec;
                switch (combatEvents[e].abilityId) {
                case PROJ_HOOK:
                  ec = (Color){200, 160, 100, 255};
                  break;
                case PROJ_MAELSTROM:
                  ec = (Color){100, 180, 255, 255};
                  break;
                case PROJ_DEVIL_BOLT:
                  ec = (Color){200, 50, 50, 255};
                  break;
                case PROJ_MAGIC_MISSILE:
                  ec = (Color){120, 80, 255, 255};
                  break;
                case PROJ_CHAIN_FROST:
                  ec = (Color){80, 200, 255, 255};
                  break;
                default:
                  ec = (Color){255, 200, 100, 255};
                  break;
                }
                SpawnParticle(particles, impactPos, ev,
                              0.3f + (float)GetRandomValue(0, 3) / 10.0f,
                              (float)GetRandomValue(3, 8) / 10.0f, ec);
              }
            }
            break;
          case COMBAT_EVT_MULTICAST: {
            int ui = combatEvents[e].unitIndex;
            int casts =
                (int)combatEvents[e].value1 + 1; // 1 extra = 2x, 2 extra = 3x
            if (ui >= 0 && ui < unitCount) {
              const char *mcText = TextFormat("MULTICAST x%d!", casts);
              SpawnFloatingText(floatingTexts, units[ui].position, mcText,
                                (Color){255, 180, 60, 255}, 1.5f);
            }
          } break;
          default:
            break;
          }
        }

        // === State diff (snapshot vs current) for visual effects ===
        for (int i = 0; i < unitCount; i++) {
          float dmg = unitsBefore[i].currentHealth - units[i].currentHealth;
          if (dmg > 0.5f) {
            SpawnDamageNumber(floatingTexts, units[i].position, dmg, false);
            units[i].hitFlash = HIT_FLASH_DURATION;
          }
          if (unitsBefore[i].active && !units[i].active) {
            // Unit died
            PlaySound(dieSfxByType[units[i].typeIndex]);
            SpawnDeathExplosion(particles, units[i].position, units[i].team);
            TriggerShake(&shake, 6.0f, 0.3f);
            // Kill feed
            {
              Team killerTeam =
                  (units[i].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
              if (killerTeam != lastKillTeam)
                multiKillCount = 0;
              lastKillTeam = killerTeam;
            }
            killCount++;
            multiKillCount++;
            multiKillTimer = 2.0f;
            if (killCount == 1) {
              snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.0f;
            } else if (multiKillCount == 2) {
              snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.0f;
            } else if (multiKillCount == 3) {
              snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.0f;
            } else if (multiKillCount >= 4) {
              snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.5f;
            }
            // Slow-mo on last kill
            int ba2, ra2;
            CountTeams(units, unitCount, &ba2, &ra2);
            if (ba2 == 0 || ra2 == 0) {
              slowmoTimer = 0.5f;
              slowmoScale = 0.3f;
            }
          }
        }

        // Projectile whoosh sound (chargeTimer crossed from >0 to <=0)
        for (int pp = 0; pp < MAX_PROJECTILES; pp++) {
          if (projBefore[pp].chargeTimer > 0 &&
              projectiles[pp].chargeTimer <= 0 && projectiles[pp].active)
            PlaySound(sfxProjectileWhoosh);
        }

        // Dig particles (visual only)
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active)
            continue;
          if (UnitHasModifier(modifiers, i, MOD_DIG_HEAL)) {
            UnitType *dtype = &unitTypes[units[i].typeIndex];
            float modelH = (dtype->baseBounds.max.y - dtype->baseBounds.min.y) *
                           dtype->scale;
            float modelR = (dtype->baseBounds.max.x - dtype->baseBounds.min.x) *
                           dtype->scale * 0.6f;
            for (int pp = 0; pp < 3; pp++) {
              float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
              float r = modelR + (float)GetRandomValue(5, 20) / 10.0f;
              Vector3 pos = {
                  units[i].position.x + cosf(angle) * r,
                  units[i].position.y +
                      (float)GetRandomValue(0, (int)(modelH * 10.0f)) / 10.0f,
                  units[i].position.z + sinf(angle) * r};
              Vector3 vel = {cosf(angle) * 3.0f,
                             (float)GetRandomValue(20, 60) / 10.0f,
                             sinf(angle) * 3.0f};
              int shade = GetRandomValue(100, 180);
              Color brown = {(unsigned char)shade,
                             (unsigned char)(shade * 0.6f),
                             (unsigned char)(shade * 0.3f), 255};
              float sz = (float)GetRandomValue(3, 8) / 10.0f;
              SpawnParticle(particles, pos, vel,
                            0.5f + (float)GetRandomValue(0, 3) / 10.0f, sz,
                            brown);
            }
          }
        }

        // Update visual effects with REAL dt for smooth particles
        UpdateParticles(particles, realDt);
        UpdateFloatingTexts(floatingTexts, realDt);
        combatElapsedTime += realDt;

        // Smooth Y toward ground
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active)
            continue;
          units[i].position.y += (0.0f - units[i].position.y) * 0.1f;
        }

        // Poll + check round end (server-authoritative)
        goto combat_check_end;
      }
      // Pause combat in single-player when ESC menu is open
      if (showEscMenu && !isMultiplayer) {
        UpdateParticles(particles, dt);
        UpdateFloatingTexts(floatingTexts, dt);
        goto combat_skip;
      }
      // === SINGLEPLAYER: deterministic CombatTick with visual feedback ===
      {
        float realDt = dt;
        combatAccum += realDt;

        // Sub-tick: visual-only updates
        if (combatAccum < COMBAT_DT) {
          UpdateParticles(particles, realDt);
          UpdateFloatingTexts(floatingTexts, realDt);
          combatElapsedTime += realDt;
          goto combat_skip;
        }

        // Death spiral prevention
        if (combatAccum > 1.0f)
          combatAccum = COMBAT_DT;

        // Count ticks to catch up (cap at 4)
        int ticksToRun = 0;
        float tempAccum = combatAccum;
        while (tempAccum >= COMBAT_DT) {
          tempAccum -= COMBAT_DT;
          ticksToRun++;
        }
        if (ticksToRun > 4)
          ticksToRun = 4;

        // Full struct snapshots BEFORE all ticks (for visual effect diffing)
        Unit unitsBefore[MAX_UNITS];
        Projectile projBefore[MAX_PROJECTILES];
        memcpy(unitsBefore, units, sizeof(Unit) * unitCount);
        memcpy(projBefore, projectiles, sizeof(Projectile) * MAX_PROJECTILES);

        // Run all catch-up ticks — accumulate CombatEvents across all ticks
        CombatEvent combatEvents[MAX_COMBAT_EVENTS];
        int combatEventCount = 0;
        for (int tick = 0; tick < ticksToRun; tick++) {
          combatAccum -= COMBAT_DT;
          CombatEvent tickEvents[MAX_COMBAT_EVENTS];
          int tickEventCount = 0;
          CombatTick(units, &unitCount, modifiers, projectiles, fissures,
                     COMBAT_DT, tickEvents, &tickEventCount);
          for (int te = 0; te < tickEventCount && combatEventCount < MAX_COMBAT_EVENTS; te++)
            combatEvents[combatEventCount++] = tickEvents[te];
        }
        if (combatAccum > 4.0f * COMBAT_DT)
          combatAccum = 4.0f * COMBAT_DT;

        // === Process CombatEvents (accumulated from all ticks) ===
        for (int e = 0; e < combatEventCount; e++) {
          switch (combatEvents[e].type) {
          case COMBAT_EVT_SHAKE:
            TriggerShake(&shake, combatEvents[e].value1,
                         combatEvents[e].value2);
            break;
          case COMBAT_EVT_ABILITY_CAST: {
            int ui = combatEvents[e].unitIndex;
            int ai = combatEvents[e].abilityId;
            if (ui >= 0 && ui < unitCount && ai >= 0 && ai < ABILITY_COUNT) {
              BattleLogAddCast(&battleLog, combatElapsedTime, units[ui].team,
                               units[ui].typeIndex, ai);
              PlaySound(shoutSfxByType[units[ui].typeIndex]);
              SpawnFloatingText(floatingTexts, units[ui].position,
                                ABILITY_DEFS[ai].name,
                                (Color){255, 220, 100, 255}, 1.2f);
            }
          } break;
          case COMBAT_EVT_MELEE_HIT:
            PlaySound(sfxMeleeHit);
            if (combatEvents[e].unitIndex >= 0 &&
                combatEvents[e].unitIndex < unitCount)
              SpawnMeleeImpact(particles,
                               units[combatEvents[e].unitIndex].position);
            break;
          case COMBAT_EVT_PROJECTILE_HIT:
            PlaySound(sfxProjectileHit);
            if (combatEvents[e].unitIndex >= 0 &&
                combatEvents[e].unitIndex < unitCount) {
              Vector3 impactPos = combatEvents[e].position;
              for (int ep = 0; ep < PROJ_EXPLODE_COUNT; ep++) {
                float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
                float spd = (float)GetRandomValue(100, 250) / 10.0f;
                Vector3 ev = {cosf(angle) * spd,
                              (float)GetRandomValue(40, 150) / 10.0f,
                              sinf(angle) * spd};
                Color ec;
                switch (combatEvents[e].abilityId) {
                case PROJ_HOOK:
                  ec = (Color){200, 160, 100, 255};
                  break;
                case PROJ_MAELSTROM:
                  ec = (Color){100, 180, 255, 255};
                  break;
                case PROJ_DEVIL_BOLT:
                  ec = (Color){200, 50, 50, 255};
                  break;
                case PROJ_MAGIC_MISSILE:
                  ec = (Color){120, 80, 255, 255};
                  break;
                case PROJ_CHAIN_FROST:
                  ec = (Color){80, 200, 255, 255};
                  break;
                default:
                  ec = (Color){255, 200, 100, 255};
                  break;
                }
                SpawnParticle(particles, impactPos, ev,
                              0.3f + (float)GetRandomValue(0, 3) / 10.0f,
                              (float)GetRandomValue(3, 8) / 10.0f, ec);
              }
            }
            break;
          case COMBAT_EVT_MULTICAST: {
            int ui = combatEvents[e].unitIndex;
            int casts = (int)combatEvents[e].value1 + 1;
            if (ui >= 0 && ui < unitCount) {
              const char *mcText = TextFormat("MULTICAST x%d!", casts);
              SpawnFloatingText(floatingTexts, units[ui].position, mcText,
                                (Color){255, 180, 60, 255}, 1.5f);
            }
          } break;
          default:
            break;
          }
        }

        // === State diff (snapshot vs current) for visual effects ===
        for (int i = 0; i < unitCount; i++) {
          float dmg = unitsBefore[i].currentHealth - units[i].currentHealth;
          if (dmg > 0.5f) {
            SpawnDamageNumber(floatingTexts, units[i].position, dmg, false);
            units[i].hitFlash = HIT_FLASH_DURATION;
          }
          if (unitsBefore[i].active && !units[i].active) {
            // Unit died
            PlaySound(dieSfxByType[units[i].typeIndex]);
            SpawnDeathExplosion(particles, units[i].position, units[i].team);
            TriggerShake(&shake, 6.0f, 0.3f);
            // Kill feed
            {
              Team killerTeam =
                  (units[i].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
              if (killerTeam != lastKillTeam)
                multiKillCount = 0;
              lastKillTeam = killerTeam;
            }
            killCount++;
            multiKillCount++;
            multiKillTimer = 2.0f;
            if (killCount == 1) {
              snprintf(killFeedText, sizeof(killFeedText), "FIRST BLOOD!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.0f;
            } else if (multiKillCount == 2) {
              snprintf(killFeedText, sizeof(killFeedText), "DOUBLE KILL!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.0f;
            } else if (multiKillCount == 3) {
              snprintf(killFeedText, sizeof(killFeedText), "TRIPLE KILL!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.0f;
            } else if (multiKillCount >= 4) {
              snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!");
              killFeedTimer = 0.0f;
              killFeedScale = 2.5f;
            }
            // Slow-mo on last kill
            int ba2, ra2;
            CountTeams(units, unitCount, &ba2, &ra2);
            if (ba2 == 0 || ra2 == 0) {
              slowmoTimer = 0.5f;
              slowmoScale = 0.3f;
            }
            // Track last killed enemy type for capture mechanic
            if (units[i].team == TEAM_RED) {
              lastKilledEnemyType = units[i].typeIndex;
              lastKilledRarity = units[i].rarity;
              for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                lastKilledAbilities[ab] = units[i].abilities[ab];
            }
          }
        }

        // Projectile whoosh sound (chargeTimer crossed from >0 to <=0)
        for (int pp = 0; pp < MAX_PROJECTILES; pp++) {
          if (projBefore[pp].chargeTimer > 0 &&
              projectiles[pp].chargeTimer <= 0 && projectiles[pp].active)
            PlaySound(sfxProjectileWhoosh);
        }

        // Dig particles (visual only)
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active)
            continue;
          if (UnitHasModifier(modifiers, i, MOD_DIG_HEAL)) {
            UnitType *dtype = &unitTypes[units[i].typeIndex];
            float modelH = (dtype->baseBounds.max.y - dtype->baseBounds.min.y) *
                           dtype->scale;
            float modelR = (dtype->baseBounds.max.x - dtype->baseBounds.min.x) *
                           dtype->scale * 0.6f;
            for (int pp = 0; pp < 3; pp++) {
              float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
              float r = modelR + (float)GetRandomValue(5, 20) / 10.0f;
              Vector3 pos = {
                  units[i].position.x + cosf(angle) * r,
                  units[i].position.y +
                      (float)GetRandomValue(0, (int)(modelH * 10.0f)) / 10.0f,
                  units[i].position.z + sinf(angle) * r};
              Vector3 vel = {cosf(angle) * 3.0f,
                             (float)GetRandomValue(20, 60) / 10.0f,
                             sinf(angle) * 3.0f};
              int shade = GetRandomValue(100, 180);
              Color brown = {(unsigned char)shade,
                             (unsigned char)(shade * 0.6f),
                             (unsigned char)(shade * 0.3f), 255};
              float sz = (float)GetRandomValue(3, 8) / 10.0f;
              SpawnParticle(particles, pos, vel,
                            0.5f + (float)GetRandomValue(0, 3) / 10.0f, sz,
                            brown);
            }
          }
        }

        // Update visual effects with REAL dt for smooth particles
        UpdateParticles(particles, realDt);
        UpdateFloatingTexts(floatingTexts, realDt);
        combatElapsedTime += realDt;

        // Smooth Y toward ground
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active)
            continue;
          units[i].position.y += (0.0f - units[i].position.y) * 0.1f;
        }
      } // end singleplayer CombatTick block
      if (0) { // dead code — old inline singleplayer combat removed
        for (int m = 0; m < MAX_MODIFIERS; m++) {
          if (!modifiers[m].active)
            continue;
          int ui = modifiers[m].unitIndex;
          if (ui < 0 || ui >= unitCount || !units[ui].active) {
            modifiers[m].active = false;
            continue;
          }
          if (modifiers[m].duration > 0) {
            modifiers[m].duration -= dt;
            if (modifiers[m].duration <= 0) {
              if (modifiers[m].type == MOD_SHIELD)
                units[ui].shieldHP = 0;
              modifiers[m].active = false;
              continue;
            }
          }
          // Per-tick effects
          if (modifiers[m].type == MOD_DIG_HEAL) {
            const UnitStats *s = &UNIT_STATS[units[ui].typeIndex];
            units[ui].currentHealth += modifiers[m].value * dt;
            if (units[ui].currentHealth > s->health)
              units[ui].currentHealth = s->health;
          }
        }

        // === STEP 1b: Spawn dig particles + update all particles ===
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active)
            continue;
          if (UnitHasModifier(modifiers, i, MOD_DIG_HEAL)) {
            UnitType *dtype = &unitTypes[units[i].typeIndex];
            float modelH = (dtype->baseBounds.max.y - dtype->baseBounds.min.y) *
                           dtype->scale;
            float modelR = (dtype->baseBounds.max.x - dtype->baseBounds.min.x) *
                           dtype->scale * 0.6f;
            // Spawn brown dirt particles around the model
            for (int pp = 0; pp < 3; pp++) {
              float angle = (float)GetRandomValue(0, 360) * DEG2RAD;
              float r = modelR + (float)GetRandomValue(5, 20) / 10.0f;
              Vector3 pos = {
                  units[i].position.x + cosf(angle) * r,
                  units[i].position.y +
                      (float)GetRandomValue(0, (int)(modelH * 10.0f)) / 10.0f,
                  units[i].position.z + sinf(angle) * r};
              Vector3 vel = {cosf(angle) * 3.0f,
                             (float)GetRandomValue(20, 60) / 10.0f,
                             sinf(angle) * 3.0f};
              int shade = GetRandomValue(100, 180);
              Color brown = {(unsigned char)shade,
                             (unsigned char)(shade * 0.6f),
                             (unsigned char)(shade * 0.3f), 255};
              float sz = (float)GetRandomValue(3, 8) / 10.0f;
              SpawnParticle(particles, pos, vel,
                            0.5f + (float)GetRandomValue(0, 3) / 10.0f, sz,
                            brown);
            }
          }
        }
        UpdateParticles(particles, dt);
        UpdateFloatingTexts(floatingTexts, dt);

        // === STEP 2: Update projectiles ===
        for (int p = 0; p < MAX_PROJECTILES; p++) {
          if (!projectiles[p].active)
            continue;
          // Charge-up phase: stay in place and grow
          if (projectiles[p].chargeTimer > 0) {
            projectiles[p].chargeTimer -= dt;
            if (projectiles[p].chargeTimer > 0)
              continue;
            PlaySound(sfxProjectileWhoosh);
          }
          int ti = projectiles[p].targetIndex;
          // Target gone?
          if (ti < 0 || ti >= unitCount || !units[ti].active) {
            if ((projectiles[p].type == PROJ_CHAIN_FROST ||
                 projectiles[p].type == PROJ_MAELSTROM) &&
                projectiles[p].bouncesRemaining > 0) {
              int next = FindChainFrostTarget(
                  units, unitCount, projectiles[p].position,
                  projectiles[p].sourceTeam, projectiles[p].lastHitUnit,
                  projectiles[p].bounceRange);
              if (next >= 0) {
                projectiles[p].targetIndex = next;
                continue;
              }
            }
            projectiles[p].active = false;
            continue;
          }
          // Move toward target
          Vector3 tgt = {units[ti].position.x, units[ti].position.y + 3.0f,
                         units[ti].position.z};
          float pdx = tgt.x - projectiles[p].position.x;
          float pdy = tgt.y - projectiles[p].position.y;
          float pdz = tgt.z - projectiles[p].position.z;
          float pdist = sqrtf(pdx * pdx + pdy * pdy + pdz * pdz);
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
                              (float)GetRandomValue(70, 130) / 10.0f,
                              projectiles[p].color);
              }
              TriggerShake(&shake, 4.0f, 0.2f);
              // Tile wobble ripple from impact
              float gridOriginImp = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
              for (int tr = 0; tr < TILE_GRID_SIZE; tr++) {
                for (int tc = 0; tc < TILE_GRID_SIZE; tc++) {
                  float cx = gridOriginImp + (tc + 0.5f) * TILE_WORLD_SIZE;
                  float cz = gridOriginImp + (tr + 0.5f) * TILE_WORLD_SIZE;
                  float dxw = cx - impactPos.x, dzw = cz - impactPos.z;
                  float dist = sqrtf(dxw * dxw + dzw * dzw);
                  float wobbleR = 50.0f;
                  if (dist < wobbleR) {
                    float strength = expf(-2.0f * dist / wobbleR);
                    if (tileWobble[tr][tc] <
                        TILE_WOBBLE_MAX * 0.5f * strength) {
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
            // HIT — Hook: damage by distance, then pull target to caster
            if (projectiles[p].type == PROJ_HOOK) {
              if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                float hookDist =
                    DistXZ(units[ti].position,
                           units[projectiles[p].sourceIndex].position);
                float hitDmg = hookDist * projectiles[p].damage;
                if (units[ti].shieldHP > 0) {
                  if (hitDmg <= units[ti].shieldHP) {
                    units[ti].shieldHP -= hitDmg;
                    hitDmg = 0;
                  } else {
                    hitDmg -= units[ti].shieldHP;
                    units[ti].shieldHP = 0;
                  }
                }
                units[ti].currentHealth -= hitDmg;
                CheckMushroomSpawn(units, &unitCount, ti, hitDmg);
                units[ti].hitFlash = HIT_FLASH_DURATION;
                SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg,
                                  true);

                if (units[ti].currentHealth <= 0) {
                  PlaySound(dieSfxByType[units[ti].typeIndex]);
                  SpawnDeathExplosion(particles, units[ti].position,
                                      units[ti].team);
                  TriggerShake(&shake, 6.0f, 0.3f);

                  // Kill feed
                  {
                    Team killerTeam =
                        (units[ti].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                    if (killerTeam != lastKillTeam)
                      multiKillCount = 0;
                    lastKillTeam = killerTeam;
                  }
                  killCount++;
                  multiKillCount++;
                  multiKillTimer = 2.0f;
                  if (killCount == 1) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "FIRST BLOOD!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 2) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "DOUBLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 3) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "TRIPLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount >= 4) {
                    snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.5f;
                  }
                  // Slow-mo check: is this the last unit on a team?
                  int ba2, ra2;
                  CountTeams(units, unitCount, &ba2, &ra2);
                  if (ba2 == 0 || ra2 == 0) {
                    slowmoTimer = 0.5f;
                    slowmoScale = 0.3f;
                  }
                  BattleLogAddKill(&battleLog, combatElapsedTime,
                                   units[projectiles[p].sourceIndex].team,
                                   units[projectiles[p].sourceIndex].typeIndex,
                                   units[ti].team, units[ti].typeIndex,
                                   ABILITY_HOOK);
                  if (units[ti].team == TEAM_RED) {
                    lastKilledEnemyType = units[ti].typeIndex;
                    lastKilledRarity = units[ti].rarity;
                    for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                      lastKilledAbilities[ab] = units[ti].abilities[ab];
                  }
                  units[ti].active = false;
                } else {
                  // Start pulling target to caster
                  units[ti].hookPullDest =
                      units[projectiles[p].sourceIndex].position;
                  units[ti].hookPullSpeed = projectiles[p].speed;
                  AddModifier(modifiers, ti, MOD_STUN, 10.0f,
                              0); // stun during pull (cleared on arrival)
                }
              }
              projectiles[p].active = false;
            }
            // HIT — Maelstrom: bounce like chain frost
            else if (projectiles[p].type == PROJ_MAELSTROM) {
              if (!UnitHasModifier(modifiers, ti, MOD_INVULNERABLE)) {
                float hitDmg = projectiles[p].damage;
                if (units[ti].shieldHP > 0) {
                  if (hitDmg <= units[ti].shieldHP) {
                    units[ti].shieldHP -= hitDmg;
                    hitDmg = 0;
                  } else {
                    hitDmg -= units[ti].shieldHP;
                    units[ti].shieldHP = 0;
                  }
                }
                units[ti].currentHealth -= hitDmg;
                CheckMushroomSpawn(units, &unitCount, ti, hitDmg);
                units[ti].hitFlash = HIT_FLASH_DURATION;
                SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg,
                                  true);

                if (units[ti].currentHealth <= 0) {
                  PlaySound(dieSfxByType[units[ti].typeIndex]);
                  SpawnDeathExplosion(particles, units[ti].position,
                                      units[ti].team);
                  TriggerShake(&shake, 6.0f, 0.3f);

                  {
                    Team killerTeam =
                        (units[ti].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                    if (killerTeam != lastKillTeam)
                      multiKillCount = 0;
                    lastKillTeam = killerTeam;
                  }
                  killCount++;
                  multiKillCount++;
                  multiKillTimer = 2.0f;
                  if (killCount == 1) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "FIRST BLOOD!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 2) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "DOUBLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 3) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "TRIPLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount >= 4) {
                    snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.5f;
                  }
                  int ba2, ra2;
                  CountTeams(units, unitCount, &ba2, &ra2);
                  if (ba2 == 0 || ra2 == 0) {
                    slowmoTimer = 0.5f;
                    slowmoScale = 0.3f;
                  }
                  BattleLogAddKill(&battleLog, combatElapsedTime,
                                   units[projectiles[p].sourceIndex].team,
                                   units[projectiles[p].sourceIndex].typeIndex,
                                   units[ti].team, units[ti].typeIndex,
                                   ABILITY_MAELSTROM);
                  if (units[ti].team == TEAM_RED) {
                    lastKilledEnemyType = units[ti].typeIndex;
                    lastKilledRarity = units[ti].rarity;
                    for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                      lastKilledAbilities[ab] = units[ti].abilities[ab];
                  }
                  units[ti].active = false;
                }
              }
              if (projectiles[p].bouncesRemaining > 0) {
                projectiles[p].bouncesRemaining--;
                projectiles[p].lastHitUnit = ti;
                projectiles[p].position = units[ti].position;
                projectiles[p].position.y += 3.0f;
                int next = FindChainFrostTarget(
                    units, unitCount, units[ti].position,
                    projectiles[p].sourceTeam, ti, projectiles[p].bounceRange);
                if (next >= 0)
                  projectiles[p].targetIndex = next;
                else
                  projectiles[p].active = false;
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
                if (hitDmg < 0)
                  hitDmg = 0;
                if (units[ti].shieldHP > 0) {
                  if (hitDmg <= units[ti].shieldHP) {
                    units[ti].shieldHP -= hitDmg;
                    hitDmg = 0;
                  } else {
                    hitDmg -= units[ti].shieldHP;
                    units[ti].shieldHP = 0;
                  }
                }
                units[ti].currentHealth -= hitDmg;
                CheckMushroomSpawn(units, &unitCount, ti, hitDmg);
                PlaySound(sfxProjectileHit);
                units[ti].hitFlash = HIT_FLASH_DURATION;
                SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg,
                                  false);
                // Lifesteal from devil bolt
                if (si >= 0 && si < unitCount && units[si].active) {
                  float ls = GetModifierValue(modifiers, si, MOD_LIFESTEAL);
                  if (ls > 0) {
                    float maxHP = UNIT_STATS[units[si].typeIndex].health *
                                  units[si].hpMultiplier;
                    units[si].currentHealth += hitDmg * ls;
                    if (units[si].currentHealth > maxHP)
                      units[si].currentHealth = maxHP;
                  }
                }
                if (units[ti].currentHealth <= 0) {
                  PlaySound(dieSfxByType[units[ti].typeIndex]);
                  SpawnDeathExplosion(particles, units[ti].position,
                                      units[ti].team);
                  TriggerShake(&shake, 4.0f, 0.2f);
                  {
                    Team killerTeam =
                        (units[ti].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                    if (killerTeam != lastKillTeam)
                      multiKillCount = 0;
                    lastKillTeam = killerTeam;
                  }
                  killCount++;
                  multiKillCount++;
                  multiKillTimer = 2.0f;
                  if (killCount == 1) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "FIRST BLOOD!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 2) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "DOUBLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 3) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "TRIPLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount >= 4) {
                    snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.5f;
                  }
                  int ba2, ra2;
                  CountTeams(units, unitCount, &ba2, &ra2);
                  if (ba2 == 0 || ra2 == 0) {
                    slowmoTimer = 0.5f;
                    slowmoScale = 0.3f;
                  }
                  BattleLogAddKill(&battleLog, combatElapsedTime,
                                   units[si].team, units[si].typeIndex,
                                   units[ti].team, units[ti].typeIndex, -1);
                  if (units[ti].team == TEAM_RED) {
                    lastKilledEnemyType = units[ti].typeIndex;
                    lastKilledRarity = units[ti].rarity;
                    for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                      lastKilledAbilities[ab] = units[ti].abilities[ab];
                  }
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
                  if (hitDmg <= units[ti].shieldHP) {
                    units[ti].shieldHP -= hitDmg;
                    hitDmg = 0;
                  } else {
                    hitDmg -= units[ti].shieldHP;
                    units[ti].shieldHP = 0;
                  }
                }
                units[ti].currentHealth -= hitDmg;
                CheckMushroomSpawn(units, &unitCount, ti, hitDmg);
                units[ti].hitFlash = HIT_FLASH_DURATION;
                SpawnDamageNumber(floatingTexts, units[ti].position, hitDmg,
                                  true);
                if (projectiles[p].stunDuration > 0) {
                  AddModifier(modifiers, ti, MOD_STUN,
                              projectiles[p].stunDuration, 0);
                  TriggerShake(&shake, 5.0f, 0.25f);
                }
                if (units[ti].currentHealth <= 0) {
                  PlaySound(dieSfxByType[units[ti].typeIndex]);
                  SpawnDeathExplosion(particles, units[ti].position,
                                      units[ti].team);
                  TriggerShake(&shake, 6.0f, 0.3f);
                  killCount++;
                  multiKillCount++;
                  multiKillTimer = 2.0f;
                  if (killCount == 1) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "FIRST BLOOD!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 2) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "DOUBLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount == 3) {
                    snprintf(killFeedText, sizeof(killFeedText),
                             "TRIPLE KILL!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.0f;
                  } else if (multiKillCount >= 4) {
                    snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!");
                    killFeedTimer = 0.0f;
                    killFeedScale = 2.5f;
                  }
                  int ba2, ra2;
                  CountTeams(units, unitCount, &ba2, &ra2);
                  if (ba2 == 0 || ra2 == 0) {
                    slowmoTimer = 0.5f;
                    slowmoScale = 0.3f;
                  }
                  {
                    int abilId = (projectiles[p].type == PROJ_MAGIC_MISSILE)
                                     ? ABILITY_MAGIC_MISSILE
                                     : ABILITY_CHAIN_FROST;
                    BattleLogAddKill(
                        &battleLog, combatElapsedTime,
                        units[projectiles[p].sourceIndex].team,
                        units[projectiles[p].sourceIndex].typeIndex,
                        units[ti].team, units[ti].typeIndex, abilId);
                  }
                  if (units[ti].team == TEAM_RED) {
                    lastKilledEnemyType = units[ti].typeIndex;
                    lastKilledRarity = units[ti].rarity;
                    for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                      lastKilledAbilities[ab] = units[ti].abilities[ab];
                  }
                  units[ti].active = false;
                }
              }
              // Chain Frost bounce
              if (projectiles[p].type == PROJ_CHAIN_FROST &&
                  projectiles[p].bouncesRemaining > 0) {
                projectiles[p].bouncesRemaining--;
                projectiles[p].damage += projectiles[p].damageIncrease;
                projectiles[p].lastHitUnit = ti;
                projectiles[p].position = units[ti].position;
                projectiles[p].position.y += 3.0f;
                int next = FindChainFrostTarget(
                    units, unitCount, units[ti].position,
                    projectiles[p].sourceTeam, ti, projectiles[p].bounceRange);
                if (next >= 0)
                  projectiles[p].targetIndex = next;
                else
                  projectiles[p].active = false;
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
                ((GetRandomValue(0, 100)) / 100.0f) * 4.0f +
                    3.0f, // upward bias to fight gravity
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
            .units = units,
            .unitCount = unitCount,
            .modifiers = modifiers,
            .projectiles = projectiles,
            .particles = particles,
            .fissures = fissures,
            .floatingTexts = floatingTexts,
            .shake = &shake,
            .battleLog = &battleLog,
            .combatTime = combatElapsedTime,
        };

        // === STEP 3: Process each unit ===
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active)
            continue;
          const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];
          bool stunned = UnitHasModifier(modifiers, i, MOD_STUN);

          // Tick ability cooldowns
          for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
            if (units[i].abilities[a].abilityId < 0)
              continue;
            if (units[i].abilities[a].cooldownRemaining > 0)
              units[i].abilities[a].cooldownRemaining -= dt;
          }

          // Passive triggers (Dig, Sunder) — blocked by stun
          if (!stunned) {
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
              AbilitySlot *slot = &units[i].abilities[a];
              if (slot->abilityId == ABILITY_DIG) {
                if (slot->triggered || slot->cooldownRemaining > 0)
                  continue;
                const AbilityDef *def = &ABILITY_DEFS[ABILITY_DIG];
                float threshold = def->values[slot->level][AV_DIG_HP_THRESH];
                float unitMaxHP = stats->health * units[i].hpMultiplier;
                if (units[i].currentHealth > 0 &&
                    units[i].currentHealth <= unitMaxHP * threshold) {
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

          // Hook pull movement — drag unit toward hook destination
          if (units[i].hookPullSpeed > 0) {
            float hdx = units[i].hookPullDest.x - units[i].position.x;
            float hdz = units[i].hookPullDest.z - units[i].position.z;
            float hlen = sqrtf(hdx * hdx + hdz * hdz);
            float hstep = units[i].hookPullSpeed * dt;
            if (hlen <= hstep) {
              // Arrived at destination
              units[i].position.x = units[i].hookPullDest.x;
              units[i].position.z = units[i].hookPullDest.z;
              units[i].hookPullSpeed = 0;
              TriggerShake(&shake, 6.0f, 0.3f);
              // Remove the pull stun
              for (int m = 0; m < MAX_MODIFIERS; m++) {
                if (modifiers[m].active && modifiers[m].unitIndex == i &&
                    modifiers[m].type == MOD_STUN)
                  modifiers[m].active = false;
              }
            } else {
              units[i].position.x += (hdx / hlen) * hstep;
              units[i].position.z += (hdz / hlen) * hstep;
            }
            continue; // skip normal movement while being pulled
          }

          bool digging = UnitHasModifier(modifiers, i, MOD_DIG_HEAL);
          if (stunned || digging)
            continue;

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
            while (diff > 180.0f)
              diff -= 360.0f;
            while (diff < -180.0f)
              diff += 360.0f;
            float turnSpeed = 360.0f; // degrees per second
            if (fabsf(diff) < turnSpeed * dt)
              units[i].facingAngle = goalAngle;
            else
              units[i].facingAngle +=
                  (diff > 0 ? 1.0f : -1.0f) * turnSpeed * dt;
          }

          // Tick ability cast delay
          if (units[i].abilityCastDelay > 0)
            units[i].abilityCastDelay -= dt;

          // Active ability casting — one per frame, clockwise rotation
          bool castThisFrame = false;
          if (units[i].abilityCastDelay <= 0)
            for (int attempt = 0;
                 attempt < MAX_ABILITIES_PER_UNIT && !castThisFrame;
                 attempt++) {
              int slotIdx = ACTIVATION_ORDER[units[i].nextAbilitySlot];
              units[i].nextAbilitySlot =
                  (units[i].nextAbilitySlot + 1) % MAX_ABILITIES_PER_UNIT;

              AbilitySlot *slot = &units[i].abilities[slotIdx];
              if (slot->abilityId < 0 || slot->cooldownRemaining > 0)
                continue;
              const AbilityDef *def = &ABILITY_DEFS[slot->abilityId];
              if (def->isPassive)
                continue; // skip passives (Dig, Sunder)

              // Range gate: if ability has a cast range, check closest enemy is
              // within it
              float castRange = def->range[slot->level];
              if (castRange > 0 && target >= 0) {
                float d = DistXZ(units[i].position, units[target].position);
                if (d > castRange)
                  continue;
              } else if (castRange > 0 && target < 0) {
                continue; // need a target but none exists
              }

              switch (slot->abilityId) {
              case ABILITY_MAGIC_MISSILE:
                castThisFrame = CastMagicMissile(&combatState, i, slot, target);
                break;
              case ABILITY_VACUUM:
                castThisFrame = CastVacuum(&combatState, i, slot);
                break;
              case ABILITY_CHAIN_FROST:
                castThisFrame = CastChainFrost(&combatState, i, slot, target);
                break;
              case ABILITY_BLOOD_RAGE:
                castThisFrame = CastBloodRage(&combatState, i, slot);
                break;
              case ABILITY_EARTHQUAKE:
                castThisFrame = CastEarthquake(&combatState, i, slot);
                if (castThisFrame) {
                  // Aggressive tile ripple from earthquake epicenter
                  float eqX = units[i].position.x;
                  float eqZ = units[i].position.z;
                  float eqRadius = ABILITY_DEFS[ABILITY_EARTHQUAKE]
                                       .values[slot->level][AV_EQ_RADIUS];
                  float gridOriginEq =
                      -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                  for (int tr = 0; tr < TILE_GRID_SIZE; tr++) {
                    for (int tc = 0; tc < TILE_GRID_SIZE; tc++) {
                      float cx = gridOriginEq + (tc + 0.5f) * TILE_WORLD_SIZE;
                      float cz = gridOriginEq + (tr + 0.5f) * TILE_WORLD_SIZE;
                      float dxw = cx - eqX, dzw = cz - eqZ;
                      float dist = sqrtf(dxw * dxw + dzw * dzw);
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
              case ABILITY_SPELL_PROTECT:
                castThisFrame = CastSpellProtect(&combatState, i, slot);
                break;
              case ABILITY_CRAGGY_ARMOR:
                castThisFrame = CastCraggyArmor(&combatState, i, slot);
                break;
              case ABILITY_STONE_GAZE:
                castThisFrame = CastStoneGaze(&combatState, i, slot);
                break;
              case ABILITY_FISSURE:
                castThisFrame = CastFissure(&combatState, i, slot, target);
                break;
              case ABILITY_VLAD_AURA:
                castThisFrame = CastVladAura(&combatState, i, slot);
                break;
              case ABILITY_MAELSTROM:
                castThisFrame = CastMaelstrom(&combatState, i, slot);
                break;
              case ABILITY_SWAP:
                castThisFrame = CastSwap(&combatState, i, slot);
                break;
              case ABILITY_APHOTIC_SHIELD:
                castThisFrame = CastAphoticShield(&combatState, i, slot);
                break;
              case ABILITY_HOOK:
                castThisFrame = CastHook(&combatState, i, slot);
                break;
              case ABILITY_PRIMAL_CHARGE:
                castThisFrame = CastPrimalCharge(&combatState, i, slot);
                break;
              default:
                break;
              }
              if (castThisFrame) {
                PlaySound(sfxMagicHit);
                PlaySound(shoutSfxByType[units[i].typeIndex]);
                SpawnFloatingText(floatingTexts, units[i].position, def->name,
                                  def->color, 1.0f);
                BattleLogAddCast(&battleLog, combatElapsedTime, units[i].team,
                                 units[i].typeIndex, slot->abilityId);
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
              if (chargeSpeed <= 0)
                chargeSpeed = 80.0f;
              if (chargeDist <= ATTACK_RANGE) {
                // IMPACT — AoE damage + knockback
                int chargeLvl = 0;
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                  if (units[i].abilities[a].abilityId ==
                      ABILITY_PRIMAL_CHARGE) {
                    chargeLvl = units[i].abilities[a].level;
                    break;
                  }
                }
                const AbilityDef *pcDef = &ABILITY_DEFS[ABILITY_PRIMAL_CHARGE];
                float pcDmg = pcDef->values[chargeLvl][AV_PC_DAMAGE];
                float pcKnock = pcDef->values[chargeLvl][AV_PC_KNOCKBACK];
                float pcRadius = pcDef->values[chargeLvl][AV_PC_AOE_RADIUS];
                for (int j = 0; j < unitCount; j++) {
                  if (!units[j].active || units[j].team == units[i].team)
                    continue;
                  if (UnitHasModifier(modifiers, j, MOD_INVULNERABLE))
                    continue;
                  float dd = DistXZ(units[ct].position, units[j].position);
                  if (dd <= pcRadius) {
                    float dmgHit = pcDmg;
                    if (units[j].shieldHP > 0) {
                      if (dmgHit <= units[j].shieldHP) {
                        units[j].shieldHP -= dmgHit;
                        dmgHit = 0;
                      } else {
                        dmgHit -= units[j].shieldHP;
                        units[j].shieldHP = 0;
                      }
                    }
                    units[j].currentHealth -= dmgHit;
                    CheckMushroomSpawn(units, &unitCount, j, dmgHit);
                    units[j].hitFlash = HIT_FLASH_DURATION;
                    SpawnDamageNumber(floatingTexts, units[j].position, dmgHit,
                                      true);

                    if (units[j].currentHealth <= 0) {
                      PlaySound(dieSfxByType[units[j].typeIndex]);
                      SpawnDeathExplosion(particles, units[j].position,
                                          units[j].team);
                      TriggerShake(&shake, 6.0f, 0.3f);

                      {
                        Team killerTeam =
                            (units[j].team == TEAM_BLUE) ? TEAM_RED : TEAM_BLUE;
                        if (killerTeam != lastKillTeam)
                          multiKillCount = 0;
                        lastKillTeam = killerTeam;
                      }
                      killCount++;
                      multiKillCount++;
                      multiKillTimer = 2.0f;
                      if (killCount == 1) {
                        snprintf(killFeedText, sizeof(killFeedText),
                                 "FIRST BLOOD!");
                        killFeedTimer = 0.0f;
                        killFeedScale = 2.0f;
                      } else if (multiKillCount == 2) {
                        snprintf(killFeedText, sizeof(killFeedText),
                                 "DOUBLE KILL!");
                        killFeedTimer = 0.0f;
                        killFeedScale = 2.0f;
                      } else if (multiKillCount == 3) {
                        snprintf(killFeedText, sizeof(killFeedText),
                                 "TRIPLE KILL!");
                        killFeedTimer = 0.0f;
                        killFeedScale = 2.0f;
                      } else if (multiKillCount >= 4) {
                        snprintf(killFeedText, sizeof(killFeedText),
                                 "RAMPAGE!");
                        killFeedTimer = 0.0f;
                        killFeedScale = 2.5f;
                      }
                      int ba2, ra2;
                      CountTeams(units, unitCount, &ba2, &ra2);
                      if (ba2 == 0 || ra2 == 0) {
                        slowmoTimer = 0.5f;
                        slowmoScale = 0.3f;
                      }
                      BattleLogAddKill(&battleLog, combatElapsedTime,
                                       units[i].team, units[i].typeIndex,
                                       units[j].team, units[j].typeIndex,
                                       ABILITY_PRIMAL_CHARGE);
                      if (units[j].team == TEAM_RED) {
                        lastKilledEnemyType = units[j].typeIndex;
                        lastKilledRarity = units[j].rarity;
                        for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                          lastKilledAbilities[ab] = units[j].abilities[ab];
                      }
                      units[j].active = false;
                    }
                    // Knockback
                    float kx = units[j].position.x - units[ct].position.x;
                    float kz = units[j].position.z - units[ct].position.z;
                    float klen = sqrtf(kx * kx + kz * kz);
                    if (klen > 0.001f) {
                      units[j].position.x += (kx / klen) * pcKnock;
                      units[j].position.z += (kz / klen) * pcKnock;
                    }
                  }
                }
                TriggerShake(&shake, 8.0f, 0.4f);
                units[i].chargeTarget = -1;
                // Remove charging modifier
                for (int m = 0; m < MAX_MODIFIERS; m++) {
                  if (modifiers[m].active && modifiers[m].unitIndex == i &&
                      modifiers[m].type == MOD_CHARGING)
                    modifiers[m].active = false;
                }
              } else {
                float cdx = units[ct].position.x - units[i].position.x;
                float cdz = units[ct].position.z - units[i].position.z;
                float clen = sqrtf(cdx * cdx + cdz * cdz);
                units[i].position.x += (cdx / clen) * chargeSpeed * dt;
                units[i].position.z += (cdz / clen) * chargeSpeed * dt;
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
          if (target < 0)
            continue;
          float moveSpeed = stats->movementSpeed * units[i].speedMultiplier;
          float speedMult = GetModifierValue(modifiers, i, MOD_SPEED_MULT);
          if (speedMult > 0)
            moveSpeed *= speedMult;

          bool isDevil = (units[i].typeIndex == DEVIL_TYPE_INDEX);
          float unitAttackRange = isDevil ? DEVIL_RANGED_RANGE : ATTACK_RANGE;

          float dist = DistXZ(units[i].position, units[target].position);
          if (dist > unitAttackRange) {
            Vector3 oldPos = units[i].position;
            float dx = units[target].position.x - units[i].position.x;
            float dz = units[target].position.z - units[i].position.z;
            float len = sqrtf(dx * dx + dz * dz);
            if (len > 0.001f) {
              units[i].position.x += (dx / len) * moveSpeed * dt;
              units[i].position.z += (dz / len) * moveSpeed * dt;
            }
            // Fissure collision — slide along impassable terrain
            float unitRadius = 2.0f;
            units[i].position = ResolveFissureCollision(
                fissures, units[i].position, oldPos, unitRadius);

            // Unit-unit collision — push overlapping units apart on XZ plane
            for (int j = 0; j < unitCount; j++) {
              if (j == i || !units[j].active)
                continue;
              float cdist = DistXZ(units[i].position, units[j].position);
              float minDist = UNIT_COLLISION_RADIUS * 2.0f;
              if (cdist < minDist && cdist > 0.001f) {
                float overlap = minDist - cdist;
                float pushX =
                    (units[i].position.x - units[j].position.x) / cdist;
                float pushZ =
                    (units[i].position.z - units[j].position.z) / cdist;
                units[i].position.x += pushX * overlap * 0.5f;
                units[i].position.z += pushZ * overlap * 0.5f;
                units[j].position.x -= pushX * overlap * 0.5f;
                units[j].position.z -= pushZ * overlap * 0.5f;
              }
            }
          } else {
            units[i].attackCooldown -= dt;
            if (units[i].attackCooldown <= 0.0f) {
              if (isDevil) {
                // Devil ranged attack — spawn a bolt projectile
                float dmg = stats->attackDamage * units[i].dmgMultiplier;
                SpawnProjectile(projectiles, PROJ_DEVIL_BOLT, units[i].position,
                                target, i, units[i].team, 0, 50.0f, dmg, 0,
                                (Color){200, 50, 50, 255});
                PlaySound(sfxProjectileWhoosh);
                units[i].attackCooldown = stats->attackSpeed;
                units[i].castPause = CAST_PAUSE_TIME;
              } else {
                if (!UnitHasModifier(modifiers, target, MOD_INVULNERABLE)) {
                  float dmg = stats->attackDamage * units[i].dmgMultiplier;
                  float armor = GetModifierValue(modifiers, target, MOD_ARMOR);
                  dmg -= armor;
                  if (dmg < 0)
                    dmg = 0;
                  // Shield absorption
                  if (units[target].shieldHP > 0) {
                    if (dmg <= units[target].shieldHP) {
                      units[target].shieldHP -= dmg;
                      dmg = 0;
                    } else {
                      dmg -= units[target].shieldHP;
                      units[target].shieldHP = 0;
                    }
                  }
                  units[target].currentHealth -= dmg;
                  CheckMushroomSpawn(units, &unitCount, target, dmg);
                  PlaySound(sfxMeleeHit);
                  units[target].hitFlash = HIT_FLASH_DURATION;
                  SpawnDamageNumber(floatingTexts, units[target].position, dmg,
                                    false);
                  SpawnMeleeImpact(particles, units[target].position);
                  // Minor tile wobble on melee hit
                  {
                    float gridOriginMH =
                        -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
                    for (int tr = 0; tr < TILE_GRID_SIZE; tr++) {
                      for (int tc = 0; tc < TILE_GRID_SIZE; tc++) {
                        float cx = gridOriginMH + (tc + 0.5f) * TILE_WORLD_SIZE;
                        float cz = gridOriginMH + (tr + 0.5f) * TILE_WORLD_SIZE;
                        float dxw = cx - units[target].position.x,
                              dzw = cz - units[target].position.z;
                        float dist = sqrtf(dxw * dxw + dzw * dzw);
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
                    float procChance =
                        GetModifierValue(modifiers, i, MOD_MAELSTROM);
                    float roll = (float)GetRandomValue(0, 100) / 100.0f;
                    if (roll < procChance) {
                      // Find maelstrom ability level
                      int mlLvl = 0;
                      for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
                        if (units[i].abilities[a].abilityId ==
                            ABILITY_MAELSTROM) {
                          mlLvl = units[i].abilities[a].level;
                          break;
                        }
                      }
                      const AbilityDef *mlDef =
                          &ABILITY_DEFS[ABILITY_MAELSTROM];
                      SpawnMaelstromProjectile(
                          projectiles, units[target].position, target, i,
                          units[i].team, mlLvl,
                          mlDef->values[mlLvl][AV_ML_SPEED],
                          mlDef->values[mlLvl][AV_ML_DAMAGE],
                          (int)mlDef->values[mlLvl][AV_ML_BOUNCES],
                          mlDef->values[mlLvl][AV_ML_BOUNCE_RANGE]);
                    }
                  }
                  if (units[target].currentHealth <= 0) {
                    PlaySound(dieSfxByType[units[target].typeIndex]);
                    SpawnDeathExplosion(particles, units[target].position,
                                        units[target].team);
                    TriggerShake(&shake, 6.0f, 0.3f);

                    {
                      Team killerTeam = (units[target].team == TEAM_BLUE)
                                            ? TEAM_RED
                                            : TEAM_BLUE;
                      if (killerTeam != lastKillTeam)
                        multiKillCount = 0;
                      lastKillTeam = killerTeam;
                    }
                    killCount++;
                    multiKillCount++;
                    multiKillTimer = 2.0f;
                    if (killCount == 1) {
                      snprintf(killFeedText, sizeof(killFeedText),
                               "FIRST BLOOD!");
                      killFeedTimer = 0.0f;
                      killFeedScale = 2.0f;
                    } else if (multiKillCount == 2) {
                      snprintf(killFeedText, sizeof(killFeedText),
                               "DOUBLE KILL!");
                      killFeedTimer = 0.0f;
                      killFeedScale = 2.0f;
                    } else if (multiKillCount == 3) {
                      snprintf(killFeedText, sizeof(killFeedText),
                               "TRIPLE KILL!");
                      killFeedTimer = 0.0f;
                      killFeedScale = 2.0f;
                    } else if (multiKillCount >= 4) {
                      snprintf(killFeedText, sizeof(killFeedText), "RAMPAGE!");
                      killFeedTimer = 0.0f;
                      killFeedScale = 2.5f;
                    }
                    int ba2, ra2;
                    CountTeams(units, unitCount, &ba2, &ra2);
                    if (ba2 == 0 || ra2 == 0) {
                      slowmoTimer = 0.5f;
                      slowmoScale = 0.3f;
                    }
                    BattleLogAddKill(&battleLog, combatElapsedTime,
                                     units[i].team, units[i].typeIndex,
                                     units[target].team,
                                     units[target].typeIndex, -1);
                    // Track last killed enemy type for capture mechanic
                    if (units[target].team == TEAM_RED) {
                      lastKilledEnemyType = units[target].typeIndex;
                      lastKilledRarity = units[target].rarity;
                      for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                        lastKilledAbilities[ab] = units[target].abilities[ab];
                    }
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
          if (!units[i].active)
            continue;
          // Check if any enemy has Stone Gaze active
          bool beingGazed = false;
          for (int g = 0; g < unitCount; g++) {
            if (!units[g].active || units[g].team == units[i].team)
              continue;
            if (!UnitHasModifier(modifiers, g, MOD_STONE_GAZE))
              continue;
            // Check if unit i is facing toward gazer g (within cone)
            float dx = units[g].position.x - units[i].position.x;
            float dz = units[g].position.z - units[i].position.z;
            float distToGazer = sqrtf(dx * dx + dz * dz);
            if (distToGazer < 0.1f)
              continue;
            // Unit i's facing direction
            float facingRad = units[i].facingAngle * (PI / 180.0f);
            float faceDirX = sinf(facingRad);
            float faceDirZ = cosf(facingRad);
            // Dot product to check if facing toward gazer
            float dot =
                (dx / distToGazer) * faceDirX + (dz / distToGazer) * faceDirZ;
            float coneAngle = 45.0f; // default cone half-angle
            // Get cone angle from the gazer's Stone Gaze ability
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
              if (units[g].abilities[a].abilityId == ABILITY_STONE_GAZE) {
                int lvl = units[g].abilities[a].level;
                coneAngle = ABILITY_DEFS[ABILITY_STONE_GAZE]
                                .values[lvl][AV_SG_CONE_ANGLE];
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
                  float thresh = ABILITY_DEFS[ABILITY_STONE_GAZE]
                                     .values[lvl][AV_SG_GAZE_THRESH];
                  float stunDur = ABILITY_DEFS[ABILITY_STONE_GAZE]
                                      .values[lvl][AV_SG_STUN_DUR];
                  if (units[i].gazeAccum >= thresh) {
                    AddModifier(modifiers, i, MOD_STUN, stunDur, 0);
                    units[i].gazeAccum = 0;
                    TriggerShake(&shake, 3.0f, 0.2f);
                    SpawnFloatingText(floatingTexts, units[i].position,
                                      "PETRIFIED!", (Color){160, 80, 200, 255},
                                      1.0f);
                  }
                  break;
                }
              }
              break; // only accumulate from one gazer at a time
            }
          }
          if (!beingGazed && units[i].gazeAccum > 0) {
            units[i].gazeAccum -= dt * 2.0f; // decay twice as fast
            if (units[i].gazeAccum < 0)
              units[i].gazeAccum = 0;
          }
        }

        // Smooth Y toward ground during combat
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active)
            continue;
          units[i].position.y += (0.0f - units[i].position.y) * 0.1f;
        }
      } // end if(0) dead code block

    // Check round end
    combat_check_end:
      if (isMultiplayer) {
        // In multiplayer, poll for server result
#ifdef USE_EOS
        if (useEos)
          eos_client_poll(&eosClient);
        else
#endif
          net_client_poll(&netClient);
        if (NC_FLAG(roundResultReady)) {
          NC_CLEAR(roundResultReady);
          int rw = NC_FLAG(roundWinner);
          mpHealth[0] = NC_FLAG(playerHealth[0]);
          mpHealth[1] = NC_FLAG(playerHealth[1]);
          lastRoundDamage = NC_FLAG(lastRoundDamage);
          if (rw == 0) {
            roundResultText = "YOU WIN THE ROUND!";
          } else if (rw == 1) {
            snprintf(mpRoundResultBuf, sizeof(mpRoundResultBuf),
                     "OPPONENT WINS! (-%d HP)", lastRoundDamage);
            roundResultText = mpRoundResultBuf;
          } else {
            roundResultText = "DRAW — NO DAMAGE!";
          }
          currentRound = NC_FLAG(currentRound);
          lastOutcomeWin = (rw == 0);
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
              Vector3 cpos = {(float)GetRandomValue(-80, 80),
                              (float)GetRandomValue(30, 60),
                              (float)GetRandomValue(-80, 80)};
              Vector3 cvel = {(float)GetRandomValue(-20, 20) / 10.0f,
                              (float)GetRandomValue(-10, -2) / 10.0f,
                              (float)GetRandomValue(-20, 20) / 10.0f};
              Color cc = (Color){(unsigned char)GetRandomValue(100, 255),
                                 (unsigned char)GetRandomValue(100, 255),
                                 (unsigned char)GetRandomValue(100, 255), 255};
              SpawnParticle(particles, cpos, cvel,
                            2.0f + (float)GetRandomValue(0, 10) / 10.0f,
                            (float)GetRandomValue(3, 8) / 10.0f, cc);
            }
          }
        }
        if (NC_FLAG(gameOver)) {
          NC_CLEAR(gameOver);
          int gw = NC_FLAG(gameWinner);
          if (gw == 0)
            roundResultText = "YOU WIN THE MATCH!";
          else
            roundResultText = "OPPONENT WINS THE MATCH!";
          lastOutcomeWin = (gw == 0);
          phase = PHASE_GAME_OVER;
          ClearAllParticles(particles);
          ClearAllFloatingTexts(floatingTexts);
          ClearAllFissures(fissures);
        }
        // Peer disconnected during combat
        if (NC_FLAG(peerDisconnected)) {
          NC_CLEAR(peerDisconnected);
          lastOutcomeWin = true;
          roundResultText = "Opponent disconnected";
          phase = PHASE_GAME_OVER;
          ClearAllParticles(particles);
          ClearAllFloatingTexts(floatingTexts);
          ClearAllFissures(fissures);
        }
      } else {
        int ba, ra;
        CountTeams(units, unitCount, &ba, &ra);
        if (ba == 0 || ra == 0) {
          if (ba > 0) {
            blueWins++;
            roundResultText = "BLUE WINS THE ROUND!";
            blueLostLastRound = false;
          } else if (ra > 0) {
            redWins++;
            roundResultText = "RED WINS THE ROUND!";
            blueLostLastRound = true;
          } else {
            roundResultText = "DRAW — NO SURVIVORS!";
            blueLostLastRound = true;
          }
          // Calculate gold reward breakdown
          {
            int enemyKills = 0, bossKills = 0, alliesAlive = 0;
            for (int i = 0; i < unitCount; i++) {
              if (units[i].team == TEAM_RED && !units[i].active) {
                if (units[i].scaleOverride > 1.5f)
                  bossKills++;
                else
                  enemyKills++;
              }
              if (units[i].team == TEAM_BLUE && units[i].active &&
                  !units[i].isMushling)
                alliesAlive++;
            }
            goldFlat = 3;
            goldKills = enemyKills * 3;
            goldBoss = bossKills * 5;
            goldAlive = alliesAlive * 1;
            goldInterest = playerGold / 5;
            roundGoldReward = goldFlat + goldKills + goldBoss + goldAlive;
          }
          if (!mapActive) currentRound++;
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
              Vector3 cpos = {(float)GetRandomValue(-80, 80),
                              (float)GetRandomValue(30, 60),
                              (float)GetRandomValue(-80, 80)};
              Vector3 cvel = {(float)GetRandomValue(-20, 20) / 10.0f,
                              (float)GetRandomValue(-10, -2) / 10.0f,
                              (float)GetRandomValue(-20, 20) / 10.0f};
              Color cc = (Color){(unsigned char)GetRandomValue(100, 255),
                                 (unsigned char)GetRandomValue(100, 255),
                                 (unsigned char)GetRandomValue(100, 255), 255};
              SpawnParticle(particles, cpos, cvel,
                            2.0f + (float)GetRandomValue(0, 10) / 10.0f,
                            (float)GetRandomValue(3, 8) / 10.0f, cc);
            }
          }
        }
      }
    combat_skip:
      (void)0;
    }
    //------------------------------------------------------------------------------
    // PHASE: ROUND_OVER — brief pause, then milestone/death/prep
    //------------------------------------------------------------------------------
    else if (phase == PHASE_ROUND_OVER) {
      // Multiplayer: poll for next prep from server
      if (isMultiplayer) {
#ifdef USE_EOS
        if (useEos)
          eos_client_poll(&eosClient);
        else
#endif
          net_client_poll(&netClient);
        roundOverTimer -= dt;
        if (NC_FLAG(prepStarted)) {
          NC_CLEAR(prepStarted);
          playerGold = NC_FLAG(currentGold);
          currentRound = NC_FLAG(currentRound);
          currentRoundIsPve = NC_FLAG(isPveRound);
          for (int i = 0; i < MAX_SHOP_SLOTS; i++) {
#ifdef USE_EOS
            shopSlots[i] =
                useEos ? eosClient.serverShop[i] : netClient.serverShop[i];
#else
            shopSlots[i] = netClient.serverShop[i];
#endif
          }
          RestoreSnapshot(units, &unitCount, snapshots, snapshotCount);
          for (int i = 0; i < unitCount; i++)
            if (units[i].team == TEAM_RED)
              units[i].active = false;
          ClearAllModifiers(modifiers);
          ClearAllProjectiles(projectiles);
          ClearAllFloatingTexts(floatingTexts);
          ClearAllFissures(fissures);
          playerReady = false;
          waitingForOpponent = false;
          opponentIsReady = false;
          phase = PHASE_PREP;
        }
        if (NC_FLAG(gameOver)) {
          NC_CLEAR(gameOver);
          int gw = NC_FLAG(gameWinner);
          if (gw == 0)
            roundResultText = "YOU WIN THE MATCH!";
          else
            roundResultText = "OPPONENT WINS THE MATCH!";
          lastOutcomeWin = (gw == 0);
          phase = PHASE_GAME_OVER;
        }
      }
      // Solo: original logic
      else {
        roundOverTimer -= dt;

        if (roundOverTimer <= 0.0f) {
          // Auto-capture: if party not full and we killed a red unit, add it
          if (!blueLostLastRound && lastKilledEnemyType >= 0 &&
              !(currentRound > 0 && currentRound % 5 == 0)) {
            int blueAlive = CountTeamUnits(units, unitCount, TEAM_BLUE);
            if (blueAlive < BLUE_TEAM_MAX_SIZE) {
              RestoreSnapshot(units, &unitCount, snapshots, snapshotCount);
              ClearRedUnits(units, &unitCount);
              if (SpawnUnit(units, &unitCount, lastKilledEnemyType,
                            TEAM_BLUE)) {
                int newIdx = unitCount - 1;
                units[newIdx].position.x = (float)GetRandomValue(-30, 30);
                units[newIdx].position.z = (float)GetRandomValue(20, 60);
                units[newIdx].rarity = lastKilledRarity;
                // Copy abilities from killed enemy
                for (int ab = 0; ab < MAX_ABILITIES_PER_UNIT; ab++)
                  units[newIdx].abilities[ab] = lastKilledAbilities[ab];
                PlaySound(sfxNewCharacter);
                intro = (UnitIntro){.active = true,
                                    .timer = 0.0f,
                                    .typeIndex = lastKilledEnemyType,
                                    .unitIndex = newIdx,
                                    .animFrame = 0,
                                    .rarity = lastKilledRarity};
                SaveSnapshot(units, unitCount, snapshots, &snapshotCount);
              }
            }
          }
          lastKilledEnemyType = -1;
          if (blueLostLastRound && lastMilestoneRound > 0) {
            // DEATH PENALTY: lost after a milestone — units gone
            deathPenalty = true;
            lastOutcomeWin = false;
            phase = PHASE_GAME_OVER;
          } else if (!firstWaveDone) {
            // First wave beaten — now generate map and enter map phase
            firstWaveDone = true;
            // Defer intro so it plays after map pick, not over the map
            if (intro.active) {
              pendingIntro = intro;
              intro.active = false;
            }
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
            playerGold += roundGoldReward + goldInterest;
            RollShop(shopSlots, &playerGold, 0, currentRound, activeShopSlots);
            rollCost = rollCostBase;
            GenerateMap(&actMap, 1, (uint32_t)GetRandomValue(1, 999999));
            ResetMapScroll();
            // Auto-complete layer 0 (wave 1 already beaten)
            for (int i = 0; i < actMap.nodeCount; i++) {
              if (actMap.nodes[i].layer == 0 && actMap.nodes[i].available) {
                actMap.nodes[i].visited = true;
                actMap.nodes[i].available = false;
                actMap.currentNode = i;
                actMap.currentLayer = 0;
                for (int j = 0; j < actMap.nodeCount; j++)
                  actMap.nodes[j].available = false;
                for (int e = 0; e < actMap.nodes[i].edgeCount; e++) {
                  int next = actMap.nodes[i].edges[e];
                  if (next >= 0 && next < actMap.nodeCount)
                    actMap.nodes[next].available = true;
                }
                break;
              }
            }
            ScrollMapToLayer(1);
            mapActive = true;
            showingMapEvent = false;
            mapEventChoice = -1;
            phase = PHASE_MAP;
          } else if (mapActive) {
            // Map mode: return to map after combat
            // Defer intro so it plays after the next combat pick, not over the map
            if (intro.active) {
              pendingIntro = intro;
              intro.active = false;
            }
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
            {
              playerGold += roundGoldReward + goldInterest;
            }
            // If boss node was beaten, go to milestone
            if (mapSelectedNodeType == NODE_BOSS && !blueLostLastRound) {
              phase = PHASE_MILESTONE;
            } else if (mapSelectedNodeType == NODE_BOSS && blueLostLastRound) {
              // Lost to the boss — game over
              deathPenalty = true;
              lastOutcomeWin = false;
              phase = PHASE_GAME_OVER;
            } else {
              ScrollMapToLayer(actMap.currentLayer);
              phase = PHASE_MAP;
            }
          } else if (currentRound > 0 && currentRound % 5 == 0) {
            // Milestone reached — go to selection screen (legacy non-map mode)
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
            // Normal round transition (legacy non-map mode)
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
            SpawnWave(units, &unitCount, currentRound, unitTypeCount, currentRound >= TOTAL_ROUNDS && currentRound % 5 == 4);
            // Generate wave upgrade description
            if (currentRound < TOTAL_ROUNDS) {
              switch (currentRound) {
              case 1:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "+1 Enemy, each with 1 ability");
                break;
              case 2:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "Enemies now have 2 abilities");
                break;
              case 3:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "+1 Enemy, abilities leveled up");
                break;
              case 4:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "BOSS ROUND!");
                break;
              default:
                waveUpgradeText[0] = '\0';
                break;
              }
            } else {
              int extraR = currentRound - TOTAL_ROUNDS;
              int roll = ((extraR * 7 + 13) * 31) % 100;
              if (roll < 50)
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "+1 Enemy unit");
              else
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "Enemy abilities upgraded");
            }
            {
              playerGold += roundGoldReward + goldInterest;
            }
            RollShop(shopSlots, &playerGold, 0, currentRound, activeShopSlots);
            rollCost = rollCostBase;
            phase = PHASE_PREP;
          }
        }
      } // end solo else
    }
    //------------------------------------------------------------------------------
    // PHASE: MILESTONE — "Set in Stone" selection screen
    //------------------------------------------------------------------------------
    else if (phase == PHASE_MILESTONE) {
      if (IsKeyPressed(KEY_ESCAPE)) {
        if (showHelp)
          showHelp = false;
        else
          showEscMenu = !showEscMenu;
      }
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !showEscMenu &&
          !showHelp) {
        Vector2 mouse = GetMousePosition();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();

        // Collect active blue units
        int msBlue[BLUE_TEAM_MAX_SIZE];
        int msCount = 0;
        for (int i = 0; i < unitCount && msCount < BLUE_TEAM_MAX_SIZE; i++)
          if (units[i].active && units[i].team == TEAM_BLUE)
            msBlue[msCount++] = i;

        // Card layout (display only, no toggles)
        int cardW = 200, cardH = 140, cardGap = 20;
        int totalW =
            msCount * cardW + (msCount > 1 ? (msCount - 1) * cardGap : 0);
        int startX = (sw - totalW) / 2;
        int cardY = sh / 2 - cardH / 2 - 20;
        (void)totalW;
        (void)startX; // positioning computed for drawing code below

        // Buttons (two: SET IN STONE, CONTINUE)
        int btnW = 240, btnH = 54;
        int btnY = cardY + cardH + 30;
        int btnGap = 40;
        int totalBtnW = 2 * btnW + btnGap;
        int btnStartX = (sw - totalBtnW) / 2;

        // SET IN STONE button — saves entire party to leaderboard, then game
        // over
        Rectangle setBtn = {(float)btnStartX, (float)btnY, (float)btnW,
                            (float)btnH};
        if (CheckCollisionPointRec(mouse, setBtn) && msCount > 0) {
          // Build leaderboard entry from all blue units
          LeaderboardEntry entry = {0};
          snprintf(entry.playerName, sizeof(entry.playerName), "%s",
                   playerName);
          entry.highestRound = currentRound;
          entry.unitCount = msCount;
          for (int h = 0; h < msCount; h++) {
            int ui = msBlue[h];
            entry.units[h].typeIndex = units[ui].typeIndex;
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
              entry.units[h].abilities[a].abilityId =
                  units[ui].abilities[a].abilityId;
              entry.units[h].abilities[a].level = units[ui].abilities[a].level;
            }
          }
          InsertLeaderboardEntry(&leaderboard, &entry);
          SaveLeaderboard(&leaderboard, LEADERBOARD_FILE);

          lastMilestoneRound = currentRound;
          deathPenalty = false;
          lastOutcomeWin = true;
          phase = PHASE_GAME_OVER;
        }

        // CONTINUE button — skip prestige, keep playing
        Rectangle contBtn = {(float)(btnStartX + btnW + btnGap), (float)btnY,
                             (float)btnW, (float)btnH};
        if (CheckCollisionPointRec(mouse, contBtn)) {
          lastMilestoneRound = currentRound;
          if (mapActive) {
            // Generate next act map and go to map
            actMap.act++;
            GenerateMap(&actMap, actMap.act, actMap.seed + actMap.act);
            ResetMapScroll();
            ScrollMapToLayer(0);
            {
              playerGold += roundGoldReward + goldInterest;
            }
            phase = PHASE_MAP;
          } else {
            SpawnWave(units, &unitCount, currentRound, unitTypeCount, currentRound >= TOTAL_ROUNDS && currentRound % 5 == 4);
            // Generate wave upgrade description
            if (currentRound < TOTAL_ROUNDS) {
              switch (currentRound) {
              case 1:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "+1 Enemy, each with 1 ability");
                break;
              case 2:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "Enemies now have 2 abilities");
                break;
              case 3:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "+1 Enemy, abilities leveled up");
                break;
              case 4:
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "BOSS ROUND!");
                break;
              default:
                waveUpgradeText[0] = '\0';
                break;
              }
            } else {
              int extraR = currentRound - TOTAL_ROUNDS;
              int roll = ((extraR * 7 + 13) * 31) % 100;
              if (roll < 50)
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "+1 Enemy unit");
              else
                snprintf(waveUpgradeText, sizeof(waveUpgradeText),
                         "Enemy abilities upgraded");
            }
            {
              playerGold += roundGoldReward + goldInterest;
            }
            RollShop(shopSlots, &playerGold, 0, currentRound, activeShopSlots);
            rollCost = rollCostBase;
            phase = PHASE_PREP;
          }
        }
      }
    }
    //------------------------------------------------------------------------------
    // PHASE: GAME_OVER — show final result, press R to return to menu
    //------------------------------------------------------------------------------
    else if (phase == PHASE_GAME_OVER) {
      // Multiplayer: press R, ESC, or click EXIT button to return to menu
      bool mpExitClicked = false;
      if (isMultiplayer && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
          !showEscMenu && !showHelp) {
        int exW = 180, exH = 44;
        int exSw = GetScreenWidth(), exSh = GetScreenHeight();
        Rectangle exBtn = {(float)(exSw / 2 - exW / 2), (float)(exSh / 2 + 70),
                           (float)exW, (float)exH};
        if (CheckCollisionPointRec(GetMousePosition(), exBtn))
          mpExitClicked = true;
      }
      if (IsKeyPressed(KEY_ESCAPE)) {
        if (showHelp)
          showHelp = false;
        else
          showEscMenu = !showEscMenu;
      }
      if (isMultiplayer && (IsKeyPressed(KEY_R) || mpExitClicked)) {
#ifdef USE_EOS
        if (useEos) {
          eos_client_disconnect(&eosClient);
          useEos = false;
        } else
#endif
        {
          if (isHosting) {
            host_stop();
            isHosting = false;
          }
          net_client_disconnect(&netClient);
        }
        isMultiplayer = false;
        for (int u2 = 0; u2 < MAX_UNITS; u2++) {
          units[u2].active = false;
        }
        unitCount = 0;
        snapshotCount = 0;
        currentRound = 0;
        mpHealth[0] = 20;
        mpHealth[1] = 20;
        lastRoundDamage = 0;
        roundResultText = "";
        ClearAllModifiers(modifiers);
        ClearAllProjectiles(projectiles);
        ClearAllParticles(particles);
        ClearAllFloatingTexts(floatingTexts);
        ClearAllFissures(fissures);
        playerGold = 20;
        for (int i = 0; i < MAX_INVENTORY_SLOTS; i++)
          inventory[i].abilityId = -1;
        dragState.dragging = false;
        unitCount = 0;
        memset(plazaData, 0, sizeof(plazaData));
        PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);
        plazaState = PLAZA_ROAMING;
        phase = PHASE_PLAZA;
        PlayMusicStream(bgm);
      }

      // Solo: existing game over logic
      if (!isMultiplayer && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
          !deathPenalty && !showEscMenu && !showHelp) {
        Vector2 mouse = GetMousePosition();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();

        // Collect surviving blue units for withdraw
        int goBlue[BLUE_TEAM_MAX_SIZE];
        int goCount = 0;
        for (int i = 0; i < unitCount && goCount < BLUE_TEAM_MAX_SIZE; i++)
          if (units[i].active && units[i].team == TEAM_BLUE)
            goBlue[goCount++] = i;

        // Withdraw buttons per unit card
        int cardW = 200, cardH = 140, cardGap = 20;
        int totalW =
            goCount * cardW + (goCount > 1 ? (goCount - 1) * cardGap : 0);
        int startX = (sw - totalW) / 2;
        int cardY = sh / 2 - 40;
        for (int h = 0; h < goCount; h++) {
          int cx = startX + h * (cardW + cardGap);
          Rectangle wdBtn = {(float)(cx + 10), (float)(cardY + cardH - 34),
                             (float)(cardW - 20), 28};
          if (CheckCollisionPointRec(mouse, wdBtn)) {
            // Withdraw unit
            int wi = goBlue[h];
            printf("[WITHDRAW] Unit %d (%s) withdrawn\n", wi,
                   unitTypes[units[wi].typeIndex].name);
            units[wi].active = false;
            CompactBlueUnits(units, &unitCount);
            break; // re-layout next frame
          }
        }

        // RESET button
        int resetBtnW = 180, resetBtnH = 44;
        int resetBtnY = cardY + cardH + 30;
        Rectangle resetBtn = {(float)(sw / 2 - resetBtnW / 2), (float)resetBtnY,
                              (float)resetBtnW, (float)resetBtnH};
        if (CheckCollisionPointRec(mouse, resetBtn)) {
          PlaySound(sfxUiClick);
          // Full reset — go to menu
          for (int u2 = 0; u2 < MAX_UNITS; u2++) {
            units[u2].active = false;
          }
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
          playerGold = 20;
          for (int i = 0; i < MAX_INVENTORY_SLOTS; i++)
            inventory[i].abilityId = -1;
          dragState.dragging = false;
          unitCount = 0;
          memset(plazaData, 0, sizeof(plazaData));
          PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);
          plazaState = PLAZA_ROAMING;
          phase = PHASE_PLAZA;
          PlayMusicStream(bgm);
        }
      }

      // Death penalty: just press R (no withdraw possible)
      if (deathPenalty && IsKeyPressed(KEY_R)) {
        // Full reset — clear all units
        for (int u2 = 0; u2 < MAX_UNITS; u2++) {
          units[u2].active = false;
        }
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
        activeShopSlots = 3;
        playerGold = 20;
        for (int i = 0; i < MAX_INVENTORY_SLOTS; i++)
          inventory[i].abilityId = -1;
        dragState.dragging = false;
        memset(plazaData, 0, sizeof(plazaData));
        PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);
        plazaState = PLAZA_ROAMING;
        phase = PHASE_PLAZA;
        PlayMusicStream(bgm);
      }
    }

    //==============================================================================
    // ANIMATION UPDATE
    //==============================================================================
    for (int i = 0; i < unitCount; i++) {
      if (!units[i].active)
        continue;
      if (units[i].hitFlash > 0)
        units[i].hitFlash -= dt;
      if (IsUnitInStatueSpawn(&statueSpawn, i))
        continue; // frozen as statue
      UnitType *type = &unitTypes[units[i].typeIndex];
      if (!type->hasAnimations)
        continue;

      // Determine desired anim state
      AnimState desired = ANIM_IDLE;

      if (units[i].castPause > 0 && type->animIndex[ANIM_CAST] >= 0) {
        desired = ANIM_CAST;
      } else if (units[i].attackAnimTimer > 0 &&
                 type->animIndex[ANIM_ATTACK] >= 0) {
        units[i].attackAnimTimer -= dt;
        desired = ANIM_ATTACK;
      } else if (phase == PHASE_COMBAT && units[i].targetIndex >= 0) {
        float animRange = (units[i].typeIndex == DEVIL_TYPE_INDEX)
                              ? DEVIL_RANGED_RANGE
                              : ATTACK_RANGE;
        float dist =
            DistXZ(units[i].position, units[units[i].targetIndex].position);
        if (dist > animRange)
          desired = ANIM_WALK;
      } else if (phase == PHASE_PLAZA) {
        desired = units[i].currentAnim; // set by plaza roam/flee logic
      }

      // Reset frame on anim change
      if (desired != units[i].currentAnim) {
        units[i].currentAnim = desired;
        units[i].animFrame = 0;
        units[i].animTimer = 0.0f;
      }

      // Advance frame — pick anim array based on current state
      int idx = type->animIndex[units[i].currentAnim];
      if (idx >= 0) {
        ModelAnimation *arr = GetAnimArray(type, units[i].currentAnim);
        if (arr) {
          int frameCount = arr[idx].frameCount;
          if (frameCount > 0) {
            float animFps = 30.0f;
            units[i].animTimer += dt;
            float frameDur = 1.0f / animFps;
            while (units[i].animTimer >= frameDur) {
              units[i].animTimer -= frameDur;
              units[i].animFrame = (units[i].animFrame + 1) % frameCount;
            }
          }
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
    double profLogicEnd = GetTime();
    profLogicTime = profLogicEnd - profFrameStart;
    double profRenderStart = profLogicEnd;
    BeginDrawing();
    ClearBackground((Color){45, 40, 35, 255});

    // Mark portraits dirty on phase change
    if (phase != prevPhase) {
      for (int pd = 0; pd < BLUE_TEAM_MAX_SIZE; pd++)
        portraitDirty[pd] = true;
    }

    // Collect active blue units for HUD (skip mushlings)
    int blueHudUnits[BLUE_TEAM_MAX_SIZE];
    int blueHudCount = 0;
    for (int i = 0; i < unitCount && blueHudCount < BLUE_TEAM_MAX_SIZE; i++) {
      if (units[i].active && units[i].team == TEAM_BLUE && !units[i].isMushling)
        blueHudUnits[blueHudCount++] = i;
    }

    // Render unit portraits into offscreen textures (only when dirty)
    for (int h = 0; h < blueHudCount; h++) {
      int ui = blueHudUnits[h];
      // Auto-detect dirtiness: type or rarity changed
      if (portraitTypeCache[h] != units[ui].typeIndex ||
          portraitRarityCache[h] != units[ui].rarity) {
        portraitDirty[h] = true;
        portraitTypeCache[h] = units[ui].typeIndex;
        portraitRarityCache[h] = units[ui].rarity;
      }
      if (!portraitDirty[h])
        continue;

      UnitType *type = &unitTypes[units[ui].typeIndex];
      if (!type->loaded)
        continue;

      // Auto-center camera on model
      BoundingBox bb = type->baseBounds;
      float centerY = (bb.min.y + bb.max.y) / 2.0f * type->scale;
      float extent = (bb.max.y - bb.min.y) * type->scale;
      portraitCam.target = (Vector3){0.0f, centerY, 0.0f};
      portraitCam.position = (Vector3){0.0f, centerY, extent * 2.5f};

      BeginTextureMode(portraits[h]);
      ClearBackground((Color){30, 30, 40, 255});
      BeginMode3D(portraitCam);
      if (type->hasAnimations && type->animIndex[ANIM_IDLE] >= 0)
        UpdateModelAnimation(type->model,
                             type->idleAnims[type->animIndex[ANIM_IDLE]], 0);
      Color portraitTint = (units[ui].typeIndex == 1) ? WHITE : GetTeamTint(TEAM_BLUE);
      DrawModel(type->model, (Vector3){0, 0, 0}, type->scale, portraitTint);
      EndMode3D();
      EndTextureMode();
      portraitDirty[h] = false;
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
        sceneRT.texture.id =
            rlLoadTexture(NULL, sceneRTWidth, sceneRTHeight,
                          RL_PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
        sceneRT.texture.width = sceneRTWidth;
        sceneRT.texture.height = sceneRTHeight;
        sceneRT.depth.id =
            rlLoadTextureDepth(sceneRTWidth, sceneRTHeight, false);
        sceneRT.depth.width = sceneRTWidth;
        sceneRT.depth.height = sceneRTHeight;
        rlFramebufferAttach(sceneRT.id, sceneRT.texture.id,
                            RL_ATTACHMENT_COLOR_CHANNEL0,
                            RL_ATTACHMENT_TEXTURE2D, 0);
        rlFramebufferAttach(sceneRT.id, sceneRT.depth.id, RL_ATTACHMENT_DEPTH,
                            RL_ATTACHMENT_TEXTURE2D, 0);

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
        if (!unitTypes[i].loaded)
          continue;
        for (int m = 0; m < unitTypes[i].model.materialCount; m++)
          unitTypes[i].model.materials[m].shader = shadowDepthShader;
      }
      for (int i = 0; i < TILE_VARIANTS; i++)
        for (int m = 0; m < tileModels[i].materialCount; m++)
          tileModels[i].materials[m].shader = shadowDepthShader;
      // Swap env model materials to shadow depth shader (covers ground, stairs,
      // circle, etc.) — foliage uses foliageShadowShader for instanced sway
      for (int ei = 0; ei < envModelCount; ei++) {
        if (!envModels[ei].loaded)
          continue;
        Shader shadowShd = (ei >= FOLIAGE_MODEL_FIRST && ei <= FOLIAGE_MODEL_LAST)
                               ? foliageShadowShader
                               : shadowDepthShader;
        for (int m = 0; m < envModels[ei].model.materialCount; m++)
          envModels[ei].model.materials[m].shader = shadowShd;
      }

      // Draw shadow-casting geometry
      {
        float gridOrigin = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
        for (int r = 0; r < TILE_GRID_SIZE; r++) {
          for (int c = 0; c < TILE_GRID_SIZE; c++) {
            int vi = tileVariantGrid[r][c];
            float cellX =
                gridOrigin + (c + 0.5f) * TILE_WORLD_SIZE + tileJitterX[r][c];
            float cellZ =
                gridOrigin + (r + 0.5f) * TILE_WORLD_SIZE + tileJitterZ[r][c];
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
                -tileCenters[vi].y * tileScale -
                    1.0f, // moved slightly deeper into floor (was -0.5f)
                cellZ - rzo,
            };
            DrawModelEx(tileModels[vi], pos, (Vector3){0.0f, 1.0f, 0.0f},
                        totalRot, (Vector3){tileScale, tileScale, tileScale},
                        WHITE);
          }
        }
      }
      // Draw env pieces (shadow pass — includes ground, stairs, circle, foliage)
      for (int ep = 0; ep < envPieceCount; ep++) {
        if (!envPieces[ep].active)
          continue;
        int mi = envPieces[ep].modelIndex;
        EnvModelDef *emd = &envModels[mi];
        if (!emd->loaded)
          continue;
        float es = envPieces[ep].scale;
        EnvPiece p = envPieces[ep];
        Vector3 pos = p.position;
        Matrix matS = MatrixScale(es, es, es);
        Matrix matTransform =
            MatrixMultiply(matS, MatrixRotateX(p.rotationX * DEG2RAD));
        matTransform =
            MatrixMultiply(matTransform, MatrixRotateY(p.rotationY * DEG2RAD));
        matTransform =
            MatrixMultiply(matTransform, MatrixRotateZ(p.rotationZ * DEG2RAD));
        matTransform =
            MatrixMultiply(matTransform, MatrixTranslate(pos.x, pos.y, pos.z));
        Matrix oldTransform = emd->model.transform;
        emd->model.transform = MatrixMultiply(oldTransform, matTransform);
        DrawModel(emd->model, (Vector3){0, 0, 0}, 1.0f, WHITE);
        emd->model.transform = oldTransform;
      }
      for (int i = 0; i < unitCount; i++) {
        if (!units[i].active)
          continue;
        UnitType *type = &unitTypes[units[i].typeIndex];
        if (!type->loaded)
          continue;
        // Update animation pose so shadow matches current frame
        if (type->hasAnimations) {
          int idx = type->animIndex[units[i].currentAnim];
          if (idx >= 0) {
            ModelAnimation *arr = GetAnimArray(type, units[i].currentAnim);
            if (arr)
              UpdateModelAnimation(type->model, arr[idx], units[i].animFrame);
          }
        }
        float s = type->scale * units[i].scaleOverride;
        Vector3 drawPos = units[i].position;
        drawPos.y += type->yOffset;
        DrawModelEx(type->model, drawPos, (Vector3){0, 1, 0},
                    units[i].facingAngle, (Vector3){s, s, s}, WHITE);
      }

      // Restore lighting shader on all materials
      for (int i = 0; i < unitTypeCount; i++) {
        if (!unitTypes[i].loaded)
          continue;
        for (int m = 0; m < unitTypes[i].model.materialCount; m++)
          unitTypes[i].model.materials[m].shader = lightShader;
      }
      for (int i = 0; i < TILE_VARIANTS; i++)
        for (int m = 0; m < tileModels[i].materialCount; m++)
          tileModels[i].materials[m].shader = lightShader;
      // Restore lighting shader on env model materials
      // (foliage models get foliageShader, others get lightShader)
      for (int ei = 0; ei < envModelCount; ei++) {
        if (!envModels[ei].loaded)
          continue;
        Shader restoreShd = (ei >= FOLIAGE_MODEL_FIRST && ei <= FOLIAGE_MODEL_LAST)
                                ? foliageShader
                                : lightShader;
        for (int m = 0; m < envModels[ei].model.materialCount; m++)
          envModels[ei].model.materials[m].shader = restoreShd;
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
    // Also bind shadow map + lightVP to foliage shader
    SetShaderValue(foliageShader, fShadowMapLoc, (int[]){2}, SHADER_UNIFORM_INT);
    SetShaderValueMatrix(foliageShader, fLightVPLoc, lightVP);

    // Render 3D scene into offscreen texture (for SSAO post-process)
    BeginTextureMode(sceneRT);
    ClearBackground((Color){45, 40, 35, 255});
    // significantly reduces z-fighting. remove if stuff is clipping near or far
    rlSetClipPlanes(0.05f, 500.0f);
    BeginMode3D(camera);
    // Draw tiled floor (bind normal map for tiles)
    rlActiveTextureSlot(3);
    rlEnableTexture(tileNormal.id);
    SetShaderValue(lightShader, normalMapLoc, (int[]){3}, SHADER_UNIFORM_INT);
    SetShaderValue(lightShader, useNormalMapLoc, (int[]){1},
                   SHADER_UNIFORM_INT);
    {
      float gridOrigin = -(TILE_GRID_SIZE * TILE_WORLD_SIZE) / 2.0f;
      int scrW = GetScreenWidth(), scrH = GetScreenHeight();
      for (int r = 0; r < TILE_GRID_SIZE; r++) {
        for (int c = 0; c < TILE_GRID_SIZE; c++) {
          int vi = tileVariantGrid[r][c];
          float cellX =
              gridOrigin + (c + 0.5f) * TILE_WORLD_SIZE + tileJitterX[r][c];
          float cellZ =
              gridOrigin + (r + 0.5f) * TILE_WORLD_SIZE + tileJitterZ[r][c];

          // Frustum cull: skip tiles whose center projects off-screen
          // Use generous margin (half tile size) to avoid popping
          {
            Vector2 tsp = GetWorldToScreen((Vector3){cellX, 0, cellZ}, camera);
            float margin = TILE_WORLD_SIZE *
                           8.0f; // generous margin for tile extent + wobble
            if (tsp.x < -margin || tsp.x > scrW + margin || tsp.y < -margin ||
                tsp.y > scrH + margin)
              continue;
          }

          float totalRot = tileRotationGrid[r][c] + tileJitterAngle[r][c];
          // DrawModelEx applies scale→rotate→translate, so the OBJ-space
          // center offset gets rotated. Rotate it by the same angle to
          // compensate.
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
            wobbleY =
                envelope * fabsf(osc) * (TILE_WOBBLE_BOUNCE / TILE_WOBBLE_MAX);
            // Kill wobble when envelope is negligible
            if (envelope < 0.05f)
              tileWobble[r][c] = 0.0f;
          }

          Vector3 pos = {
              cellX - rxo,
              wobbleY - tileCenters[vi].y * tileScale -
                  1.0f, // moved slightly deeper into floor (was -0.5f)
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
          DrawModelEx(tileModels[vi], pos, (Vector3){0.0f, 1.0f, 0.0f},
                      totalRot, (Vector3){tileScale, tileScale, tileScale},
                      WHITE);
          if (wobbleTiltX != 0.0f || wobbleTiltZ != 0.0f) {
            rlPopMatrix();
          }
        }
      }
    }
    SetShaderValue(lightShader, useNormalMapLoc, (int[]){0},
                   SHADER_UNIFORM_INT);

    // Draw env pieces (main render pass — includes ground, stairs, circle, foliage)
    for (int ep = 0; ep < envPieceCount; ep++) {
      if (!envPieces[ep].active)
        continue;
      int mi = envPieces[ep].modelIndex;
      EnvModelDef *emd = &envModels[mi];
      if (!emd->loaded)
        continue;
      float es = envPieces[ep].scale;
      EnvPiece p = envPieces[ep];
      Vector3 pos = p.position;
      Matrix matS = MatrixScale(es, es, es);
      Matrix matTransform =
          MatrixMultiply(matS, MatrixRotateX(p.rotationX * DEG2RAD));
      matTransform =
          MatrixMultiply(matTransform, MatrixRotateY(p.rotationY * DEG2RAD));
      matTransform =
          MatrixMultiply(matTransform, MatrixRotateZ(p.rotationZ * DEG2RAD));
      matTransform =
          MatrixMultiply(matTransform, MatrixTranslate(pos.x, pos.y, pos.z));
      Color eTint = WHITE;
      if (debugMode && ep == envSelectedPiece)
        eTint = (Color){150, 255, 150, 255};
      // Bind normal map for the appropriate shader
      bool isFoliage = (mi >= FOLIAGE_MODEL_FIRST && mi <= FOLIAGE_MODEL_LAST);
      if (emd->normalTexture.id > 0) {
        rlActiveTextureSlot(3);
        rlEnableTexture(emd->normalTexture.id);
        if (isFoliage) {
          SetShaderValue(foliageShader, fNormalMapLoc, (int[]){3},
                         SHADER_UNIFORM_INT);
          SetShaderValue(foliageShader, fUseNormalMapLoc, (int[]){1},
                         SHADER_UNIFORM_INT);
        } else {
          SetShaderValue(lightShader, normalMapLoc, (int[]){3},
                         SHADER_UNIFORM_INT);
          SetShaderValue(lightShader, useNormalMapLoc, (int[]){1},
                         SHADER_UNIFORM_INT);
        }
      } else {
        if (isFoliage) {
          SetShaderValue(foliageShader, fUseNormalMapLoc, (int[]){0},
                         SHADER_UNIFORM_INT);
        } else {
          SetShaderValue(lightShader, useNormalMapLoc, (int[]){0},
                         SHADER_UNIFORM_INT);
        }
      }
      Matrix oldTransform = emd->model.transform;
      emd->model.transform = MatrixMultiply(oldTransform, matTransform);
      DrawModel(emd->model, (Vector3){0, 0, 0}, 1.0f, eTint);
      emd->model.transform = oldTransform;
    }
    SetShaderValue(foliageShader, fUseNormalMapLoc, (int[]){0},
                   SHADER_UNIFORM_INT);
    // Reset normal map after env pieces so other models don't use it
    SetShaderValue(lightShader, useNormalMapLoc, (int[]){0},
                   SHADER_UNIFORM_INT);

    // Draw units
    for (int i = 0; i < unitCount; i++) {
      if (!units[i].active)
        continue;
      if (IsUnitInStatueSpawn(&statueSpawn, i))
        continue; // drawn separately as falling statue
      if (intro.active && intro.unitIndex == i)
        continue; // hidden during intro splash
      UnitType *type = &unitTypes[units[i].typeIndex];
      if (!type->loaded)
        continue;
      Color tint = (units[i].typeIndex == 1) ? WHITE : GetTeamTint(units[i].team);
      if (units[i].hitFlash > 0) {
        float f = units[i].hitFlash / HIT_FLASH_DURATION;
        if (f > 1.0f)
          f = 1.0f;
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
          if (arr)
            UpdateModelAnimation(type->model, arr[idx], units[i].animFrame);
        }
      }
      float s = type->scale * units[i].scaleOverride;
      Vector3 drawPos = units[i].position;
      drawPos.y += type->yOffset;
      DrawModelEx(type->model, drawPos, (Vector3){0, 1, 0},
                  units[i].facingAngle, (Vector3){s, s, s}, tint);

      if (units[i].selected) {
        BoundingBox sb = GetUnitBounds(&units[i], type);
        DrawBoundingBox(sb, GREEN);
      }
    }

    // Draw falling statue (spawning unit rendered separately with stone tint at
    // elevated Y + drift)
    if (statueSpawn.phase == SSPAWN_FALLING) {
      int si = statueSpawn.unitIndex;
      if (si >= 0 && si < unitCount && units[si].active) {
        UnitType *stype = &unitTypes[units[si].typeIndex];
        if (stype->loaded) {
          // Force idle frame 0 pose (frozen statue)
          if (stype->hasAnimations && stype->animIndex[ANIM_IDLE] >= 0)
            UpdateModelAnimation(
                stype->model, stype->idleAnims[stype->animIndex[ANIM_IDLE]], 0);
          float ss = stype->scale * units[si].scaleOverride;
          // Compute drift offset based on height fraction
          float hRange = SPAWN_ANIM_START_Y - statueSpawn.targetY;
          float dFrac =
              (hRange > 0.0f)
                  ? (statueSpawn.currentY - statueSpawn.targetY) / hRange
                  : 0.0f;
          if (dFrac < 0.0f)
            dFrac = 0.0f;
          if (dFrac > 1.0f)
            dFrac = 1.0f;
          Vector3 statuePos = {
              units[si].position.x + statueSpawn.driftX * dFrac,
              statueSpawn.currentY,
              units[si].position.z + statueSpawn.driftZ * dFrac};
          Color stoneTint = {160, 160, 170, 255}; // grayish stone tint
          DrawModelEx(stype->model, statuePos, (Vector3){0, 1, 0},
                      units[si].facingAngle, (Vector3){ss, ss, ss}, stoneTint);
        }
      }
    }

    // Draw modifier timer rings (duration-aware arcs, stacked outward per
    // modifier type)
    {
      // Fixed ordering for ring stacking
      const ModifierType ringOrder[] = {
          MOD_STUN,         MOD_SPELL_PROTECT, MOD_CRAGGY_ARMOR, MOD_STONE_GAZE,
          MOD_INVULNERABLE, MOD_LIFESTEAL,     MOD_ARMOR,        MOD_DIG_HEAL,
          MOD_SPEED_MULT,   MOD_SHIELD,        MOD_MAELSTROM,    MOD_VLAD_AURA,
          MOD_CHARGING,     MOD_POISON,        MOD_FERVOR,
      };
      const Color ringColors[] = {
          {255, 255, 0, 255},   // STUN - yellow
          {200, 240, 255, 255}, // SPELL_PROTECT - cyan
          {140, 140, 160, 255}, // CRAGGY_ARMOR - gray
          {160, 80, 200, 255},  // STONE_GAZE - purple
          {135, 206, 235, 255}, // INVULNERABLE - skyblue
          {230, 40, 40, 255},   // LIFESTEAL - red
          {130, 130, 130, 255}, // ARMOR - gray
          {139, 90, 43, 255},   // DIG_HEAL - brown
          {0, 228, 48, 255},    // SPEED_MULT - green
          {80, 160, 255, 255},  // SHIELD - blue
          {255, 230, 50, 255},  // MAELSTROM - yellow lightning
          {180, 30, 30, 255},   // VLAD_AURA - dark red
          {255, 140, 0, 255},   // CHARGING - orange
          {40, 200, 40, 255},   // POISON - green
          {255, 140, 40, 255},  // FERVOR - orange
      };
      const int ringOrderCount = sizeof(ringOrder) / sizeof(ringOrder[0]);

      for (int i = 0; i < unitCount; i++) {
        if (!units[i].active)
          continue;
        Vector3 ringPos = {units[i].position.x, units[i].position.y + 0.3f,
                           units[i].position.z};
        int ringIdx = 0;
        for (int r = 0; r < ringOrderCount; r++) {
          // Find this modifier on unit i
          Modifier *found = NULL;
          for (int m = 0; m < MAX_MODIFIERS; m++) {
            if (modifiers[m].active && modifiers[m].unitIndex == i &&
                modifiers[m].type == ringOrder[r]) {
              found = &modifiers[m];
              break;
            }
          }
          if (!found)
            continue;
          float radius = 3.5f + ringIdx * 1.5f;
          float frac = (found->maxDuration > 0.0f)
                           ? found->duration / found->maxDuration
                           : 0.0f;
          if (frac < 0.0f)
            frac = 0.0f;
          if (frac > 1.0f)
            frac = 1.0f;
          Color bright = ringColors[r];
          Color dim = {(unsigned char)(bright.r / 4),
                       (unsigned char)(bright.g / 4),
                       (unsigned char)(bright.b / 4), 100};
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
      if (!projectiles[p].active)
        continue;
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
      Vector3 camFwd =
          Vector3Normalize(Vector3Subtract(camera.target, camera.position));
      Vector3 camRight =
          Vector3Normalize(Vector3CrossProduct(camFwd, camera.up));
      Vector3 camUp = Vector3CrossProduct(camRight, camFwd);

      rlDisableDepthMask();
      rlDrawRenderBatchActive();
      rlSetBlendFactors(RL_SRC_ALPHA, RL_ONE, RL_FUNC_ADD); // additive blending
      rlSetBlendMode(BLEND_CUSTOM);
      rlSetTexture(particleTex.id);
      rlBegin(RL_QUADS);
      for (int p = 0; p < MAX_PARTICLES; p++) {
        if (!particles[p].active)
          continue;
        float sz = particles[p].size;
        Vector3 pos = particles[p].position;
        Color c = particles[p].color;

        // 4 corners: pos ± right*sz ± up*sz
        Vector3 r = {camRight.x * sz, camRight.y * sz, camRight.z * sz};
        Vector3 u = {camUp.x * sz, camUp.y * sz, camUp.z * sz};

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
      rlSetBlendMode(BLEND_ALPHA); // restore normal blending
      rlEnableDepthMask();
    }

    // Draw fissures (gray cubes along the line)
    for (int f = 0; f < MAX_FISSURES; f++) {
      if (!fissures[f].active)
        continue;
      float rot = fissures[f].rotation * (PI / 180.0f);
      float dirX = sinf(rot), dirZ = cosf(rot);
      int numSegments = (int)(fissures[f].length / 7.0f);
      if (numSegments < 1)
        numSegments = 1;
      float segLen = fissures[f].length / numSegments;
      float startOffset = -fissures[f].length * 0.5f;
      for (int s = 0; s < numSegments; s++) {
        float t = startOffset + segLen * (s + 0.5f);
        Vector3 segPos = {
            fissures[f].position.x + dirX * t,
            fissures[f].position.y + 2.5f,
            fissures[f].position.z + dirZ * t,
        };
        DrawCube(segPos, fissures[f].width, 5.0f, segLen * 0.95f,
                 (Color){100, 95, 85, 255});
        DrawCubeWires(segPos, fissures[f].width, 5.0f, segLen * 0.95f,
                      (Color){70, 65, 55, 255});
      }
    }

    // (modifier rings now drawn above via DrawArc3D)

    // Arena boundary wall (fades in as blue unit is dragged near it)
    if (phase == PHASE_PREP) {
      float closestDragZ = 999.0f;
      for (int i = 0; i < unitCount; i++) {
        if (units[i].active && units[i].dragging &&
            units[i].team == TEAM_BLUE) {
          if (units[i].position.z < closestDragZ)
            closestDragZ = units[i].position.z;
        }
      }
      if (closestDragZ < 999.0f) {
        float fadeRange = 40.0f;
        float dz = closestDragZ - ARENA_BOUNDARY_Z;
        float proximity = 1.0f - fminf(fmaxf(dz / fadeRange, 0.0f), 1.0f);
        if (proximity > 0.01f) {
          float currentTime = (float)GetTime();
          SetShaderValue(borderShader, borderTimeLoc, &currentTime,
                         SHADER_UNIFORM_FLOAT);
          SetShaderValue(borderShader, borderProximityLoc, &proximity,
                         SHADER_UNIFORM_FLOAT);
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
                       plazaHoverObject == 2, plazaHoverObject == 1,
                       plazaSparkleTimer);
    }

    // (range circle moved to after post-processing — see below)
    EndMode3D();
    EndTextureMode();

    // Render offscreen textures before fxaaRT to avoid nested render targets
    // (raylib's EndTextureMode always restores to FBO 0, breaking nesting)

    // Game-over portraits
    if (phase == PHASE_GAME_OVER && !isMultiplayer && !deathPenalty) {
      int noShadowOn = 1, noShadowOff = 0;
      SetShaderValue(lightShader, noShadowLoc, &noShadowOn, SHADER_UNIFORM_INT);
      int goBlueRT[BLUE_TEAM_MAX_SIZE];
      int goCountRT = 0;
      for (int i = 0; i < unitCount && goCountRT < BLUE_TEAM_MAX_SIZE; i++)
        if (units[i].active && units[i].team == TEAM_BLUE)
          goBlueRT[goCountRT++] = i;
      for (int h = 0; h < goCountRT; h++) {
        int ui = goBlueRT[h];
        UnitType *type = &unitTypes[units[ui].typeIndex];
        if (!type->loaded)
          continue;
        BoundingBox bb = type->baseBounds;
        float centerY = (bb.min.y + bb.max.y) / 2.0f * type->scale;
        float extent = (bb.max.y - bb.min.y) * type->scale;
        portraitCam.target = (Vector3){0.0f, centerY, 0.0f};
        portraitCam.position = (Vector3){0.0f, centerY, extent * 2.5f};
        BeginTextureMode(portraits[h]);
        ClearBackground((Color){30, 30, 40, 255});
        BeginMode3D(portraitCam);
        if (type->hasAnimations && type->animIndex[ANIM_IDLE] >= 0)
          UpdateModelAnimation(type->model,
                               type->idleAnims[type->animIndex[ANIM_IDLE]], 0);
        DrawModel(type->model, (Vector3){0, 0, 0}, type->scale,
                  GetTeamTint(TEAM_BLUE));
        EndMode3D();
        EndTextureMode();
      }
      SetShaderValue(lightShader, noShadowLoc, &noShadowOff,
                     SHADER_UNIFORM_INT);
    }

    // Intro model
    if (intro.active) {
      UnitType *itype = &unitTypes[intro.typeIndex];
      if (itype->loaded) {
        BoundingBox ib = itype->baseBounds;
        float icenterY = (ib.min.y + ib.max.y) / 2.0f * itype->scale;
        float iextent = (ib.max.y - ib.min.y) * itype->scale;

        Camera introCam = {0};
        introCam.up = (Vector3){0.0f, 1.0f, 0.0f};
        introCam.fovy = 30.0f;
        introCam.projection = CAMERA_PERSPECTIVE;
        introCam.target = (Vector3){0.0f, icenterY, 0.0f};
        introCam.position = (Vector3){0.0f, icenterY, iextent * 2.0f};

        int noShadowOn = 1, noShadowOff = 0;
        SetShaderValue(lightShader, noShadowLoc, &noShadowOn,
                       SHADER_UNIFORM_INT);
        BeginTextureMode(introModelRT);
        ClearBackground(BLANK);
        BeginMode3D(introCam);
        if (itype->hasAnimations && itype->animIndex[ANIM_IDLE] >= 0)
          UpdateModelAnimation(itype->model,
                               itype->idleAnims[itype->animIndex[ANIM_IDLE]],
                               intro.animFrame);
        Color introTint = (intro.typeIndex == 1) ? WHITE : GetTeamTint(TEAM_BLUE);
        DrawModel(itype->model, (Vector3){0, 0, 0}, itype->scale, introTint);
        EndMode3D();
        EndTextureMode();
        SetShaderValue(lightShader, noShadowLoc, &noShadowOff,
                       SHADER_UNIFORM_INT);
      }
    }

    // Composite scene + post-process into FXAA RT (avoid nesting render
    // targets)
    BeginTextureMode(fxaaRT);
    ClearBackground((Color){45, 40, 35, 255});

    // Draw scene with SSAO post-process
    {
      float res[2] = {(float)sceneRTWidth, (float)sceneRTHeight};
      float nearPlane = 0.1f, farPlane = 1000.0f;
      SetShaderValue(ssaoShader, ssaoResLoc, res, SHADER_UNIFORM_VEC2);
      SetShaderValue(ssaoShader, ssaoNearLoc, &nearPlane, SHADER_UNIFORM_FLOAT);
      SetShaderValue(ssaoShader, ssaoFarLoc, &farPlane, SHADER_UNIFORM_FLOAT);
      // Bind depth texture to texture unit 1
      rlActiveTextureSlot(1);
      rlEnableTexture(sceneRT.depth.id);
      SetShaderValue(ssaoShader, ssaoDepthLoc, (int[]){1}, SHADER_UNIFORM_INT);
      BeginShaderMode(ssaoShader);
      DrawTextureRec(
          sceneRT.texture,
          (Rectangle){0, 0, (float)sceneRTWidth, -(float)sceneRTHeight},
          (Vector2){0, 0}, WHITE);
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
      float fxaaRes[2] = {(float)fxaaRTWidth, (float)fxaaRTHeight};
      SetShaderValue(fxaaShader, fxaaResLoc, fxaaRes, SHADER_UNIFORM_VEC2);
      BeginShaderMode(fxaaShader);
      DrawTextureRec(
          fxaaRT.texture,
          (Rectangle){0, 0, (float)fxaaRTWidth, -(float)fxaaRTHeight},
          (Vector2){0, 0}, WHITE);
      EndShaderMode();
    }
    EndTextureMode();

    // Color grading pass → screen
    {
      SetShaderValue(colorGradeShader, cgExposureLoc, &cgExposure,
                     SHADER_UNIFORM_FLOAT);
      SetShaderValue(colorGradeShader, cgContrastLoc, &cgContrast,
                     SHADER_UNIFORM_FLOAT);
      SetShaderValue(colorGradeShader, cgSaturationLoc, &cgSaturation,
                     SHADER_UNIFORM_FLOAT);
      SetShaderValue(colorGradeShader, cgTemperatureLoc, &cgTemperature,
                     SHADER_UNIFORM_FLOAT);
      SetShaderValue(colorGradeShader, cgVigStrLoc, &cgVignetteStr,
                     SHADER_UNIFORM_FLOAT);
      SetShaderValue(colorGradeShader, cgVigSoftLoc, &cgVignetteSoft,
                     SHADER_UNIFORM_FLOAT);
      SetShaderValue(colorGradeShader, cgLiftLoc, cgLift, SHADER_UNIFORM_VEC3);
      SetShaderValue(colorGradeShader, cgGainLoc, cgGain, SHADER_UNIFORM_VEC3);
      BeginShaderMode(colorGradeShader);
      DrawTextureRec(
          colorGradeRT.texture,
          (Rectangle){0, 0, (float)fxaaRTWidth, -(float)fxaaRTHeight},
          (Vector2){0, 0}, WHITE);
      EndShaderMode();
    }

    // Draw ability range/radius circle on hover (prev frame's values, 1-frame
    // delay)
    if (prevHoverAbilityUnitIndex >= 0 &&
        prevHoverAbilityUnitIndex < unitCount &&
        units[prevHoverAbilityUnitIndex].active && prevHoverAbilityId >= 0 &&
        prevHoverAbilityId < ABILITY_COUNT) {
      const AbilityDef *def = &ABILITY_DEFS[prevHoverAbilityId];
      float displayRadius = 0.0f;
      // Determine which radius to show based on ability type
      if (def->isPassive) {
        // Passives: no indicator
      } else if (def->targetType == TARGET_CLOSEST_ENEMY) {
        // Targeted abilities: show cast range
        displayRadius = def->range[prevHoverAbilityLevel];
      } else if (def->targetType == TARGET_SELF_AOE) {
        // Self AoE: show actual effect radius from values
        switch (prevHoverAbilityId) {
        case ABILITY_EARTHQUAKE:
          displayRadius = def->values[prevHoverAbilityLevel][AV_EQ_RADIUS];
          break;
        case ABILITY_VACUUM:
          displayRadius = def->values[prevHoverAbilityLevel][AV_VAC_RADIUS];
          break;
        default:
          displayRadius = def->range[prevHoverAbilityLevel];
          break;
        }
      } else {
        // Other active abilities: check for special radius values
        switch (prevHoverAbilityId) {
        case ABILITY_HOOK:
          displayRadius = def->values[prevHoverAbilityLevel][AV_HK_RANGE];
          break;
        case ABILITY_PRIMAL_CHARGE:
          break; // no range limit (targets furthest enemy)
        default:
          break; // self-buff / no spatial component
        }
      }
      if (displayRadius > 0.0f) {
        BeginMode3D(camera);
        Vector3 ringPos = units[prevHoverAbilityUnitIndex].position;
        ringPos.y += 0.2f;
        Color ringCol = def->color;
        ringCol.a = 120;
        DrawArc3D(ringPos, displayRadius - 0.15f, 1.0f, ringCol);
        DrawArc3D(ringPos, displayRadius, 1.0f, ringCol);
        DrawArc3D(ringPos, displayRadius + 0.15f, 1.0f, ringCol);
        EndMode3D();
      }
    }

    // 2D overlay: labels + health bars (drawn directly to screen, no FXAA)
    for (int i = 0; i < unitCount; i++) {
      if (!units[i].active)
        continue;
      if (intro.active && intro.unitIndex == i)
        continue; // hidden during intro splash
      if (IsUnitInStatueSpawn(&statueSpawn, i) &&
          statueSpawn.phase == SSPAWN_DELAY)
        continue; // hidden during pre-fall delay
      UnitType *type = &unitTypes[units[i].typeIndex];
      if (!type->loaded)
        continue;
      const UnitStats *stats = &UNIT_STATS[units[i].typeIndex];

      // Use statue spawn position (with drift) for falling units
      Vector3 labelWorldPos = units[i].position;
      if (IsUnitInStatueSpawn(&statueSpawn, i) &&
          statueSpawn.phase == SSPAWN_FALLING) {
        float hRange = SPAWN_ANIM_START_Y - statueSpawn.targetY;
        float dFrac =
            (hRange > 0.0f)
                ? (statueSpawn.currentY - statueSpawn.targetY) / hRange
                : 0.0f;
        if (dFrac < 0.0f)
          dFrac = 0.0f;
        if (dFrac > 1.0f)
          dFrac = 1.0f;
        labelWorldPos.x += statueSpawn.driftX * dFrac;
        labelWorldPos.y = statueSpawn.currentY;
        labelWorldPos.z += statueSpawn.driftZ * dFrac;
      }
      Vector2 sp = GetWorldToScreen(
          (Vector3){labelWorldPos.x,
                    labelWorldPos.y + (type->baseBounds.max.y * type->scale) +
                        1.0f,
                    labelWorldPos.z},
          camera);

      // Skip offscreen units
      if (sp.x < -50 || sp.x > GetScreenWidth() + 50 || sp.y < -50 ||
          sp.y > GetScreenHeight() + 50)
        continue;

      if (units[i].rarity > 0) {
        const char *stars = (units[i].rarity == RARITY_LEGENDARY) ? "* * *" : "* *";
        int starsW = GameMeasureText(stars, S(14));
        Color starColor = (units[i].rarity == RARITY_LEGENDARY)
                              ? (Color){255, 60, 60, 255}
                              : (Color){180, 100, 255, 255};
        GameDrawText(stars, (int)sp.x - starsW / 2, (int)sp.y - S(26), S(14),
                     starColor);
      }

      const char *label = units[i].isMushling ? "Mushling" : type->name;
      int nameFontSize = S(16);
      int tw = GameMeasureText(label, nameFontSize);
      // Dark background for readability
      int bgPad = S(2);
      DrawRectangle((int)sp.x - tw / 2 - bgPad, (int)sp.y - S(14) - bgPad,
                    tw + bgPad * 2, nameFontSize + bgPad * 2,
                    (Color){0, 0, 0, 120});
      GameDrawText(label, (int)sp.x - tw / 2, (int)sp.y - S(14), nameFontSize,
                   (units[i].team == TEAM_BLUE) ? WHITE
                                                : (Color){255, 200, 200, 255});

      // Health bar (hide for enemies in plaza/lobby)
      int bw = S(52), bh = S(7);
      int bx = (int)sp.x - bw / 2, by = (int)sp.y + 4;
      if (!((phase == PHASE_PLAZA || phase == PHASE_LOBBY) &&
            units[i].team == TEAM_RED)) {
        float maxHP = stats->health * units[i].hpMultiplier;
        float hpRatio = units[i].currentHealth / maxHP;
        if (hpRatio < 0)
          hpRatio = 0;
        if (hpRatio > 1)
          hpRatio = 1;
        // Background with subtle rounded feel
        DrawRectangle(bx, by, bw, bh, (Color){20, 20, 25, 200});
        // HP fill with gradient: brighter at top, darker at bottom
        int fillW = (int)(bw * hpRatio);
        Color hpC = (units[i].team == TEAM_BLUE) ? (Color){70, 190, 70, 255}
                                                 : (Color){200, 55, 55, 255};
        Color hpDark = (units[i].team == TEAM_BLUE) ? (Color){40, 130, 40, 255}
                                                    : (Color){140, 30, 30, 255};
        if (fillW > 0) {
          DrawRectangle(bx, by, fillW, bh / 2, hpC);
          DrawRectangle(bx, by + bh / 2, fillW, bh - bh / 2, hpDark);
        }
        // Low HP warning pulse
        if (hpRatio < 0.3f && hpRatio > 0.0f) {
          float pulse =
              0.3f + 0.7f * (0.5f + 0.5f * sinf((float)GetTime() * 8.0f));
          DrawRectangle(bx, by, fillW, bh,
                        (Color){255, 255, 255, (unsigned char)(40.0f * pulse)});
        }
        // Shield bar (blue) extending rightward from HP
        if (units[i].shieldHP > 0) {
          float shieldRatio = units[i].shieldHP / maxHP;
          if (shieldRatio > 1)
            shieldRatio = 1;
          int shieldW = (int)(bw * shieldRatio);
          int shieldX = bx + fillW;
          if (shieldX + shieldW > bx + bw)
            shieldW = bx + bw - shieldX;
          DrawRectangle(shieldX, by, shieldW, bh, (Color){80, 160, 255, 200});
        }
        // Poison bar (green overlay) showing pending poison damage
        {
          float poisonDPS = GetModifierValue(modifiers, i, MOD_POISON);
          if (poisonDPS > 0) {
            float poisonPreview = poisonDPS * 2.0f; // 2s of damage
            float poisonRatio = poisonPreview / maxHP;
            if (poisonRatio > hpRatio) poisonRatio = hpRatio;
            int poisonW = (int)(bw * poisonRatio);
            int poisonX = bx + fillW - poisonW;
            if (poisonW > 0)
              DrawRectangle(poisonX, by, poisonW, bh, (Color){40, 180, 40, 120});
          }
        }
        // HP separator notches — only show when bar is wide enough to look good
        {
          float notchHP = (maxHP > 100.0f) ? 50.0f : 25.0f;
          int notches = (int)(maxHP / notchHP);
          if (notches > 1 && notches <= 8) {
            for (int n = 1; n < notches; n++) {
              int nx = bx + (int)(bw * (n * notchHP / maxHP));
              DrawRectangle(nx, by + 1, 1, bh - 2, (Color){0, 0, 0, 100});
            }
          }
        }
        // Thin border
        DrawRectangleLinesEx(
            (Rectangle){(float)bx, (float)by, (float)bw, (float)bh}, 1,
            (Color){0, 0, 0, 160});
        // Bright edge highlight on top
        DrawRectangle(bx + 1, by, bw - 2, 1, (Color){255, 255, 255, 30});

        const char *hpT =
            TextFormat("%.0f/%.0f", units[i].currentHealth, maxHP);
        int htw = GameMeasureText(hpT, S(10));
        GameDrawText(hpT, (int)sp.x - htw / 2 + 1, by + bh + 2 + 1, S(10),
                     (Color){0, 0, 0, 180});
        GameDrawText(hpT, (int)sp.x - htw / 2, by + bh + 2, S(10), WHITE);
      }

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
        float eAlphaFrac =
            1.0f - (eMouseDist - eFadeNear) / (eFadeFar - eFadeNear);
        if (eAlphaFrac < 0.25f)
          eAlphaFrac = 0.25f;
        if (eAlphaFrac > 1.0f)
          eAlphaFrac = 1.0f;
        unsigned char eAlpha = (unsigned char)(eAlphaFrac * 255);
        unsigned char eAlphaLow = (unsigned char)(eAlphaFrac * 200);
        // Background panel
        DrawRectangle(egx - 3, egy - 3, eGridW + 6, eGridH + 6,
                      (Color){20, 20, 30, eAlphaLow});
        DrawRectangleLinesEx((Rectangle){(float)(egx - 3), (float)(egy - 3),
                                         (float)(eGridW + 6),
                                         (float)(eGridH + 6)},
                             1, (Color){80, 60, 60, eAlphaLow});
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
            bool eHovered = CheckCollisionPointRec(
                mpos, (Rectangle){(float)eax, (float)eay, (float)eSlotSz,
                                  (float)eSlotSz});
            if (eHovered) {
              hoverAbilityId = eslot->abilityId;
              hoverAbilityLevel = eslot->level;
            }
            const char *eabbr = ABILITY_DEFS[eslot->abilityId].abbrev;
            int eaw = GameMeasureText(eabbr, S(10));
            Color etxtCol = {255, 255, 255, eAlpha};
            GameDrawText(eabbr, eax + (eSlotSz - eaw) / 2,
                         eay + (eSlotSz - S(10)) / 2, S(10), etxtCol);
            const char *elvl = TextFormat("L%d", eslot->level + 1);
            GameDrawText(elvl, eax + 2, eay + eSlotSz - S(8), S(8), etxtCol);
          } else {
            DrawRectangle(eax, eay, eSlotSz, eSlotSz,
                          (Color){40, 40, 55, eAlphaLow});
          }
          DrawRectangleLines(eax, eay, eSlotSz, eSlotSz,
                             (Color){80, 80, 100, eAlphaLow});
        }
      }

      // Modifier labels (deduplicated — only one per type due to AddModifier
      // dedup) Duration-colored text: active portion in modColor, expired
      // portion in dim gray
      int modY = by + bh + 14;
      for (int m = 0; m < MAX_MODIFIERS; m++) {
        if (!modifiers[m].active || modifiers[m].unitIndex != i)
          continue;
        const char *modLabel = NULL;
        Color modColor = WHITE;
        switch (modifiers[m].type) {
        case MOD_STUN:
          modLabel = "STUNNED";
          modColor = YELLOW;
          break;
        case MOD_INVULNERABLE:
          modLabel = "INVULN";
          modColor = SKYBLUE;
          break;
        case MOD_LIFESTEAL:
          modLabel = "LIFESTEAL";
          modColor = RED;
          break;
        case MOD_SPEED_MULT:
          modLabel = "SPEED";
          modColor = GREEN;
          break;
        case MOD_ARMOR:
          modLabel = "ARMOR";
          modColor = GRAY;
          break;
        case MOD_DIG_HEAL:
          modLabel = "DIGGING";
          modColor = BROWN;
          break;
        case MOD_SPELL_PROTECT:
          modLabel = "SPELL SHIELD";
          modColor = (Color){200, 240, 255, 255};
          break;
        case MOD_CRAGGY_ARMOR:
          modLabel = "CRAGGY";
          modColor = (Color){140, 140, 160, 255};
          break;
        case MOD_STONE_GAZE:
          modLabel = "STONE GAZE";
          modColor = (Color){160, 80, 200, 255};
          break;
        case MOD_SHIELD:
          modLabel = "SHIELD";
          modColor = (Color){80, 160, 255, 255};
          break;
        case MOD_MAELSTROM:
          modLabel = "MAELSTROM";
          modColor = (Color){255, 230, 50, 255};
          break;
        case MOD_VLAD_AURA:
          modLabel = "VLAD AURA";
          modColor = (Color){180, 30, 30, 255};
          break;
        case MOD_CHARGING:
          modLabel = "CHARGING";
          modColor = (Color){255, 140, 0, 255};
          break;
        case MOD_MULTICAST:
          modLabel = "MULTICAST";
          modColor = (Color){255, 180, 60, 255};
          break;
        case MOD_SHARE_PAIN:
          modLabel = "SHARE PAIN";
          modColor = (Color){180, 60, 180, 255};
          break;
        case MOD_COOLDOWN_REDUCTION:
          modLabel = "CDR";
          modColor = (Color){180, 120, 255, 255};
          break;
        case MOD_POISON:
          modLabel = "POISON";
          modColor = (Color){40, 200, 40, 255};
          break;
        case MOD_FERVOR: {
          int fvStacks = (int)modifiers[m].value;
          modLabel = TextFormat("FERVOR x%d", fvStacks);
          modColor = (Color){255, 140, 40, 255};
        } break;
        }
        if (modLabel) {
          int totalLen = (int)strlen(modLabel);
          int mlw = GameMeasureText(modLabel, S(11));
          int startX = (int)sp.x - mlw / 2;
          float frac = (modifiers[m].maxDuration > 0.0f)
                           ? modifiers[m].duration / modifiers[m].maxDuration
                           : 0.0f;
          if (frac < 0.0f)
            frac = 0.0f;
          if (frac > 1.0f)
            frac = 1.0f;
          int activeChars = (int)(frac * totalLen + 0.5f);
          Color dimGray = {100, 100, 120, 255};
          if (activeChars >= totalLen) {
            // Fully active — single draw call
            GameDrawText(modLabel, startX, modY, S(11), modColor);
          } else if (activeChars <= 0) {
            // Fully dim — single draw call
            GameDrawText(modLabel, startX, modY, S(11), dimGray);
          } else {
            // Two-pass: active portion then dim portion
            char activeBuf[32];
            int len = activeChars < (int)sizeof(activeBuf) - 1
                          ? activeChars
                          : (int)sizeof(activeBuf) - 1;
            memcpy(activeBuf, modLabel, len);
            activeBuf[len] = '\0';
            GameDrawText(activeBuf, startX, modY, S(11), modColor);
            int activeW = GameMeasureText(activeBuf, S(11));
            char dimBuf[32];
            int dimLen = totalLen - activeChars;
            if (dimLen > (int)sizeof(dimBuf) - 1)
              dimLen = (int)sizeof(dimBuf) - 1;
            memcpy(dimBuf, modLabel + activeChars, dimLen);
            dimBuf[dimLen] = '\0';
            GameDrawText(dimBuf, startX + activeW, modY, S(11), dimGray);
          }
          modY += S(13);
        }
      }
    }

    // 2D overlay: Stone Gaze progress bars
    for (int i = 0; i < unitCount; i++) {
      if (!units[i].active || units[i].gazeAccum <= 0)
        continue;
      // Find the gaze threshold from the active Stone Gaze buff on an enemy
      float gazeThresh = 2.0f; // default
      for (int g = 0; g < unitCount; g++) {
        if (!units[g].active || units[g].team == units[i].team)
          continue;
        if (!UnitHasModifier(modifiers, g, MOD_STONE_GAZE))
          continue;
        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
          if (units[g].abilities[a].abilityId == ABILITY_STONE_GAZE) {
            gazeThresh =
                ABILITY_DEFS[ABILITY_STONE_GAZE]
                    .values[units[g].abilities[a].level][AV_SG_GAZE_THRESH];
            break;
          }
        }
        break;
      }
      Vector2 gsp = GetWorldToScreen(units[i].position, camera);
      float progress = units[i].gazeAccum / gazeThresh;
      if (progress > 1.0f)
        progress = 1.0f;
      int barW = 30, barH = 4;
      int gx = (int)gsp.x - barW / 2;
      int gy = (int)gsp.y - 30;
      DrawRectangle(gx, gy, barW, barH, (Color){40, 20, 60, 180});
      DrawRectangle(gx, gy, (int)(barW * progress), barH,
                    (Color){160, 80, 200, 220});
      DrawRectangleLines(gx, gy, barW, barH, (Color){160, 80, 200, 255});
    }

    // 2D overlay: floating texts (spell shouts + damage numbers)
    for (int i = 0; i < MAX_FLOATING_TEXTS; i++) {
      if (!floatingTexts[i].active)
        continue;
      Vector2 fsp = GetWorldToScreen(floatingTexts[i].position, camera);
      float alpha = floatingTexts[i].life / floatingTexts[i].maxLife;
      int fsize =
          floatingTexts[i].fontSize > 0 ? floatingTexts[i].fontSize : 16;
      // Apply horizontal drift
      float elapsed = floatingTexts[i].maxLife - floatingTexts[i].life;
      float driftOffset = floatingTexts[i].driftX * elapsed;
      int ftw = GameMeasureText(floatingTexts[i].text, fsize);
      Color ftc = floatingTexts[i].color;
      ftc.a = (unsigned char)(255.0f * alpha);
      GameDrawText(floatingTexts[i].text, (int)(fsp.x + driftOffset) - ftw / 2,
                   (int)fsp.y, fsize, ftc);
    }

    // ── Spawn buttons + Play — during prep and plaza ──
    if (phase == PHASE_PREP || phase == PHASE_PLAZA) {
      int sw = GetScreenWidth();
      int sh = GetScreenHeight();
      int dHudTop = sh - hudTotalH;
      int dBtnXBlue = btnMargin;
      int dBtnXRed = sw - btnWidth - btnMargin;
      int validTypeCount = 0;
      for (int i = 0; i < unitTypeCount; i++)
        if (unitTypes[i].name)
          validTypeCount++;
      int dBtnYStart =
          dHudTop - (validTypeCount * (btnHeight + btnMargin)) - btnMargin;

      // Spawn buttons (debug mode only — F1 to toggle)
      if (debugMode) {
        int drawIdx = 0;
        for (int i = 0; i < unitTypeCount; i++) {
          if (!unitTypes[i].name)
            continue;
          Rectangle r = {
              (float)dBtnXBlue,
              (float)(dBtnYStart + drawIdx * (btnHeight + btnMargin)),
              (float)btnWidth, (float)btnHeight};
          Color c =
              unitTypes[i].loaded ? (Color){100, 140, 230, 255} : LIGHTGRAY;
          if (CheckCollisionPointRec(GetMousePosition(), r) &&
              unitTypes[i].loaded)
            c = BLUE;
          DrawRectangleRec(r, c);
          DrawRectangleLinesEx(r, 2, unitTypes[i].loaded ? DARKBLUE : GRAY);
          const char *rarityPrefix =
              (debugSpawnRarity == RARITY_LEGENDARY) ? "LEG "
              : (debugSpawnRarity == RARITY_RARE)    ? "RARE "
                                                     : "";
          const char *l =
              TextFormat("BLUE %s%s", rarityPrefix, unitTypes[i].name);
          int lw = GameMeasureText(l, 14);
          GameDrawText(l, r.x + (btnWidth - lw) / 2, r.y + (btnHeight - 14) / 2,
                       14, WHITE);
          drawIdx++;
        }

        drawIdx = 0;
        for (int i = 0; i < unitTypeCount; i++) {
          if (!unitTypes[i].name)
            continue;
          Rectangle r = {
              (float)dBtnXRed,
              (float)(dBtnYStart + drawIdx * (btnHeight + btnMargin)),
              (float)btnWidth, (float)btnHeight};
          Color c =
              unitTypes[i].loaded ? (Color){230, 100, 100, 255} : LIGHTGRAY;
          if (CheckCollisionPointRec(GetMousePosition(), r) &&
              unitTypes[i].loaded)
            c = RED;
          DrawRectangleRec(r, c);
          DrawRectangleLinesEx(r, 2, unitTypes[i].loaded ? MAROON : GRAY);
          const char *l = TextFormat("RED %s", unitTypes[i].name);
          int lw = GameMeasureText(l, 14);
          GameDrawText(l, r.x + (btnWidth - lw) / 2, r.y + (btnHeight - 14) / 2,
                       14, WHITE);
          drawIdx++;
        }

        GameDrawText("[F1] DEBUG MODE", dBtnXBlue, dBtnYStart - 20, 12, YELLOW);
        {
          const char *rarityNames[] = {"COMMON", "RARE", "LEGENDARY"};
          const char *gLabel =
              TextFormat("[G] Rarity: %s", rarityNames[debugSpawnRarity]);
          GameDrawText(gLabel, dBtnXBlue, dBtnYStart - 36, 12, YELLOW);
        }
        GameDrawText(TextFormat("[</>] Tiles: %s", tileLayoutNames[tileLayout]),
                     dBtnXBlue, dBtnYStart - 36, 12, YELLOW);
        GameDrawText(TextFormat("[+/-] Shop Slots: %d", activeShopSlots),
                     dBtnXBlue, dBtnYStart - 52, 12, YELLOW);

        // --- ENV PIECE spawn buttons (centered column) ---
        {
          int envBtnW = 110, envBtnH = 24, envBtnGap = 4;
          int envColX = sw / 2 - envBtnW / 2;
          int envStartY = dBtnYStart;
          GameDrawText("[ENV PIECES]", envColX, envStartY - 16, 12, YELLOW);
          for (int ei = 0; ei < envModelCount; ei++) {
            if (!envModels[ei].loaded)
              continue;
            Rectangle er = {(float)envColX,
                            (float)(envStartY + ei * (envBtnH + envBtnGap)),
                            (float)envBtnW, (float)envBtnH};
            Color ec = (Color){80, 160, 80, 255};
            if (CheckCollisionPointRec(GetMousePosition(), er))
              ec = GREEN;
            DrawRectangleRec(er, ec);
            DrawRectangleLinesEx(er, 1, DARKGREEN);
            const char *el = TextFormat("+ %s", envModels[ei].name);
            int elw = GameMeasureText(el, 12);
            GameDrawText(el, (int)(er.x + (envBtnW - elw) / 2), (int)(er.y + 6),
                         12, WHITE);
          }
          // SAVE LAYOUT button
          int saveY = envStartY + envModelCount * (envBtnH + envBtnGap) + 4;
          Rectangle saveBtn = {(float)envColX, (float)saveY, (float)envBtnW,
                               (float)envBtnH};
          Color savCol = (Color){160, 120, 40, 255};
          if (CheckCollisionPointRec(GetMousePosition(), saveBtn))
            savCol = GOLD;
          DrawRectangleRec(saveBtn, savCol);
          DrawRectangleLinesEx(saveBtn, 1, DARKBROWN);
          const char *savLbl = TextFormat("SAVE (%d pcs)", envPieceCount);
          int savLblW = GameMeasureText(savLbl, 12);
          GameDrawText(savLbl, (int)(saveBtn.x + (envBtnW - savLblW) / 2),
                       (int)(saveBtn.y + 6), 12, WHITE);

          // Flash "SAVED!" text
          if (envSaveFlashTimer > 0.0f) {
            float alpha = envSaveFlashTimer > 1.0f ? 1.0f : envSaveFlashTimer;
            GameDrawText("SAVED!", envColX + envBtnW + 8, saveY + 4, 14,
                         (Color){50, 255, 50, (unsigned char)(255 * alpha)});
          }

          // Selected piece info overlay
          if (envSelectedPiece >= 0 && envSelectedPiece < envPieceCount &&
              envPieces[envSelectedPiece].active) {
            EnvPiece *sp = &envPieces[envSelectedPiece];
            const char *infoName = envModels[sp->modelIndex].name;
            int infoY = saveY + envBtnH + 12;
            GameDrawText(TextFormat("%s  [X:%.1f Y:%.1f Z:%.1f]", infoName,
                                    sp->position.x, sp->position.y,
                                    sp->position.z),
                         envColX, infoY, 12, WHITE);
            GameDrawText(
                TextFormat("RotX: %.0f  Y: %.0f  Z: %.0f  Scale: %.2fx",
                           sp->rotationX, sp->rotationY, sp->rotationZ,
                           sp->scale),
                envColX, infoY + 14, 12, WHITE);
            GameDrawText("[WASD] Move XZ  [R/F] Y  [Q/E] RotY  [X/C] RotX  "
                         "[\\/ Z] RotZ  [[ / ]] Scale  [DEL] Remove",
                         envColX, infoY + 28, 10, (Color){180, 180, 180, 200});
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
        GameDrawText(waveLabel, sw / 2 - wlw / 2, dBtnYStart - 25, S(20),
                     WHITE);

        // Drag hint (first round only, until player drags a unit)
        if (currentRound == 0 && !hasDraggedUnit) {
          const char *dhint = "Click and drag your units to reposition them!";
          int dhSz = S(14);
          int dhW = GameMeasureText(dhint, dhSz);
          int dhX = sw / 2 - dhW / 2;
          int dhY = dBtnYStart - 25 - dhSz - S(14);
          float dpulse = 0.5f + 0.5f * sinf((float)GetTime() * 3.0f);
          unsigned char dhAlpha = (unsigned char)(160 + (int)(dpulse * 95));
          DrawRectangle(dhX - 8, dhY - 4, dhW + 16, dhSz + 8,
                        (Color){20, 20, 35, (unsigned char)(dhAlpha * 0.7f)});
          DrawRectangleLinesEx((Rectangle){(float)(dhX - 8), (float)(dhY - 4),
                                           (float)(dhW + 16),
                                           (float)(dhSz + 8)},
                               1, (Color){255, 220, 100, dhAlpha});
          GameDrawText(dhint, dhX, dhY, dhSz, (Color){255, 230, 120, dhAlpha});
        }
      }

      // PLAY / READY button (left side, prep phase only)
      if (phase == PHASE_PREP) {
        Rectangle dPlayBtn = {(float)(20),
                              (float)(dHudTop - playBtnH - btnMargin),
                              (float)playBtnW, (float)playBtnH};
        int ba, ra;
        CountTeams(units, unitCount, &ba, &ra);
        bool canPlay = isMultiplayer
                           ? (ba > 0)
                           : (ba > 0 && ra > 0);
        bool alreadyReady = isMultiplayer && playerReady;
        Color pc;
        if (alreadyReady)
          pc = (Color){80, 80, 80, 255};
        else if (canPlay)
          pc = (Color){50, 180, 80, 255};
        else
          pc = LIGHTGRAY;
        if (canPlay && !alreadyReady &&
            CheckCollisionPointRec(GetMousePosition(), dPlayBtn))
          pc = (Color){30, 220, 60, 255};
        DrawRectangleRec(dPlayBtn, pc);
        DrawRectangleLinesEx(dPlayBtn, 2,
                             canPlay && !alreadyReady ? DARKGREEN : GRAY);
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
        if (ptw > playBtnW - S(8)) {
          playFontSz = S(14);
          ptw = GameMeasureText(pt, playFontSz);
        }
        GameDrawText(pt, dPlayBtn.x + (playBtnW - ptw) / 2,
                     dPlayBtn.y + (playBtnH - playFontSz) / 2, playFontSz,
                     WHITE);
        // Opponent ready indicator with countdown
        if (isMultiplayer && opponentIsReady) {
          const char *oppTxt;
          if (oppReadyCountdown > 0.0f)
            oppTxt = TextFormat("STARTING IN %ds", (int)oppReadyCountdown + 1);
          else
            oppTxt = "OPPONENT READY";
          int oppFsz = S(14);
          int oppW = GameMeasureText(oppTxt, oppFsz);
          float oppPulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.0f);
          unsigned char oppAlpha = (unsigned char)(180 + (int)(oppPulse * 75));
          GameDrawText(oppTxt, (int)(dPlayBtn.x + (playBtnW - oppW) / 2),
                       (int)(dPlayBtn.y + playBtnH + S(6)), oppFsz,
                       (Color){0, 220, 180, oppAlpha});
        }
      }
    }

    // ── HUD: round + score info (top of screen) ──
    {
      int sw = GetScreenWidth();
      int sh = GetScreenHeight();
      if (phase != PHASE_PLAZA) {
        // Round indicator (large, top center)
        // During GAME_OVER, currentRound was already incremented — show the
        // wave we died/finished on
        int displayRound = mapActive ? battleCount
            : ((phase == PHASE_GAME_OVER) ? currentRound : currentRound + 1);
        const char *roundText = isMultiplayer
                                    ? TextFormat("Round %d", displayRound)
                                    : TextFormat("Wave %d", displayRound);
        int roundSz = S(28);
        int roundW = GameMeasureText(roundText, roundSz);
        int roundX = sw / 2 - roundW / 2;
        int roundY = S(8);
        DrawRectangle(roundX - S(12), roundY - S(4), roundW + S(24),
                      roundSz + S(8), (Color){20, 20, 30, 200});
        GameDrawText(roundText, roundX, roundY, roundSz,
                     (Color){255, 230, 120, 255});

        // Wave upgrade description (below round indicator)
        if (waveUpgradeText[0] != '\0') {
          int wuSz = S(18);
          int wuW = GameMeasureText(waveUpgradeText, wuSz);
          int wuX = sw / 2 - wuW / 2;
          int wuY = roundY + roundSz + S(6);
          bool isBoss = (currentRound == 4);
          Color wuColor =
              isBoss ? (Color){255, 255, 255, 240} : (Color){255, 255, 255, 220};
          DrawRectangle(wuX - S(8), wuY - S(2), wuW + S(16), wuSz + S(4),
                        (Color){20, 20, 30, 180});
          GameDrawText(waveUpgradeText, wuX, wuY, wuSz, wuColor);
        }

        // Next milestone info (top center, below wave info)
        if (lastMilestoneRound > 0) {
          int infoY = roundY + roundSz + S(6);
          if (waveUpgradeText[0] != '\0')
            infoY += S(24);
          int nextMilestone = ((currentRound / 5) + 1) * 5;
          const char *nextText =
              TextFormat("Next milestone: Wave %d", nextMilestone);
          int nextSz = S(16);
          int ntw = GameMeasureText(nextText, nextSz);
          GameDrawText(nextText, sw / 2 - ntw / 2, infoY, nextSz,
                       (Color){200, 200, 220, 200});
        }

        GameDrawText(TextFormat("Units: %d / %d", unitCount, MAX_UNITS), 10, 30,
                     10, DARKGRAY);
      }
      if (isMultiplayer) {
        int mySlot = NC_FLAG(playerSlot);
        int myHp = mpHealth[mySlot];
        int oppHp = mpHealth[1 - mySlot];
#ifdef USE_EOS
        const char *oppName =
            useEos
                ? (eosClient.opponentName[0] ? eosClient.opponentName : "???")
                : (netClient.opponentName[0] ? netClient.opponentName : "???");
#else
        const char *oppName =
            netClient.opponentName[0] ? netClient.opponentName : "???";
#endif
        // Stacked health bars (top-left, near gold)
        int hpBarW = S(100), hpBarH = S(8);
        int hpX = S(10), hpY = S(6);
        int mpNameSz = S(12);
        int hpGap = S(2);

        // Player health bar
        GameDrawText(playerName, hpX, hpY, mpNameSz, (Color){120, 200, 255, 255});
        int barY1 = hpY + mpNameSz + S(2);
        DrawRectangle(hpX, barY1, hpBarW, hpBarH, (Color){40, 40, 50, 200});
        float youPct = (float)myHp / 20.0f;
        if (youPct < 0) youPct = 0;
        DrawRectangle(hpX, barY1, (int)(hpBarW * youPct), hpBarH,
                      (Color){50, 180, 50, 255});
        DrawRectangleLinesEx((Rectangle){(float)hpX, (float)barY1,
                             (float)hpBarW, (float)hpBarH},
                             1, (Color){100, 100, 120, 200});
        const char *youHpTxt = TextFormat("%d", myHp);
        GameDrawText(youHpTxt, hpX + hpBarW + S(4), barY1, mpNameSz, WHITE);

        // Opponent health bar
        int oppLabelY = barY1 + hpBarH + hpGap;
        GameDrawText(oppName, hpX, oppLabelY, mpNameSz, (Color){255, 120, 120, 255});
        int barY2 = oppLabelY + mpNameSz + S(2);
        DrawRectangle(hpX, barY2, hpBarW, hpBarH, (Color){40, 40, 50, 200});
        float oppPct = (float)oppHp / 20.0f;
        if (oppPct < 0) oppPct = 0;
        DrawRectangle(hpX, barY2, (int)(hpBarW * oppPct), hpBarH,
                      (Color){180, 50, 50, 255});
        DrawRectangleLinesEx((Rectangle){(float)hpX, (float)barY2,
                             (float)hpBarW, (float)hpBarH},
                             1, (Color){100, 100, 120, 200});
        const char *oppHpTxt = TextFormat("%d", oppHp);
        GameDrawText(oppHpTxt, hpX + hpBarW + S(4), barY2, mpNameSz, WHITE);
      }

      // Phase label
      if (phase == PHASE_COMBAT) {
        // Animated "FIGHT!" banner
        if (fightBannerTimer >= 0.0f && fightBannerTimer < 1.5f) {
          const char *fightText = "FIGHT!";
          int baseFontSize = S(56);
          float t = fightBannerTimer;
          float scale;
          if (t < 0.15f)
            scale = t / 0.15f * 1.5f; // 0→1.5
          else if (t < 0.5f)
            scale = 1.5f - (t - 0.15f) / 0.35f * 0.5f; // 1.5→1.0
          else
            scale = 1.0f;
          float alpha = t < 1.0f ? 1.0f : 1.0f - (t - 1.0f) / 0.5f;
          if (alpha < 0)
            alpha = 0;
          int drawSize = (int)(baseFontSize * scale);
          if (drawSize < 1)
            drawSize = 1;
          int ftw = GameMeasureText(fightText, drawSize);
          // Shake during punch-in
          int shakeX = 0, shakeY = 0;
          if (t < 0.5f) {
            shakeX = GetRandomValue(-3, 3);
            shakeY = GetRandomValue(-2, 2);
          }
          Color fc = RED;
          fc.a = (unsigned char)(255.0f * alpha);
          GameDrawText(fightText, sw / 2 - ftw / 2 + shakeX,
                       sh / 2 - 60 + shakeY, drawSize, fc);
        }
        // Kill feed announcement
        if (killFeedTimer >= 0.0f && killFeedTimer < 3.0f) {
          float kft = killFeedTimer;
          int kfFontSize = 36;
          float kfScale;
          if (kft < 0.15f)
            kfScale = killFeedScale * (kft / 0.15f);
          else if (kft < 0.4f)
            kfScale = killFeedScale -
                      (killFeedScale - 1.0f) * ((kft - 0.15f) / 0.25f);
          else
            kfScale = 1.0f;
          float kfAlpha = kft < 2.0f ? 1.0f : 1.0f - (kft - 2.0f) / 1.0f;
          if (kfAlpha < 0)
            kfAlpha = 0;
          int kfDrawSize = (int)(kfFontSize * kfScale);
          if (kfDrawSize < 1)
            kfDrawSize = 1;
          int kfw = GameMeasureText(killFeedText, kfDrawSize);
          Color kfc = (Color){255, 200, 50, (unsigned char)(255.0f * kfAlpha)};
          GameDrawText(killFeedText, sw / 2 - kfw / 2, sh / 2 - 20, kfDrawSize,
                       kfc);
        }
      } else if (phase == PHASE_ROUND_OVER) {
        // Animated round result text with scale punch-in
        float rot = roundOverTimer; // counts down from 2.5
        float elapsed = 2.5f - rot;
        float rtScale;
        if (elapsed < 0.15f)
          rtScale = elapsed / 0.15f * 1.3f;
        else if (elapsed < 0.4f)
          rtScale = 1.3f - (elapsed - 0.15f) / 0.25f * 0.3f;
        else
          rtScale = 1.0f;
        int rtFontSize = (int)(S(30) * rtScale);
        if (rtFontSize < 1)
          rtFontSize = 1;
        Color rtColor = lastOutcomeWin ? (Color){50, 200, 50, 255} : DARKPURPLE;
        // Color pulse for win
        if (lastOutcomeWin) {
          float pulse = 0.5f + 0.5f * sinf(elapsed * 6.0f);
          rtColor.r = (unsigned char)(50 + pulse * 100);
          rtColor.g = (unsigned char)(200 + pulse * 55);
        }
        int rtw = GameMeasureText(roundResultText, rtFontSize);
        int rtY = sh / 2 - rtFontSize - S(5);
        GameDrawText(roundResultText, sw / 2 - rtw / 2, rtY, rtFontSize,
                     rtColor);

        const char *scoreText;
        if (isMultiplayer) {
          int mySlot2 = NC_FLAG(playerSlot);
          scoreText = TextFormat("HP: You %d — Opp %d", mpHealth[mySlot2],
                                 mpHealth[1 - mySlot2]);
        } else {
          scoreText = TextFormat("Score: %d - %d", blueWins, redWins);
        }
        int stFontSize = S(22);
        int stw = GameMeasureText(scoreText, stFontSize);
        GameDrawText(scoreText, sw / 2 - stw / 2, rtY + rtFontSize + S(8),
                     stFontSize, WHITE);

        // Balatro-style gold breakdown (hide when death penalty imminent)
        if (!isMultiplayer && !(blueLostLastRound && lastMilestoneRound > 0)) {
          int gbW = S(200), gbH = S(120);
          int gbX = sw / 2 - gbW / 2;
          int gbY = rtY + rtFontSize + S(38);
          // Background card
          DrawRectangle(gbX, gbY, gbW, gbH, (Color){20, 20, 30, 220});
          DrawRectangleLinesEx(
              (Rectangle){(float)gbX, (float)gbY, (float)gbW, (float)gbH}, 2,
              (Color){180, 160, 80, 200});

          int lnH = S(16);
          int lnY = gbY + S(6);
          int lnFS = S(13);
          Color goldCol = (Color){255, 210, 60, 255};
          Color dimCol = (Color){160, 160, 180, 200};

          // Header
          const char *hdr = "GOLD EARNED";
          int hw = GameMeasureText(hdr, lnFS);
          GameDrawText(hdr, gbX + gbW / 2 - hw / 2, lnY, lnFS, goldCol);
          lnY += lnH;

          // Thin separator
          DrawRectangle(gbX + S(10), lnY, gbW - S(20), 1,
                        (Color){100, 100, 120, 150});
          lnY += S(4);

          // Line items
          int valX = gbX + gbW - S(12);
          Color bossCol = {255, 120, 60, 255};
          Color aliveCol = {80, 220, 120, 255};
          Color intCol = {120, 200, 255, 255};

          const char *_v;
          int _vw;

          // Round bonus
          _v = TextFormat("+%dg", goldFlat);
          GameDrawText("Round bonus", gbX + S(12), lnY, lnFS, dimCol);
          _vw = GameMeasureText(_v, lnFS);
          GameDrawText(_v, valX - _vw, lnY, lnFS, goldCol);
          lnY += lnH;

          // Enemy kills
          if (goldKills > 0) {
            _v = TextFormat("+%dg", goldKills);
            GameDrawText("Enemy kills", gbX + S(12), lnY, lnFS, dimCol);
            _vw = GameMeasureText(_v, lnFS);
            GameDrawText(_v, valX - _vw, lnY, lnFS, goldCol);
            lnY += lnH;
          }
          // Boss kills
          if (goldBoss > 0) {
            _v = TextFormat("+%dg", goldBoss);
            GameDrawText("Boss kills", gbX + S(12), lnY, lnFS, dimCol);
            _vw = GameMeasureText(_v, lnFS);
            GameDrawText(_v, valX - _vw, lnY, lnFS, bossCol);
            lnY += lnH;
          }
          // Allies alive
          if (goldAlive > 0) {
            _v = TextFormat("+%dg", goldAlive);
            GameDrawText("Allies alive", gbX + S(12), lnY, lnFS, dimCol);
            _vw = GameMeasureText(_v, lnFS);
            GameDrawText(_v, valX - _vw, lnY, lnFS, aliveCol);
            lnY += lnH;
          }
          // Interest
          if (goldInterest > 0) {
            _v = TextFormat("+%dg", goldInterest);
            GameDrawText("Interest", gbX + S(12), lnY, lnFS, dimCol);
            _vw = GameMeasureText(_v, lnFS);
            GameDrawText(_v, valX - _vw, lnY, lnFS, intCol);
            lnY += lnH;
          }

          // Bottom separator + total
          DrawRectangle(gbX + S(10), lnY, gbW - S(20), 1,
                        (Color){180, 160, 80, 150});
          lnY += S(4);
          int total = roundGoldReward + goldInterest;
          const char *totLabel = "Total";
          const char *totVal = TextFormat("+%dg", total);
          GameDrawText(totLabel, gbX + S(12), lnY, lnFS, WHITE);
          int tvw = GameMeasureText(totVal, lnFS);
          GameDrawText(totVal, valX - tvw, lnY, lnFS, goldCol);
        }
      }

      // Battle Log panel (during combat, round over, and next prep)
      if ((phase == PHASE_COMBAT || phase == PHASE_ROUND_OVER ||
           phase == PHASE_PREP) &&
          battleLog.count > 0) {
        int blogW = S(240);
        int blogX = sw - blogW;
        int blogY = 60;
        int blogH = sh - hudTotalH - blogY;
        // Background
        DrawRectangle(blogX, blogY, blogW, blogH, (Color){16, 16, 24, 160});
        DrawRectangleLines(blogX, blogY, blogW, blogH,
                           (Color){80, 80, 100, 120});
        // Title
        const char *blogTitle = "BATTLE LOG";
        int btw = GameMeasureText(blogTitle, S(14));
        GameDrawText(blogTitle, blogX + blogW / 2 - btw / 2, blogY + S(4),
                     S(14), (Color){200, 200, 220, 255});
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
            if (battleLog.scroll < 0)
              battleLog.scroll = 0;
            int maxScroll = battleLog.count - maxVisible;
            if (maxScroll < 0)
              maxScroll = 0;
            if (battleLog.scroll > maxScroll)
              battleLog.scroll = maxScroll;
          }
        } else {
          // Auto-scroll to bottom during combat
          int maxScroll = battleLog.count - maxVisible;
          if (maxScroll < 0)
            maxScroll = 0;
          battleLog.scroll = maxScroll;
        }
        // Scissor clip
        BeginScissorMode(blogX, entryY, blogW, entryH);
        int startIdx = battleLog.scroll;
        for (int ei = startIdx;
             ei < battleLog.count && (ei - startIdx) < maxVisible; ei++) {
          BattleLogEntry *e = &battleLog.entries[ei];
          int drawY = entryY + (ei - startIdx) * lineH;
          // Timestamp
          const char *ts = (e->timestamp < 60.0f)
                               ? TextFormat("%.1fs", e->timestamp)
                               : TextFormat("%d:%02d", (int)e->timestamp / 60,
                                            (int)e->timestamp % 60);
          GameDrawText(ts, blogX + S(4), drawY, S(12),
                       (Color){140, 140, 140, 200});
          // Icon
          const char *icon = (e->type == BLOG_KILL) ? "X" : "*";
          Color iconColor = (e->type == BLOG_KILL) ? (Color){255, 80, 80, 255}
                                                   : (Color){80, 200, 255, 255};
          GameDrawText(icon, blogX + S(34), drawY, S(12), iconColor);
          // Text (truncated to fit)
          GameDrawText(e->text, blogX + S(44), drawY, S(12), e->color);
        }
        EndScissorMode();
      }

      else if (phase == PHASE_GAME_OVER) {
        if (deathPenalty) {
          int titleFsz = S(34), subFsz = S(22), hintFsz = S(20);
          int totalH = titleFsz + S(12) + subFsz + S(16) + hintFsz;
          int startY = sh / 2 - totalH / 2;

          const char *deathMsg =
              TextFormat("YOUR UNITS HAVE FALLEN - Wave %d", currentRound);
          int dw = GameMeasureText(deathMsg, titleFsz);
          GameDrawText(deathMsg, sw / 2 - dw / 2, startY, titleFsz, RED);

          const char *deathSub = "Defeated! Your units are lost forever!";
          int dsw2 = GameMeasureText(deathSub, subFsz);
          GameDrawText(deathSub, sw / 2 - dsw2 / 2, startY + titleFsz + S(12),
                       subFsz, (Color){255, 100, 100, 255});

          const char *restartMsg = "Press R to return to menu";
          int rw2 = GameMeasureText(restartMsg, hintFsz);
          GameDrawText(restartMsg, sw / 2 - rw2 / 2,
                       startY + titleFsz + S(12) + subFsz + S(16), hintFsz,
                       GRAY);
        }
        // Non-death game over is drawn as a full overlay below
      }
    }

    // F1 debug hint (always visible, top-right)
    {
      const char *dbgHint = "[F1] Debug";
      int dbgW = GameMeasureText(dbgHint, 14);
      Color dbgCol = debugMode ? YELLOW : (Color){180, 180, 180, 120};
      GameDrawText(dbgHint, GetScreenWidth() - dbgW - 10, 10, 14, dbgCol);
    }

    // Camera debug sliders (debug mode only)
    if (debugMode) {
      // Override toggle button
      Rectangle overrideBtn = {10, 60, 80, 20};
      DrawRectangleRec(overrideBtn, camOverride ? GREEN : GRAY);
      GameDrawText(camOverride ? "Override ON" : "Override OFF", 14, 64, 10,
                   WHITE);
      if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
          CheckCollisionPointRec(GetMousePosition(), overrideBtn))
        camOverride = !camOverride;

      Color sliderBg = camOverride ? LIGHTGRAY : (Color){100, 100, 100, 255};
      Color sliderFill = camOverride ? SKYBLUE : (Color){80, 80, 120, 255};

      // Height slider: range -50 to 500
      Rectangle hBar = {10, 85, 200, 20};
      float hPerc = (camHeight - (-50.0f)) / (500.0f - (-50.0f));
      if (hPerc > 1)
        hPerc = 1;
      if (hPerc < 0)
        hPerc = 0;
      DrawRectangleRec(hBar, sliderBg);
      DrawRectangle(10, 85, (int)(200 * hPerc), 20, sliderFill);
      GameDrawText(TextFormat("Height: %.1f", camHeight), 220, 85, 10, BLACK);
      if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
          CheckCollisionPointRec(GetMousePosition(), hBar)) {
        float t = (GetMousePosition().x - 10.0f) / 200.0f;
        if (t < 0)
          t = 0;
        if (t > 1)
          t = 1;
        camHeight = -50.0f + t * 550.0f;
      }

      // Distance slider: range -300 to 500
      Rectangle dBar = {10, 110, 200, 20};
      float dPerc = (camDistance - (-300.0f)) / (500.0f - (-300.0f));
      if (dPerc > 1)
        dPerc = 1;
      if (dPerc < 0)
        dPerc = 0;
      DrawRectangleRec(dBar, sliderBg);
      DrawRectangle(10, 110, (int)(200 * dPerc), 20, sliderFill);
      GameDrawText(TextFormat("Distance: %.1f", camDistance), 220, 110, 10,
                   BLACK);
      if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
          CheckCollisionPointRec(GetMousePosition(), dBar)) {
        float t = (GetMousePosition().x - 10.0f) / 200.0f;
        if (t < 0)
          t = 0;
        if (t > 1)
          t = 1;
        camDistance = -300.0f + t * 800.0f;
      }

      // FOV slider: range 5 to 160
      Rectangle fBar = {10, 135, 200, 20};
      float fPerc = (camFOV - 5.0f) / (160.0f - 5.0f);
      if (fPerc > 1)
        fPerc = 1;
      if (fPerc < 0)
        fPerc = 0;
      DrawRectangleRec(fBar, sliderBg);
      DrawRectangle(10, 135, (int)(200 * fPerc), 20, sliderFill);
      GameDrawText(TextFormat("FOV: %.1f", camFOV), 220, 135, 10, BLACK);
      if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
          CheckCollisionPointRec(GetMousePosition(), fBar)) {
        float t = (GetMousePosition().x - 10.0f) / 200.0f;
        if (t < 0)
          t = 0;
        if (t > 1)
          t = 1;
        camFOV = 5.0f + t * 155.0f;
      }

      // X Offset slider: range -200 to 200
      Rectangle xBar = {10, 160, 200, 20};
      float xPerc = (camX - (-200.0f)) / (200.0f - (-200.0f));
      if (xPerc > 1)
        xPerc = 1;
      if (xPerc < 0)
        xPerc = 0;
      DrawRectangleRec(xBar, sliderBg);
      DrawRectangle(10, 160, (int)(200 * xPerc), 20, sliderFill);
      GameDrawText(TextFormat("X Offset: %.1f", camX), 220, 160, 10, BLACK);
      if (camOverride && IsMouseButtonDown(MOUSE_LEFT_BUTTON) &&
          CheckCollisionPointRec(GetMousePosition(), xBar)) {
        float t = (GetMousePosition().x - 10.0f) / 200.0f;
        if (t < 0)
          t = 0;
        if (t > 1)
          t = 1;
        camX = -200.0f + t * 400.0f;
      }

      // Save button
      Rectangle saveBtn = {10, 185, 50, 20};
      DrawRectangleRec(saveBtn, (Color){60, 60, 200, 255});
      GameDrawText("Save", 18, 189, 10, WHITE);
      if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
          CheckCollisionPointRec(GetMousePosition(), saveBtn)) {
        FILE *f = fopen("cam_debug.txt", "w");
        if (f) {
          fprintf(f, "%f %f %f %f\n", camHeight, camDistance, camFOV, camX);
          fclose(f);
        }
      }

      // Load button
      Rectangle loadBtn = {65, 185, 50, 20};
      DrawRectangleRec(loadBtn, (Color){60, 150, 60, 255});
      GameDrawText("Load", 73, 189, 10, WHITE);
      if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
          CheckCollisionPointRec(GetMousePosition(), loadBtn)) {
        FILE *f = fopen("cam_debug.txt", "r");
        if (f) {
          if (fscanf(f, "%f %f %f %f", &camHeight, &camDistance, &camFOV,
                     &camX) == 4)
            camOverride = true;
          fclose(f);
        }
      }
    }

    // ── UNIT HUD BAR + SHOP ── (visible during prep, combat, round_over only)
    if (phase != PHASE_GAME_OVER && phase != PHASE_PLAZA &&
        phase != PHASE_MILESTONE) {
      int hudSw = GetScreenWidth();
      int hudSh = GetScreenHeight();
      int hudTop = hudSh - hudTotalH;

      // --- Dark background panel (full width, bottom) ---
      DrawRectangle(0, hudTop, hudSw, hudTotalH, (Color){24, 24, 32, 230});
      DrawRectangle(0, hudTop, hudSw, 2, (Color){60, 60, 80, 255});

      // --- Unit cards (centered horizontally in the unit bar) ---
      int totalCardsW = BLUE_TEAM_MAX_SIZE * hudCardW +
                        (BLUE_TEAM_MAX_SIZE - 1) * hudCardSpacing;
      int cardsStartX = (hudSw - totalCardsW) / 2;
      int cardsY = hudTop + hudShopH + 5;

      for (int slot = 0; slot < BLUE_TEAM_MAX_SIZE; slot++) {
        int cardX = cardsStartX + slot * (hudCardW + hudCardSpacing);

        // Card background
        DrawRectangle(cardX, cardsY, hudCardW, hudCardH,
                      (Color){35, 35, 50, 255});
        DrawRectangleLines(cardX, cardsY, hudCardW, hudCardH,
                           (Color){60, 60, 80, 255});

        if (slot < blueHudCount) {
          int ui = blueHudUnits[slot];
          UnitType *type = &unitTypes[units[ui].typeIndex];
          const UnitStats *stats = &UNIT_STATS[units[ui].typeIndex];

          // Selection highlight
          if (units[ui].selected)
            DrawRectangleLinesEx(
                (Rectangle){(float)(cardX - 1), (float)(cardsY - 1),
                            (float)(hudCardW + 2), (float)(hudCardH + 2)},
                2, (Color){100, 255, 100, 255});

          // X button (remove unit) — prep phase only
          if (phase == PHASE_PREP) {
            int xBtnSize = S(18);
            int xBtnX = cardX + hudCardW - xBtnSize - 2;
            int xBtnY = cardsY + 2;
            Color xBg = (Color){180, 50, 50, 200};
            if (CheckCollisionPointRec(GetMousePosition(),
                                       (Rectangle){(float)xBtnX, (float)xBtnY,
                                                   (float)xBtnSize,
                                                   (float)xBtnSize}))
              xBg = (Color){230, 70, 70, 255};
            DrawRectangle(xBtnX, xBtnY, xBtnSize, xBtnSize, xBg);
            DrawRectangleLines(xBtnX, xBtnY, xBtnSize, xBtnSize,
                               (Color){100, 30, 30, 255});
            int xw = GameMeasureText("X", 12);
            GameDrawText("X", xBtnX + (xBtnSize - xw) / 2, xBtnY + 2, 12,
                         WHITE);
          }

          // Portrait (left side of card) — Y-flipped for RenderTexture
          // srcRect uses base texture size, dstRect uses scaled size
          Rectangle srcRect = {0, 0, (float)HUD_PORTRAIT_SIZE_BASE,
                               -(float)HUD_PORTRAIT_SIZE_BASE};
          Rectangle dstRect = {(float)(cardX + S(4)), (float)(cardsY + S(4)),
                               (float)hudPortraitSize, (float)hudPortraitSize};
          DrawTexturePro(portraits[slot].texture, srcRect, dstRect,
                         (Vector2){0, 0}, 0.0f, WHITE);
          DrawRectangleLines(cardX + S(4), cardsY + S(4), hudPortraitSize,
                             hudPortraitSize, (Color){60, 60, 80, 255});

          // Unit name below portrait
          const char *unitName = type->name;
          int nameW = GameMeasureText(unitName, S(12));
          GameDrawText(unitName, cardX + S(4) + (hudPortraitSize - nameW) / 2,
                       cardsY + S(4) + hudPortraitSize + S(2), S(12),
                       (Color){200, 200, 220, 255});

          if (units[ui].rarity > 0) {
            const char *stars =
                (units[ui].rarity == RARITY_LEGENDARY) ? "* * *" : "* *";
            int starsW = GameMeasureText(stars, S(10));
            Color starColor = (units[ui].rarity == RARITY_LEGENDARY)
                                  ? (Color){255, 60, 60, 255}
                                  : (Color){180, 100, 255, 255};
            int starsX = cardX + S(4) + (hudPortraitSize - starsW) / 2;
            int starsY = cardsY + S(4) + hudPortraitSize - S(4);
            GameDrawText(stars, starsX, starsY, S(10), starColor);
            const char *multLabel =
                (units[ui].rarity == RARITY_LEGENDARY) ? "x1.3" : "x1.1";
            GameDrawText(multLabel, starsX + starsW + S(2), starsY, S(8),
                         starColor);
          }

          // Mini health bar
          int hbX = cardX + S(4);
          int hbY = cardsY + S(4) + hudPortraitSize + S(16);
          int hbW = hudPortraitSize;
          int hbH = S(6);
          float cardMaxHP = stats->health * units[ui].hpMultiplier;
          float hpRatio = units[ui].currentHealth / cardMaxHP;
          if (hpRatio < 0)
            hpRatio = 0;
          if (hpRatio > 1)
            hpRatio = 1;
          DrawRectangle(hbX, hbY, hbW, hbH, (Color){20, 20, 20, 255});
          Color hpCol = (hpRatio > 0.5f)    ? GREEN
                        : (hpRatio > 0.25f) ? ORANGE
                                            : RED;
          int hpFillW = (int)(hbW * hpRatio);
          DrawRectangle(hbX, hbY, hpFillW, hbH, hpCol);
          if (units[ui].shieldHP > 0) {
            float shieldRatio = units[ui].shieldHP / cardMaxHP;
            if (shieldRatio > 1)
              shieldRatio = 1;
            int shieldW = (int)(hbW * shieldRatio);
            int shieldX = hbX + hpFillW;
            if (shieldX + shieldW > hbX + hbW)
              shieldW = hbX + hbW - shieldX;
            DrawRectangle(shieldX, hbY, shieldW, hbH,
                          (Color){80, 160, 255, 200});
          }
          DrawRectangleLines(hbX, hbY, hbW, hbH, (Color){60, 60, 80, 255});

          // Concise stat line below health bar (live-updated with modifiers)
          {
            int effHP = (int)(stats->health * units[ui].hpMultiplier);
            int effDMG = (int)(stats->attackDamage * units[ui].dmgMultiplier);
            float effMS = stats->movementSpeed * units[ui].speedMultiplier;
            float spdMult = GetModifierValue(modifiers, ui, MOD_SPEED_MULT);
            if (spdMult > 0) effMS *= spdMult;
            int effMSPD = (int)effMS;
            float effAtkSpd = stats->attackSpeed;
            float fvStacks = GetModifierValue(modifiers, ui, MOD_FERVOR);
            if (fvStacks > 0) {
              int fvLvl = GetUnitAbilityLevel(units, ui, ABILITY_FERVOR);
              if (fvLvl >= 0) {
                float redPerStack = ABILITY_DEFS[ABILITY_FERVOR].values[fvLvl][AV_FV_SPEED_RED];
                float speedMlt = 1.0f - fvStacks * redPerStack;
                if (speedMlt < 0.3f) speedMlt = 0.3f;
                effAtkSpd *= speedMlt;
              }
            }
            float effASPD = 1.0f / effAtkSpd;
            const char *statLine =
                TextFormat("%d HP  %d DMG  %d MS  %.1f AS", effHP, effDMG, effMSPD, effASPD);
            int statFsz = S(10);
            int statW = GameMeasureText(statLine, statFsz);
            int statMinX = cardX + S(2);
            int statMaxX = cardX + hudCardW - S(2);
            // Scale font down if text wider than card
            while (statW > (statMaxX - statMinX) && statFsz > S(6)) {
              statFsz--;
              statW = GameMeasureText(statLine, statFsz);
            }
            int statX = hbX + (hbW - statW) / 2 + S(6);
            if (statX < statMinX) statX = statMinX;
            GameDrawText(statLine, statX, hbY + hbH + S(1),
                         statFsz, (Color){160, 160, 180, 255});
          }

          // 2x2 Ability slot grid (right side of card)
          int abilGridW = 2 * hudAbilSlotSize + hudAbilSlotGap;
          int rightArea = hudCardW - hudPortraitSize - S(4);
          int abilStartX = cardX + S(4) + hudPortraitSize + (rightArea - abilGridW) / 2;
          int abilStartY = cardsY + 8;
          for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
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
              bool slotHovered = CheckCollisionPointRec(
                  GetMousePosition(),
                  (Rectangle){(float)ax, (float)ay, (float)hudAbilSlotSize,
                              (float)hudAbilSlotSize});
              if (slotHovered) {
                hoverAbilityId = aslot->abilityId;
                hoverAbilityLevel = aslot->level;
                hoverAbilityUnitIndex = ui;
              }
              // Abbreviation (scale up when charging tooltip)
              int abbrSize = S(13);
              if (slotHovered && hoverTimer > 0 && hoverTimer < tooltipDelay)
                abbrSize = S(13) + (int)(3.0f * (hoverTimer / tooltipDelay));
              const char *abbr = ABILITY_DEFS[aslot->abilityId].abbrev;
              int aw2 = GameMeasureText(abbr, abbrSize);
              GameDrawTextOnColor(abbr, ax + (hudAbilSlotSize - aw2) / 2,
                                  ay + (hudAbilSlotSize - abbrSize) / 2,
                                  abbrSize,
                                  ABILITY_DEFS[aslot->abilityId].color);
              // Level indicator (bottom-left)
              const char *lvl = TextFormat("L%d", aslot->level + 1);
              int lvlFsz = S(11);
              GameDrawTextOnColor(lvl, ax + S(2), ay + hudAbilSlotSize - lvlFsz,
                                  lvlFsz, ABILITY_DEFS[aslot->abilityId].color);
              // Cooldown overlay (combat only)
              if (aslot->cooldownRemaining > 0 && phase == PHASE_COMBAT) {
                const AbilityDef *adef = &ABILITY_DEFS[aslot->abilityId];
                float cdFrac =
                    aslot->cooldownRemaining / adef->cooldown[aslot->level];
                if (cdFrac > 1)
                  cdFrac = 1;
                int overlayH = (int)(hudAbilSlotSize * cdFrac);
                DrawRectangle(ax, ay, hudAbilSlotSize, overlayH,
                              (Color){0, 0, 0, 150});
                int cdFsz = S(14);
                const char *cdTxt =
                    TextFormat("%.0f", aslot->cooldownRemaining);
                int cdw = GameMeasureText(cdTxt, cdFsz);
                GameDrawText(cdTxt, ax + (hudAbilSlotSize - cdw) / 2,
                             ay + (hudAbilSlotSize - cdFsz) / 2, cdFsz, WHITE);
              }
            } else {
              // Empty slot
              DrawRectangle(ax, ay, hudAbilSlotSize, hudAbilSlotSize,
                            (Color){40, 40, 55, 255});
              const char *q = "?";
              int qFsz = S(18);
              int qw = GameMeasureText(q, qFsz);
              GameDrawText(q, ax + (hudAbilSlotSize - qw) / 2,
                           ay + (hudAbilSlotSize - qFsz) / 2, qFsz,
                           (Color){80, 80, 100, 255});
            }
            DrawRectangleLines(ax, ay, hudAbilSlotSize, hudAbilSlotSize,
                               (Color){90, 90, 110, 255});
            // Shop ability highlight glow
            {
              int highlightId = (shopHoverAbilityId >= 0)
                                    ? shopHoverAbilityId
                                    : shopHighlightAbilityId;
              if (highlightId >= 0 && aslot->abilityId == highlightId) {
                unsigned char glowAlpha = 255;
                if (shopHoverAbilityId < 0 && shopHighlightTimer > 0)
                  glowAlpha =
                      (unsigned char)(255 * (shopHighlightTimer / 0.5f));
                DrawRectangleLinesEx((Rectangle){(float)ax, (float)ay,
                                                 (float)hudAbilSlotSize,
                                                 (float)hudAbilSlotSize},
                                     2, (Color){100, 255, 255, glowAlpha});
              }
            }
            // Activation order number (top-right corner)
            // Find which activation position this slot is
            int orderNum = 0;
            for (int o = 0; o < MAX_ABILITIES_PER_UNIT; o++)
              if (ACTIVATION_ORDER[o] == a) {
                orderNum = o + 1;
                break;
              }
            Color orderCol = (Color){100, 100, 120, 255};
            if (phase == PHASE_COMBAT &&
                ACTIVATION_ORDER[units[ui].nextAbilitySlot] == a)
              orderCol = YELLOW;
            int ordFsz = S(11);
            const char *ordTxt = TextFormat("%d", orderNum);
            GameDrawText(ordTxt, ax + hudAbilSlotSize - ordFsz + 1,
                         ay + S(1) + 1, ordFsz, (Color){0, 0, 0, 180});
            GameDrawText(ordTxt, ax + hudAbilSlotSize - ordFsz, ay + S(1),
                         ordFsz, orderCol);
          }
          // Activation order arrows between slots (clockwise: TL>TR>BR>BL)
          {
            int arFsz = S(10);
            Color arCol = (Color){160, 160, 180, 180};
            int step = hudAbilSlotSize + hudAbilSlotGap;
            // TL(0)->TR(1): right arrow between col 0 and col 1, row 0
            int arX = abilStartX + hudAbilSlotSize +
                      (hudAbilSlotGap - GameMeasureText(">", arFsz)) / 2;
            int arY = abilStartY + (hudAbilSlotSize - arFsz) / 2;
            GameDrawText(">", arX, arY, arFsz, arCol);
            // TR(1)->BR(3): down arrow on right side, between row 0 and row 1
            arX = abilStartX + step +
                  (hudAbilSlotSize - GameMeasureText("v", arFsz)) / 2;
            arY = abilStartY + hudAbilSlotSize + (hudAbilSlotGap - arFsz) / 2;
            GameDrawText("v", arX, arY, arFsz, arCol);
            // BR(3)->BL(2): left arrow between col 1 and col 0, row 1
            arX = abilStartX + hudAbilSlotSize +
                  (hudAbilSlotGap - GameMeasureText("<", arFsz)) / 2;
            arY = abilStartY + step + (hudAbilSlotSize - arFsz) / 2;
            GameDrawText("<", arX, arY, arFsz, arCol);
          }
          // Item slot (below ability grid)
          {
            int iSlotSize = S(24);
            int iSlotX = abilStartX +
                         (2 * hudAbilSlotSize + hudAbilSlotGap - iSlotSize) / 2;
            int iSlotY =
                abilStartY + 2 * (hudAbilSlotSize + hudAbilSlotGap) + S(2);
            Rectangle iSlotRect = {(float)iSlotX, (float)iSlotY,
                                   (float)iSlotSize, (float)iSlotSize};
            bool iSlotHover =
                CheckCollisionPointRec(GetMousePosition(), iSlotRect);
            if (units[ui].itemId >= 0 && units[ui].itemId < ITEM_COUNT) {
              const ItemDef *idef = &ITEM_DEFS[units[ui].itemId];
              DrawRectangle(iSlotX, iSlotY, iSlotSize, iSlotSize, idef->color);
              DrawRectangleLines(iSlotX, iSlotY, iSlotSize, iSlotSize,
                                 (Color){200, 200, 220, 255});
              // Item initial letter
              char iLetter[2] = {idef->name[0], '\0'};
              int ilFsz = S(14);
              int ilw2 = GameMeasureText(iLetter, ilFsz);
              GameDrawTextOnColor(iLetter, iSlotX + (iSlotSize - ilw2) / 2,
                                  iSlotY + (iSlotSize - ilFsz) / 2, ilFsz,
                                  idef->color);
              // Tooltip on hover
              if (iSlotHover) {
                const char *iTip =
                    TextFormat("%s: %s", idef->name, idef->description);
                int iTipFsz = S(12);
                int iTipW = GameMeasureText(iTip, iTipFsz);
                int iTipX = iSlotX - iTipW / 2 + iSlotSize / 2;
                int iTipY = iSlotY - iTipFsz - S(6);
                DrawRectangle(iTipX - 4, iTipY - 2, iTipW + 8, iTipFsz + 4,
                              (Color){20, 20, 30, 230});
                GameDrawText(iTip, iTipX, iTipY, iTipFsz, WHITE);
              }
              // Drop on occupied slot: swap items
              if (phase == PHASE_PREP && itemDrag.dragging && iSlotHover &&
                  IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                int oldItem = units[ui].itemId;
                UnapplyItemStatMults(&units[ui]); // unapply old item
                units[ui].itemId = itemDrag.itemId;
                ApplyItemStatMults(&units[ui]); // apply new item
                if (itemDrag.sourceType == 0 && itemDrag.sourceIndex >= 0 &&
                    itemDrag.sourceIndex < MAX_ITEMS) {
                  itemInventory[itemDrag.sourceIndex] = oldItem;
                }
                itemDrag.dragging = false;
              }
              // Click to unequip (prep phase only)
              else if (phase == PHASE_PREP && iSlotHover &&
                       IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                       !itemDrag.dragging) {
                int unequipId = units[ui].itemId;
                UnapplyItemStatMults(&units[ui]); // unapply before removing
                units[ui].itemId = ITEM_NONE;
                for (int ii = 0; ii < MAX_ITEMS; ii++) {
                  if (itemInventory[ii] == ITEM_NONE) {
                    itemInventory[ii] = unequipId;
                    itemInventoryCount++;
                    break;
                  }
                }
              }
            } else {
              DrawRectangle(iSlotX, iSlotY, iSlotSize, iSlotSize,
                            (Color){30, 30, 40, 255});
              DrawRectangleLines(iSlotX, iSlotY, iSlotSize, iSlotSize,
                                 (Color){70, 70, 90, 255});
              int pFsz = S(14);
              const char *pTxt = "+";
              int pw2 = GameMeasureText(pTxt, pFsz);
              GameDrawText(pTxt, iSlotX + (iSlotSize - pw2) / 2,
                           iSlotY + (iSlotSize - pFsz) / 2, pFsz,
                           (Color){70, 70, 90, 255});
              // Drop target for item drag
              if (phase == PHASE_PREP && itemDrag.dragging && iSlotHover &&
                  IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                units[ui].itemId = itemDrag.itemId;
                ApplyItemStatMults(&units[ui]); // apply new item stats
                // Remove from source
                if (itemDrag.sourceType == 0 && itemDrag.sourceIndex >= 0 &&
                    itemDrag.sourceIndex < MAX_ITEMS) {
                  itemInventory[itemDrag.sourceIndex] = ITEM_NONE;
                  itemInventoryCount--;
                }
                itemDrag.dragging = false;
              }
            }
          }
        } else {
          // Empty slot placeholder
          const char *emptyText = "EMPTY";
          int emptyFsz = S(16);
          int ew = GameMeasureText(emptyText, emptyFsz);
          GameDrawText(emptyText, cardX + (hudCardW - ew) / 2,
                       cardsY + (hudCardH - emptyFsz) / 2, emptyFsz,
                       (Color){60, 60, 80, 255});
        }
      }

      // --- Inventory (left of unit cards) ---
      {
        int invStartX =
            cardsStartX -
            (HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap)) - 20;
        int invLabelY = cardsY + S(2);
        GameDrawText("INV", invStartX, invLabelY, S(14),
                     (Color){160, 160, 180, 255});
        int invStartY = invLabelY + S(16);
        for (int inv = 0; inv < MAX_INVENTORY_SLOTS; inv++) {
          int icol = inv % HUD_INVENTORY_COLS;
          int irow = inv / HUD_INVENTORY_COLS;
          int ix = invStartX + icol * (hudAbilSlotSize + hudAbilSlotGap);
          int iy = invStartY + irow * (hudAbilSlotSize + hudAbilSlotGap);
          DrawRectangle(ix, iy, hudAbilSlotSize, hudAbilSlotSize,
                        (Color){40, 40, 55, 255});
          DrawRectangleLines(ix, iy, hudAbilSlotSize, hudAbilSlotSize,
                             (Color){90, 90, 110, 255});
          if (inventory[inv].abilityId >= 0 &&
              inventory[inv].abilityId < ABILITY_COUNT) {
            DrawRectangle(ix + 1, iy + 1, hudAbilSlotSize - 2,
                          hudAbilSlotSize - 2,
                          ABILITY_DEFS[inventory[inv].abilityId].color);
            // Hover detection
            bool invHovered = CheckCollisionPointRec(
                GetMousePosition(),
                (Rectangle){(float)ix, (float)iy, (float)hudAbilSlotSize,
                            (float)hudAbilSlotSize});
            if (invHovered) {
              hoverAbilityId = inventory[inv].abilityId;
              hoverAbilityLevel = inventory[inv].level;
            }
            int invAbbrSize = S(13);
            if (invHovered && hoverTimer > 0 && hoverTimer < tooltipDelay)
              invAbbrSize = S(13) + (int)(3.0f * (hoverTimer / tooltipDelay));
            Color invAbilColor = ABILITY_DEFS[inventory[inv].abilityId].color;
            const char *iabbr = ABILITY_DEFS[inventory[inv].abilityId].abbrev;
            int iaw = GameMeasureText(iabbr, invAbbrSize);
            GameDrawTextOnColor(iabbr, ix + (hudAbilSlotSize - iaw) / 2,
                                iy + (hudAbilSlotSize - invAbbrSize) / 2,
                                invAbbrSize, invAbilColor);
            const char *ilvl = TextFormat("L%d", inventory[inv].level + 1);
            int ilvlFsz = S(11);
            GameDrawTextOnColor(ilvl, ix + S(2), iy + hudAbilSlotSize - ilvlFsz,
                                ilvlFsz, invAbilColor);
          }
        }
        // Item inventory (to the left of ability inventory)
        int itemSlotSize = hudAbilSlotSize;
        int itemSlotGap = hudAbilSlotGap;
        int itemCols = 2;
        int itemInvX = invStartX - itemCols * (itemSlotSize + itemSlotGap) - S(8);
        if (itemInventoryCount > 0 || phase == PHASE_PREP) {
          GameDrawText("ITEMS", itemInvX, invLabelY, S(14),
                       (Color){160, 160, 180, 255});
          for (int ii = 0; ii < MAX_ITEMS && ii < 6; ii++) {
            int ic = ii % itemCols;
            int ir = ii / itemCols;
            int iix = itemInvX + ic * (itemSlotSize + itemSlotGap);
            int iiy = invStartY + ir * (itemSlotSize + itemSlotGap);
            Rectangle iiRect = {(float)iix, (float)iiy, (float)itemSlotSize,
                                (float)itemSlotSize};
            bool iiHover = CheckCollisionPointRec(GetMousePosition(), iiRect);
            if (itemInventory[ii] >= 0 && itemInventory[ii] < ITEM_COUNT) {
              const ItemDef *iidef = &ITEM_DEFS[itemInventory[ii]];
              DrawRectangle(iix, iiy, itemSlotSize, itemSlotSize, iidef->color);
              DrawRectangleLines(iix, iiy, itemSlotSize, itemSlotSize,
                                 (Color){200, 200, 220, 255});
              char iLet[2] = {iidef->name[0], '\0'};
              int ilFsz2 = S(16);
              int ilw3 = GameMeasureText(iLet, ilFsz2);
              GameDrawTextOnColor(iLet, iix + (itemSlotSize - ilw3) / 2,
                                  iiy + (itemSlotSize - ilFsz2) / 2, ilFsz2,
                                  iidef->color);
              // Tooltip
              if (iiHover) {
                const char *tip =
                    TextFormat("%s: %s", iidef->name, iidef->description);
                int tipFsz = S(12);
                int tipW = GameMeasureText(tip, tipFsz);
                int tipX = iix - tipW / 2 + itemSlotSize / 2;
                int tipY2 = iiy - tipFsz - S(6);
                DrawRectangle(tipX - 4, tipY2 - 2, tipW + 8, tipFsz + 4,
                              (Color){20, 20, 30, 230});
                GameDrawText(tip, tipX, tipY2, tipFsz, WHITE);
              }
              // Start drag (prep phase only)
              if (phase == PHASE_PREP && iiHover &&
                  IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
                  !itemDrag.dragging && !dragState.dragging) {
                itemDrag.dragging = true;
                itemDrag.sourceType = 0;
                itemDrag.sourceIndex = ii;
                itemDrag.itemId = itemInventory[ii];
              }
            } else {
              DrawRectangle(iix, iiy, itemSlotSize, itemSlotSize,
                            (Color){30, 30, 40, 255});
              DrawRectangleLines(iix, iiy, itemSlotSize, itemSlotSize,
                                 (Color){60, 60, 80, 255});
              // Drop target: return dragged item to inventory
              if (phase == PHASE_PREP && itemDrag.dragging && iiHover &&
                  IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                itemInventory[ii] = itemDrag.itemId;
                itemInventoryCount++;
                if (itemDrag.sourceType == 0 && itemDrag.sourceIndex >= 0 &&
                    itemDrag.sourceIndex < MAX_ITEMS) {
                  itemInventory[itemDrag.sourceIndex] = ITEM_NONE;
                  itemInventoryCount--;
                }
                itemDrag.dragging = false;
              }
            }
          }
        }
      // --- Sell Zone (prep phase, directly below ability inventory) ---
      if (phase == PHASE_PREP) {
        int sellW = HUD_INVENTORY_COLS * (hudAbilSlotSize + hudAbilSlotGap);
        int sellH = S(20);
        int sellX = invStartX;
        int sellY = invStartY + HUD_INVENTORY_ROWS * (hudAbilSlotSize + hudAbilSlotGap) + S(2);
        bool sellHovered = CheckCollisionPointRec(GetMousePosition(),
            (Rectangle){(float)sellX, (float)sellY, (float)sellW, (float)sellH});
        bool draggingAbil = dragState.dragging && dragState.abilityId >= 0;
        Color sellBg = (sellHovered && draggingAbil) ? (Color){120, 40, 40, 255} : (Color){60, 30, 30, 200};
        DrawRectangle(sellX, sellY, sellW, sellH, sellBg);
        DrawRectangleLinesEx((Rectangle){(float)sellX, (float)sellY, (float)sellW, (float)sellH},
                             1, (Color){200, 60, 60, 255});
        // Show sell value if dragging an ability
        const char *sellLabel;
        if (draggingAbil) {
          int sellVal = ABILITY_DEFS[dragState.abilityId].goldCost / 2 + dragState.level;
          if (sellVal < 1) sellVal = 1;
          sellLabel = TextFormat("SELL (%dg)", sellVal);
        } else {
          sellLabel = "SELL";
        }
        int sellFsz = S(12);
        int slw = GameMeasureText(sellLabel, sellFsz);
        GameDrawText(sellLabel, sellX + (sellW - slw) / 2, sellY + (sellH - sellFsz) / 2,
                     sellFsz, (Color){255, 100, 100, 255});
      }
      } // end Inventory block

      // --- Synergy Panel (right of unit cards) ---
      {
        // Compute synergy tiers for blue team (display only)
        int synTier[SYNERGY_COUNT];
        int synMatchCount[SYNERGY_COUNT];
        (void)synMatchCount;
        for (int s = 0; s < (int)SYNERGY_COUNT; s++)
          synTier[s] = -1;
        for (int s = 0; s < (int)SYNERGY_COUNT; s++)
          synMatchCount[s] = 0;

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
              if (typePresent[r])
                matchCount++;
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

        }

        // Draw synergy panel rows (right of the cards)
        int synPanelX = cardsStartX + totalCardsW + S(16);
        int synPanelY = cardsY - S(2);
        int synRowH = S(22);
        int maxSynRows = hudCardH / synRowH;
        int activeSynCount = 0;
        for (int s = 0; s < (int)SYNERGY_COUNT; s++) {
          if (synTier[s] < 0)
            continue;
          if (activeSynCount >= maxSynRows)
            break;
          const SynergyDef *syn = &SYNERGY_DEFS[s];
          int rowY = synPanelY + activeSynCount * synRowH;

          // Hover detection for tooltip
          Rectangle synRow = {(float)synPanelX, (float)rowY, (float)S(160),
                              (float)synRowH};
          bool synHovered = CheckCollisionPointRec(GetMousePosition(), synRow);
          if (synHovered)
            hoverSynergyIdx = s;

          // Colored dot
          DrawCircle(synPanelX + S(5), rowY + synRowH / 2, S(4), syn->color);
          // Synergy name
          int textY = rowY + (synRowH - S(11)) / 2;
          GameDrawText(syn->name, synPanelX + S(14), textY, S(11), WHITE);
          // Tier pips
          int pipX =
              synPanelX + S(14) + GameMeasureText(syn->name, S(11)) + S(6);
          for (int t = 0; t < syn->tierCount; t++) {
            Color pipColor =
                (t <= synTier[s]) ? syn->color : (Color){60, 60, 80, 255};
            DrawCircle(pipX + t * S(10), rowY + synRowH / 2, S(3), pipColor);
          }
          // (buff text shown in hover tooltip only)
          activeSynCount++;
        }

      }

      // --- Drag ghost ---
      if (dragState.dragging && dragState.abilityId >= 0 &&
          dragState.abilityId < ABILITY_COUNT) {
        Vector2 dmouse = GetMousePosition();
        DrawRectangle((int)dmouse.x - 16, (int)dmouse.y - 16, 32, 32,
                      ABILITY_DEFS[dragState.abilityId].color);
        DrawRectangleLines((int)dmouse.x - 16, (int)dmouse.y - 16, 32, 32,
                           WHITE);
        const char *dabbr = ABILITY_DEFS[dragState.abilityId].abbrev;
        int daw = GameMeasureText(dabbr, S(13));
        GameDrawText(dabbr, (int)dmouse.x - daw / 2, (int)dmouse.y - 5, S(13),
                     WHITE);
      }

      // Item drag visual
      if (itemDrag.dragging && itemDrag.itemId >= 0 &&
          itemDrag.itemId < ITEM_COUNT) {
        Vector2 imouse = GetMousePosition();
        const ItemDef *idragDef = &ITEM_DEFS[itemDrag.itemId];
        DrawRectangle((int)imouse.x - 14, (int)imouse.y - 14, 28, 28,
                      idragDef->color);
        DrawRectangleLines((int)imouse.x - 14, (int)imouse.y - 14, 28, 28,
                           WHITE);
        char idLet[2] = {idragDef->name[0], '\0'};
        int idw = GameMeasureText(idLet, S(14));
        GameDrawTextOnColor(idLet, (int)imouse.x - idw / 2,
                            (int)imouse.y - S(7), S(14), idragDef->color);
      }
      // Cancel item drag if released without dropping on valid target
      if (itemDrag.dragging && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        itemDrag.dragging = false;
      }

      // --- Shop panel (only during PREP, above unit bar) ---
      if (phase == PHASE_PREP) {
        int shopY = hudTop + 2;
        int shopH = hudShopH - 2;
        DrawRectangle(0, shopY, hudSw, shopH, (Color){20, 20, 28, 240});
        DrawRectangle(0, shopY + shopH - 1, hudSw, 1, (Color){60, 60, 80, 255});

        // ROLL button (left) — show cost
        Rectangle rollBtn = {20, (float)(shopY + (shopH - S(34)) / 2), S(90), S(34)};
        bool canRoll = (playerGold >= rollCost);
        Color rollColor =
            canRoll ? (Color){180, 140, 40, 255} : (Color){80, 70, 40, 255};
        if (canRoll && CheckCollisionPointRec(GetMousePosition(), rollBtn))
          rollColor = (Color){220, 180, 60, 255};
        DrawRectangleRec(rollBtn, rollColor);
        DrawRectangleLinesEx(rollBtn, 2, (Color){120, 90, 20, 255});
        const char *rollText = TextFormat("ROLL %dg", rollCost);
        int rollW = GameMeasureText(rollText, S(16));
        GameDrawText(rollText, (int)(rollBtn.x + (S(90) - rollW) / 2),
                     (int)(rollBtn.y + (S(34) - S(16)) / 2), S(16), WHITE);
        GameDrawText("[R]", (int)(rollBtn.x + 2), (int)(rollBtn.y + 2), S(10),
                     (Color){255, 255, 200, 240});

        // Roll hotkey hint (first round only, until player uses it)
        if (currentRound == 0 && !usedRollHotkey) {
          const char *rhint = "Press [R] to reroll shop!";
          int rhSz = S(14);
          int rhW = GameMeasureText(rhint, rhSz);
          int rhX = (int)(rollBtn.x + rollBtn.width + 10);
          int rhY = (int)(rollBtn.y + (rollBtn.height - rhSz) / 2);
          float rpulse = 0.5f + 0.5f * sinf((float)GetTime() * 3.0f);
          unsigned char rhAlpha = (unsigned char)(160 + (int)(rpulse * 95));
          DrawRectangle(rhX - 6, rhY - 4, rhW + 12, rhSz + 8,
                        (Color){20, 20, 35, (unsigned char)(rhAlpha * 0.7f)});
          DrawRectangleLinesEx((Rectangle){(float)(rhX - 6), (float)(rhY - 4),
                                           (float)(rhW + 12),
                                           (float)(rhSz + 8)},
                               1, (Color){255, 220, 100, rhAlpha});
          GameDrawText(rhint, rhX, rhY, rhSz, (Color){255, 230, 120, rhAlpha});
        }

        // Shop ability cards (up to 6 slots, centered)
        int shopCardW = S(160);
        int shopCardH = S(38);
        int shopCardGap = 10;
        int totalShopW =
            activeShopSlots * shopCardW + (activeShopSlots - 1) * shopCardGap;
        int shopCardsX = (hudSw - totalShopW) / 2;
        for (int s = 0; s < activeShopSlots; s++) {
          int scx = shopCardsX + s * (shopCardW + shopCardGap);
          int scy = shopY + (shopH - shopCardH) / 2;
          if (shopSlots[s].abilityId >= 0 &&
              shopSlots[s].abilityId < ABILITY_COUNT) {
            const AbilityDef *sdef = &ABILITY_DEFS[shopSlots[s].abilityId];
            bool canAfford = (playerGold >= sdef->goldCost);
            Color cardBg = canAfford
                               ? ABILITY_DEFS[shopSlots[s].abilityId].color
                               : (Color){50, 50, 65, 255};
            bool shopHovered = CheckCollisionPointRec(
                GetMousePosition(),
                (Rectangle){(float)scx, (float)scy, (float)shopCardW,
                            (float)shopCardH});
            if (shopHovered) {
              hoverAbilityId = shopSlots[s].abilityId;
              hoverAbilityLevel = 0;
              shopHoverAbilityId = shopSlots[s].abilityId;
            }
            if (canAfford && shopHovered)
              cardBg =
                  (Color){cardBg.r + 30, cardBg.g + 30, cardBg.b + 30, 255};
            DrawRectangle(scx, scy, shopCardW, shopCardH, cardBg);
            if (shopSlots[s].locked)
              DrawRectangleLinesEx((Rectangle){(float)scx, (float)scy,
                                               (float)shopCardW,
                                               (float)shopCardH},
                                   2, (Color){0, 220, 220, 255});
            else
              DrawRectangleLines(scx, scy, shopCardW, shopCardH,
                                 (Color){90, 90, 110, 255});
            // Upgrade indicator glow: check if any unit or inventory has this ability
            {
              bool wouldUpgrade = false;
              for (int ui = 0; ui < unitCount && !wouldUpgrade; ui++) {
                if (!units[ui].active || units[ui].team != TEAM_BLUE) continue;
                for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++)
                  if (units[ui].abilities[a].abilityId == shopSlots[s].abilityId) { wouldUpgrade = true; break; }
              }
              for (int inv = 0; inv < MAX_INVENTORY_SLOTS && !wouldUpgrade; inv++)
                if (inventory[inv].abilityId == shopSlots[s].abilityId) { wouldUpgrade = true; break; }
              if (wouldUpgrade) {
                float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 4.0f);
                unsigned char upAlpha = (unsigned char)(150 + (int)(pulse * 105));
                DrawRectangleLinesEx((Rectangle){(float)scx, (float)scy, (float)shopCardW, (float)shopCardH},
                                     2, (Color){100, 255, 255, upAlpha});
              }
            }
            const char *sname =
                TextFormat("%s %dg", sdef->name, sdef->goldCost);
            int shopFontSz = S(14);
            int snw = GameMeasureText(sname, shopFontSz);
            if (canAfford) {
              GameDrawTextOnColor(sname, scx + (shopCardW - snw) / 2,
                                  scy + (shopCardH - shopFontSz) / 2,
                                  shopFontSz, cardBg);
            } else {
              GameDrawText(sname, scx + (shopCardW - snw) / 2,
                           scy + (shopCardH - shopFontSz) / 2, shopFontSz,
                           (Color){100, 100, 120, 255});
            }
          } else {
            int shopFontSz = S(14);
            DrawRectangle(scx, scy, shopCardW, shopCardH,
                          (Color){35, 35, 45, 255});
            DrawRectangleLines(scx, scy, shopCardW, shopCardH,
                               (Color){60, 60, 80, 255});
            GameDrawText(
                "SOLD",
                scx + (shopCardW - GameMeasureText("SOLD", shopFontSz)) / 2,
                scy + (shopCardH - shopFontSz) / 2, shopFontSz,
                (Color){60, 60, 80, 255});
          }
          // Keybind indicator
          const char *keyLabel = TextFormat("[%d]", s + 1);
          GameDrawText(keyLabel, scx + 2, scy + 2, S(12),
                       (Color){255, 255, 220, 240});
          // Lock indicator
          if (shopSlots[s].locked) {
            const char *lockTxt = "LOCKED";
            int lkFsz = S(10);
            int lkW = GameMeasureText(lockTxt, lkFsz);
            GameDrawText(lockTxt, scx + shopCardW - lkW - 3, scy + 2, lkFsz,
                         (Color){0, 220, 220, 255});
          }
        }

        // Hint: right-click to lock slots
        if (phase == PHASE_PREP && !usedLockHint) {
          const char *lhint = "Right-click to lock slots";
          int lhSz = S(12);
          int lhW = GameMeasureText(lhint, lhSz);
          int lhX = (hudSw - lhW) / 2;
          int lhY = shopY + shopCardH + S(14);
          float lhPulse = 0.5f + 0.5f * sinf((float)GetTime() * 3.0f);
          unsigned char lhAlpha = (unsigned char)(120 + (int)(lhPulse * 100));
          DrawRectangle(lhX - S(6), lhY - S(2), lhW + S(12), lhSz + S(4),
                       (Color){20, 20, 35, (unsigned char)(lhAlpha * 3 / 4)});
          DrawRectangleLines(lhX - S(6), lhY - S(2), lhW + S(12), lhSz + S(4),
                       (Color){255, 220, 100, (unsigned char)(lhAlpha / 2)});
          GameDrawText(lhint, lhX, lhY, lhSz, (Color){255, 230, 120, lhAlpha});
        }

        // Gold display (right side)
        const char *goldText = TextFormat("Gold: %d", playerGold);
        int gw = GameMeasureText(goldText, S(20));
        int goldY = shopY + (shopH - S(20)) / 2 - S(6);
        GameDrawText(goldText, hudSw - gw - 20, goldY, S(20),
                     (Color){240, 200, 60, 255});
        {
          const char *intText = TextFormat("+%d interest", playerGold / 5);
          int iw = GameMeasureText(intText, S(12));
          GameDrawText(intText, hudSw - iw - 20, goldY + S(20) + S(2), S(12),
                       (Color){200, 180, 60, 180});
        }

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
          DrawRectangle(hintX - 8, hintY - 4, hintW + 16, hintSz + 8,
                        (Color){20, 20, 35, (unsigned char)(hintAlpha * 0.7f)});
          DrawRectangleLinesEx(
              (Rectangle){(float)(hintX - 8), (float)(hintY - 4),
                          (float)(hintW + 16), (float)(hintSz + 8)},
              1, (Color){255, 220, 100, hintAlpha});
          GameDrawText(hint, hintX, hintY, hintSz,
                       (Color){255, 230, 120, hintAlpha});
        }
      }
    }

    // --- Confirm removal popup (drawn on top of everything) ---
    if (removeConfirmUnit >= 0 && phase == PHASE_PREP) {
      int sw2 = GetScreenWidth(), sh2 = GetScreenHeight();
      DrawRectangle(0, 0, sw2, sh2, (Color){0, 0, 0, 120}); // dim overlay
      int popW = 280, popH = 110;
      int popX = sw2 / 2 - popW / 2;
      int popY = sh2 / 2 - popH / 2;
      DrawRectangle(popX, popY, popW, popH, (Color){40, 40, 55, 240});
      DrawRectangleLinesEx(
          (Rectangle){(float)popX, (float)popY, (float)popW, (float)popH}, 2,
          (Color){180, 60, 60, 255});
      const char *confirmText = "Remove this unit?";
      int ctw = GameMeasureText(confirmText, 20);
      GameDrawText(confirmText, popX + (popW - ctw) / 2, popY + 14, 20, WHITE);
      // Abilities returned note
      const char *noteText = "(abilities stay on figurine)";
      int ntw = GameMeasureText(noteText, 12);
      GameDrawText(noteText, popX + (popW - ntw) / 2, popY + 40, 12,
                   (Color){160, 160, 180, 255});
      // Yes / No buttons
      int rmBtnW = 100, rmBtnH = 30;
      Rectangle yesBtn = {(float)(popX + 24),
                          (float)(popY + popH - rmBtnH - 12), (float)rmBtnW,
                          (float)rmBtnH};
      Rectangle noBtn = {(float)(popX + popW - rmBtnW - 24),
                         (float)(popY + popH - rmBtnH - 12), (float)rmBtnW,
                         (float)rmBtnH};
      Color yesBg = (Color){180, 50, 50, 255};
      Color noBg = (Color){60, 60, 80, 255};
      if (CheckCollisionPointRec(GetMousePosition(), yesBtn))
        yesBg = (Color){230, 70, 70, 255};
      if (CheckCollisionPointRec(GetMousePosition(), noBtn))
        noBg = (Color){80, 80, 110, 255};
      DrawRectangleRec(yesBtn, yesBg);
      DrawRectangleRec(noBtn, noBg);
      DrawRectangleLinesEx(yesBtn, 1, (Color){120, 40, 40, 255});
      DrawRectangleLinesEx(noBtn, 1, (Color){80, 80, 100, 255});
      int yw = GameMeasureText("YES", 16), nw = GameMeasureText("NO", 16);
      GameDrawText("YES", (int)(yesBtn.x + (rmBtnW - yw) / 2),
                   (int)(yesBtn.y + 7), 16, WHITE);
      GameDrawText("NO", (int)(noBtn.x + (rmBtnW - nw) / 2), (int)(noBtn.y + 7),
                   16, WHITE);
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
      typedef struct {
        const char *label;
        int valueIndex;
        bool isPercent;
      } StatLine;
      StatLine statLines[8];
      int numStatLines = 0;

      switch (hoverAbilityId) {
      case ABILITY_MAGIC_MISSILE:
        statLines[numStatLines++] = (StatLine){"% Max HP", AV_MM_DAMAGE, true};
        statLines[numStatLines++] = (StatLine){"Stun", AV_MM_STUN_DUR, false};
        statLines[numStatLines++] =
            (StatLine){"Range", -1, false}; // uses .range[]
        break;
      case ABILITY_DIG:
        statLines[numStatLines++] =
            (StatLine){"HP Thresh", AV_DIG_HP_THRESH, true};
        statLines[numStatLines++] =
            (StatLine){"Heal Dur", AV_DIG_HEAL_DUR, false};
        break;
      case ABILITY_VACUUM:
        statLines[numStatLines++] = (StatLine){"Radius", AV_VAC_RADIUS, false};
        statLines[numStatLines++] = (StatLine){"Stun", AV_VAC_STUN_DUR, false};
        break;
      case ABILITY_CHAIN_FROST:
        statLines[numStatLines++] = (StatLine){"Damage", AV_CF_DAMAGE, false};
        statLines[numStatLines++] = (StatLine){"Bounces", AV_CF_BOUNCES, false};
        statLines[numStatLines++] =
            (StatLine){"Dmg/Bounce", AV_CF_DMG_INCREASE, false};
        break;
      case ABILITY_BLOOD_RAGE:
        statLines[numStatLines++] =
            (StatLine){"Lifesteal", AV_BR_LIFESTEAL, true};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_BR_DURATION, false};
        break;
      case ABILITY_EARTHQUAKE:
        statLines[numStatLines++] = (StatLine){"Damage", AV_EQ_DAMAGE, false};
        statLines[numStatLines++] = (StatLine){"Radius", AV_EQ_RADIUS, false};
        statLines[numStatLines++] = (StatLine){"Stun", AV_EQ_STUN_DUR, false};
        break;
      case ABILITY_SPELL_PROTECT:
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_SP_DURATION, false};
        break;
      case ABILITY_CRAGGY_ARMOR:
        statLines[numStatLines++] = (StatLine){"Armor", AV_CA_ARMOR, false};
        statLines[numStatLines++] =
            (StatLine){"Stun %", AV_CA_STUN_CHANCE, true};
        statLines[numStatLines++] =
            (StatLine){"Stun Dur", AV_CA_STUN_DUR, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_CA_DURATION, false};
        break;
      case ABILITY_STONE_GAZE:
        statLines[numStatLines++] =
            (StatLine){"Gaze Time", AV_SG_GAZE_THRESH, false};
        statLines[numStatLines++] = (StatLine){"Stun", AV_SG_STUN_DUR, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_SG_DURATION, false};
        break;
      case ABILITY_SUNDER:
        statLines[numStatLines++] =
            (StatLine){"HP Thresh", AV_SU_HP_THRESH, true};
        break;
      case ABILITY_FISSURE:
        statLines[numStatLines++] = (StatLine){"Damage", AV_FI_DAMAGE, false};
        statLines[numStatLines++] = (StatLine){"Length", AV_FI_LENGTH, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_FI_DURATION, false};
        statLines[numStatLines++] = (StatLine){"Stun", AV_FI_STUN_DUR, false};
        break;
      case ABILITY_VLAD_AURA:
        statLines[numStatLines++] =
            (StatLine){"Lifesteal", AV_VA_LIFESTEAL, true};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_VA_DURATION, false};
        break;
      case ABILITY_MAELSTROM:
        statLines[numStatLines++] =
            (StatLine){"Proc %", AV_ML_PROC_CHANCE, true};
        statLines[numStatLines++] = (StatLine){"Damage", AV_ML_DAMAGE, false};
        statLines[numStatLines++] = (StatLine){"Bounces", AV_ML_BOUNCES, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_ML_DURATION, false};
        break;
      case ABILITY_SWAP:
        statLines[numStatLines++] =
            (StatLine){"Shield HP", AV_SW_SHIELD, false};
        statLines[numStatLines++] =
            (StatLine){"Shield Dur", AV_SW_SHIELD_DUR, false};
        break;
      case ABILITY_APHOTIC_SHIELD:
        statLines[numStatLines++] =
            (StatLine){"Shield HP", AV_AS_SHIELD, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_AS_DURATION, false};
        break;
      case ABILITY_HOOK:
        statLines[numStatLines++] =
            (StatLine){"Base Dmg", AV_HK_BASE_DMG, false};
        statLines[numStatLines++] =
            (StatLine){"Dmg/Dist", AV_HK_DMG_PER_DIST, false};
        statLines[numStatLines++] = (StatLine){"Range", AV_HK_RANGE, false};
        break;
      case ABILITY_PRIMAL_CHARGE:
        statLines[numStatLines++] = (StatLine){"Damage", AV_PC_DAMAGE, false};
        statLines[numStatLines++] =
            (StatLine){"Knockback", AV_PC_KNOCKBACK, false};
        break;
      case ABILITY_MULTICAST:
        statLines[numStatLines++] =
            (StatLine){"2x Chance", AV_MC_CHANCE_2X, true};
        statLines[numStatLines++] =
            (StatLine){"3x Chance", AV_MC_CHANCE_3X, true};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_MC_DURATION, false};
        break;
      case ABILITY_SHARE_PAIN:
        statLines[numStatLines++] =
            (StatLine){"Share %", AV_SPP_SHARE_PCT, true};
        statLines[numStatLines++] = (StatLine){"Radius", AV_SPP_RADIUS, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_SPP_DURATION, false};
        break;
      case ABILITY_VENOM_STRIKE:
        statLines[numStatLines++] =
            (StatLine){"Poison DPS", AV_VS_POISON_DPS, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_VS_DURATION, false};
        break;
      case ABILITY_TOXIC_CLOUD:
        statLines[numStatLines++] =
            (StatLine){"Poison DPS", AV_TC_POISON_DPS, false};
        statLines[numStatLines++] = (StatLine){"Radius", AV_TC_RADIUS, false};
        statLines[numStatLines++] =
            (StatLine){"Duration", AV_TC_DURATION, false};
        break;
      case ABILITY_FERVOR:
        statLines[numStatLines++] =
            (StatLine){"Spd/Stack", AV_FV_SPEED_RED, true};
        statLines[numStatLines++] =
            (StatLine){"Max Stacks", AV_FV_MAX_STACKS, false};
        break;
      default:
        break;
      }
      // Add cooldown line for non-passive abilities
      int cdLineIdx = -1;
      if (!tipDef->isPassive) {
        cdLineIdx = numStatLines;
        numStatLines++;
      }

      int tipW = S(300);
      int tipH = S(50) + numStatLines * S(18);
      int tipX = (int)mpos.x + S(10);
      int tipY = (int)mpos.y - tipH - S(8);
      if (tipX + tipW > GetScreenWidth())
        tipX = (int)mpos.x - tipW - S(8);
      if (tipY < 0)
        tipY = (int)mpos.y + S(12);
      DrawRectangle(tipX, tipY, tipW, tipH, (Color){20, 20, 30, 230});
      DrawRectangleLines(tipX, tipY, tipW, tipH, (Color){100, 100, 130, 255});
      GameDrawText(tipDef->name, tipX + S(6), tipY + S(4), S(16), WHITE);
      const char *lvlText =
          TextFormat("Lvl:%d/%d", hoverAbilityLevel + 1, ABILITY_MAX_LEVELS);
      int lvlW = GameMeasureText(lvlText, S(12));
      GameDrawText(lvlText, tipX + tipW - lvlW - S(6), tipY + S(6), S(12),
                   (Color){180, 180, 200, 255});
      if (tipDef->isPassive) {
        GameDrawText("Passive", tipX + S(6), tipY + S(22), S(12),
                     (Color){120, 200, 140, 255});
        int passW = GameMeasureText("Passive", S(12));
        GameDrawText(TextFormat(" - %s", tipDef->description),
                     tipX + S(6) + passW, tipY + S(22), S(12),
                     (Color){180, 180, 200, 255});
      } else {
        GameDrawText(tipDef->description, tipX + S(6), tipY + S(22), S(12),
                     (Color){180, 180, 200, 255});
      }

      Color dimStatColor = {100, 100, 120, 255};
      // Rolling 3-window: show 3 levels centered on current
      int winStart = hoverAbilityLevel <= 0
                         ? 0
                         : (hoverAbilityLevel >= ABILITY_MAX_LEVELS - 1
                                ? ABILITY_MAX_LEVELS - 3
                                : hoverAbilityLevel - 1);
      if (winStart < 0)
        winStart = 0;
      int winEnd = winStart + 3;
      if (winEnd > ABILITY_MAX_LEVELS)
        winEnd = ABILITY_MAX_LEVELS;

      int lineY = tipY + S(40);
      for (int sl = 0; sl < numStatLines; sl++) {
        int lx = tipX + S(6);
        if (sl == cdLineIdx) {
          // Cooldown line
          const char *cdLabel = "CD: ";
          GameDrawText(cdLabel, lx, lineY, S(12), (Color){180, 180, 200, 255});
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
          GameDrawText(labelBuf, lx, lineY, S(12), (Color){180, 180, 200, 255});
          lx += GameMeasureText(labelBuf, S(12));
          for (int lv = winStart; lv < winEnd; lv++) {
            float v = (statLines[sl].valueIndex >= 0)
                          ? tipDef->values[lv][statLines[sl].valueIndex]
                          : tipDef->range[lv];
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

    if (hoverSynergyIdx >= 0 && hoverSynergyIdx < (int)SYNERGY_COUNT &&
        hoverSynergyTimer >= synergyTooltipDelay) {
      const SynergyDef *syn = &SYNERGY_DEFS[hoverSynergyIdx];
      Vector2 mpos = GetMousePosition();

      // Count matching blue units for the tooltip
      int synMatch = 0;
      if (syn->requireAllTypes) {
        bool tp[4] = {0};
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active || units[i].team != TEAM_BLUE)
            continue;
          for (int r = 0; r < syn->requiredTypeCount; r++)
            if (units[i].typeIndex == syn->requiredTypes[r])
              tp[r] = true;
        }
        for (int r = 0; r < syn->requiredTypeCount; r++)
          if (tp[r])
            synMatch++;
      } else {
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active || units[i].team != TEAM_BLUE)
            continue;
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
        if (synMatch >= syn->tiers[t].minUnits)
          curTier = t;

      // Next tier threshold
      int nextThresh = 0;
      if (curTier + 1 < syn->tierCount)
        nextThresh = syn->tiers[curTier + 1].minUnits;

      // Build tooltip content
      const char *tierLabel = (curTier >= 0)
                                  ? TextFormat("%s %s", syn->name,
                                               (curTier == 0)   ? "I"
                                               : (curTier == 1) ? "II"
                                                                : "III")
                                  : syn->name;
      const char *bonusText = (curTier >= 0 && syn->buffDesc[curTier])
                                  ? syn->buffDesc[curTier]
                                  : "Inactive";
      const char *countText;
      if (syn->requireAllTypes)
        countText = TextFormat("%d/%d types", synMatch, syn->requiredTypeCount);
      else {
        int maxNeeded = syn->tiers[syn->tierCount - 1].minUnits;
        countText = TextFormat("%d/%d %s", synMatch, maxNeeded,
                               (syn->requiredTypeCount == 1)
                                   ? GetUnitTypeName(syn->requiredTypes[0])
                                   : "units");
      }

      int tipW = S(300);
      int tipH = S(50) + S(18) * 2;
      if (nextThresh > 0)
        tipH += S(18);
      int tipX = (int)mpos.x + S(10);
      int tipY = (int)mpos.y - tipH - S(8);
      if (tipX + tipW > GetScreenWidth())
        tipX = (int)mpos.x - tipW - S(8);
      if (tipY < 0)
        tipY = (int)mpos.y + S(12);
      DrawRectangle(tipX, tipY, tipW, tipH, (Color){20, 20, 30, 230});
      DrawRectangleLines(tipX, tipY, tipW, tipH, (Color){100, 100, 130, 255});
      GameDrawText(tierLabel, tipX + S(6), tipY + S(4), S(16), WHITE);
      GameDrawText(countText, tipX + S(6), tipY + S(22), S(12),
                   (Color){180, 180, 200, 255});
      GameDrawText(bonusText, tipX + S(6), tipY + S(40), S(12),
                   (Color){200, 200, 220, 255});
      if (nextThresh > 0) {
        const char *nextText = TextFormat("Next: %d for tier %s", nextThresh,
                                          (curTier + 1 == 1) ? "II" : "III");
        GameDrawText(nextText, tipX + S(6), tipY + S(58), S(12),
                     (Color){120, 120, 140, 200});
      }
    }

    //==============================================================================
    // PHASE_PLAZA DRAWING (2D overlays on top of the 3D world)
    //==============================================================================
    if (phase == PHASE_PLAZA) {
      int msw = GetScreenWidth();
      int msh = GetScreenHeight();

      // Title text (floating over the 3D scene, scales with window)
      const char *title = "Relic Rivals";
      float titleScale = (float)msh / 720.0f;
      int titleSize = (int)(128.0f * titleScale);
      if (titleSize < 48) titleSize = 48;
      int tw = GameMeasureText(title, titleSize);
      int titleY = (int)(60.0f * titleScale);
      GameDrawText(title, msw / 2 - tw / 2, titleY, titleSize,
                   (Color){245, 245, 235, 220});

      const char *subtitle = lobbySelection.heroSelected
                                 ? "Hero selected!"
                                 : "Select a hero to begin";
      int subSize = (int)(54.0f * titleScale);
      if (subSize < 20) subSize = 20;
      int sw2 = GameMeasureText(subtitle, subSize);
      int subY = titleY + titleSize + (int)(10.0f * titleScale);
      GameDrawText(subtitle, msw / 2 - sw2 / 2, subY, subSize,
                   (Color){200, 190, 175, 160});

      // Draw hover arrow + tooltip above roaming red units (hero selection)
      if (!lobbySelection.heroSelected) {
        for (int i = 0; i < unitCount; i++) {
          if (!units[i].active || units[i].team != TEAM_RED)
            continue;
          Vector2 uScreen = GetWorldToScreen(
              (Vector3){units[i].position.x, units[i].position.y + 12.0f,
                        units[i].position.z},
              camera);
          int ux = (int)uScreen.x;
          int uy = (int)uScreen.y;

          // Hover arrow + tooltip
          if (i == plazaHoverUnit) {
            // Bouncing gold arrow
            float bounce = sinf((float)GetTime() * 5.0f) * 6.0f;
            int arrowY = uy - 30 + (int)bounce;
            const char *arrow = "v";
            int arrowW = GameMeasureText(arrow, 24);
            GameDrawText(arrow, ux - arrowW / 2, arrowY, 24, GOLD);

            // Class name
            const char *className = GetUnitTypeName(units[i].typeIndex);
            int cnW = GameMeasureText(className, 20);
            GameDrawText(className, ux - cnW / 2, uy + 4, 20, WHITE);

            // Archetype description
            const char *archetype = "Fighter";
            switch (units[i].typeIndex) {
            case 0:
              archetype = "Slow but Unstoppable";
              break;
            case 1:
              archetype = "Fast & Frenzied";
              break;
            case 2:
              archetype = "Ranged Glass Cannon";
              break;
            case 5:
              archetype = "High-Damage Bruiser";
              break;
            }
            int atW = GameMeasureText(archetype, 14);
            GameDrawText(archetype, ux - atW / 2, uy + 26, 14,
                         (Color){180, 180, 200, 180});

            // "Click to recruit"
            const char *recruit = "Click to recruit";
            int rcW = GameMeasureText(recruit, 14);
            GameDrawText(recruit, ux - rcW / 2, uy + 44, 14,
                         (Color){255, 230, 100, 200});
          }
        }
      }

      // Draw floating 2D labels above 3D objects
      {
        Vector2 trophyScreen = GetWorldToScreen(
            (Vector3){trophyPos.x, trophyPos.y + 14.0f, trophyPos.z}, camera);
        const char *tLabel = "LEADERBOARD";
        int tlw = GameMeasureText(tLabel, 14);
        Color tlCol =
            (plazaHoverObject == 1) ? YELLOW : (Color){200, 200, 220, 200};
        GameDrawText(tLabel, (int)trophyScreen.x - tlw / 2, (int)trophyScreen.y,
                     14, tlCol);

        Vector2 doorScreen = GetWorldToScreen(
            (Vector3){doorPos.x, doorPos.y + 18.0f, doorPos.z}, camera);
        const char *dLabel = "MULTIPLAYER";
        int dlw = GameMeasureText(dLabel, 14);
        Color dlCol =
            (plazaHoverObject == 2) ? YELLOW : (Color){200, 200, 220, 200};
        GameDrawText(dLabel, (int)doorScreen.x - dlw / 2, (int)doorScreen.y, 14,
                     dlCol);
      }

      // Leaderboard overlay (reused from old menu)
      if (showLeaderboard) {
        DrawRectangle(0, 0, msw, msh, (Color){0, 0, 0, 180});

        int panelW = 600, panelH = 500;
        int panelX = msw / 2 - panelW / 2;
        int panelY = msh / 2 - panelH / 2;
        DrawRectangle(panelX, panelY, panelW, panelH, (Color){24, 24, 32, 240});
        DrawRectangleLinesEx((Rectangle){(float)panelX, (float)panelY,
                                         (float)panelW, (float)panelH},
                             2, (Color){100, 100, 130, 255});

        const char *lbTitle = "LEADERBOARD";
        int ltw = GameMeasureText(lbTitle, 24);
        GameDrawText(lbTitle, panelX + panelW / 2 - ltw / 2, panelY + 10, 24,
                     GOLD);

        Rectangle closeBtn = {(float)(panelX + panelW - 40), (float)panelY, 40,
                              40};
        Color closeBg = (Color){180, 50, 50, 200};
        if (CheckCollisionPointRec(GetMousePosition(), closeBtn))
          closeBg = (Color){230, 70, 70, 255};
        DrawRectangleRec(closeBtn, closeBg);
        int xw = GameMeasureText("X", 18);
        GameDrawText("X", (int)(closeBtn.x + 20 - xw / 2),
                     (int)(closeBtn.y + 11), 18, WHITE);

        int listTop = panelY + 50;
        int listH = panelH - 60;
        int rowH = 70;
        BeginScissorMode(panelX + 4, listTop, panelW - 8, listH);
        for (int e = 0; e < leaderboard.entryCount; e++) {
          int rowY = listTop + e * rowH - leaderboardScroll;
          if (rowY + rowH < listTop || rowY > listTop + listH)
            continue;

          LeaderboardEntry *le = &leaderboard.entries[e];
          Color rowBg = (e % 2 == 0) ? (Color){30, 30, 42, 255}
                                     : (Color){36, 36, 48, 255};
          DrawRectangle(panelX + 4, rowY, panelW - 8, rowH - 2, rowBg);

          const char *rankText = TextFormat("#%d", e + 1);
          GameDrawText(rankText, panelX + 12, rowY + 8, 20, GOLD);
          const char *roundText = TextFormat("Wave %d", le->highestRound);
          GameDrawText(roundText, panelX + 60, rowY + 8, 18, WHITE);
          GameDrawText(le->playerName, panelX + 180, rowY + 8, 16,
                       (Color){180, 180, 200, 255});

          int ux = panelX + 180;
          int uy = rowY + 32;
          for (int u = 0; u < le->unitCount && u < BLUE_TEAM_MAX_SIZE; u++) {
            SavedUnit *su = &le->units[u];
            const char *uname = (su->typeIndex < unitTypeCount)
                                    ? unitTypes[su->typeIndex].name
                                    : "???";
            GameDrawText(uname, ux, uy, 12, (Color){150, 180, 255, 255});
            int nameW = GameMeasureText(uname, 12);
            int gridX = ux + nameW + 6;
            int miniSize = 14, miniGap = 2;
            for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
              int col = a % 2, row = a / 2;
              int ax = gridX + col * (miniSize + miniGap);
              int ay = uy + row * (miniSize + miniGap) - 4;
              if (su->abilities[a].abilityId >= 0 &&
                  su->abilities[a].abilityId < ABILITY_COUNT) {
                DrawRectangle(ax, ay, miniSize, miniSize,
                              ABILITY_DEFS[su->abilities[a].abilityId].color);
                const char *abbr =
                    ABILITY_DEFS[su->abilities[a].abilityId].abbrev;
                GameDrawText(abbr, ax + 1, ay + 2, 7, WHITE);
              } else {
                DrawRectangle(ax, ay, miniSize, miniSize,
                              (Color){40, 40, 55, 255});
              }
            }
            ux += nameW + 6 + 2 * (14 + 2) + 12;
          }
        }
        EndScissorMode();

        if (leaderboard.entryCount == 0) {
          const char *emptyText = "No entries yet - play and Set in Stone!";
          int etw = GameMeasureText(emptyText, 16);
          GameDrawText(emptyText, panelX + panelW / 2 - etw / 2,
                       panelY + panelH / 2, 16, (Color){100, 100, 120, 255});
        }
      }

      // Multiplayer panel overlay
      if (showMultiplayerPanel) {
        DrawRectangle(0, 0, msw, msh, (Color){0, 0, 0, 140});

        int panelW = 400, panelH = 340;
        int panelX = msw / 2 - panelW / 2;
        int panelY = msh / 2 - panelH / 2;
        DrawRectangle(panelX, panelY, panelW, panelH, (Color){24, 24, 32, 240});
        DrawRectangleLinesEx((Rectangle){(float)panelX, (float)panelY,
                                         (float)panelW, (float)panelH},
                             2, (Color){100, 100, 130, 255});

        const char *mpTitle = "MULTIPLAYER";
        int mptw = GameMeasureText(mpTitle, 24);
        GameDrawText(mpTitle, panelX + panelW / 2 - mptw / 2, panelY + 10, 24,
                     (Color){200, 180, 255, 255});

        // Close button
        Rectangle closeBtn = {(float)(panelX + panelW - 36),
                              (float)(panelY + 4), 32, 32};
        Color closeBg = (Color){180, 50, 50, 200};
        if (CheckCollisionPointRec(GetMousePosition(), closeBtn))
          closeBg = (Color){230, 70, 70, 255};
        DrawRectangleRec(closeBtn, closeBg);
        GameDrawText("X", (int)(closeBtn.x + 10), (int)(closeBtn.y + 7), 18,
                     WHITE);

        // Name input
        GameDrawText("Player Name:", panelX + 50, panelY + 45, 14,
                     (Color){180, 180, 200, 255});
        Rectangle nameField = {(float)(panelX + 50), (float)(panelY + 60),
                               (float)(panelW - 100), 36};
        Color nameBg = nameInputActive ? (Color){50, 50, 70, 255}
                                       : (Color){35, 35, 50, 255};
        DrawRectangleRec(nameField, nameBg);
        DrawRectangleLinesEx(nameField, 2,
                             nameInputActive ? (Color){150, 140, 200, 255}
                                             : (Color){80, 80, 100, 255});
        GameDrawText(playerName, panelX + 58, panelY + 69, 18, WHITE);
        if (nameInputActive) {
          float blinkTime = (float)GetTime();
          if ((int)(blinkTime * 2) % 2 == 0) {
            int cw = GameMeasureText(playerName, 18);
            DrawRectangle(panelX + 58 + cw + 2, panelY + 69, 2, 18, WHITE);
          }
        }

#ifdef USE_EOS
        // LAN / Online toggle tabs
        {
          int tabW = (panelW - 100) / 2;
          Rectangle lanTab = {(float)(panelX + 50), (float)(panelY + 105),
                              (float)tabW, 24};
          Rectangle onlineTab = {(float)(panelX + 50 + tabW),
                                 (float)(panelY + 105), (float)tabW, 24};
          Color lanBg =
              useEos ? (Color){40, 40, 55, 255} : (Color){60, 100, 140, 255};
          Color onlBg =
              useEos ? (Color){60, 100, 140, 255} : (Color){40, 40, 55, 255};
          bool eosReady = eos_is_logged_in();
          // Hover highlights the inactive (clickable) tab
          if (useEos && CheckCollisionPointRec(GetMousePosition(), lanTab))
            lanBg = (Color){70, 120, 160, 255};
          if (!useEos && eosReady &&
              CheckCollisionPointRec(GetMousePosition(), onlineTab))
            onlBg = (Color){70, 120, 160, 255};
          DrawRectangleRec(lanTab, lanBg);
          DrawRectangleRec(onlineTab, onlBg);
          DrawRectangleLinesEx(lanTab, 1, (Color){80, 80, 100, 255});
          DrawRectangleLinesEx(onlineTab, 1, (Color){80, 80, 100, 255});
          const char *lanText = "LAN";
          const char *onlText = eosReady ? "ONLINE" : "ONLINE ...";
          int lw = GameMeasureText(lanText, 12);
          int ow = GameMeasureText(onlText, 12);
          GameDrawText(lanText, (int)(lanTab.x + tabW / 2 - lw / 2),
                       (int)(lanTab.y + 6), 12, WHITE);
          Color onlTextColor = eosReady ? WHITE : (Color){120, 120, 140, 255};
          GameDrawText(onlText, (int)(onlineTab.x + tabW / 2 - ow / 2),
                       (int)(onlineTab.y + 6), 12, onlTextColor);
        }
#endif

        // HOST GAME button
        Rectangle hostBtn = {(float)(panelX + 50), (float)(panelY + 140),
                             (float)(panelW - 100), 40};
        Color hBg = (Color){40, 130, 60, 255};
        if (CheckCollisionPointRec(GetMousePosition(), hostBtn))
          hBg = (Color){50, 170, 70, 255};
        DrawRectangleRec(hostBtn, hBg);
        DrawRectangleLinesEx(hostBtn, 2, (Color){30, 100, 40, 255});
#ifdef USE_EOS
        const char *hText = useEos ? "HOST ONLINE" : "HOST GAME";
#else
        const char *hText = "HOST GAME";
#endif
        int htw = GameMeasureText(hText, 16);
        GameDrawText(hText, (int)(hostBtn.x + (panelW - 100) / 2 - htw / 2),
                     (int)(hostBtn.y + 12), 16, WHITE);

        // JOIN GAME button
#ifdef USE_EOS
        bool ipReady =
            useEos ? (joinLobbyCodeLen == LOBBY_CODE_LEN) : (joinIpLen > 0);
#else
        bool ipReady = (joinIpLen > 0);
#endif
        Rectangle joinBtn = {(float)(panelX + 50), (float)(panelY + 200),
                             (float)(panelW - 100), 40};
        Color jBg =
            ipReady ? (Color){160, 100, 30, 255} : (Color){80, 80, 80, 255};
        if (ipReady && CheckCollisionPointRec(GetMousePosition(), joinBtn))
          jBg = (Color){200, 130, 40, 255};
        DrawRectangleRec(joinBtn, jBg);
        DrawRectangleLinesEx(joinBtn, 2, (Color){100, 70, 20, 255});
#ifdef USE_EOS
        const char *jText = useEos ? "JOIN ONLINE" : "JOIN GAME";
#else
        const char *jText = "JOIN GAME";
#endif
        int jtw = GameMeasureText(jText, 16);
        GameDrawText(jText, (int)(joinBtn.x + (panelW - 100) / 2 - jtw / 2),
                     (int)(joinBtn.y + 12), 16, WHITE);

        // IP address or Lobby Code input
#ifdef USE_EOS
        if (useEos) {
          GameDrawText("Lobby Code:", panelX + 50, panelY + 250, 12,
                       (Color){150, 150, 170, 255});
          Rectangle codeBox = {(float)(panelX + 50), (float)(panelY + 268), 120,
                               30};
          DrawRectangleRec(codeBox, (Color){35, 35, 50, 255});
          DrawRectangleLinesEx(codeBox, 2, (Color){80, 80, 100, 255});
          GameDrawText(joinLobbyCode, panelX + 58, panelY + 274, 18, WHITE);
          if (!nameInputActive) {
            float blinkTime = (float)GetTime();
            if ((int)(blinkTime * 2) % 2 == 0) {
              int cw2 = GameMeasureText(joinLobbyCode, 18);
              DrawRectangle(panelX + 58 + cw2 + 2, panelY + 274, 2, 18, WHITE);
            }
          }
        } else
#endif
        {
          GameDrawText("Host IP Address:", panelX + 50, panelY + 250, 12,
                       (Color){150, 150, 170, 255});
          Rectangle ipBox = {(float)(panelX + 50), (float)(panelY + 268), 200,
                             30};
          DrawRectangleRec(ipBox, (Color){35, 35, 50, 255});
          DrawRectangleLinesEx(ipBox, 2, (Color){80, 80, 100, 255});
          GameDrawText(joinIpAddress, panelX + 58, panelY + 274, 18, WHITE);
          // Blinking cursor
          if (!nameInputActive) {
            float blinkTime = (float)GetTime();
            if ((int)(blinkTime * 2) % 2 == 0) {
              int ipw = GameMeasureText(joinIpAddress, 18);
              DrawRectangle(panelX + 58 + ipw + 2, panelY + 274, 2, 18, WHITE);
            }
          }
        }

        // Error message
        if (menuError[0]) {
          int ew = GameMeasureText(menuError, 12);
          GameDrawText(menuError, panelX + panelW / 2 - ew / 2,
                       panelY + panelH - 20, 12, RED);
        }
      }
    }

    //==============================================================================
    // PHASE_LOBBY DRAWING
    //==============================================================================
    if (phase == PHASE_LOBBY) {
      int lsw = GetScreenWidth();
      int lsh = GetScreenHeight();
      DrawRectangle(0, 0, lsw, lsh, (Color){20, 20, 30, 255});

      // Title
      const char *lobbyTitle = "SELECT YOUR ARMY";
      int ltw = GameMeasureText(lobbyTitle, 30);
      GameDrawText(lobbyTitle, lsw / 2 - ltw / 2, 40, 30,
                   (Color){200, 180, 255, 255});

      // Party slots (top row)
      int slotW = 100, slotH = 60, slotGap = 16;
      int slotTotalW =
          BLUE_TEAM_MAX_SIZE * slotW + (BLUE_TEAM_MAX_SIZE - 1) * slotGap;
      int slotStartX = lsw / 2 - slotTotalW / 2;
      int slotY = lsh / 2 - 80;

      for (int s = 0; s < BLUE_TEAM_MAX_SIZE; s++) {
        int sx = slotStartX + s * (slotW + slotGap);
        bool isActive = (s == mpLobby.activeSlot && !mpLobby.selectionComplete);
        bool isFilled = (s < mpLobby.activeSlot);

        // Glow for active slot
        if (isActive) {
          float pulse = 0.5f + 0.5f * sinf(mpLobby.glowTimer * 4.0f);
          unsigned char ga = (unsigned char)(80 + pulse * 80);
          DrawRectangle(sx - 3, slotY - 3, slotW + 6, slotH + 6,
                        (Color){255, 200, 50, ga});
        }

        Color slotBg =
            isFilled ? (Color){40, 50, 70, 255} : (Color){30, 30, 45, 255};
        DrawRectangle(sx, slotY, slotW, slotH, slotBg);
        Color slotBorder = isActive ? GOLD : (Color){60, 60, 80, 255};
        DrawRectangleLinesEx(
            (Rectangle){(float)sx, (float)slotY, (float)slotW, (float)slotH}, 2,
            slotBorder);

        if (isFilled) {
          const char *sName = GetUnitTypeName(mpLobby.slotTypes[s]);
          int snw = GameMeasureText(sName, 16);
          GameDrawText(sName, sx + slotW / 2 - snw / 2, slotY + 10, 16, WHITE);
          // Click hint
          const char *undoHint = "(click to undo)";
          int uhw = GameMeasureText(undoHint, 10);
          GameDrawText(undoHint, sx + slotW / 2 - uhw / 2, slotY + 36, 10,
                       (Color){120, 120, 140, 200});
        } else {
          const char *emptyLabel = isActive ? ">" : "-";
          int elw = GameMeasureText(emptyLabel, 20);
          GameDrawText(emptyLabel, sx + slotW / 2 - elw / 2, slotY + 18, 20,
                       (Color){80, 80, 100, 255});
        }
      }

      // Class picker buttons
      int cpBtnW = 140, cpBtnH = 50, cpBtnGap = 16;
      int cpTotalW = VALID_UNIT_TYPE_COUNT * cpBtnW +
                     (VALID_UNIT_TYPE_COUNT - 1) * cpBtnGap;
      int cpStartX = lsw / 2 - cpTotalW / 2;
      int cpY = lsh / 2 + 40;
      Vector2 lmouse = GetMousePosition();

      for (int c = 0; c < VALID_UNIT_TYPE_COUNT; c++) {
        int bx = cpStartX + c * (cpBtnW + cpBtnGap);
        Rectangle cpBtn = {(float)bx, (float)cpY, (float)cpBtnW, (float)cpBtnH};
        bool hover = CheckCollisionPointRec(lmouse, cpBtn) &&
                     mpLobby.activeSlot < BLUE_TEAM_MAX_SIZE;
        Color cpBg =
            hover ? (Color){60, 80, 120, 255} : (Color){40, 45, 65, 255};
        if (mpLobby.selectionComplete)
          cpBg = (Color){30, 30, 40, 255};
        DrawRectangleRec(cpBtn, cpBg);
        DrawRectangleLinesEx(cpBtn, 2,
                             hover ? (Color){150, 170, 220, 255}
                                   : (Color){70, 70, 100, 255});
        const char *cName = GetUnitTypeName(VALID_UNIT_TYPES[c]);
        int cnw = GameMeasureText(cName, 18);
        Color cCol =
            mpLobby.selectionComplete ? (Color){80, 80, 80, 255} : WHITE;
        GameDrawText(cName, bx + cpBtnW / 2 - cnw / 2, cpY + 15, 18, cCol);
      }

      // Status text
      if (mpLobby.selectionComplete) {
        const char *readyText = "Army ready! Waiting for opponent...";
        int rtw = GameMeasureText(readyText, 20);
        GameDrawText(readyText, lsw / 2 - rtw / 2, cpY + cpBtnH + 30, 20,
                     (Color){100, 200, 100, 255});
        // Animated dots
        int dots = (int)(GetTime() * 2) % 4;
        char dotBuf[8] = "";
        for (int d = 0; d < dots; d++)
          strcat(dotBuf, ".");
        GameDrawText(dotBuf, lsw / 2 + rtw / 2 + 4, cpY + cpBtnH + 30, 20,
                     WHITE);
#ifdef USE_EOS
        // Show lobby code when hosting online
        if (useEos && eosClient.isHost && eosClient.lobbyCode[0]) {
          const char *codeLabel =
              TextFormat("LOBBY CODE: %s", eosClient.lobbyCode);
          int clw = GameMeasureText(codeLabel, 24);
          GameDrawText(codeLabel, lsw / 2 - clw / 2, cpY + cpBtnH + 60, 24,
                       (Color){255, 220, 100, 255});
        }
#endif
      } else {
        const char *pickText = TextFormat(
            "Pick unit %d of %d", mpLobby.activeSlot + 1, BLUE_TEAM_MAX_SIZE);
        int ptw = GameMeasureText(pickText, 16);
        GameDrawText(pickText, lsw / 2 - ptw / 2, cpY + cpBtnH + 30, 16,
                     (Color){160, 160, 180, 255});
      }

      // Connection info at bottom
      if (isHosting) {
        char localIp[64];
        net_get_local_ip(localIp, sizeof(localIp));
        char ipInfo[128];
        snprintf(ipInfo, sizeof(ipInfo), "Your IP: %s  |  Port %d", localIp,
                 NET_PORT);
        int iiw = GameMeasureText(ipInfo, 14);
        GameDrawText(ipInfo, lsw / 2 - iiw / 2, lsh - 50, 14,
                     (Color){180, 170, 120, 200});
      }

      const char *escText = "Press ESC to cancel";
      int ew = GameMeasureText(escText, 14);
      GameDrawText(escText, lsw / 2 - ew / 2, lsh - 30, 14,
                   (Color){100, 100, 120, 255});
    }

    //==============================================================================
    // PHASE_MAP DRAWING — Slay the Spire branching map
    //==============================================================================
    if (phase == PHASE_MAP) {
      DrawMap(&actMap);

      // Event overlay (data-driven from EVENT_DEFS)
      if (showingMapEvent) {
        int esw = GetScreenWidth(), esh = GetScreenHeight();
        DrawRectangle(0, 0, esw, esh, (Color){10, 10, 20, 220});

        const EventDef *evt = &EVENT_DEFS[currentEventIndex];

        int titleSz = 38, descSz = 24;
        int ttw = GameMeasureText(evt->title, titleSz);
        GameDrawText(evt->title, esw / 2 - ttw / 2, esh / 2 - 120, titleSz, GOLD);
        int dtw = GameMeasureText(evt->description, descSz);
        GameDrawText(evt->description, esw / 2 - dtw / 2, esh / 2 - 70, descSz,
                 (Color){200, 200, 220, 255});

        int btnW = 260, btnH = 60, btnGap = 24;
        int totalBtnW = evt->choiceCount * btnW + (evt->choiceCount - 1) * btnGap;
        int btnStartX = esw / 2 - totalBtnW / 2;
        int btnY = esh / 2 + 40;

        Color btnColors[] = {
          {50, 120, 180, 255}, {120, 80, 50, 255}, {80, 120, 80, 255}
        };
        Color btnHoverColors[] = {
          {70, 150, 220, 255}, {160, 110, 70, 255}, {100, 160, 100, 255}
        };

        for (int c = 0; c < evt->choiceCount; c++) {
          Rectangle btnRect = {(float)(btnStartX + c * (btnW + btnGap)),
                               (float)btnY, (float)btnW, (float)btnH};
          Color bg = btnColors[c % 3];
          if (CheckCollisionPointRec(GetMousePosition(), btnRect))
            bg = btnHoverColors[c % 3];
          DrawRectangleRec(btnRect, bg);
          DrawRectangleLinesEx(btnRect, 2, WHITE);

          const char *label = evt->choices[c].label;
          if (evt->choices[c].cost > 0)
            label = TextFormat("%s (%dg)", evt->choices[c].label, evt->choices[c].cost);
          int lw = GameMeasureText(label, 20);
          GameDrawText(label, (int)(btnRect.x + (float)btnW / 2.0f - (float)lw / 2.0f),
                   btnY + 20, 20, WHITE);
        }
      }

      // Item shop overlay (on map)
      if (showingItemShop && itemShopGenerated) {
        int esw = GetScreenWidth(), esh = GetScreenHeight();
        DrawRectangle(0, 0, esw, esh, (Color){10, 10, 20, 220});

        const char *iTitle = "ITEM SHOP";
        int itSz = 28;
        int itw = MeasureText(iTitle, itSz);
        DrawText(iTitle, esw / 2 - itw / 2, esh / 2 - 110, itSz, GOLD);

        const char *iSub = (itemShopBuyCount >= 1) ? "SOLD OUT" : "Pick 1 item";
        int isSz = 16;
        int isw2 = MeasureText(iSub, isSz);
        DrawText(iSub, esw / 2 - isw2 / 2, esh / 2 - 75, isSz,
                 (itemShopBuyCount >= 1) ? (Color){120, 80, 80, 200}
                                         : (Color){200, 200, 220, 200});

        int iCardW = 200, iCardH = 80, iCardGap = 20;
        int iTotalW = 3 * iCardW + 2 * iCardGap;
        int iStartX = esw / 2 - iTotalW / 2;
        int iCardY = esh / 2 - 20;

        for (int io = 0; io < 3; io++) {
          int ix = iStartX + io * (iCardW + iCardGap);
          int iid = itemShopOffers[io];
          if (iid < 0 || iid >= ITEM_COUNT) {
            DrawRectangle(ix, iCardY, iCardW, iCardH, (Color){35, 35, 45, 255});
            DrawRectangleLines(ix, iCardY, iCardW, iCardH, (Color){60, 60, 80, 255});
            int soldFsz = 18;
            int sw2 = MeasureText("SOLD", soldFsz);
            DrawText("SOLD", ix + (iCardW - sw2) / 2,
                     iCardY + (iCardH - soldFsz) / 2, soldFsz, (Color){60, 60, 80, 255});
            continue;
          }
          const ItemDef *idef = &ITEM_DEFS[iid];
          bool canBuy = (playerGold >= idef->cost && itemInventoryCount < MAX_ITEMS &&
                         itemShopBuyCount < 1);
          Color icBg = canBuy ? idef->color : (Color){50, 50, 65, 255};
          Rectangle iRect = {(float)ix, (float)iCardY, (float)iCardW, (float)iCardH};
          bool iHover = CheckCollisionPointRec(GetMousePosition(), iRect);
          if (canBuy && iHover)
            icBg = (Color){(unsigned char)(icBg.r + 30),
                           (unsigned char)(icBg.g + 30),
                           (unsigned char)(icBg.b + 30), 255};
          DrawRectangleRec(iRect, icBg);
          DrawRectangleLinesEx(iRect, 1, (Color){90, 90, 110, 255});

          const char *iLabel = TextFormat("%s  %dg", idef->name, idef->cost);
          int ilFsz = 16;
          int ilw = MeasureText(iLabel, ilFsz);
          DrawText(iLabel, ix + (iCardW - ilw) / 2, iCardY + 15, ilFsz,
                   canBuy ? WHITE : (Color){100, 100, 120, 255});
          int dFsz = 14;
          int dlw = MeasureText(idef->description, dFsz);
          DrawText(idef->description, ix + (iCardW - dlw) / 2, iCardY + 40, dFsz,
                   (Color){180, 180, 200, 220});
        }

        // Continue button
        int contW = 180, contH = 40;
        int contX = esw / 2 - contW / 2;
        int contY = iCardY + iCardH + 30;
        Rectangle contRect = {(float)contX, (float)contY, (float)contW, (float)contH};
        Color contBg = (Color){50, 120, 80, 255};
        if (CheckCollisionPointRec(GetMousePosition(), contRect))
          contBg = (Color){70, 160, 100, 255};
        DrawRectangleRec(contRect, contBg);
        DrawRectangleLinesEx(contRect, 2, WHITE);
        const char *contTxt = "Continue";
        int ctw = MeasureText(contTxt, 16);
        DrawText(contTxt, contX + (contW - ctw) / 2, contY + 12, 16, WHITE);
      }

      // Gold display
      {
        const char *goldText = TextFormat("Gold: %d", playerGold);
        int gsz = 18;
        int gw = MeasureText(goldText, gsz);
        DrawText(goldText, GetScreenWidth() - gw - 20, 20, gsz, GOLD);
      }
    }

    //==============================================================================
    // PHASE_MILESTONE DRAWING
    //==============================================================================
    if (phase == PHASE_MILESTONE) {
      int msw = GetScreenWidth();
      int msh = GetScreenHeight();

      // Dim overlay on top of 3D scene
      DrawRectangle(0, 0, msw, msh, (Color){0, 0, 0, 160});

      // Title
      const char *msTitle = TextFormat("MILESTONE - Wave %d", currentRound);
      int mstw = GameMeasureText(msTitle, 40);
      GameDrawText(msTitle, msw / 2 - mstw / 2, 30, 40, GOLD);

      // Collect active blue units
      int msBlue[BLUE_TEAM_MAX_SIZE];
      int msCount = 0;
      for (int i = 0; i < unitCount && msCount < BLUE_TEAM_MAX_SIZE; i++)
        if (units[i].active && units[i].team == TEAM_BLUE)
          msBlue[msCount++] = i;

      // Unit cards (display only)
      int cardW = 200, cardH = 140, cardGap = 20;
      int totalW =
          msCount * cardW + (msCount > 1 ? (msCount - 1) * cardGap : 0);
      int startX = (msw - totalW) / 2;
      int cardY = msh / 2 - cardH / 2 - 20;

      for (int h = 0; h < msCount; h++) {
        int cx = startX + h * (cardW + cardGap);
        int ui = msBlue[h];
        UnitType *type = &unitTypes[units[ui].typeIndex];

        // Card background
        DrawRectangle(cx, cardY, cardW, cardH, (Color){35, 35, 50, 240});
        DrawRectangleLinesEx(
            (Rectangle){(float)cx, (float)cardY, (float)cardW, (float)cardH}, 2,
            (Color){60, 60, 80, 255});

        // Portrait
        if (h < blueHudCount) {
          int portSize = 80;
          Rectangle srcRect = {0, 0, (float)hudPortraitSize,
                               -(float)hudPortraitSize};
          Rectangle dstRect = {(float)(cx + 10), (float)(cardY + 10),
                               (float)portSize, (float)portSize};
          DrawTexturePro(portraits[h].texture, srcRect, dstRect,
                         (Vector2){0, 0}, 0.0f, WHITE);
          DrawRectangleLines(cx + 10, cardY + 10, portSize, portSize,
                             (Color){60, 60, 80, 255});
        }

        // Unit name
        GameDrawText(type->name, cx + 10, cardY + 96, 14,
                     (Color){200, 200, 220, 255});

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
            DrawRectangle(ax, ay, slotSize, slotSize,
                          ABILITY_DEFS[aslot->abilityId].color);
            const char *abbr = ABILITY_DEFS[aslot->abilityId].abbrev;
            int aw = GameMeasureText(abbr, 10);
            GameDrawText(abbr, ax + (slotSize - aw) / 2,
                         ay + (slotSize - 10) / 2, 10, WHITE);
            const char *lvl = TextFormat("L%d", aslot->level + 1);
            GameDrawText(lvl, ax + 2, ay + slotSize - 8, 7,
                         (Color){220, 220, 220, 200});
          } else {
            DrawRectangle(ax, ay, slotSize, slotSize, (Color){40, 40, 55, 255});
          }
          DrawRectangleLines(ax, ay, slotSize, slotSize,
                             (Color){90, 90, 110, 255});
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
        Rectangle setBtn = {(float)btnStartX2, (float)btnY2, (float)btnW2,
                            (float)btnH2};
        Color setBg = (Color){200, 170, 40, 255};
        if (CheckCollisionPointRec(GetMousePosition(), setBtn))
          setBg = (Color){240, 200, 60, 255};
        DrawRectangleRec(setBtn, setBg);
        DrawRectangleLinesEx(setBtn, 2, (Color){140, 120, 30, 255});
        const char *setText = "SET IN STONE";
        int setW = GameMeasureText(setText, 22);
        GameDrawText(setText, (int)(setBtn.x + btnW2 / 2 - setW / 2),
                     (int)(setBtn.y + 16), 22, WHITE);

        const char *setDesc = "Saves to leaderboard. Imprisons your party.";
        int sdw = GameMeasureText(setDesc, 11);
        GameDrawText(setDesc, (int)(setBtn.x + btnW2 / 2 - sdw / 2),
                     (int)(setBtn.y + 36), 11, (Color){255, 210, 80, 200});
      }

      // CONTINUE button
      {
        Rectangle contBtn = {(float)(btnStartX2 + btnW2 + btnGap2),
                             (float)btnY2, (float)btnW2, (float)btnH2};
        Color contBg = (Color){50, 160, 70, 255};
        if (CheckCollisionPointRec(GetMousePosition(), contBtn))
          contBg = (Color){30, 200, 50, 255};
        DrawRectangleRec(contBtn, contBg);
        DrawRectangleLinesEx(contBtn, 2, DARKGREEN);
        const char *contText = "CONTINUE";
        int contW = GameMeasureText(contText, 22);
        GameDrawText(contText, (int)(contBtn.x + btnW2 / 2 - contW / 2),
                     (int)(contBtn.y + 16), 22, WHITE);

        const char *contDesc = "Keep fighting. Lose and they die.";
        int cdw = GameMeasureText(contDesc, 11);
        GameDrawText(contDesc, (int)(contBtn.x + btnW2 / 2 - cdw / 2),
                     (int)(contBtn.y + 36), 11, (Color){255, 100, 80, 200});
      }
    }

    //==============================================================================
    // PHASE_GAME_OVER DRAWING — multiplayer
    //==============================================================================
    if (phase == PHASE_GAME_OVER && isMultiplayer) {
      int gosw = GetScreenWidth();
      int gosh = GetScreenHeight();
      DrawRectangle(0, 0, gosw, gosh, (Color){20, 20, 30, 240});

      const char *goTitle = roundResultText;
      int gotw = GameMeasureText(goTitle, 36);
      GameDrawText(goTitle, gosw / 2 - gotw / 2, gosh / 2 - 60, 36, GOLD);

      int goMySlot = NC_FLAG(playerSlot);
      const char *goScore = TextFormat(
          "HP: You %d — Opp %d", mpHealth[goMySlot], mpHealth[1 - goMySlot]);
      int gsw = GameMeasureText(goScore, 20);
      GameDrawText(goScore, gosw / 2 - gsw / 2, gosh / 2, 20, WHITE);

      const char *goRestart = "Press R / ESC to exit";
      int grw = GameMeasureText(goRestart, 16);
      GameDrawText(goRestart, gosw / 2 - grw / 2, gosh / 2 + 40, 16,
                   (Color){150, 150, 170, 255});

      // EXIT button
      int exBtnW = 180, exBtnH = 44;
      Rectangle exRect = {(float)(gosw / 2 - exBtnW / 2),
                          (float)(gosh / 2 + 70), (float)exBtnW, (float)exBtnH};
      bool exHov = CheckCollisionPointRec(GetMousePosition(), exRect);
      DrawRectangleRec(exRect, exHov ? (Color){200, 60, 60, 255}
                                     : (Color){140, 40, 40, 255});
      DrawRectangleLinesEx(exRect, 2, (Color){180, 80, 80, 255});
      const char *exTxt = "EXIT";
      int exTw = GameMeasureText(exTxt, 22);
      GameDrawText(exTxt, (int)(exRect.x + exBtnW / 2 - exTw / 2),
                   (int)(exRect.y + (exBtnH - 22) / 2), 22, WHITE);
    }

    //==============================================================================
    // PHASE_GAME_OVER DRAWING — non-death: withdraw units + reset (solo only)
    //==============================================================================
    if (phase == PHASE_GAME_OVER && !isMultiplayer && !deathPenalty) {
      int gosw = GetScreenWidth();
      int gosh = GetScreenHeight();

      // Full dark overlay
      DrawRectangle(0, 0, gosw, gosh, (Color){20, 20, 30, 240});

      // Title
      const char *goTitle = "SET IN STONE";
      int gotw = GameMeasureText(goTitle, 36);
      GameDrawText(goTitle, gosw / 2 - gotw / 2, 40, 36, GOLD);

      const char *goRound = TextFormat("Reached Wave %d  |  Score: %d - %d",
                                       currentRound, blueWins, redWins);
      int gorw = GameMeasureText(goRound, 18);
      GameDrawText(goRound, gosw / 2 - gorw / 2, 85, 18, WHITE);

      // Collect surviving blue units
      int goBlue[BLUE_TEAM_MAX_SIZE];
      int goCount = 0;
      for (int i = 0; i < unitCount && goCount < BLUE_TEAM_MAX_SIZE; i++)
        if (units[i].active && units[i].team == TEAM_BLUE)
          goBlue[goCount++] = i;

      // Subtitle
      if (goCount > 0) {
        const char *goSub = "Withdraw your units or reset";
        int gosub = GameMeasureText(goSub, 14);
        GameDrawText(goSub, gosw / 2 - gosub / 2, 115, 14,
                     (Color){180, 180, 200, 180});
      } else {
        const char *goSub = "All units have been set in stone!";
        int gosub = GameMeasureText(goSub, 14);
        GameDrawText(goSub, gosw / 2 - gosub / 2, 115, 14,
                     (Color){180, 180, 200, 180});
      }

      // Unit cards with WITHDRAW button
      int goCardW = 200, goCardH = 140, goCardGap = 20;
      int goTotalW =
          goCount * goCardW + (goCount > 1 ? (goCount - 1) * goCardGap : 0);
      int goStartX = (gosw - goTotalW) / 2;
      int goCardY = gosh / 2 - 40;

      for (int h = 0; h < goCount; h++) {
        int cx = goStartX + h * (goCardW + goCardGap);
        int ui = goBlue[h];
        UnitType *type = &unitTypes[units[ui].typeIndex];

        DrawRectangle(cx, goCardY, goCardW, goCardH, (Color){35, 35, 50, 240});
        DrawRectangleLinesEx((Rectangle){(float)cx, (float)goCardY,
                                         (float)goCardW, (float)goCardH},
                             2, (Color){60, 60, 80, 255});

        // Portrait
        if (h < BLUE_TEAM_MAX_SIZE) {
          int portSize = 80;
          Rectangle srcRect = {0, 0, (float)hudPortraitSize,
                               -(float)hudPortraitSize};
          Rectangle dstRect = {(float)(cx + 10), (float)(goCardY + 6),
                               (float)portSize, (float)portSize};
          DrawTexturePro(portraits[h].texture, srcRect, dstRect,
                         (Vector2){0, 0}, 0.0f, WHITE);
          DrawRectangleLines(cx + 10, goCardY + 6, portSize, portSize,
                             (Color){60, 60, 80, 255});
        }

        // Unit name
        GameDrawText(type->name, cx + 10, goCardY + 90, 14,
                     (Color){200, 200, 220, 255});

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
            DrawRectangle(ax, ay, goSlotSize, goSlotSize,
                          ABILITY_DEFS[aslot->abilityId].color);
            const char *abbr = ABILITY_DEFS[aslot->abilityId].abbrev;
            int aw = GameMeasureText(abbr, 10);
            GameDrawText(abbr, ax + (goSlotSize - aw) / 2,
                         ay + (goSlotSize - 10) / 2, 10, WHITE);
            const char *lvl = TextFormat("L%d", aslot->level + 1);
            GameDrawText(lvl, ax + 2, ay + goSlotSize - 8, 7,
                         (Color){220, 220, 220, 200});
          } else {
            DrawRectangle(ax, ay, goSlotSize, goSlotSize,
                          (Color){40, 40, 55, 255});
          }
          DrawRectangleLines(ax, ay, goSlotSize, goSlotSize,
                             (Color){90, 90, 110, 255});
        }

        // WITHDRAW button
        Rectangle wdBtn = {(float)(cx + 10), (float)(goCardY + goCardH - 34),
                           (float)(goCardW - 20), 28};
        Color wdBg = (Color){60, 50, 120, 255};
        if (CheckCollisionPointRec(GetMousePosition(), wdBtn))
          wdBg = (Color){90, 70, 180, 255};
        DrawRectangleRec(wdBtn, wdBg);
        DrawRectangleLinesEx(wdBtn, 1, (Color){100, 80, 160, 255});
        const char *wdText = "WITHDRAW";
        int wdw = GameMeasureText(wdText, 12);
        GameDrawText(wdText, (int)(wdBtn.x + (goCardW - 20) / 2 - wdw / 2),
                     (int)(wdBtn.y + 8), 12, WHITE);
      }

      // RESET button
      int resetBtnW = 180, resetBtnH = 44;
      int resetBtnY = goCardY + goCardH + 30;
      Rectangle resetBtn = {(float)(gosw / 2 - resetBtnW / 2), (float)resetBtnY,
                            (float)resetBtnW, (float)resetBtnH};
      {
        Color resetBg = (Color){180, 50, 50, 255};
        if (CheckCollisionPointRec(GetMousePosition(), resetBtn))
          resetBg = (Color){220, 70, 70, 255};
        DrawRectangleRec(resetBtn, resetBg);
        DrawRectangleLinesEx(resetBtn, 2, (Color){120, 40, 40, 255});
        const char *resetText = "RESET";
        int rstw = GameMeasureText(resetText, 18);
        GameDrawText(resetText, (int)(resetBtn.x + resetBtnW / 2 - rstw / 2),
                     (int)(resetBtn.y + 13), 18, WHITE);
      }
    }

    //==============================================================================
    // UNIT INTRO SCREEN ("New Challenger" splash)
    //==============================================================================
    if (intro.active) {
      int isw = GetScreenWidth();
      int ish = GetScreenHeight();
      float t = intro.timer;

      // Animation progress values
      float wipeProgress = (t < INTRO_WIPE_IN) ? (t / INTRO_WIPE_IN) : 1.0f;
      float fadeAlpha = 1.0f;
      if (t >= INTRO_FADE_OUT_START) {
        fadeAlpha = 1.0f - (t - INTRO_FADE_OUT_START) /
                               (INTRO_FADE_OUT_END - INTRO_FADE_OUT_START);
        if (fadeAlpha < 0.0f)
          fadeAlpha = 0.0f;
      }
      unsigned char alpha = (unsigned char)(255.0f * fadeAlpha);

      // --- Procedural background (clipped to wipe) ---
      int wipeW = (int)(isw * wipeProgress);
      if (intro.typeIndex == 0) {
        // Mushroom: dark forest green
        DrawRectangle(0, 0, wipeW, ish, (Color){30, 45, 25, alpha});
        for (int ring = 0; ring < 8; ring++) {
          float radius = 100.0f + ring * 80.0f;
          unsigned char ra = (unsigned char)(alpha * 0.3f);
          DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                          (Color){(unsigned char)(50 + ring * 8),
                                  (unsigned char)(70 + ring * 5), 30, ra});
        }
        for (int ln = 0; ln < 12; ln++) {
          int y = (ish / 12) * ln;
          DrawLine(0, y, wipeW, y - 40,
                   (Color){80, 120, 50, (unsigned char)(alpha * 0.2f)});
        }
      } else if (intro.typeIndex == 1) {
        // Goblin: dark swamp green with sharp diagonal streaks
        DrawRectangle(0, 0, wipeW, ish, (Color){20, 35, 15, alpha});
        for (int ln = 0; ln < 20; ln++) {
          int y = (ish / 20) * ln;
          DrawLine(0, y + 120, wipeW, y - 80,
                   (Color){60, 100, 30, (unsigned char)(alpha * 0.25f)});
          DrawLine(0, y + 80, wipeW, y - 120,
                   (Color){40, 80, 20, (unsigned char)(alpha * 0.15f)});
        }
      } else if (intro.typeIndex == 2) {
        // Devil: deep infernal with fire-colored rings
        DrawRectangle(0, 0, wipeW, ish, (Color){50, 15, 10, alpha});
        for (int ring = 0; ring < 8; ring++) {
          float radius = 100.0f + ring * 80.0f;
          unsigned char ra = (unsigned char)(alpha * 0.3f);
          DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                          (Color){(unsigned char)(200 + ring * 6),
                                  (unsigned char)(80 + ring * 10), 20, ra});
        }
        for (int ln = 0; ln < 15; ln++) {
          int y = (ish / 15) * ln;
          DrawLine(0, y + 60, wipeW, y - 60,
                   (Color){220, 80, 20, (unsigned char)(alpha * 0.15f)});
        }
      } else if (intro.typeIndex == 3) {
        // Puppycat: warm pink
        DrawRectangle(0, 0, wipeW, ish, (Color){50, 25, 40, alpha});
        for (int ring = 0; ring < 8; ring++) {
          float radius = 100.0f + ring * 80.0f;
          unsigned char ra = (unsigned char)(alpha * 0.3f);
          DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                          (Color){(unsigned char)(180 + ring * 6),
                                  (unsigned char)(80 + ring * 5),
                                  (unsigned char)(140 + ring * 4), ra});
        }
        for (int ln = 0; ln < 12; ln++) {
          int y = (ish / 12) * ln;
          DrawLine(0, y, wipeW, y - 30,
                   (Color){200, 100, 160, (unsigned char)(alpha * 0.15f)});
        }
      } else if (intro.typeIndex == 4) {
        // Siren: deep ocean
        DrawRectangle(0, 0, wipeW, ish, (Color){15, 25, 50, alpha});
        for (int ring = 0; ring < 8; ring++) {
          float radius = 100.0f + ring * 80.0f;
          unsigned char ra = (unsigned char)(alpha * 0.3f);
          DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                          (Color){(unsigned char)(40 + ring * 5),
                                  (unsigned char)(120 + ring * 8),
                                  (unsigned char)(180 + ring * 6), ra});
        }
        for (int ln = 0; ln < 15; ln++) {
          int y = (ish / 15) * ln;
          DrawLine(0, y + 80, wipeW, y - 80,
                   (Color){60, 140, 200, (unsigned char)(alpha * 0.15f)});
        }
      } else if (intro.typeIndex == 5) {
        // Reptile: earthy amber with warm rings
        DrawRectangle(0, 0, wipeW, ish, (Color){35, 30, 15, alpha});
        for (int ring = 0; ring < 8; ring++) {
          float radius = 100.0f + ring * 80.0f;
          unsigned char ra = (unsigned char)(alpha * 0.3f);
          DrawCircleLines(isw * 65 / 100, ish / 2, radius,
                          (Color){(unsigned char)(140 + ring * 10),
                                  (unsigned char)(100 + ring * 8),
                                  (unsigned char)(40 + ring * 5), ra});
        }
        for (int ln = 0; ln < 15; ln++) {
          int y = (ish / 15) * ln;
          DrawLine(0, y + 60, wipeW, y - 60,
                   (Color){160, 120, 40, (unsigned char)(alpha * 0.15f)});
        }
      } else {
        // Default: dark crimson
        DrawRectangle(0, 0, wipeW, ish, (Color){45, 20, 20, alpha});
        for (int ring = 0; ring < 8; ring++) {
          float radius = 100.0f + ring * 80.0f;
          unsigned char ra = (unsigned char)(alpha * 0.3f);
          DrawCircleLines(
              isw * 65 / 100, ish / 2, radius,
              (Color){(unsigned char)(120 + ring * 10), 40, 30, ra});
        }
        for (int ln = 0; ln < 15; ln++) {
          int y = (ish / 15) * ln;
          DrawLine(0, y + 60, wipeW, y - 60,
                   (Color){180, 60, 30, (unsigned char)(alpha * 0.15f)});
        }
      }

      // --- Slash wipe edge ---
      if (wipeProgress < 1.0f) {
        int wipeX = wipeW;
        DrawLine(wipeX, -20, wipeX - 80, ish + 20,
                 (Color){255, 255, 255, alpha});
        DrawLine(wipeX + 3, -20, wipeX - 77, ish + 20,
                 (Color){255, 255, 200, (unsigned char)(alpha * 0.5f)});
        // Thicker glow
        DrawLine(wipeX - 1, -20, wipeX - 81, ish + 20,
                 (Color){255, 255, 255, (unsigned char)(alpha * 0.4f)});
      }

      // --- White flash at wipe completion ---
      if (t >= INTRO_WIPE_IN && t < INTRO_WIPE_IN + 0.15f) {
        float flashAlpha = 1.0f - (t - INTRO_WIPE_IN) / 0.15f;
        DrawRectangle(
            0, 0, isw, ish,
            (Color){255, 255, 255,
                    (unsigned char)(200.0f * flashAlpha * fadeAlpha)});
      }

      // --- 3D model composited (slide in from right) ---
      float modelSlide = 0.0f;
      if (t >= INTRO_HOLD_START) {
        float slideT = (t - INTRO_HOLD_START) / 0.3f;
        if (slideT > 1.0f)
          slideT = 1.0f;
        modelSlide = 1.0f - (1.0f - slideT) * (1.0f - slideT); // ease-out
      }
      float modelSize = ish * 0.85f;
      float modelFinalX = isw * 0.45f;
      float modelStartX = isw * 1.2f;
      float modelX = modelStartX + (modelFinalX - modelStartX) * modelSlide;
      float modelY = (ish - modelSize) / 2.0f;

      Rectangle introSrc = {0, 0, 512.0f, -512.0f};
      Rectangle introDst = {modelX, modelY, modelSize, modelSize};
      DrawTexturePro(introModelRT.texture, introSrc, introDst, (Vector2){0, 0},
                     0.0f, (Color){255, 255, 255, alpha});

      // --- Unit name (slide in from left) ---
      float textSlide = 0.0f;
      if (t >= INTRO_HOLD_START + 0.1f) {
        float textT = (t - INTRO_HOLD_START - 0.1f) / 0.25f;
        if (textT > 1.0f)
          textT = 1.0f;
        textSlide = 1.0f - (1.0f - textT) * (1.0f - textT);
      }
      // Determine display name
      const char *className = unitTypes[intro.typeIndex].name;
      const char *introName = className;
      int nameFontSize = ish / 8;
      int nameW = GameMeasureText(introName, nameFontSize);
      float nameFinalX = isw * 0.08f;
      float nameStartX = (float)(-nameW - 20);
      float nameX = nameStartX + (nameFinalX - nameStartX) * textSlide;
      float nameY = ish * 0.2f;

      // Shadow
      GameDrawText(introName, (int)nameX + 3, (int)nameY + 3, nameFontSize,
                   (Color){0, 0, 0, (unsigned char)(alpha * 0.6f)});
      // Main text
      Color nameColor = GetTeamTint(TEAM_BLUE);
      nameColor.a = alpha;
      GameDrawText(introName, (int)nameX, (int)nameY, nameFontSize, nameColor);

      // Subtitle
      int subSize = nameFontSize / 3;
      if (subSize < 12)
        subSize = 12;
      const char *subText = "joins the battle!";
      switch (intro.typeIndex) {
      case 0:
        subText = "sprouts into action!";
        break;
      case 1:
        subText = "dashes into the fray!";
        break;
      case 2:
        subText = "rises from the flames!";
        break;
      case 5:
        subText = "stalks its prey!";
        break;
      }
      GameDrawText(subText, (int)nameX + 4, (int)nameY + nameFontSize + 4,
                   subSize,
                   (Color){200, 200, 220, (unsigned char)(alpha * 0.8f)});

      // --- Decorative line under subtitle ---
      int lineY2 = (int)nameY + nameFontSize + subSize + 12;
      if (textSlide > 0.0f) {
        int lineW = (int)((nameW + 40) * textSlide);
        DrawRectangle((int)nameFinalX, lineY2, lineW, 3,
                      (Color){nameColor.r, nameColor.g, nameColor.b,
                              (unsigned char)(alpha * 0.7f)});
      }

      // --- Rarity stars (below line, above abilities) ---
      int starHeight = 0;
      if (intro.rarity > 0 && textSlide > 0.0f) {
        int starSize = subSize + 8;
        int starY = lineY2 + 10;
        starHeight = starSize + 10;
        if (intro.rarity == RARITY_RARE) {
          const char *star = "* *";
          GameDrawText(star, (int)nameFinalX + 4, starY, starSize,
                       (Color){180, 100, 255, (unsigned char)(alpha * 0.9f)});
        } else if (intro.rarity == RARITY_LEGENDARY) {
          const char *stars = "* * *";
          GameDrawText(stars, (int)nameFinalX + 4, starY, starSize,
                       (Color){255, 215, 0, alpha});
        }
      }

      // --- Ability slots (fade in with delay) ---
      if (t >= INTRO_HOLD_START + 0.4f) {
        float abilAlpha = (t - INTRO_HOLD_START - 0.4f) / 0.2f;
        if (abilAlpha > 1.0f)
          abilAlpha = 1.0f;
        abilAlpha *= fadeAlpha;
        unsigned char aa = (unsigned char)(255.0f * abilAlpha);

        int slotSize = 48;
        int slotGap = 8;
        int abilX = (int)nameFinalX;
        int abilY = lineY2 + 10 + starHeight;

        for (int a = 0; a < MAX_ABILITIES_PER_UNIT; a++) {
          int ax = abilX + a * (slotSize + slotGap);
          AbilitySlot *slot = &units[intro.unitIndex].abilities[a];

          if (slot->abilityId >= 0 && slot->abilityId < ABILITY_COUNT) {
            Color abilCol = ABILITY_DEFS[slot->abilityId].color;
            abilCol.a = aa;
            DrawRectangle(ax, abilY, slotSize, slotSize, abilCol);
            const char *abbr = ABILITY_DEFS[slot->abilityId].abbrev;
            int aw = GameMeasureText(abbr, 16);
            GameDrawText(abbr, ax + (slotSize - aw) / 2,
                         abilY + (slotSize - 16) / 2, 16,
                         (Color){255, 255, 255, aa});
            // Level
            const char *lvl = TextFormat("L%d", slot->level + 1);
            GameDrawText(lvl, ax + 2, abilY + slotSize - 10, 8,
                         (Color){220, 220, 220, aa});
          } else {
            DrawRectangle(ax, abilY, slotSize, slotSize,
                          (Color){40, 40, 55, aa});
            const char *q = "?";
            int qw = GameMeasureText(q, 22);
            GameDrawText(q, ax + (slotSize - qw) / 2,
                         abilY + (slotSize - 22) / 2, 22,
                         (Color){80, 80, 100, aa});
          }
          DrawRectangleLines(ax, abilY, slotSize, slotSize,
                             (Color){120, 120, 150, aa});
        }
      }
    }

    // Shadow debug overlay
    if (shadowDebugMode > 0) {
      const char *modeNames[] = {"", "Shadow Factor", "Light Depth", "Light UV",
                                 "Sampled Depth"};
      GameDrawText(TextFormat("[F10] Shadow Debug: %d - %s", shadowDebugMode,
                              modeNames[shadowDebugMode]),
                   10, GetScreenHeight() - 30, 20, YELLOW);
      // Draw shadow map depth as small preview in corner
      float previewSize = 256.0f;
      Rectangle srcRec = {0, 0, (float)SHADOW_MAP_SIZE,
                          -(float)SHADOW_MAP_SIZE};
      Rectangle dstRec = {GetScreenWidth() - previewSize - 10, 10, previewSize,
                          previewSize};
      DrawTexturePro(shadowRT.texture, srcRec, dstRec, (Vector2){0, 0}, 0.0f,
                     WHITE);
      DrawRectangleLines((int)dstRec.x, (int)dstRec.y, (int)previewSize,
                         (int)previewSize, YELLOW);
      GameDrawText("Shadow Color RT", (int)dstRec.x,
                   (int)dstRec.y + (int)previewSize + 4, 16, YELLOW);
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
      DrawText(
          TextFormat(
              "Color Grade [F6]  1/2:exp 3/4:con 5/6:sat 7/8:temp 9/0:vig"),
          10, oy, 10, GREEN);
      oy += 16;
      DrawText(TextFormat("exposure:    %.3f", cgExposure), 10, oy, 10, WHITE);
      oy += 14;
      DrawText(TextFormat("contrast:    %.3f", cgContrast), 10, oy, 10, WHITE);
      oy += 14;
      DrawText(TextFormat("saturation:  %.3f", cgSaturation), 10, oy, 10,
               WHITE);
      oy += 14;
      DrawText(TextFormat("temperature: %.3f", cgTemperature), 10, oy, 10,
               WHITE);
      oy += 14;
      DrawText(TextFormat("vignetteStr: %.3f", cgVignetteStr), 10, oy, 10,
               WHITE);
      oy += 14;
      DrawText(TextFormat("vignetteSft: %.3f", cgVignetteSoft), 10, oy, 10,
               WHITE);
      oy += 14;
      DrawText(
          TextFormat("lift: %.2f %.2f %.2f", cgLift[0], cgLift[1], cgLift[2]),
          10, oy, 10, WHITE);
      oy += 14;
      DrawText(
          TextFormat("gain: %.2f %.2f %.2f", cgGain[0], cgGain[1], cgGain[2]),
          10, oy, 10, WHITE);
      oy += 14;
      DrawText("-/=: vignetteSoftness", 10, oy, 10, GRAY);
    }

    // ---- Help Overlay ----
    if (showHelp) {
      int hsw = GetScreenWidth(), hsh = GetScreenHeight();
      DrawRectangle(0, 0, hsw, hsh, (Color){0, 0, 0, 160});

      int hpW = S(420), hpH = S(380);
      int hpX = hsw / 2 - hpW / 2;
      int hpY = hsh / 2 - hpH / 2;
      DrawRectangle(hpX, hpY, hpW, hpH, (Color){24, 24, 32, 240});
      DrawRectangleLinesEx(
          (Rectangle){(float)hpX, (float)hpY, (float)hpW, (float)hpH}, 2,
          (Color){100, 100, 130, 255});

      const char *helpTitle = "CONTROLS";
      int httw = GameMeasureText(helpTitle, S(22));
      GameDrawText(helpTitle, hpX + hpW / 2 - httw / 2, hpY + S(10), S(22),
                   (Color){200, 180, 255, 255});

      int lx = hpX + S(20), ly = hpY + S(44);
      int fsz = S(13), gap = S(20);
      Color kc = (Color){120, 200, 255, 255}, dc = (Color){200, 200, 220, 255};
      GameDrawText("Mouse", lx, ly, fsz, kc);
      GameDrawText("Place / drag units", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("1-5", lx, ly, fsz, kc);
      GameDrawText("Buy shop slot", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("R", lx, ly, fsz, kc);
      GameDrawText("Reroll shop", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("Space", lx, ly, fsz, kc);
      GameDrawText("Start combat / skip", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("L", lx, ly, fsz, kc);
      GameDrawText("Lock/unlock shop", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("H", lx, ly, fsz, kc);
      GameDrawText("Toggle this help", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("ESC", lx, ly, fsz, kc);
      GameDrawText("Settings menu", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("F1", lx, ly, fsz, kc);
      GameDrawText("Debug mode", lx + S(110), ly, fsz, dc);
      ly += gap;
      GameDrawText("F11", lx, ly, fsz, kc);
      GameDrawText("Toggle fullscreen", lx + S(110), ly, fsz, dc);
      ly += gap;

      // Close hint
      ly += S(6);
      const char *closeHint = "Press H or ESC to close";
      int chw = GameMeasureText(closeHint, S(11));
      GameDrawText(closeHint, hpX + hpW / 2 - chw / 2, ly, S(11),
                   (Color){140, 140, 160, 255});
    }

    // ---- Escape Menu Overlay ----
    if (showEscMenu) {
      int esw = GetScreenWidth(), esh = GetScreenHeight();
      DrawRectangle(0, 0, esw, esh, (Color){0, 0, 0, 140});

      int panelW = S(400), panelH = S(440);
      int panelX = esw / 2 - panelW / 2;
      int panelY = esh / 2 - panelH / 2;
      DrawRectangle(panelX, panelY, panelW, panelH, (Color){24, 24, 32, 240});
      DrawRectangleLinesEx((Rectangle){(float)panelX, (float)panelY,
                                       (float)panelW, (float)panelH},
                           2, (Color){100, 100, 130, 255});

      const char *escTitle = "SETTINGS";
      int esctw = GameMeasureText(escTitle, S(24));
      GameDrawText(escTitle, panelX + panelW / 2 - esctw / 2, panelY + S(10),
                   S(24), (Color){200, 180, 255, 255});

      // Close (X) button
      Rectangle escCloseBtn = {(float)(panelX + panelW - S(36)),
                               (float)(panelY + S(4)), (float)S(32),
                               (float)S(32)};
      Color escCloseBg = (Color){180, 50, 50, 200};
      if (CheckCollisionPointRec(GetMousePosition(), escCloseBtn))
        escCloseBg = (Color){230, 70, 70, 255};
      DrawRectangleRec(escCloseBtn, escCloseBg);
      GameDrawText("X", (int)(escCloseBtn.x + S(10)),
                   (int)(escCloseBtn.y + S(7)), S(18), WHITE);
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
          CheckCollisionPointRec(GetMousePosition(), escCloseBtn)) {
        PlaySound(sfxUiClick);
        SaveSettings(musicVolume, sfxVolume, isFullscreen, playerName);
        showEscMenu = false;
      }

      // --- Music Volume Slider ---
      GameDrawText("Music Volume", panelX + S(20), panelY + S(50), S(16),
                   WHITE);
      int sliderX = panelX + S(20);
      int sliderW = panelW - S(100);
      int sliderH = S(16);
      int musicSliderY = panelY + S(70);
      Rectangle musicTrack = {(float)sliderX, (float)musicSliderY,
                              (float)sliderW, (float)sliderH};
      DrawRectangleRec(musicTrack, (Color){40, 40, 55, 255});
      DrawRectangle(sliderX, musicSliderY, (int)(sliderW * musicVolume),
                    sliderH, (Color){80, 160, 255, 255});
      DrawRectangleLinesEx(musicTrack, 1,
                           focusedSlider == 0 ? (Color){180, 180, 255, 255}
                                              : (Color){100, 100, 130, 255});
      GameDrawText(TextFormat("%d%%", (int)(musicVolume * 100)),
                   sliderX + sliderW + S(8), musicSliderY, S(14), WHITE);
      if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Rectangle expanded = {musicTrack.x, musicTrack.y - 5, musicTrack.width,
                              musicTrack.height + 10};
        if (CheckCollisionPointRec(GetMousePosition(), expanded)) {
          focusedSlider = 0;
          musicVolume =
              (GetMousePosition().x - musicTrack.x) / musicTrack.width;
          if (musicVolume < 0)
            musicVolume = 0;
          if (musicVolume > 1)
            musicVolume = 1;
          if (musicVolume < 0.01f)
            musicVolume = 0.0f;
          SetMusicVolume(bgm, musicVolume);
        }
      }

      // --- SFX Volume Slider ---
      GameDrawText("SFX Volume", panelX + S(20), panelY + S(105), S(16), WHITE);
      int sfxSliderY = panelY + S(125);
      Rectangle sfxTrack = {(float)sliderX, (float)sfxSliderY, (float)sliderW,
                            (float)sliderH};
      DrawRectangleRec(sfxTrack, (Color){40, 40, 55, 255});
      DrawRectangle(sliderX, sfxSliderY, (int)(sliderW * sfxVolume), sliderH,
                    (Color){80, 160, 255, 255});
      DrawRectangleLinesEx(sfxTrack, 1,
                           focusedSlider == 1 ? (Color){180, 180, 255, 255}
                                              : (Color){100, 100, 130, 255});
      GameDrawText(TextFormat("%d%%", (int)(sfxVolume * 100)),
                   sliderX + sliderW + S(8), sfxSliderY, S(14), WHITE);
      if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Rectangle expanded = {sfxTrack.x, sfxTrack.y - 5, sfxTrack.width,
                              sfxTrack.height + 10};
        if (CheckCollisionPointRec(GetMousePosition(), expanded)) {
          focusedSlider = 1;
          sfxVolume = (GetMousePosition().x - sfxTrack.x) / sfxTrack.width;
          if (sfxVolume < 0)
            sfxVolume = 0;
          if (sfxVolume > 1)
            sfxVolume = 1;
          if (sfxVolume < 0.01f)
            sfxVolume = 0.0f;
          for (int si = 0; si < sfxCount; si++)
            SetSoundVolume(allSfx[si], sfxBaseVol[si] * sfxVolume);
        }
      }
      // Click outside both sliders resets focus
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Rectangle musicExp = {musicTrack.x, musicTrack.y - 5, musicTrack.width,
                              musicTrack.height + 10};
        Rectangle sfxExp = {sfxTrack.x, sfxTrack.y - 5, sfxTrack.width,
                            sfxTrack.height + 10};
        if (!CheckCollisionPointRec(GetMousePosition(), musicExp) &&
            !CheckCollisionPointRec(GetMousePosition(), sfxExp))
          focusedSlider = -1;
      }
      // Arrow key adjustment for focused slider
      if (focusedSlider >= 0) {
        bool left = KeyRepeat(KEY_LEFT, dt, &sliderKeyTimer);
        bool right = KeyRepeat(KEY_RIGHT, dt, &sliderKeyTimer);
        if (left || right) {
          float delta = right ? 0.01f : -0.01f;
          if (focusedSlider == 0) {
            musicVolume += delta;
            if (musicVolume < 0)
              musicVolume = 0;
            if (musicVolume > 1)
              musicVolume = 1;
            if (musicVolume < 0.01f)
              musicVolume = 0.0f;
            SetMusicVolume(bgm, musicVolume);
          } else {
            sfxVolume += delta;
            if (sfxVolume < 0)
              sfxVolume = 0;
            if (sfxVolume > 1)
              sfxVolume = 1;
            if (sfxVolume < 0.01f)
              sfxVolume = 0.0f;
            for (int si = 0; si < sfxCount; si++)
              SetSoundVolume(allSfx[si], sfxBaseVol[si] * sfxVolume);
          }
        }
      }

      // --- Fullscreen Checkbox ---
      int checkY = panelY + S(160);
      Rectangle checkBox = {(float)(panelX + S(20)), (float)checkY,
                            (float)S(20), (float)S(20)};
      DrawRectangleRec(checkBox, (Color){40, 40, 55, 255});
      DrawRectangleLinesEx(checkBox, 1, (Color){100, 100, 130, 255});
      if (isFullscreen) {
        DrawRectangle(panelX + S(24), checkY + S(4), S(12), S(12),
                      (Color){80, 160, 255, 255});
      }
      GameDrawText("Fullscreen (F11)", panelX + S(48), checkY + S(2), S(16),
                   WHITE);
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
          CheckCollisionPointRec(GetMousePosition(), checkBox)) {
        PlaySound(sfxUiClick);
        ToggleBorderlessWindowed();
        isFullscreen = !isFullscreen;
      }

      // --- Main Menu Button (singleplayer only) ---
      if (!isMultiplayer) {
        int mmBtnW = panelW - S(60), mmBtnH = S(36);
        int mmBtnX = panelX + panelW / 2 - mmBtnW / 2;
        int mmBtnY = panelY + S(210);
        Rectangle mmBtn = {(float)mmBtnX, (float)mmBtnY, (float)mmBtnW, (float)mmBtnH};
        Color mmBg = (Color){50, 80, 160, 220};
        if (CheckCollisionPointRec(GetMousePosition(), mmBtn))
          mmBg = (Color){60, 100, 210, 255};
        DrawRectangleRec(mmBtn, mmBg);
        DrawRectangleLinesEx(mmBtn, 1, (Color){100, 100, 130, 255});
        const char *mmText = "MAIN MENU";
        int mmtw = GameMeasureText(mmText, S(18));
        GameDrawText(mmText, mmBtnX + mmBtnW / 2 - mmtw / 2, mmBtnY + S(8), S(18), WHITE);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(GetMousePosition(), mmBtn)) {
          PlaySound(sfxUiClick);
          SaveSettings(musicVolume, sfxVolume, isFullscreen, playerName);
          unitCount = 0;
          memset(plazaData, 0, sizeof(plazaData));
          PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);
          plazaState = PLAZA_ROAMING;
          mapActive = false;
          phase = PHASE_PLAZA;
          showEscMenu = false;
        }
      }

      // --- Quit / Disconnect Button ---
      int quitBtnW = panelW - S(60), quitBtnH = S(36);
      int quitBtnX = panelX + panelW / 2 - quitBtnW / 2;
      int quitBtnY = panelY + S(isMultiplayer ? 210 : 260);
      Rectangle quitBtn = {(float)quitBtnX, (float)quitBtnY, (float)quitBtnW,
                           (float)quitBtnH};
      Color quitBg = (Color){160, 50, 50, 220};
      if (CheckCollisionPointRec(GetMousePosition(), quitBtn))
        quitBg = (Color){210, 60, 60, 255};
      DrawRectangleRec(quitBtn, quitBg);
      DrawRectangleLinesEx(quitBtn, 1, (Color){100, 100, 130, 255});
      const char *quitText = isMultiplayer ? "DISCONNECT" : "QUIT GAME";
      int qtw = GameMeasureText(quitText, S(18));
      GameDrawText(quitText, quitBtnX + quitBtnW / 2 - qtw / 2, quitBtnY + S(8),
                   S(18), WHITE);
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
          CheckCollisionPointRec(GetMousePosition(), quitBtn)) {
        PlaySound(sfxUiClick);
        SaveSettings(musicVolume, sfxVolume, isFullscreen, playerName);
        if (isMultiplayer) {
#ifdef USE_EOS
          if (useEos) {
            eos_client_disconnect(&eosClient);
            useEos = false;
          } else
#endif
          {
            if (isHosting) {
              host_stop();
              isHosting = false;
            }
            net_client_disconnect(&netClient);
          }
          isMultiplayer = false;
          unitCount = 0;
          memset(plazaData, 0, sizeof(plazaData));
          PlazaSpawnLobbyPool(units, &unitCount, plazaData, &lobbySelection);
          plazaState = PLAZA_ROAMING;
          phase = PHASE_PLAZA;
          showEscMenu = false;
        } else {
          showEscMenu = false;
          break; // exit game loop
        }
      }

      // --- Help Button ---
      {
        int helpBtnW = panelW - S(60), helpBtnH = S(36);
        int helpBtnX = panelX + panelW / 2 - helpBtnW / 2;
        int helpBtnY = panelY + S(isMultiplayer ? 265 : 310);
        Rectangle helpBtn = {(float)helpBtnX, (float)helpBtnY, (float)helpBtnW,
                             (float)helpBtnH};
        Color helpBg = (Color){60, 60, 120, 220};
        if (CheckCollisionPointRec(GetMousePosition(), helpBtn))
          helpBg = (Color){80, 80, 170, 255};
        DrawRectangleRec(helpBtn, helpBg);
        DrawRectangleLinesEx(helpBtn, 1, (Color){100, 100, 130, 255});
        const char *helpText = "CONTROLS (H)";
        int htw = GameMeasureText(helpText, S(18));
        GameDrawText(helpText, helpBtnX + helpBtnW / 2 - htw / 2,
                     helpBtnY + S(8), S(18), WHITE);
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
            CheckCollisionPointRec(GetMousePosition(), helpBtn)) {
          PlaySound(sfxUiClick);
          showHelp = true;
          showEscMenu = false;
        }
      }

      // --- Resume Button ---
      int resBtnW = panelW - S(60), resBtnH = S(36);
      int resBtnX = panelX + panelW / 2 - resBtnW / 2;
      int resBtnY = panelY + S(360);
      Rectangle resBtn = {(float)resBtnX, (float)resBtnY, (float)resBtnW,
                          (float)resBtnH};
      Color resBg = (Color){50, 120, 80, 220};
      if (CheckCollisionPointRec(GetMousePosition(), resBtn))
        resBg = (Color){60, 160, 100, 255};
      DrawRectangleRec(resBtn, resBg);
      DrawRectangleLinesEx(resBtn, 1, (Color){100, 100, 130, 255});
      const char *resText = "RESUME";
      int rtw = GameMeasureText(resText, S(18));
      GameDrawText(resText, resBtnX + resBtnW / 2 - rtw / 2, resBtnY + S(8),
                   S(18), WHITE);
      if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
          CheckCollisionPointRec(GetMousePosition(), resBtn)) {
        PlaySound(sfxUiClick);
        SaveSettings(musicVolume, sfxVolume, isFullscreen, playerName);
        showEscMenu = false;
      }
    }

    DrawFPS(10, 10);

    // Frame-time budget bar (debug mode)
    {
      double profRenderEnd = GetTime();
      profRenderTime = profRenderEnd - profRenderStart;
      profTotalTime = profRenderEnd - profFrameStart;

      // Exponential moving average (alpha=0.05 for smooth display)
      const double alpha = 0.05;
      profLogicSmooth += alpha * (profLogicTime - profLogicSmooth);
      profRenderSmooth += alpha * (profRenderTime - profRenderSmooth);
      profTotalSmooth += alpha * (profTotalTime - profTotalSmooth);

      if (debugMode) {
        int bx = 10, by = 30;
        int bw = 260, bh = 14;
        float budgetMs = 16.667f; // 60 Hz budget
        float logicMs = (float)(profLogicSmooth * 1000.0);
        float renderMs = (float)(profRenderSmooth * 1000.0);
        float totalMs = (float)(profTotalSmooth * 1000.0);

        // Background
        DrawRectangle(bx - 2, by - 2, bw + 4, bh * 3 + 30,
                      (Color){0, 0, 0, 180});

        // Budget bar: logic (green) + render (blue) proportional to 16.67ms
        float logicFrac = logicMs / budgetMs;
        float renderFrac = renderMs / budgetMs;
        if (logicFrac > 1.0f)
          logicFrac = 1.0f;
        if (renderFrac > 1.0f)
          renderFrac = 1.0f;

        // Bar outline
        DrawRectangleLines(bx, by, bw, bh, (Color){180, 180, 180, 200});
        // Logic portion (green)
        int logicW = (int)(logicFrac * bw);
        DrawRectangle(bx, by, logicW, bh, (Color){80, 200, 80, 220});
        // Render portion (blue, stacked after logic)
        int renderW = (int)(renderFrac * bw);
        if (logicW + renderW > bw)
          renderW = bw - logicW;
        DrawRectangle(bx + logicW, by, renderW, bh, (Color){80, 120, 220, 220});

        // Labels
        DrawText(TextFormat("Logic:  %.2f ms", logicMs), bx, by + bh + 2, 10,
                 (Color){80, 200, 80, 255});
        DrawText(TextFormat("Render: %.2f ms", renderMs), bx, by + bh + 14, 10,
                 (Color){80, 120, 220, 255});
        DrawText(
            TextFormat("Total:  %.2f ms (budget: %.1f ms)", totalMs, budgetMs),
            bx, by + bh + 26, 10,
            totalMs > budgetMs ? RED : (Color){220, 220, 220, 255});
      }
    }

    EndDrawing();
  }

  // Cleanup
#ifdef USE_EOS
  if (useEos && isMultiplayer)
    eos_client_disconnect(&eosClient);
  else
#endif
  {
    if (isHosting)
      host_stop();
    if (isMultiplayer)
      net_client_disconnect(&netClient);
  }
#ifdef USE_EOS
  eos_shutdown();
#endif
  for (int i = 0; i < BLUE_TEAM_MAX_SIZE; i++)
    UnloadRenderTexture(portraits[i]);
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
      UnloadModelAnimations(unitTypes[i].scaredAnims,
                            unitTypes[i].scaredAnimCount);
    if (unitTypes[i].attackAnims)
      UnloadModelAnimations(unitTypes[i].attackAnims,
                            unitTypes[i].attackAnimCount);
    if (unitTypes[i].castAnims)
      UnloadModelAnimations(unitTypes[i].castAnims, unitTypes[i].castAnimCount);
    if (unitTypes[i].loaded)
      UnloadModel(unitTypes[i].model);
  }
  for (int i = 0; i < TILE_VARIANTS; i++)
    UnloadModel(tileModels[i]);
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
  // Unload env models (skip 2=stairs, 3=circle, 5=ground which alias
  // stairsModel/circleModel/platformModel) Skip textures for 7=PillarSmall
  // which shares textures with 6=PillarBig
  for (int i = 0; i < envModelCount; i++) {
    if (i == 2 || i == 3 || i == 5)
      continue; // reused models, already unloaded above
    if (envModels[i].loaded)
      UnloadModel(envModels[i].model);
    if (i == 4 || i == 7)
      continue; // shared textures (FloorTiles=tiles, PillarSmall=PillarBig)
    if (envModels[i].texture.id > 0)
      UnloadTexture(envModels[i].texture);
    if (envModels[i].ormTexture.id > 0)
      UnloadTexture(envModels[i].ormTexture);
    if (envModels[i].normalTexture.id > 0)
      UnloadTexture(envModels[i].normalTexture);
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
  UnloadSound(sfxDevilShout);
  UnloadSound(sfxDevilDie);
  UnloadSound(sfxLizardShout);
  UnloadSound(sfxLizardDie);
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
