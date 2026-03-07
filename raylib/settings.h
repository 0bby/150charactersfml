#pragma once
#include <stdbool.h>

void SaveSettings(float musicVol, float sfxVol, bool fullscreen, const char *name);
void LoadSettings(float *musicVol, float *sfxVol, bool *fullscreen, char *name, int *nameLen);
