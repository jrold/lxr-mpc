// Random kits: every sound parameter at random values (0..255, wider than
// the menu allows), random hits, random tempo; the output must stay finite
// and in range. Run it under the sanitizers (build-asan) to catch the
// firmware reading or writing out of bounds.
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../engine/volt_engine.h"

int main(int argc, char** argv)
{
    const char* lib = argc > 1 ? argv[1] : VOLTKIT_TEST_ENGINE;
    const int rounds = argc > 2 ? atoi(argv[2]) : 400;
    void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h)
    {
        fprintf(stderr, "%s\n", dlerror());
        return 2;
    }
    const VoltEngineApi* e = ((VoltEngineApiFn)dlsym(h, "volt_engine_api"))();
    e->init();
    srand(argc > 3 ? (unsigned)atoi(argv[3]) : 1234u);
    float l[512], r[512];
    int bad = 0;
    for (int round = 0; round < rounds; ++round)
    {
        for (int i = 0; i < 40; ++i)
            e->set_param(1 + rand() % (VOLT_PARAMS - 1), rand() % (round % 4 == 0 ? 256 : 128));
        e->set_tempo(40.0f + (float)(rand() % 260), rand() & 1);
        if (round % 50 == 0)
        {
            uint8_t kit[VOLT_KIT_BYTES];
            for (int i = 0; i < VOLT_KIT_BYTES; ++i)
                kit[i] = (uint8_t)rand();
            e->load_kit(kit, VOLT_KIT_BYTES);
        }
        for (int v = 0; v < VOLT_VOICES; ++v)
            if (rand() % 3 == 0)
                e->trigger(v, rand() % 128, 1 + rand() % 127);
        if (round % 97 == 0)
            e->all_off();
        for (int t = 0; t < 1 + rand() % 3; ++t)
            e->retarget(rand() % 230, 1 + rand() % 6);
        const int n = 1 + rand() % 512;
        e->render(l, r, n);
        for (int i = 0; i < n; ++i)
            if (!isfinite(l[i]) || !isfinite(r[i]) || fabsf(l[i]) > 2.0f || fabsf(r[i]) > 2.0f)
                ++bad;
    }
    printf("fuzz: %d rounds, %d bad samples\n", rounds, bad);
    return bad ? 1 : 0;
}
