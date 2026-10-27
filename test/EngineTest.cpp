// VOLTKIT's MPC adapter and engine, driven the way the Open MPC wrapper drives
// them: through mpc_engine(). Uses a scratch data folder, never a real one.

extern "C" {
#include "engine.h"
}

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

extern "C" int mpc_engine_changed(void* inst, char* key, int len);

namespace
{

int failures = 0;
int checks = 0;

#define CHECK(cond)                                                                 \
    do                                                                              \
    {                                                                               \
        ++checks;                                                                   \
        if (!(cond))                                                                \
        {                                                                           \
            ++failures;                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                           \
    } while (0)

const mpc_engine_t* E = nullptr;

std::string get(void* inst, const char* key)
{
    char buf[4096] = {};
    return E->get_param(inst, key, buf, sizeof buf) > 0 ? buf : "";
}

void set(void* inst, const char* key, const std::string& v) { E->set_param(inst, key, v.c_str()); }

void note(void* inst, const int n, const int vel = 110)
{
    const std::uint8_t msg[3] = { 0x90, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(vel) };
    E->midi(inst, msg, 3);
}

// Peak level of `frames` rendered frames.
double peak(void* inst, const int frames)
{
    std::vector<std::int16_t> out(256);
    double p = 0;
    for (int done = 0; done < frames; done += 128)
    {
        E->render(inst, out.data(), 128);
        for (const auto s : out)
            p = std::max(p, std::abs(s) / 32768.0);
    }
    return p;
}

void drainChanged(void* inst)
{
    char key[128];
    while (mpc_engine_changed(inst, key, sizeof key)) {}
}

}

int main()
{
    // The test's own engine library and a scratch data folder.
    setenv("VOLTKIT_ENGINE", VOLTKIT_TEST_ENGINE, 1);
    setenv("VOLTKIT_DEBOUNCE_MS", "0", 1);
    const auto data = std::filesystem::temp_directory_path() / ("voltkit-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(data);
    E = mpc_engine();

    void* a = E->create(data.string().c_str());
    void* b = E->create(data.string().c_str());
    CHECK(a && b);
    CHECK(get(a, "pads_info").find("NOT LOADED") == std::string::npos);

    // Every pad slot sounds, from the GM default notes.
    for (const int n : { 36, 45, 48, 38, 49, 42, 46 })
    {
        peak(a, 44100);   // let the last one ring out
        note(a, n);
        const double p = peak(a, 4410);
        std::printf("note %d: peak %.3f\n", n, p);
        CHECK(p > 0.02);
    }
    const std::uint8_t allOff[3] = { 0xB0, 123, 0 };
    E->midi(a, allOff, 3);
    peak(a, 4410);
    note(a, 60);   // not mapped
    CHECK(peak(a, 4410) < 1e-4);

    // Two instances keep their own sound: a muted drum 1 on A only.
    peak(a, 88200);
    peak(b, 88200);
    set(a, "d1_volume", "0");
    peak(a, 4410);   // the firmware glides volume changes (a one-pole, ~10 ms to settle)
    note(a, 36);
    note(b, 36);
    CHECK(peak(a, 4410) < 1e-3);
    CHECK(peak(b, 4410) > 0.02);
    CHECK(get(a, "d1_volume") == "0");
    CHECK(get(b, "d1_volume") != "0");

    // Values and their text.
    set(a, "d1_coarse", "70");
    CHECK(get(a, "d1_coarse") == "70");
    CHECK(get(a, "d1_coarse_display") == "+10");
    set(a, "d1_pan", "63");
    CHECK(get(a, "d1_pan_display") == "0");
    set(a, "d1_veldest", "5");
    CHECK(get(a, "d1_veldest") == "5");
    set(a, "d1_out", "1");
    CHECK(get(a, "d1_out") == "1");

    // An LFO keeps its kind of target when its voice changes.
    set(a, "d1_ldest", "21");   // filter cutoff
    CHECK(get(a, "d1_ldest") == "21");
    CHECK(get(a, "d1_ldest_display") == "D1 Filter Cutoff");
    set(a, "d1_lvoice", "3");   // snare
    CHECK(get(a, "d1_lvoice") == "3");
    CHECK(get(a, "d1_ldest_display") == "SN Filter Cutoff");
    set(a, "d1_ldest", "0");
    CHECK(get(a, "d1_ldest_display") == "Off");

    // Each effect keeps its own four knobs.
    set(a, "fx_type", "1");
    set(a, "fx_drv_drive", "100");
    set(a, "fx_type", "4");
    set(a, "fx_dly_time", "20");
    CHECK(get(a, "fx_drv_drive") == "100");
    CHECK(get(a, "fx_dly_time") == "20");
    set(a, "fx_type", "1");
    CHECK(get(a, "fx_drv_drive") == "100");
    set(a, "fx_mix", "40");
    CHECK(get(a, "fx_mix_display") == "40%");
    set(a, "volume", "-6");
    CHECK(get(a, "volume_display") == "-6.0 dB");
    set(a, "d1_page", "3");
    drainChanged(a);

    // Save and restore: a new instance from A's state says the same.
    const std::string state = get(a, "state");
    CHECK(state.rfind("v=1;kit=", 0) == 0);
    void* c = E->create(data.string().c_str());
    set(c, "state", state);
    CHECK(get(c, "state") == state);
    CHECK(get(c, "d1_coarse") == "70");
    CHECK(get(c, "fx_drv_drive") == "100");
    CHECK(get(c, "d1_page") == "3");
    char key[128];
    int reported = 0;
    while (mpc_engine_changed(c, key, sizeof key))
        ++reported;
    CHECK(reported > 200);   // the screen follows a restored kit

    // INIT KIT: back to the firmware's init kit.
    set(c, "init_kit", "1");
    CHECK(get(c, "d1_coarse") == get(b, "d1_coarse"));
    CHECK(get(c, "d1_volume") == get(b, "d1_volume"));

    // PAD LEARN: seven pads, then it stops by itself and saves the map.
    set(b, "learn", "1");
    CHECK(get(b, "learn") == "1");
    CHECK(get(b, "pads_info").find("DRUM 1") != std::string::npos);
    const int learned[7] = { 60, 61, 62, 63, 64, 65, 66 };
    for (const int n : learned)
        note(b, n);
    CHECK(get(b, "learn") == "0");
    CHECK(get(b, "pads_info").find("LEARNED") != std::string::npos);
    E->midi(b, allOff, 3);
    peak(b, 4410);
    note(b, 36);   // no longer mapped
    CHECK(peak(b, 4410) < 1e-4);
    note(b, 63);   // the snare's pad now
    CHECK(peak(b, 4410) > 0.02);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));   // the saver
    {
        std::ifstream in(data / "padmap.txt");
        int n = 0, count = 0;
        while (in >> n)
            ++count;
        CHECK(count == 7);
    }
    void* d = E->create(data.string().c_str());   // new tracks start with the learned pads
    peak(d, 88200);
    note(d, 63);
    CHECK(peak(d, 4410) > 0.02);
    set(d, "pads_clear", "1");
    CHECK(!std::filesystem::exists(data / "padmap.txt"));

