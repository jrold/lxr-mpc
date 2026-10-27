/*
 * MachineTest.c: the whole LXR-02 (engine/volt_machine.h) driven as the
 * plug-in drives it: a card loaded sector by sector, boot, then renders of
 * the MPC's block size with MIDI notes and panel input between them.
 *
 *   voltkit-machine-test <card.img> [out.wav]
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

static void lcd_print(const VoltMachineApi *m, const char *when)
{
    const uint8_t *d = m->lcd();
    char a[17], b[17];
    for (int i = 0; i < 16; i++) {
        a[i] = d[i] >= 32 && d[i] < 127 ? (char)d[i] : '#';
        b[i] = d[0x40 + i] >= 32 && d[0x40 + i] < 127 ? (char)d[0x40 + i] : '#';
    }
    a[16] = b[16] = 0;
    printf("%-14s |%s|%s|\n", when, a, b);
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: voltkit-machine-test <card.img> [out.wav]\n"); return 2; }
    const VoltMachineApi *m = volt_machine_api();
    CHECK(m->version == VOLT_MACHINE_API_VERSION);

    /* the card */
    FILE *c = fopen(argv[1], "rb");
    if (!c) { perror(argv[1]); return 2; }
    uint8_t sector[VOLT_SECTOR_BYTES];
    uint32_t loaded = 0;
    for (uint32_t s = 0; s < VOLT_CARD_SECTORS && fread(sector, 1, sizeof sector, c) == sizeof sector; s++) {
        m->card_write(s, sector);
        loaded++;
    }
    fclose(c);
    uint32_t used = 0;
    for (uint32_t s = m->card_next(0); s < VOLT_CARD_SECTORS; s = m->card_next(s + 1)) used++;
    printf("card: %u sectors read, %u hold data (%.1f KB)\n", loaded, used, used * 0.5);
    CHECK(used > 10 && used < loaded);

    /* boot */
    clock_t t0 = clock();
    CHECK(m->boot() == 0);
    printf("boot: %.3f s of cpu\n", (double)(clock() - t0) / CLOCKS_PER_SEC);
    lcd_print(m, "after boot");

    /* 12 s in 128-frame blocks; a note per voice, then a short groove */
    const int block = 128, seconds = 12, blocks = seconds * VOLT_SAMPLE_RATE / block;
    float *l = calloc((size_t)blocks * block, sizeof(float)), *r = calloc((size_t)blocks * block, sizeof(float));
    struct { double at; uint8_t msg[3]; } notes[] = {
        { 0.5, { 0x90, 36, 127 } }, { 1.25, { 0x90, 37, 127 } }, { 2.0, { 0x90, 38, 127 } }, { 2.75, { 0x90, 39, 127 } },
        { 3.5, { 0x90, 40, 127 } }, { 4.25, { 0x90, 41, 127 } }, { 5.0, { 0x90, 42, 127 } },
        { 6.0, { 0x90, 36, 100 } }, { 6.25, { 0x90, 42, 80 } }, { 6.5, { 0x90, 38, 110 } }, { 6.75, { 0x90, 42, 80 } },
        { 7.0, { 0x90, 36, 100 } }, { 7.25, { 0x90, 42, 80 } }, { 7.5, { 0x90, 38, 110 } }, { 7.75, { 0x90, 41, 90 } },
    };
    const int nnotes = (int)(sizeof notes / sizeof notes[0]);
    int next = 0;
    t0 = clock();
    for (int b = 0; b < blocks; b++) {
        const double now = (double)b * block / VOLT_SAMPLE_RATE;
        while (next < nnotes && notes[next].at <= now) m->midi(notes[next++].msg, 3);
        if (b == (int)(8.5 * VOLT_SAMPLE_RATE / block)) { m->button(17, 1); }   /* AEG page key down */
        if (b == (int)(8.6 * VOLT_SAMPLE_RATE / block)) { m->button(17, 0); }
        m->render(l + (size_t)b * block, r + (size_t)b * block, block);
    }
    const double cpu = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("render: %d s in %.3f s of cpu (%.1fx real time)\n", seconds, cpu, seconds / cpu);
    lcd_print(m, "after AEG key");

    /* each note sounds */
    for (int i = 0; i < 7; i++) {
        const size_t a = (size_t)(notes[i].at * VOLT_SAMPLE_RATE);
        float pre = 0.0f, post = 0.0f;
        for (size_t k = a - 2000; k < a; k++) pre = fmaxf(pre, fabsf(l[k]) + fabsf(r[k]));
        for (size_t k = a; k < a + 8000; k++) post = fmaxf(post, fabsf(l[k]) + fabsf(r[k]));
        printf("note %d at %.2f s: %.3f before, %.3f after\n", notes[i].msg[1], notes[i].at, pre, post);
        CHECK(post > 0.05f);
    }
    float peak = 0.0f;
    for (int i = 0; i < blocks * block; i++) peak = fmaxf(peak, fmaxf(fabsf(l[i]), fabsf(r[i])));
    CHECK(peak > 0.1f && peak <= 2.0f);
    CHECK(m->lcd_version() > 0 && m->led_version() > 0);

    /* odd block sizes keep going */
    float tl[97], tr[97];
    for (int i = 0; i < 50; i++) m->render(tl, tr, 1 + (i * 37) % 97);
    CHECK(1);

    if (argc > 2) {
        FILE *w = fopen(argv[2], "wb");
        const unsigned n = (unsigned)(blocks * block), bytes = n * 4;
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
