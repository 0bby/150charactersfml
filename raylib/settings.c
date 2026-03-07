#include "settings.h"
#include <stdio.h>
#include <string.h>

void SaveSettings(float musicVol, float sfxVol, bool fullscreen, const char *name)
{
    FILE *fp = fopen("settings.json", "w");
    if (!fp) return;
    fprintf(fp, "{\n  \"musicVolume\": %.3f,\n  \"sfxVolume\": %.3f,\n  \"fullscreen\": %s,\n  \"playerName\": \"%s\"\n}\n",
            musicVol, sfxVol, fullscreen ? "true" : "false", name);
    fclose(fp);
}

void LoadSettings(float *musicVol, float *sfxVol, bool *fullscreen, char *name, int *nameLen)
{
    FILE *fp = fopen("settings.json", "r");
    if (!fp) return;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    char *p;
    if ((p = strstr(buf, "\"musicVolume\""))) { float v; if (sscanf(p, "\"musicVolume\": %f", &v) == 1) *musicVol = v; }
    if ((p = strstr(buf, "\"sfxVolume\"")))   { float v; if (sscanf(p, "\"sfxVolume\": %f", &v) == 1) *sfxVol = v; }
    if ((p = strstr(buf, "\"fullscreen\"")))  { *fullscreen = (strstr(p, "true") != NULL && strstr(p, "true") < p + 30); }
    if ((p = strstr(buf, "\"playerName\"")))  {
        char *q = strchr(p + 13, '"');
        if (q) {
            q++;
            char *end = strchr(q, '"');
            if (end && (end - q) < 31) {
                int len = (int)(end - q);
                memcpy(name, q, len);
                name[len] = '\0';
                *nameLen = len;
            }
        }
    }
}
