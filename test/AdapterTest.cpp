// AdapterTest.cpp: VOLTKIT's plug-in adapter (mpc/VoltkitMpc.cpp) driven as
// the MPC's wrapper drives it: parameters set and read by key, the changed
// keys collected, a render each 128-frame block.
//
//   VOLTKIT_ENGINE=<engine library> VOLTKIT_CARD=data/card.vkc voltkit-adapter-test
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

extern "C" {
#include "engine.h"
int mpc_engine_changed(void* inst, char* key, int len);
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
int failures = 0, checks = 0;
#define CHECK(c)                                                                  \
    do                                                                            \
    {                                                                             \
        ++checks;                                                                 \
        if (!(c))                                                                 \
        {                                                                         \
            ++failures;                                                           \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);              \
        }                                                                         \
    } while (0)

const mpc_engine_t* E = nullptr;
constexpr int kBlock = 128;
constexpr int kRate = 44100;
std::string dataDir;

struct Track
{
    void* inst = nullptr;
    std::set<std::string> seen;   // changed keys reported

    Track() { inst = E->create(dataDir.c_str()); }
    ~Track() { E->destroy(inst); }

    void set(const std::string& k, const std::string& v) { E->set_param(inst, k.c_str(), v.c_str()); }
    void set(const std::string& k, const double v) { set(k, std::to_string(v)); }

    std::string get(const std::string& k)
    {
        std::vector<char> buf(8 << 20);
        return E->get_param(inst, k.c_str(), buf.data(), static_cast<int>(buf.size())) > 0 ? buf.data() : "";
    }

    double num(const std::string& k) { return std::atof(get(k).c_str()); }

    // Seconds of audio; returns the loudest sample (0..1).
    double run(const double seconds)
    {
        double peak = 0.0;
        const int blocks = static_cast<int>(seconds * kRate / kBlock + 0.5);
        std::int16_t out[kBlock * 2];
        for (int b = 0; b < blocks; ++b)
        {
            E->render(inst, out, kBlock);
            for (const std::int16_t s : out)
                peak = std::fmax(peak, std::abs(s) / 32768.0);
            char key[64];
            while (mpc_engine_changed(inst, key, sizeof key))
                seen.insert(key);
        }
        return peak;
    }

    std::string lcd()
    {
        std::string s;
        for (int c = 0; c < 32; ++c)
        {
            char k[8];
            std::snprintf(k, sizeof k, "lcd_%02d", c);
            s += static_cast<char>(std::lround(num(k) - 0.3));
            if (c == 15)
                s += '|';
        }
        return s;
    }

    bool led(const int l)
    {
        char k[8];
        std::snprintf(k, sizeof k, "led_%02d", l);
        return num(k) > 0.5;
    }

    void midi(const int status, const int d1, const int d2)
    {
        const std::uint8_t m[3] = { static_cast<std::uint8_t>(status), static_cast<std::uint8_t>(d1),
                                    static_cast<std::uint8_t>(d2) };
        E->midi(inst, m, 3);
    }
    void note(const int n, const int vel) { midi(0x90, n, vel); }
    void noteOff(const int n) { midi(0x80, n, 0); }

    // What knob i shows (its value, as the display above it has it).
    int knob(const int i) { return std::atoi(get("knob" + std::to_string(i) + "_display").c_str()); }

    // A voice and page shown, the page's first screen (another page first: a
    // page key again shows its second screen), and its knobs' values.
    std::map<int, int> knobsOf(const int voice, const int page)
    {
        char k[8];
        std::snprintf(k, sizeof k, "btn_%02d", voice);
        set(k, 1.0);
        set(page == 17 ? "btn_16" : "btn_17", 1.0);
        run(0.05);
        std::snprintf(k, sizeof k, "btn_%02d", page);
        set(k, 1.0);
        run(0.2);   // the display repaints a character at a time
        if (std::getenv("ADAPTER_SHOW"))
            std::printf("  voice %d page %d |%s|\n", voice, page, lcd().c_str());
        return { { 1, knob(1) }, { 2, knob(2) }, { 3, knob(3) }, { 4, knob(4) } };
    }
};
}