    // STOP twice stops the sound; once doesn't.
    set(d, "hh_odecay", "127");
    peak(d, 88200);
    note(d, 46);
    peak(d, 1024);
    set(d, "mains", "0");
    set(d, "mains", "1");
    CHECK(peak(d, 2048) > 0.005);
    set(d, "mains", "0");
    set(d, "mains", "1");
    peak(d, 1024);
    CHECK(peak(d, 4410) < 1e-4);

    // All Sound Off (CC 123).
    note(d, 46);
    peak(d, 1024);
    const std::uint8_t off[3] = { 0xB0, 123, 0 };
    E->midi(d, off, 3);
    peak(d, 1024);
    CHECK(peak(d, 4410) < 1e-4);

    // Odd block sizes (SAMPLE_ACCURATE) and the cost of a busy kit.
    std::vector<std::int16_t> buf(256);
    for (int frames = 1; frames <= 128; frames += 37)
        E->render(a, buf.data(), frames);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 344 * 10; ++i)   // 10 s
    {
        if (i % 43 == 0)
            for (const int n : { 36, 38, 42, 45, 48, 49 })
                note(a, n);
        E->render(a, buf.data(), 128);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("10 s of busy kit rendered in %.3f s (%.1fx real time)\n", secs, 10.0 / secs);

    for (void* inst : { a, b, c, d })
        E->destroy(inst);
    std::filesystem::remove_all(data);

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
