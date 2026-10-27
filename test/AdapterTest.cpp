// AdapterTest.cpp: VOLTKIT's plug-in adapter (mpc/VoltkitMpc.cpp) driven as
// the MPC's wrapper drives it: parameters set and read by key, the changed
// keys collected, song_pos then render each 128-frame block.
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

struct Track
{
    void* inst = nullptr;
    double ppq = 0.0;
    bool playing = false;
    double bpm = 120.0;
    std::set<std::string> seen;   // changed keys reported

    Track() { inst = E->create(nullptr); }
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
        const int blocks = static_cast<int>(seconds * kRate / kBlock);
        std::int16_t out[kBlock * 2];
        for (int b = 0; b < blocks; ++b)
        {
            E->song_pos(inst, ppq, bpm, playing ? 1 : 0, kBlock);
            E->render(inst, out, kBlock);
            if (playing)
                ppq += kBlock / (60.0 / bpm * kRate);
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
            s += static_cast<char>(std::lround(num(k) * 127.0));
            if (c == 15)
                s += '|';
        }
        return s;
    }

    int ledsLit()
    {
        int n = 0;
        for (int l = 0; l < 41; ++l)
        {
            char k[8];
            std::snprintf(k, sizeof k, "led_%02d", l);
            if (num(k) > 0.5)
            {
                ++n;
                if (std::getenv("VOLTKIT_VERBOSE"))
                    std::printf(" %d", l);
            }
        }
        return n;
    }

    void note(const int n, const int vel)
    {
        const std::uint8_t m[3] = { 0x90, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(vel) };
        E->midi(inst, m, 3);
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
    E = mpc_engine();

    Track t;
    CHECK(t.inst != nullptr);
    CHECK(t.get("status").empty());
    t.run(0.2);
    std::printf("start        |%s|  status '%s'\n", t.lcd().c_str(), t.get("status").c_str());
    CHECK(t.seen.count("lcd_00") && t.seen.count("led_00"));

    // the pads: notes on channel 1 play the voices
    t.note(36, 127);
    const double kick = t.run(0.5);
    std::printf("note 36: peak %.3f\n", kick);
    CHECK(kick > 0.05);

    // a tap on AEG shows its page; the key lets go by itself
    t.seen.clear();
    t.set("btn_17", 1.0);
    CHECK(t.num("btn_17") == 1.0);
    t.run(0.3);
    const std::string aeg = t.lcd();
    std::printf("AEG key      |%s|\n", aeg.c_str());
    CHECK(aeg.find("ATK") != std::string::npos || aeg.find("atk") != std::string::npos);
    CHECK(t.num("btn_17") == 0.0);
    CHECK(t.seen.count("btn_17"));

    // knob 1 (attack) turned up by Q-Link clicks, and it springs back to centre
    for (int i = 0; i < 12; ++i)
    {
        t.set("knob1", 0.5 + 1.0 / 128.0);
        t.run(0.02);
    }
    t.run(0.3);
    const std::string turned = t.lcd();
    std::printf("knob 1 up    |%s|\n", turned.c_str());
    CHECK(turned != aeg);
    CHECK(t.num("knob1") == 0.5);
    for (int i = 0; i < 12; ++i)
    {
        t.set("knob1", 0.5 - 1.0 / 128.0);
        t.run(0.02);
    }
    t.run(0.3);
    std::printf("knob 1 back  |%s|\n", t.lcd().c_str());
    CHECK(t.lcd() == aeg);

    // the encoder and its push move through the menu
    t.set("enc", 0.5 + 1.0 / 128.0);
    t.run(0.2);
    std::printf("encoder +1   |%s|\n", t.lcd().c_str());

    // SHIFT latches, HOLD keeps the next key down until it is tapped again
    t.set("shift", 1.0);
    t.run(0.1);
    CHECK(t.num("shift") == 1.0);
    t.set("shift", 0.0);
    t.set("hold", 1.0);
    t.set("btn_02", 1.0);
    t.run(0.3);
    CHECK(t.num("btn_02") == 1.0 && t.num("hold") == 0.0);
    t.set("btn_02", 1.0);
    t.run(0.1);
    CHECK(t.num("btn_02") == 0.0);

    if (std::getenv("VOLTKIT_VERBOSE"))
    {
        t.set("btn_25", 1.0);   // PERF
        t.run(0.3);
        std::printf("perf mode    |%s|\n", t.lcd().c_str());
        t.set("btn_24", 1.0);   // VOICE
        t.run(0.3);
    }
    // steps 1, 5, 9 and 13 on voice 1, then the MPC's transport plays them
    t.set("btn_01", 1.0);   // voice 1
    t.run(0.2);
    for (const char* step : { "btn_39", "btn_31", "btn_23", "btn_15" })
    {
        t.set(step, 1.0);
        t.run(0.15);
    }
    std::printf("steps set    |%s|  %d LEDs lit\n", t.lcd().c_str(), t.ledsLit());
    const double idle = t.run(1.0);
    t.playing = true;
    t.ppq = 0.0;
    double played = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        played = std::fmax(played, t.run(0.5));
        if (std::getenv("VOLTKIT_VERBOSE"))
            std::printf("  playing ppq %.2f |%s| leds:", t.ppq, t.lcd().c_str()), t.ledsLit(), std::printf("\n");
    }
    t.playing = false;
    t.run(0.1);
    const double stopped = t.run(1.0);
    std::printf("transport: %.3f stopped, %.3f playing, %.3f after stop\n", idle, played, stopped);
    CHECK(idle < 0.01);
    CHECK(played > 0.05);
    CHECK(stopped < 0.01);

    // the faders and master level
    t.set("master", -6.0);
    CHECK(t.get("master_display") == "-6.0 dB");
    t.set("fader1", 0.5);
    CHECK(t.get("fader1_display") == "50%");

    // the state: the card and the controls, back on a new track
    const std::string state = t.get("state");
    std::printf("state: %zu bytes\n", state.size());
    CHECK(state.rfind("v=2;", 0) == 0 && state.find(";card=VktD") != std::string::npos);
    CHECK(state.size() < (4u << 20));
    {
        Track u;
        u.set("state", state);
        u.run(0.2);
        CHECK(u.get("status").empty());
        CHECK(u.num("master") == -6.0 && u.num("fader1") == 0.5);
        u.note(36, 127);
        CHECK(u.run(0.5) > 0.02);
        const std::string back = u.get("state");
        if (const char* dump = std::getenv("VOLTKIT_DUMP"))
        {
            FILE* f = std::fopen(dump, "w");
            std::fprintf(f, "%s\n%s\n", state.c_str(), back.c_str());
            std::fclose(f);
        }
        // the same card (but the FAT's free-cluster hint, rewritten when mounted)
        CHECK(std::abs(static_cast<long>(back.size()) - static_cast<long>(state.size())) <= 8);
    }

    // a damaged state falls back on the default card
    {
        Track v;
        v.set("state", "v=2;card=@@@");
        v.run(0.2);
        CHECK(v.get("status").empty());
        v.note(36, 127);
        CHECK(v.run(0.5) > 0.02);
    }

    // two tracks are two machines
    {
        Track a, b;
        a.run(1.0);
        b.run(1.0);
        a.set("btn_17", 1.0);
        a.run(0.3);
        b.run(0.3);
        std::printf("track a      |%s|\ntrack b      |%s|\n", a.lcd().c_str(), b.lcd().c_str());
        CHECK(a.lcd() != b.lcd());
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