int main()
{
    if (std::getenv("VOLTKIT_CARD") == nullptr)
    {
        std::fprintf(stderr, "set VOLTKIT_CARD (data/card.vkc) and VOLTKIT_ENGINE\n");
        return 2;
    }
    setenv("VOLTKIT_DEBOUNCE_MS", "0", 1);
    setenv("VOLTKIT_RECENTRE_MS", "0", 1);
    dataDir = (std::filesystem::temp_directory_path() / "voltkit-adapter-test").string();
    std::filesystem::remove_all(dataDir);
    std::filesystem::create_directories(dataDir);
    E = mpc_engine();

    Track t;
    CHECK(t.inst != nullptr);
    CHECK(t.get("status").empty());
    t.run(1.0);
    std::printf("start        |%s|\n", t.lcd().c_str());
    CHECK(t.seen.count("lcd_00") && t.seen.count("led_01"));

    // the pads: General MIDI drum notes play the voices
    t.note(36, 127);
    const double kick = t.run(0.5);
    t.note(42, 127);
    const double hat = t.run(0.5);
    std::printf("pads: kick %.3f, closed hat %.3f\n", kick, hat);
    CHECK(kick > 0.05 && hat > 0.05);

    // a tap on AEG shows its page at once
    t.set("btn_17", 1.0);
    t.run(0.03);
    const std::string aeg = t.lcd();
    std::printf("AEG + 30 ms  |%s|\n", aeg.c_str());
    CHECK(aeg.find("ATK") != std::string::npos || aeg.find("atk") != std::string::npos);
    CHECK(t.num("btn_17") == 0.0);

    // knob 1 shows the attack's value and goes where it is set
    const double k0 = t.num("knob1");
    t.set("knob1", k0 + 20.0 / 127.0);
    t.run(0.05);
    std::printf("knob 1 +20   |%s|  knob %.3f -> %.3f (%s)\n", t.lcd().c_str(), k0, t.num("knob1"),
                t.get("knob1_display").c_str());
    CHECK(std::fabs(t.num("knob1") - (k0 + 20.0 / 127.0)) < 1e-3);
    CHECK(t.get("knob1_display") == "20");
    // a Q-Link click: one step from where the knob reads
    t.set("knob1", t.num("knob1") + 1.0 / 127.0);
    t.run(0.03);
    CHECK(t.get("knob1_display") == "21");
    t.set("knob1", k0);
    t.run(0.05);
    CHECK(t.get("knob1_display") == "0");

    // a page key again shows its page's second screen, and again its first:
    // the snare's OSC
    t.set("btn_04", 1.0);
    t.set("btn_17", 1.0);
    t.run(0.05);
    t.set("btn_16", 1.0);   // OSC
    t.run(0.2);
    const std::string osc1 = t.lcd();
    t.set("btn_16", 1.0);
    t.run(0.2);
    const std::string osc2 = t.lcd();
    t.set("btn_16", 1.0);
    t.run(0.2);
    std::printf("OSC          |%s|  again |%s|\n", osc1.c_str(), osc2.c_str());
    CHECK(osc2 != osc1 && t.lcd() == osc1);
    t.set("btn_01", 1.0);
    t.run(0.2);

    // no sequencer: the LXR's PLAY, REC and step keys aren't keys here, and
    // nothing plays by itself
    for (const char* k : { "btn_33", "btn_34", "btn_39", "btn_31" })
    {
        t.set(k, 1.0);
        t.run(0.05);
    }
    const double quiet = t.run(2.0);
    std::printf("no sequencer: %.4f\n", quiet);
    CHECK(quiet < 0.01 && !t.led(33));

    // ALL: on DRUM 1's OSC page, knob 1 (coarse tune) +12 moves it on every
    // voice by as much, knob 3 (DRUM 1's waveform, a choice) sets the drums'
    // alike and leaves the snare's noise (another parameter there) alone; off,
    // all of it goes back
    std::map<int, std::map<int, int>> before;
    for (int v = 1; v <= 7; ++v)
        before[v] = t.knobsOf(v, 16);
    t.knobsOf(1, 16);
    t.set("all", 1.0);
    t.run(0.05);
    CHECK(t.num("all") == 1.0);
    t.set("knob1", t.num("knob1") + 12.0 / 127.0);
    t.run(0.05);
    t.set("knob3", t.num("knob3") + 2.0 / 3.0);
    t.run(0.1);
    const int wave = t.knob(3);
    std::map<int, std::map<int, int>> during;
    for (int v = 1; v <= 7; ++v)
        during[v] = t.knobsOf(v, 16);
    for (int v = 1; v <= 7; ++v)
        std::printf("ALL voice %d: coarse %d -> %d, knob 3 %d -> %d\n", v, before[v][1], during[v][1], before[v][3],
                    during[v][3]);
    CHECK(during[1][1] == before[1][1] + 12 && during[2][1] == before[2][1] + 12 && during[4][1] == before[4][1] + 12);
    CHECK(during[1][3] != before[1][3] && during[2][3] == during[1][3] && during[3][3] == wave);
    CHECK(during[4][3] == before[4][3]);   // the snare's noise: another parameter
    t.knobsOf(1, 16);
    t.set("all", 0.0);
    t.run(0.1);
    std::map<int, std::map<int, int>> after;
    for (int v = 1; v <= 7; ++v)
        after[v] = t.knobsOf(v, 16);
    CHECK(after == before);
    std::printf("ALL off: %s\n", after == before ? "every voice back" : "NOT BACK");
    // without ALL, a knob is one voice's
    t.knobsOf(1, 16);
    t.set("knob1", t.num("knob1") + 5.0 / 127.0);
    t.run(0.05);
    const int drum1Coarse = t.knob(1);
    CHECK(t.knobsOf(2, 16)[1] == before[2][1]);
    t.knobsOf(1, 16);

    // PAD LEARN: the next seven pads play D1 to OH
    t.set("learn", 1.0);
    for (int n = 60; n < 67; ++n)
    {
        t.note(n, 100);
        t.run(0.05);
    }
    std::printf("learned: %s\n", t.get("pads_info").c_str());
    CHECK(t.num("learn") == 0.0);
    t.run(0.5);
    t.note(63, 127);   // SN
    CHECK(t.run(0.3) > 0.05);
    t.note(36, 127);   // no longer a pad
    CHECK(t.run(0.3) < 0.01);
    t.run(0.3);
    std::FILE* pm = std::fopen((std::filesystem::path(dataDir) / "padmap.txt").string().c_str(), "r");
    CHECK(pm != nullptr);
    if (pm)
        std::fclose(pm);

    // the faders and master level
    t.set("master", -6.0);
    CHECK(t.get("master_display") == "-6.0 dB");
    t.set("fader1", 0.5);
    CHECK(t.get("fader1_display") == "50%");

    // the state: the card, the pads and the controls, back on a new track
    const std::string state = t.get("state");
    std::printf("state: %zu bytes\n", state.size());
    t.run(0.2);
    {
        std::ifstream log(std::filesystem::path(dataDir) / "midi-log.txt");
        std::string all((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());
        std::printf("midi log: %zu bytes, last line %s", all.size(),
                    all.size() > 40 ? all.substr(all.rfind('\n', all.size() - 2) + 1).c_str() : all.c_str());
        CHECK(all.find(" 90 3F 7F") != std::string::npos);   // the snare's learned pad, played above
    }
    CHECK(state.rfind("v=4;", 0) == 0 && state.find(";card=VktD") != std::string::npos);
    CHECK(state.find(";voices=60,61,62,63,64,65,66") != std::string::npos);
    {
        Track u;
        u.set("state", state);
        u.run(1.0);
        CHECK(u.get("status").empty());
        CHECK(u.num("master") == -6.0 && u.num("fader1") == 0.5);
        u.note(60, 127);
        CHECK(u.run(0.5) > 0.02);
        CHECK(u.knobsOf(1, 16)[1] == drum1Coarse);   // the kit came back with the card
    }

    // a damaged state falls back on the default card
    {
        Track v;
        v.set("state", "v=4;card=@@@");
        v.run(1.0);
        CHECK(v.get("status").empty());
        v.note(60, 127);   // the learned pads (data/padmap.txt)
        CHECK(v.run(0.5) > 0.02);
    }

    // a blank card is prepared by the firmware itself
    {
        setenv("VOLTKIT_CARD", "/nonexistent", 1);
        Track w;
        w.run(1.0);
        std::printf("blank card   |%s|  status '%s'\n", w.lcd().c_str(), w.get("status").c_str());
        CHECK(w.get("status").empty());
        unsetenv("VOLTKIT_CARD");
    }

    std::filesystem::remove_all(dataDir);
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
