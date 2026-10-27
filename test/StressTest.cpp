// StressTest.cpp: VOLTKIT played the way a person plays it, at random, with
// the MPC's transport running: taps on DRUM 1's step keys, double taps to
// lock, page keys, knob turns, letting go, and the occasional skin echo.
// After every run: no step key reads lit, DRUM 1's steps are the ones tapped
// and no other voice has gained a step. Odd runs also press VOICE, PERF and
// REC; there only the first holds.
//
//   VOLTKIT_ENGINE=<engine library> VOLTKIT_CARD=data/card.vkc voltkit-stress-test [runs] [first run]
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

extern "C" {
#include "engine.h"
int mpc_engine_changed(void* inst, char* key, int len);
}

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace
{
const mpc_engine_t* E = nullptr;
constexpr int kBlock = 128;
constexpr int kRate = 44100;
constexpr std::array<int, 16> kSteps { 39, 38, 37, 36, 31, 30, 29, 28, 23, 22, 21, 20, 15, 14, 13, 12 };
constexpr std::array<int, 8> kPages { 16, 17, 18, 19, 8, 9, 10, 11 };   // OSC AEG MOD FM CLICK FILTER LFO MIX
constexpr int kVoiceMode = 24, kPerfMode = 25, kRec = 34;
std::string dataDir;

struct Track
{
    void* inst = nullptr;
    double ppq = 0.0, bpm = 120.0;
    bool playing = false;
    double now = 0.0;                  // seconds rendered
    std::array<double, 41> touchedAt {};   // each key's last touch

    // Two touches on a key closer than a double tap's time are one (the MPC's
    // Double Click, which a single tap or grab here doesn't send): wait it out.
    void apart(const int b)
    {
        const double wait = touchedAt[static_cast<std::size_t>(b)] + 0.45 - now;
        if (wait > 0.0)
            run(wait);
    }

    Track() { inst = E->create(dataDir.c_str()); }
    ~Track() { E->destroy(inst); }
    void set(const std::string& k, const double v) { E->set_param(inst, k.c_str(), std::to_string(v).c_str()); }
    double num(const std::string& k)
    {
        char buf[256];
        return E->get_param(inst, k.c_str(), buf, sizeof buf) > 0 ? std::atof(buf) : 0.0;
    }
    void run(const double seconds)
    {
        const int blocks = static_cast<int>(seconds * kRate / kBlock + 0.5);
        std::int16_t out[kBlock * 2];
        for (int b = 0; b < blocks; ++b)
        {
            E->song_pos(inst, ppq, bpm, playing ? 1 : 0, kBlock);
            E->render(inst, out, kBlock);
            now += static_cast<double>(kBlock) / kRate;
            if (playing)
                ppq += kBlock / (60.0 / bpm * kRate);
            char key[64];
            while (mpc_engine_changed(inst, key, sizeof key))
                ;
        }
    }
    bool led(const int l)
    {
        char k[8];
        std::snprintf(k, sizeof k, "led_%02d", l);
        return num(k) > 0.5;
    }
    std::string btn(const int b) const
    {
        char k[8];
        std::snprintf(k, sizeof k, "btn_%02d", b);
        return k;
    }
    // A skin key's touch, as the MPC sets it: its Mouse Down flips the value
    // shown, its Button sets 1, `echo` seconds later
    void touch(const int b, const double echo)
    {
        touchedAt[static_cast<std::size_t>(b)] = now;
        set(btn(b), 1.0 - num(btn(b)));
        if (echo > 0.0)
            run(echo);
        set(btn(b), 1.0);
    }
    void tap(const int b, const double echo = 0.0) { touch(b, echo); }
    std::string steps(const int voice)
    {
        set(btn(1 + voice), 1.0);   // the voice key: its steps on the step LEDs
        run(0.05);
        std::string s;
        for (const int b : kSteps)
            s += led(b) ? 'X' : '.';
        return s;
    }
};
}

