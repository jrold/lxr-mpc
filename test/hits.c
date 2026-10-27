// Plays each VOLTKIT voice once into a WAV file (loads the engine library
// with dlopen, as the plug-in does).
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include "../engine/volt_engine.h"
int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: hits <engine.so> <out.wav>\n"); return 2; }
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    const VoltEngineApi* e = ((VoltEngineApiFn)dlsym(h, "volt_engine_api"))();
    e->init();
    const int per = VOLT_SAMPLE_RATE / 2, n = per * VOLT_VOICES;
    float* l = calloc(n, sizeof(float)); float* r = calloc(n, sizeof(float));
    for (int v = 0; v < VOLT_VOICES; ++v) {
        e->trigger(v, 60, 110);
        e->render(l + v * per, r + v * per, per);
        float peak = 0; for (int i = 0; i < per; ++i) { float a = l[v*per+i] < 0 ? -l[v*per+i] : l[v*per+i]; if (a > peak) peak = a; }
        printf("voice %d peak %.3f\n", v, peak);
    }
    FILE* f = fopen(argv[2], "wb");
    int bytes = n * 4; int rate = VOLT_SAMPLE_RATE, br = rate * 4; short ch = 2, ba = 4, bits = 16, fmt = 1; int sz = 16, total = 36 + bytes;
    fwrite("RIFF", 1, 4, f); fwrite(&total, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&sz, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    for (int i = 0; i < n; ++i) { short s[2] = { (short)(l[i] * 32767), (short)(r[i] * 32767) }; fwrite(s, 2, 2, f); }
    fclose(f);
    return 0;
}
