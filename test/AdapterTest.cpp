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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <utility>
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
    double ppq = 0.0;
    bool playing = false;
    double bpm = 120.0;
    std::set<std::string> seen;   // changed keys reported
    std::vector<float> audio;     // the left channel, when recording
    bool recording = false;
    double jitter = 0.0;          // beats the reported position wobbles by, either way
    double loop = 0.0;            // beats the MPC's sequence loops after, if set
    std::mt19937 wobble { 7 };

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
            const double off = jitter * (std::uniform_real_distribution<double>(-1.0, 1.0)(wobble));
            E->song_pos(inst, std::fmax(ppq + off, 0.0), bpm, playing ? 1 : 0, kBlock);
            E->render(inst, out, kBlock);
            if (playing)
                ppq += kBlock / (60.0 / bpm * kRate);
            if (loop > 0.0 && ppq >= loop)
                ppq -= loop;
            for (const std::int16_t s : out)
                peak = std::fmax(peak, std::abs(s) / 32768.0);
            if (recording)
                for (int i = 0; i < kBlock; ++i)
                    audio.push_back(out[2 * i] / 32768.0f);
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

    // Plays one bar from the top with the transport, recording the audio.
    void playBar()
    {
        playing = true;
        ppq = 0.0;
        recording = true;
        audio.clear();
        run(2.0);
        recording = false;
        playing = false;
        run(0.3);
    }

    // Zero crossings 30..110 ms after `at` seconds into the recording: the
    // pitch, once a drum's sweep has settled.
    int crossings(const double at)
    {
        int n = 0;
        const std::size_t a = static_cast<std::size_t>((at + 0.03) * kRate), b = a + kRate * 8 / 100;
        for (std::size_t i = a + 1; i < b && i < audio.size(); ++i)
            n += (audio[i - 1] < 0) != (audio[i] < 0);
        return n;
    }

    // Each hit's time against its step (16ths at bpm), in ms: an onset is a
    // sharp rise of the 2.5 ms loudness.
    std::vector<double> hitTimes()
    {
        std::vector<double> out;
        const double stepFrames = 60.0 / bpm / 4.0 * kRate;
        double prev = 0.0;
        long last = -100000;
        for (std::size_t w = 0; w + 110 <= audio.size(); w += 110)
        {
            double e = 0.0;
            for (std::size_t i = w; i < w + 110; ++i)
                e += audio[i] * audio[i];
            e = std::sqrt(e / 110.0);
            if (e > 0.03 && e > 4.0 * prev + 0.01 && static_cast<long>(w) - last > 2200)
            {
                // the window's first sample above a tenth of its loudness
                std::size_t i = w;
                while (i < w + 110 && std::fabs(audio[i]) < 0.1 * e)
                    ++i;
                const double steps = i / stepFrames;
                out.push_back((steps - std::round(steps)) * stepFrames * 1000.0 / kRate);
                last = static_cast<long>(w);
            }
            prev = e;
        }
        return out;
    }

    double level(const double at)
    {
        double peak = 0.0;
        const std::size_t a = static_cast<std::size_t>(at * kRate), b = a + kRate / 10;
        for (std::size_t i = a; i < b && i < audio.size(); ++i)
            peak = std::fmax(peak, std::fabs(audio[i]));
        return peak;
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
    CHECK(t.seen.count("lcd_00") && t.seen.count("led_00"));

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

    // the encoder moves the selection
    const std::string beforeEnc = t.lcd();
    t.set("enc", 0.5 + 1.0 / 128.0);
    t.run(0.05);
    std::printf("encoder +1   |%s|\n", t.lcd().c_str());
    CHECK(t.lcd() != beforeEnc);

    // SHIFT latches
    t.set("shift", 1.0);
    t.run(0.05);
    CHECK(t.num("shift") == 1.0);
    t.set("shift", 0.0);
    t.run(0.05);

    // steps 1, 5, 9 and 13 on voice 1, then the MPC's transport plays them
    t.set("btn_24", 1.0);   // VOICE
    t.set("btn_01", 1.0);   // DRUM 1
    t.set("btn_16", 1.0);   // OSC page
    t.run(0.05);
    for (const char* step : { "btn_39", "btn_31", "btn_23", "btn_15" })
    {
        t.set(step, 1.0);
        t.run(0.02);
    }
    CHECK(t.led(39) && t.led(31) && t.led(23) && t.led(15));
    const double idle = t.run(0.5);
    t.playBar();
    const double stopped = t.run(0.5);
    std::printf("transport: %.3f stopped, step 1 %.3f, %.3f after stop\n", idle, t.level(0.0), stopped);
    CHECK(idle < 0.01 && t.level(0.0) > 0.05 && stopped < 0.01);
    CHECK(std::abs(t.crossings(0.0) - t.crossings(0.5)) <= 2);
    for (const auto [jitter, loop] : { std::pair { 0.0, 0.0 }, { 0.004, 0.0 }, { 0.004, 8.0 } })
    {
        // in time: eight bars, every hit against its step; again with the MPC's
        // position wobbling by up to 1 ms (0.004 beats) either way, and looping
        // two bars
        t.jitter = jitter;
        t.loop = loop;
        t.playing = true;
        t.ppq = 0.0;
        t.recording = true;
        t.audio.clear();
        t.run(16.0);
        t.recording = false;
        t.playing = false;
        t.run(0.3);
        const std::vector<double> d = t.hitTimes();
        double lo = 1e9, hi = -1e9, sum = 0.0;
        for (std::size_t i = 1; i + 1 < d.size(); ++i)   // not the first (late by the lead after a start) or the last
        {
            lo = std::fmin(lo, d[i]);
            hi = std::fmax(hi, d[i]);
            sum += d[i];
        }
        const double mean = d.size() > 2 ? sum / (d.size() - 2) : 0.0;
        t.jitter = t.loop = 0.0;
        std::printf("timing%s%s: %zu hits, %.2f ms on average from the step, %.2f to %.2f\n",
                    jitter > 0 ? " (wobbling)" : "", loop > 0 ? " (looping)" : "", d.size(), mean, lo, hi);
        CHECK(d.size() >= 32);
        CHECK(std::fabs(mean) < 1.0 && hi - lo < 1.6);
    }

    // locks with a double tap on step 13's key (its two taps, then the skin's
    // Double Click): it reads lit and stays held; a knob turned at once waits
    // for the lock to arm; FILTER (whose release ends a lock in the firmware)
    // and a knob there lock too; a tap, here one that sets 0, lets go
    const std::string osc = t.lcd();
    t.set("btn_09", 1.0);   // FILTER page, to see the kit's own filter
    t.run(0.05);
    const std::string filter = t.lcd();
    t.set("btn_16", 1.0);   // back to OSC
    t.run(0.05);
    t.set("btn_15", 1.0);
    t.set("btn_15", 1.0);
    t.set("lock_15", 1.0);
    t.set("knob1", t.num("knob1") + 36.0 / 127.0);   // at once: waits for the lock
    t.run(1.0);
    CHECK(t.num("btn_15") == 1.0);
    const std::string lockedOsc = t.lcd();
    t.set("btn_09", 1.0);   // FILTER
    t.set("knob1", t.num("knob1") - 60.0 / 127.0);   // at once again
    t.run(3.0);
    const std::string lockedFilter = t.lcd();
    std::printf("held        OSC |%s|  FILTER |%s|\n", lockedOsc.c_str(), lockedFilter.c_str());
    CHECK(lockedOsc != osc && lockedFilter != filter && t.num("btn_15") == 1.0);
    t.set("btn_15", 0.0);   // the tap that lets go
    t.run(0.1);
    std::printf("let go   FILTER |%s|\n", t.lcd().c_str());
    CHECK(t.lcd() == filter && t.num("btn_15") == 0.0);   // the kit's own filter is back
    t.set("btn_16", 1.0);
    t.run(0.05);
    CHECK(t.lcd() == osc);
    CHECK(t.led(15));   // step 13 still on
    // both locks are on the step: held again, it shows them
    t.set("btn_15", 1.0);
    t.set("btn_15", 1.0);
    t.set("lock_15", 1.0);
    t.run(0.8);
    CHECK(t.lcd() == lockedOsc);
    t.set("btn_09", 1.0);
    t.run(0.8);
    CHECK(t.lcd() == lockedFilter);
    t.set("btn_15", 1.0);
    t.run(0.1);
    t.set("btn_16", 1.0);
    t.run(0.05);
    t.playBar();
    std::printf("double-tap locks: step 1 %d crossings, step 13 %d\n", t.crossings(0.0), t.crossings(1.5));
    CHECK(t.crossings(1.5) > t.crossings(0.0) + 3);
    // a double tap whose second tap came too soon to count: step 3 (off) stays off
    CHECK(!t.led(37));
    t.set("btn_37", 1.0);
    t.set("lock_37", 1.0);
    t.run(0.7);
    CHECK(t.num("btn_37") == 1.0);
    t.set("btn_37", 1.0);
    t.run(0.1);
    CHECK(!t.led(37) && t.num("btn_37") == 0.0);
    // in PERF a held step key rolls its voice (recording it with REC on): a double
    // tap on step 6 there is two taps, CL HH's roll, never a hold
    t.set("btn_25", 1.0);   // PERF
    t.set("btn_34", 1.0);   // REC
    t.playing = true;
    t.ppq = 0.0;
    t.run(0.3);
    t.set("btn_30", 1.0);
    t.run(0.1);
    t.set("btn_30", 1.0);
    t.set("lock_30", 1.0);
    t.run(2.0);
    CHECK(t.num("btn_30") == 0.0);
    t.playing = false;
    t.set("btn_34", 1.0);
    t.set("btn_24", 1.0);   // VOICE
    t.run(0.3);
    t.set("btn_06", 1.0);   // CL HH: what the two taps recorded, at most
    t.run(0.05);
    int clhh = 0;
    for (const int l : { 39, 38, 37, 36, 31, 30, 29, 28, 23, 22, 21, 20, 15, 14, 13, 12 })
        clhh += t.led(l) ? 1 : 0;
    std::printf("perf double tap: CL HH %d steps\n", clhh);
    CHECK(clhh <= 2);
    t.set("btn_01", 1.0);
    t.run(0.05);

    // a lock with the pads as step keys: pad 9 held, the knob turned, let go
    t.set("pad_mode", 1.0);
    t.run(0.05);
    std::printf("pads: %s\n", t.get("pads_info").c_str());
    CHECK(t.num("pad_mode") == 1.0);
    t.note(36 + 8, 100);   // step 9, held
    t.run(0.7);
    t.set("knob1", t.num("knob1") + 24.0 / 127.0);
    t.run(0.1);
    t.noteOff(36 + 8);
    t.run(0.2);
    CHECK(t.lcd() == osc);
    CHECK(t.led(23));
    t.playBar();
    std::printf("pad lock: step 1 %d crossings, step 9 %d\n", t.crossings(0.0), t.crossings(1.0));
    CHECK(t.crossings(1.0) > t.crossings(0.0) + 3);
    // a quick pad tap toggles a step, as the key does
    t.note(36 + 1, 100);
    t.run(0.05);
    t.noteOff(36 + 1);
    t.run(0.05);
    CHECK(t.led(38));   // step 2 on
    t.set("pad_mode", 0.0);
    t.run(0.05);

    // recording from the pads: REC, the transport plays, snares on beats 2 and 4
    t.set("btn_04", 1.0);   // SNARE: its steps on the step LEDs
    t.run(0.05);
    CHECK(!t.led(31) && !t.led(15));
    t.set("btn_34", 1.0);   // REC
    t.playing = true;
    t.ppq = 0.0;
    for (int b = 0; b < 4; ++b)
    {
        if (b == 1 || b == 3)
            t.note(38, 120);
        t.run(0.5);
    }
    t.playing = false;
    t.run(0.1);
    t.set("btn_34", 1.0);   // REC off
    t.run(0.1);
    std::printf("recorded: step 5 %d, step 13 %d\n", t.led(31), t.led(15));
    CHECK(t.led(31) && t.led(15));

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
    t.note(63, 127);   // SN (the drums may still carry the last lock they played)
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
    CHECK(state.rfind("v=3;", 0) == 0 && state.find(";card=VktD") != std::string::npos);
    CHECK(state.find(";voices=60,61,62,63,64,65,66") != std::string::npos);
    {
        Track u;
        u.set("state", state);
        u.run(1.0);
        CHECK(u.get("status").empty());
        CHECK(u.num("master") == -6.0 && u.num("fader1") == 0.5);
        u.note(60, 127);
        CHECK(u.run(0.5) > 0.02);
        u.set("btn_24", 1.0);
        u.set("btn_04", 1.0);   // SNARE: the recorded steps came back with the card
        u.run(0.1);
        CHECK(u.led(31) && u.led(15));
    }

    // a damaged state falls back on the default card
    {
        Track v;
        v.set("state", "v=3;card=@@@");
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
