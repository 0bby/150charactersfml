// Stubs for raylib functions used by shared code in the headless server build.
// The server links without raylib, so these provide minimal implementations.
#include <stdlib.h>
#include "raylib.h"

int GetRandomValue(int min, int max)
{
    if (min > max) { int t = min; min = max; max = t; }
    return min + rand() % (max - min + 1);
}

void DrawLine3D(Vector3 startPos, Vector3 endPos, Color color)
{
    (void)startPos; (void)endPos; (void)color;
}
