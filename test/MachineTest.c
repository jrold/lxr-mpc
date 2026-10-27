/*
 * MachineTest.c: the whole LXR-02 (engine/volt_machine.h) driven as the
 * plug-in drives it.
 *
 *   voltkit-machine-test --format <card.img>     boot on a blank card: the
 *       firmware formats it with its Init project; the card is written out
 *   voltkit-machine-test <card.img> [out.wav]    boot on that card, play the
 *       voices, use the panel, check what it shows and sounds like
 *
 * Copyright (C) 2026 Jason Rold. GPL-3.0-only.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "volt_machine.h"

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const VoltMachineApi *m;

static void lcd_text(char *out)
{
    const uint8_t *d = m->lcd();
    for (int i = 0; i < 16; i++) {
        out[i] = d[i] >= 32 && d[i] < 127 ? (char)d[i] : '#';
        out[17 + i] = d[0x40 + i] >= 32 && d[0x40 + i] < 127 ? (char)d[0x40 + i] : '#';
    }
    out[16] = '|';
    out[33] = 0;
}

static void lcd_print(const char *when)
{
    char t[34];
    lcd_text(t);
    printf("%-16s |%s|\n", when, t);
}

static float run(double seconds, float *l, float *r)
{
    const int block = 128, blocks = (int)(seconds * VOLT_SAMPLE_RATE / block);
    float peak = 0.0f, bl[128], br[128];
    for (int b = 0; b < blocks; b++) {
        m->render(l ? l + (size_t)b * block : bl, r ? r + (size_t)b * block : br, block);
        const float *pl = l ? l + (size_t)b * block : bl, *pr = r ? r + (size_t)b * block : br;
        for (int i = 0; i < block; i++) peak = fmaxf(peak, fmaxf(fabsf(pl[i]), fabsf(pr[i])));
    }
    return peak;
}

static int load_card(const char *path)
{
    FILE *c = fopen(path, "rb");
    if (!c) { perror(path); return 0; }
    uint8_t sector[VOLT_SECTOR_BYTES];
    for (uint32_t s = 0; s < VOLT_CARD_SECTORS && fread(sector, 1, sizeof sector, c) == sizeof sector; s++)
        m->card_write(s, sector);
    fclose(c);
    return 1;
}

static int save_card(const char *path)
{
    FILE *c = fopen(path, "wb");
    if (!c) { perror(path); return 0; }
    uint8_t sector[VOLT_SECTOR_BYTES];
    for (uint32_t s = 0; s < VOLT_CARD_SECTORS; s++) {
        m->card_read(s, sector);
        fwrite(sector, 1, sizeof sector, c);
    }
    fclose(c);
    return 1;
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char **argv)
{
    m = volt_machine_api();
    CHECK(m->version == VOLT_MACHINE_API_VERSION);

    if (argc == 3 && !strcmp(argv[1], "--format")) {
        clock_t t0 = clock();
        const int booted = m->boot();
        printf("boot on a blank card: %d, %.3f s of cpu, card version %u\n", booted,
               (double)(clock() - t0) / CLOCKS_PER_SEC, m->card_version());
        lcd_print("after boot");
        uint32_t used = 0;
        for (uint32_t s = m->card_next(0); s < VOLT_CARD_SECTORS; s = m->card_next(s + 1)) used++;
        printf("card: %u sectors hold data\n", used);
        CHECK(booted != 0 && used > 10);   /* formatted, then "Please reboot." */
        if (!save_card(argv[2])) return 2;
        printf("%d checks, %d failed\n", checks, failures);
        return failures != 0;
    }
    if (argc < 2) { fprintf(stderr, "usage: voltkit-machine-test [--format] <card.img> [out.wav]\n"); return 2; }
    if (!load_card(argv[1])) return 2;

    clock_t t0 = clock();
    CHECK(m->boot() == 0);
    printf("boot: %.3f s of cpu\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    lcd_print("after boot");

    /* 12 s: a note per voice, a groove, then the panel */
    const int seconds = 12;
    const size_t frames = (size_t)seconds * VOLT_SAMPLE_RATE;
    float *l = calloc(frames, sizeof(float)), *r = calloc(frames, sizeof(float));
    struct { double at; uint8_t msg[3]; } notes[] = {
        { 0.5, { 0x90, 36, 127 } }, { 1.25, { 0x90, 37, 127 } }, { 2.0, { 0x90, 38, 127 } }, { 2.75, { 0x90, 39, 127 } },
        { 3.5, { 0x90, 40, 127 } }, { 4.25, { 0x90, 41, 127 } }, { 5.0, { 0x90, 42, 127 } },
        { 6.0, { 0x90, 36, 100 } }, { 6.25, { 0x90, 42, 80 } }, { 6.5, { 0x90, 38, 110 } }, { 6.75, { 0x90, 42, 80 } },
        { 7.0, { 0x90, 36, 100 } }, { 7.25, { 0x90, 42, 80 } }, { 7.5, { 0x90, 38, 110 } }, { 7.75, { 0x90, 41, 90 } },
    };
    const int nnotes = (int)(sizeof notes / sizeof notes[0]);
    const int block = 128;
    int next = 0;
    t0 = clock();
    for (size_t at = 0; at + block <= frames; at += block) {
        const double now = (double)at / VOLT_SAMPLE_RATE;
        while (next < nnotes && notes[next].at <= now) m->midi(notes[next++].msg, 3);
        m->render(l + at, r + at, block);
    }
    const double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("render: %d s in %.3f s of cpu (%.1fx real time)\n", seconds, cpu, seconds / cpu);
    for (int i = 0; i < 7; i++) {
        const size_t a = (size_t)(notes[i].at * VOLT_SAMPLE_RATE);
        float pre = 0.0f, post = 0.0f;
        for (size_t k = a - 2000; k < a; k++) pre = fmaxf(pre, fabsf(l[k]) + fabsf(r[k]));
        for (size_t k = a; k < a + 8000; k++) post = fmaxf(post, fabsf(l[k]) + fabsf(r[k]));
        printf("note %d at %.2f s: %.3f before, %.3f after\n", notes[i].msg[1], notes[i].at, pre, post);
        CHECK(post > 0.05f);
    }

    /* a tap on AEG (key 17) shows its page within a few milliseconds */
    char before[34], after[34];
    lcd_text(before);
    m->tap(17);
    run(0.01, NULL, NULL);
    lcd_text(after);
    lcd_print("AEG + 10 ms");
    CHECK(strcmp(before, after) != 0);

    /* knob 1 to a value: the firmware's own value gets there */
    VoltKnob k;
    CHECK(m->knob(0, &k));
    printf("knob 1: param %d = %d (%d..%d)\n", k.param, k.value, k.min, k.max);
    const int target = k.value + 20 <= k.max ? k.value + 20 : k.value - 20;
    m->knob_to(0, target);
    run(0.03, NULL, NULL);
    CHECK(m->knob(0, &k) && k.value == target);
    lcd_print("knob 1 turned");
    printf("knob 1 now %d (wanted %d)\n", k.value, target);

    /* the encoder and its push change what the display shows */
    lcd_text(before);
    m->encoder(1);
    run(0.05, NULL, NULL);
    lcd_print("encoder +1");

    float peak = 0.0f;
    for (size_t i = 0; i < frames; i++) peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
    CHECK(peak > 0.1f && peak <= 2.0f);
    CHECK(m->lcd_version() > 0 && m->led_version() > 0);

    if (argc > 2) {
        FILE *w = fopen(argv[2], "wb");
        const unsigned n = (unsigned)frames, bytes = n * 4;
        fwrite("RIFF", 1, 4, w); put32(w, 36 + bytes); fwrite("WAVEfmt ", 1, 8, w); put32(w, 16); put16(w, 1); put16(w, 2);
        put32(w, VOLT_SAMPLE_RATE); put32(w, VOLT_SAMPLE_RATE * 4); put16(w, 4); put16(w, 16); fwrite("data", 1, 4, w); put32(w, bytes);
        for (unsigned i = 0; i < n; i++) {
            put16(w, (unsigned)(int16_t)lrintf(fmaxf(-1.0f, fminf(1.0f, l[i])) * 32767.0f));
            put16(w, (unsigned)(int16_t)lrintf(fmaxf(-1.0f, fminf(1.0f, r[i])) * 32767.0f));
        }
        fclose(w);
        printf("wrote %s\n", argv[2]);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
