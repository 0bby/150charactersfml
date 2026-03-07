#pragma once
#include "raylib.h"
#include "raymath.h"
#include <stdbool.h>

// Global font — loaded in main(), used by GameDrawText/GameMeasureText
extern Font g_gameFont;

// UI scaling
extern float uiScale;
#define S(x) ((int)((x) * uiScale))

// Text drawing helpers
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

// Key repeat: fires once on press, then continuously after a delay
#define KEY_REPEAT_DELAY 0.35f
#define KEY_REPEAT_RATE  0.05f

static inline bool KeyRepeat(int key, float dt, float *timer) {
    if (IsKeyPressed(key)) { *timer = 0.0f; return true; }
    if (IsKeyDown(key)) {
        *timer += dt;
        if (*timer >= KEY_REPEAT_DELAY) { *timer -= KEY_REPEAT_RATE; return true; }
    } else { *timer = 0.0f; }
    return false;
}