int main(int argc, char** argv)
{
    if (std::getenv("VOLTKIT_CARD") == nullptr)
    {
        std::fprintf(stderr, "set VOLTKIT_CARD (data/card.vkc) and VOLTKIT_ENGINE\n");
        return 2;
    }
    dataDir = (std::filesystem::temp_directory_path() / "voltkit-stress-test").string();
    std::filesystem::remove_all(dataDir);
    std::filesystem::create_directories(dataDir);
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // in order with the adapter's key log (stderr)
    E = mpc_engine();
    const int runs = argc > 1 ? std::atoi(argv[1]) : 20;
    int failures = 0;

    const int first = argc > 2 ? std::atoi(argv[2]) : 0;   // [runs] [first run]
    for (int run = first; run < first + runs; ++run)
    {
        std::mt19937 rng(static_cast<unsigned>(1000 + run));
        const auto pick = [&](const int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
        const auto finger = [&] { return 0.003 * pick(40); };   // the Button's set after the Mouse Down's
        Track t;
        t.run(1.0);
        t.set("btn_01", 1.0);   // DRUM 1
        t.run(0.05);
        std::array<bool, 16> want {};
        int held = -1;
        std::string log;
        // odd runs also change modes (VOICE, PERF: its step keys 1 to 8 roll a voice, 9 to
        // 16 change patterns) and REC; there, what the steps should be is the firmware's,
        // and the check is that no key is left held
        const bool modes = run % 2 == 1;
        t.playing = pick(2) == 0;
        for (int action = 0; action < 60; ++action)
        {
            const int what = pick(10);
            char line[96];
            if (what <= 2 && held < 0)   // a step tapped
            {
                const int s = pick(16);
                t.apart(kSteps[static_cast<std::size_t>(s)]);
                t.tap(kSteps[static_cast<std::size_t>(s)], finger());
                want[static_cast<std::size_t>(s)] = !want[static_cast<std::size_t>(s)];
                std::snprintf(line, sizeof line, "tap %d; ", s + 1);
            }
            else if (what == 3 && held < 0)   // a double tap: grab it
            {
                const int s = pick(16);
                const int b = kSteps[static_cast<std::size_t>(s)];
                t.apart(b);
                t.tap(b, finger());
                t.run(0.05 + 0.003 * pick(80));   // to the second touch: its Double Click, then its Button
                t.touchedAt[static_cast<std::size_t>(b)] = t.now;
                t.set(t.btn(b), 1.0 - t.num(t.btn(b)));
                char k[8];
                std::snprintf(k, sizeof k, "lock_%02d", b);
                t.set(k, 1.0);
                t.run(finger());
                t.set(t.btn(b), 1.0);
                held = s;
                std::snprintf(line, sizeof line, "grab %d; ", s + 1);
            }
            else if (what == 4)   // a page key
            {
                const int p = pick(8);
                t.tap(kPages[static_cast<std::size_t>(p)], finger());
                std::snprintf(line, sizeof line, "page %d; ", p);
            }
            else if (what <= 6)   // a knob or a Q-Link
            {
                const int k = 1 + pick(4);
                const std::string key = "knob" + std::to_string(k);
                t.set(key, std::clamp(t.num(key) + (pick(2) ? 1 : -1) * (1 + pick(20)) / 127.0, 0.0, 1.0));
                std::snprintf(line, sizeof line, "knob%d; ", k);
            }
            else if (what == 7 && held >= 0)   // let go
            {
                t.apart(kSteps[static_cast<std::size_t>(held)]);
                t.tap(kSteps[static_cast<std::size_t>(held)], finger());
                std::snprintf(line, sizeof line, "let go %d; ", held + 1);
                held = -1;
            }
            else if (what == 9 && modes)   // a mode key, or REC
            {
                const int m = pick(3);
                t.tap(m == 0 ? kVoiceMode : m == 1 ? kPerfMode : kRec, finger());
                std::snprintf(line, sizeof line, m == 0 ? "VOICE; " : m == 1 ? "PERF; " : "REC; ");
            }
            else if (what == 8)   // the transport
            {
                t.playing = !t.playing;
                if (t.playing)
                    t.ppq = 0.0;
                std::snprintf(line, sizeof line, t.playing ? "play; " : "stop; ");
            }
            else
                line[0] = '\0';
            log += line;
            t.run(0.05 + 0.01 * pick(76));   // no finger taps twice within the debounce
            if (std::getenv("STRESS_TRACE") && std::atoi(std::getenv("STRESS_TRACE")) == run && !t.playing)
            {
                std::string leds;
                for (const int b : kSteps)
                    leds += t.led(b) ? 'X' : '.';
                std::string want_;
                for (const bool w : want)
                    want_ += w ? 'X' : '.';
                std::printf("  %-14s leds %s want %s held %d\n", line, leds.c_str(), want_.c_str(), held + 1);
            }
        }
        if (held >= 0)
        {
            t.tap(kSteps[static_cast<std::size_t>(held)], 0.1);
            log += "let go; ";
        }
        t.playing = false;
        t.run(0.5);

        bool ok = true;
        std::string report;
        for (const int b : kSteps)
            if (t.num(t.btn(b)) > 0.5)
            {
                ok = false;
                report += "  step key " + t.btn(b) + " reads lit\n";
            }
        std::string expect;
        for (const bool w : want)
            expect += w ? 'X' : '.';
        if (!modes)
        {
            const std::string d1 = t.steps(0);
            if (d1 != expect)
            {
                ok = false;
                report += "  DRUM 1 " + d1 + " (tapped " + expect + ")\n";
            }
            for (int v = 1; v < 7; ++v)
            {
                const std::string s = t.steps(v);
                if (s != "................")
                {
                    ok = false;
                    report += "  voice " + std::to_string(v + 1) + " " + s + "\n";
                }
            }
        }
        std::printf("run %2d: %s\n", run, ok ? "ok" : "FAILED");
        if (!ok)
        {
            ++failures;
            std::printf("%s  actions: %s\n", report.c_str(), log.c_str());
        }
    }
    std::filesystem::remove_all(dataDir);
    std::printf("%d runs, %d failed\n", runs, failures);
    return failures != 0;
}
