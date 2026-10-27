// StressTest.cpp: VOLTKIT played the way a person plays it, at random: voice
// and page keys touched as the MPC touches them, knobs and Q-Links turned, ALL
// on and off. After each time ALL goes off, every voice reads, on every page
// and screen, what it read when ALL came on (a knob turned without ALL, or
// one ALL doesn't take, meanwhile drops that check); after every run, no step of any voice is set
// (the LXR's sequencer has no keys).
//
//   VOLTKIT_ENGINE=<engine library> VOLTKIT_CARD=data/card.vkc voltkit-stress-test [runs] [first run]
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

extern "C" {
#include "engine.h"
int mpc_engine_changed(void* inst, char* key, int len);
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <tuple>

namespace
{
const mpc_engine_t* E = nullptr;
constexpr int kBlock = 128;
constexpr int kRate = 44100;
constexpr std::array<int, 7> kVoices { 1, 2, 3, 4, 5, 6, 7 };
constexpr std::array<int, 8> kPages { 16, 17, 18, 19, 8, 9, 10, 11 };   // OSC AEG MOD FM CLICK FILTER LFO MIX
constexpr std::array<int, 16> kSteps { 39, 38, 37, 36, 31, 30, 29, 28, 23, 22, 21, 20, 15, 14, 13, 12 };
std::string dataDir;
using Everything = std::map<std::tuple<int, int, int>, std::array<std::string, 4>>;

struct Track
{
    void* inst = nullptr;
    int voice = 1, page = 16, screen = 0;   // what the test has shown
    std::mt19937* rng = nullptr;

    Track() { inst = E->create(dataDir.c_str()); }
    ~Track() { E->destroy(inst); }
    void set(const std::string& k, const double v) { E->set_param(inst, k.c_str(), std::to_string(v).c_str()); }
    std::string text(const std::string& k)
    {
        char buf[256];
        return E->get_param(inst, k.c_str(), buf, sizeof buf) > 0 ? buf : "";
    }
    double num(const std::string& k) { return std::atof(text(k).c_str()); }
    void run(const double seconds)
    {
        const int blocks = static_cast<int>(seconds * kRate / kBlock + 0.5);
        std::int16_t out[kBlock * 2];
        for (int b = 0; b < blocks; ++b)
        {
            E->render(inst, out, kBlock);
            char key[64];
            while (mpc_engine_changed(inst, key, sizeof key))
                ;
        }
    }
    static std::string btn(const int b)
    {
        char k[8];
        std::snprintf(k, sizeof k, "btn_%02d", b);
        return k;
    }
    // A skin key's touch, as the MPC sets it: its Mouse Down flips the value
    // shown, its Button sets 1 up to 120 ms later; then the finger is off it
    // long enough not to be a double tap
    void touch(const int b)
    {
        set(btn(b), 1.0 - num(btn(b)));
        run(0.003 * static_cast<int>((*rng)() % 40));
        set(btn(b), 1.0);
        run(0.2);
    }
    void showVoice(const int v)
    {
        touch(v);
        voice = v;
    }
    // A page key: its first screen, or, shown already, its other screen
    void showPage(const int p)
    {
        touch(p);
        screen = p == page ? 1 - screen : 0;
        page = p;
    }
    std::string lcd()
    {
        std::string s;
        for (int c = 0; c < 32; ++c)
        {
            char k[8];
            std::snprintf(k, sizeof k, "lcd_%02d", c);
            const int code = static_cast<int>(num(k));
            s += code >= 32 && code < 127 ? static_cast<char>(code) : '?';
        }
        return s;
    }
    std::array<std::string, 4> knobs()
    {
        return { text("knob1_display"), text("knob2_display"), text("knob3_display"), text("knob4_display") };
    }

    // Every voice's knobs on every page and screen; the voice, page and
    // screen shown are back as they were after.
    Everything everything()
    {
        const int v0 = voice, p0 = page, s0 = screen;
        Everything all;
        for (const int v : kVoices)
        {
            showVoice(v);
            for (const int p : kPages)
            {
                showPage(p);
                if (screen != 0)
                    showPage(p);
                all[{ v, p, 0 }] = knobs();
                const std::string first = lcd();
                showPage(p);
                if (lcd() != first)
                    all[{ v, p, 1 }] = knobs();
                else
                    screen = 0;   // no second screen: the key left it on the first
            }
        }
        showVoice(v0);
        showPage(p0);
        if (screen != s0)
            showPage(p0);
        return all;
    }
};

std::string row(const std::array<std::string, 4>& k) { return k[0] + " " + k[1] + " " + k[2] + " " + k[3]; }
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    E = mpc_engine();
    const int runs = argc > 1 ? std::atoi(argv[1]) : 10;
    const int first = argc > 2 ? std::atoi(argv[2]) : 0;
    int failures = 0;

    for (int run = first; run < first + runs; ++run)
    {
        std::mt19937 rng(static_cast<unsigned>(1000 + run));
        const auto pick = [&](const int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
        Track t;
        t.rng = &rng;
        t.run(1.0);
        t.showVoice(1);
        t.showPage(16);
        bool ok = true;
        std::string log, report;
        Everything snapshot;
        for (int action = 0; action < 40; ++action)
        {
            const int what = pick(10);
            const bool all = t.num("all") > 0.5;
            if (what <= 1)
            {
                t.showVoice(kVoices[static_cast<std::size_t>(pick(7))]);
                log += "voice " + std::to_string(t.voice) + "; ";
            }
            else if (what <= 3)
            {
                t.showPage(kPages[static_cast<std::size_t>(pick(8))]);
                log += "page " + std::to_string(t.page) + (t.screen ? "b; " : "; ");
            }
            else if (what <= 6)
            {
                const int k = 1 + pick(4);
                const std::string key = "knob" + std::to_string(k);
                const bool taken = t.num(key + "_all") > 0.5;
                t.set(key, std::clamp(t.num(key) + (pick(2) ? 1 : -1) * (1 + pick(30)) / 127.0, 0.0, 1.0));
                t.run(0.05);
                if (!all || !taken)
                    snapshot.clear();   // a change of its own
                log += key + "; ";
            }
            else if (what == 7 && !all)
            {
                snapshot = t.everything();
                t.set("all", 1.0);
                t.run(0.05);
                log += "ALL on; ";
            }
            else if (what == 8 && all)
            {
                t.set("all", 0.0);
                t.run(0.1);
                log += "ALL off; ";
                if (!snapshot.empty())
                {
                    const Everything now = t.everything();
                    for (const auto& [where, was] : snapshot)
                    {
                        const auto it = now.find(where);
                        if (it == now.end() || it->second != was)
                        {
                            ok = false;
                            const auto [v, p, s] = where;
                            report += "  voice " + std::to_string(v) + " page " + std::to_string(p) + (s ? "b" : "")
                                      + ": " + row(was) + " -> " + (it == now.end() ? "gone" : row(it->second)) + "\n";
                        }
                    }
                    snapshot.clear();
                }
            }
            t.run(0.05 + 0.01 * pick(40));
        }
        t.set("all", 0.0);
        t.run(0.5);
        for (const int v : kVoices)
        {
            t.showVoice(v);
            for (const int s : kSteps)
                if (t.num("led_" + std::to_string(s)) > 0.5)
                {
                    ok = false;
                    report += "  voice " + std::to_string(v) + ": step key " + std::to_string(s) + " lit\n";
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
