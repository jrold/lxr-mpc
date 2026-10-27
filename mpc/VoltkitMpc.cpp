// MPC OS (Open MPC VST2 wrapper) engine for VOLTKIT: the whole LXR-02
// (engine/volt_machine.h) behind the MPC's touchscreen.
//
// The panel's controls are parameters (mpc/gen_layout.py):
//   btn_00..btn_40   the panel keys (the firmware's button numbers; 40 = PAGE DOWN):
//                    a tap presses one and lets go after kTapFrames. Momentary:
//                    they read 0 (their LEDs show the state), so every tap is a 1
//   shift, hold      SHIFT latched; HOLD keeps the next key tapped held down
//                    until it is tapped again (for the LXR's key combinations)
//   knob1..knob4     the four endless knobs; enc the encoder, enc_push its push.
//                    Each move counts from the value last set, and once a knob
//                    has rested kRecentreMs it reads 0.5 again: endless, under
//                    a finger's drag or a Q-Link's clicks
//   fader1..fader6   the six faders; master the output level
//   sync             MIDI clock, start and stop from the MPC's transport (the
//                    card's tempo is MID for it; off, set a tempo on the LXR)
//   led_00..led_40   the keys' LEDs, lcd_00..lcd_31 the 2x16 display's
//                    characters: shown, never set (the engine reports them)
//
// Each instance runs its own copy of the engine library (EngineLoader.h): the
// firmware keeps its state in globals. The wrapper calls this one call at a
// time per instance, under its own lock.
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

extern "C" {
#include "engine.h"
}

#include "CardCodec.h"
#include "EngineLoader.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{
using namespace voltkit;

constexpr int kSubBlock = 64;                   // the firmware's own block
constexpr int kTapFrames = 2646;                // a tapped key is held 60 ms (the firmware scans every few ms)
constexpr int kDebounceMs = 150;                // a skin key fires its press twice
constexpr auto kDoubleStopWindow = std::chrono::milliseconds(1200);
constexpr int kButtons = VOLT_BUTTONS;          // 41
constexpr int kShiftButton = 32;                // BUT_SHIFT
constexpr int kLcdCells = 32;
constexpr double kKnobDegreesPerUnit = 512.0;   // a Q-Link click (1/128) turns a knob 4 degrees
constexpr double kKnobStep = 3.0;               // at most this many degrees per firmware block: each step registers
constexpr double kEncoderDetentsPerUnit = 128.0;
constexpr int kRecentreMs = 250;
constexpr int kDials = 5;                       // knob1..knob4, enc
constexpr const char* kCardFile = "card.vkc";   // the default card, beside the engine library

double parseDouble(const char* text, const double fallback)
{
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    return end == text || !std::isfinite(v) ? fallback : v;
}

int writeText(char* buf, const int len, const std::string& text)
{
    if (buf == nullptr || len <= 0)
        return 0;
    const auto n = static_cast<int>(std::min<std::size_t>(text.size(), static_cast<std::size_t>(len - 1)));
    std::memcpy(buf, text.data(), static_cast<std::size_t>(n));
    buf[n] = '\0';
    return n;
}

std::string formatNum(const double v, const int decimals = 4)
{
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    return b;
}

// "btn_07" -> 7 for a key with that prefix and a two-digit index below `count`; else -1.
int indexed(const std::string& key, const char* prefix, const int count)
{
    const std::size_t n = std::strlen(prefix);
    if (key.size() != n + 2 || key.compare(0, n, prefix) != 0 || !std::isdigit(static_cast<unsigned char>(key[n]))
        || !std::isdigit(static_cast<unsigned char>(key[n + 1])))
        return -1;
    const int i = (key[n] - '0') * 10 + (key[n + 1] - '0');
    return i < count ? i : -1;
}

std::string indexedKey(const char* prefix, const int i)
{
    char b[16];
    std::snprintf(b, sizeof b, "%s%02d", prefix, i);
    return b;
}

// "fader3" -> 2 for prefix "fader" and digits 1..count; else -1.
int numbered(const std::string& key, const char* prefix, const int count)
{
    const std::size_t n = std::strlen(prefix);
    if (key.size() != n + 1 || key.compare(0, n, prefix) != 0)
        return -1;
    const int i = key[n] - '1';
    return i >= 0 && i < count ? i : -1;
}

// An endless knob's two wipers at `deg`: triangle tracks 90 degrees apart, as
// the firmware's adcPots.c reads them (raw 16-bit ADC readings).
std::array<int, 2> wipers(const double deg)
{
    const double t = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
    const double b = t <= 180.0 ? t : 360.0 - t;
    const double a = t <= 90.0 ? 90.0 + t : t <= 270.0 ? 270.0 - t : t - 270.0;
    const auto raw = [](const double d) { return static_cast<int>(std::lround(d / 180.0 * 0xFFF)) << 4; };
    return { raw(a), raw(b) };
}

int faderRaw(const double v)
{
    return static_cast<int>(std::lround(v * 0xFFF)) << 4;
}

std::int16_t toPcm(const float v)
{
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

class Instance
{
public:
    explicit Instance(const char*)
    {
        // the default card ships beside the engine (VOLTKIT_CARD: another one)
        const char* forced = std::getenv("VOLTKIT_CARD");
        const auto path = forced != nullptr && *forced != '\0'
                              ? std::filesystem::path(forced)
                              : std::filesystem::path(defaultEnginePath()).parent_path() / kCardFile;
        std::ifstream in(path, std::ios::binary);
        defaultCard.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        start(defaultCard);
    }

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    void midi(const std::uint8_t* msg, const int len)
    {
        if (msg != nullptr && len > 0 && machine)
            machine->midi(msg, len);   // the pads and anything else: the LXR's DIN MIDI input
    }

    void setParam(const char* key, const char* val)
    {
        if (key == nullptr)
            return;
        const std::string k(key);
        if (k == "state")
        {
            restore(val != nullptr ? val : "");
            return;
        }
        const double v = parseDouble(val, 0.0);
        if (k == "mains")
        {
            // MPC suspends and resumes its plug-ins on every STOP press (our
            // patched wrapper passes it on): two close together stop the LXR.
            if (v == 0.0)
            {
                const auto now = std::chrono::steady_clock::now();
                if (lastSuspend && now - *lastSuspend < kDoubleStopWindow && machine)
                {
                    const std::uint8_t stop = 0xFC;
                    machine->midi(&stop, 1);
                }
                lastSuspend = now;
            }
            return;
        }
        if (const int b = indexed(k, "btn_", kButtons); b >= 0)
        {
            if (v != 0.0)
                tapKey(b);
            return;   // the release is ours (kTapFrames), not the wrapper's
        }
        if (k == "shift")
        {
            setHeld(kShiftButton, v != 0.0);
            return;
        }
        if (k == "hold")
        {
            holdNext = v != 0.0;
            return;
        }
        if (const int d = dialIndex(k); d >= 0)
        {
            const double moved = turn(d, v);
            if (d < 4)
                knobs[static_cast<std::size_t>(d)].target += moved * kKnobDegreesPerUnit;
            else
            {
                encoderRest += moved * kEncoderDetentsPerUnit;
                const int detents = static_cast<int>(encoderRest);
                encoderRest -= detents;
                if (detents != 0 && machine)
                    machine->encoder(detents);
            }
            return;
        }
        if (k == "enc_push")
        {
            if (v != 0.0 && machine && !bouncing(k))
            {
                machine->encoder_button(1);
                encoderRelease = kTapFrames;
            }
            return;
        }
        if (const int i = numbered(k, "fader", VOLT_ADC_FADERS); i >= 0)
        {
            faders[static_cast<std::size_t>(i)] = std::clamp(v, 0.0, 1.0);
            if (machine)
                machine->adc(VOLT_ADC_KNOBS + i, faderRaw(faders[static_cast<std::size_t>(i)]));
            return;
        }
        if (k == "master")
            masterDb = std::clamp(v, -24.0, 6.0);
        else if (k == "sync")
            sync = v != 0.0;
    }

    int getParam(const char* key, char* buf, const int len)
    {
        if (key == nullptr)
            return 0;
        const std::string k(key);
        const auto num = [&](const double x) { return writeText(buf, len, formatNum(x)); };
        if (k == "state")
            return writeText(buf, len, saveState());
        if (indexed(k, "btn_", kButtons) >= 0)
            return num(0.0);
        if (const int l = indexed(k, "led_", kButtons); l >= 0)
            return num(ledOn(l) ? 1.0 : 0.0);
        if (const int c = indexed(k, "lcd_", kLcdCells); c >= 0)
            return num(cellCode(c));   // 0..127: the skin's character strip
        if (k == "shift")
            return num(down[kShiftButton] ? 1.0 : 0.0);
        if (k == "hold")
            return num(holdNext ? 1.0 : 0.0);
        if (const int d = dialIndex(k); d >= 0)
            return num(dials[static_cast<std::size_t>(d)].last);
        if (k == "enc_push")
            return num(0.0);
        if (const int i = numbered(k, "fader", VOLT_ADC_FADERS); i >= 0)
            return num(faders[static_cast<std::size_t>(i)]);
        if (k.size() > 8 && k.compare(k.size() - 8, 8, "_display") == 0)
        {
            const std::string base = k.substr(0, k.size() - 8);
            if (const int i = numbered(base, "fader", VOLT_ADC_FADERS); i >= 0)
                return writeText(buf, len, formatNum(faders[static_cast<std::size_t>(i)] * 100.0, 0) + "%");
            if (base == "master")
                return writeText(buf, len, (masterDb > 0.05 ? "+" : "") + formatNum(masterDb, 1) + " dB");
            return 0;
        }
        if (k == "master")
            return num(masterDb);
        if (k == "sync")
            return num(sync ? 1.0 : 0.0);
        if (k == "status")
            return writeText(buf, len, status);
        return 0;
    }

    // Called before each render: the MIDI clocks (24 per beat), start and stop
    // that fall in the coming block, at their frames.
    void songPos(const double ppq, const double bpm, const int playing, const int frames)
    {
        clocks.clear();
        clockFrame = 0;
        if (!machine || !sync)
        {
            wasPlaying = false;
            return;
        }
        const bool isPlaying = playing != 0 && ppq >= 0.0 && bpm > 0.0;
        if (isPlaying && (!wasPlaying || ppq + 1e-6 < lastPpq))   // started, or the MPC jumped back (a loop)
            clocks.push_back({ 0, 0xFA });
        if (!isPlaying && wasPlaying)
            clocks.push_back({ 0, 0xFC });
        if (isPlaying)
        {
            const double framesPerBeat = 60.0 / bpm * VOLT_SAMPLE_RATE;
            const double end = ppq + frames / framesPerBeat;
            for (double tick = std::ceil(ppq * 24.0 - 1e-9); tick / 24.0 < end; tick += 1.0)
                clocks.push_back({ static_cast<int>((tick / 24.0 - ppq) * framesPerBeat), 0xF8 });
            lastPpq = ppq;
        }
        wasPlaying = isPlaying;
    }

    void render(std::int16_t* out, const int frames)
    {
        if (out == nullptr || frames <= 0)
            return;
        const float gain = static_cast<float>(std::pow(10.0, masterDb / 20.0));
        float l[kSubBlock], r[kSubBlock];
        for (int done = 0; done < frames;)
        {
            const int n = std::min(kSubBlock, frames - done);
            if (machine)
            {
                beforeBlock(n);
                machine->render(l, r, n);
            }
            else
            {
                std::fill(l, l + n, 0.0f);
                std::fill(r, r + n, 0.0f);
            }
            for (int i = 0; i < n; ++i)
            {
                out[(done + i) * 2] = toPcm(l[i] * gain);
                out[(done + i) * 2 + 1] = toPcm(r[i] * gain);
            }
            done += n;
        }
    }

    bool takeChangedKey(char* key, const int len)
    {
        if (machine)
        {
            // what the firmware changed on its display and LEDs since last asked
            if (machine->lcd_version() != lcdSeen)
            {
                lcdSeen = machine->lcd_version();
                for (int c = 0; c < kLcdCells; ++c)
                    if (cellCode(c) != lcdShown[static_cast<std::size_t>(c)])
                        changed.insert(indexedKey("lcd_", c));
            }
            if (machine->led_version() != ledSeen)
            {
                ledSeen = machine->led_version();
                for (int l = 0; l < kButtons; ++l)
                    if (ledOn(l) != ledShown[static_cast<std::size_t>(l)])
                        changed.insert(indexedKey("led_", l));
            }
        }
        const auto now = std::chrono::steady_clock::now();
        for (int d = 0; d < kDials; ++d)
        {
            Dial& dial = dials[static_cast<std::size_t>(d)];
            if (dial.resting || now - dial.lastSet < std::chrono::milliseconds(recentreMs))
                continue;
            dial.resting = true;
            dial.last = 0.5;
            changed.insert(dialKey(d));
        }
        if (changed.empty())
            return false;
        const std::string k = *changed.begin();
        changed.erase(changed.begin());
        writeText(key, len, k);
        if (const int c = indexed(k, "lcd_", kLcdCells); c >= 0)
            lcdShown[static_cast<std::size_t>(c)] = cellCode(c);
        if (const int l = indexed(k, "led_", kButtons); l >= 0)
            ledShown[static_cast<std::size_t>(l)] = ledOn(l);
        return true;
    }

private:
    struct Knob
    {
        double angle = 90.0;    // where the wipers are
        double target = 90.0;   // where the user turned it to
    };

    struct Clock
    {
        int frame;
        std::uint8_t byte;
    };

    struct Dial
    {
        double last = 0.5;   // the value the host last set (and reads back)
        std::chrono::steady_clock::time_point lastSet {};
        bool resting = true;
    };

    static int dialIndex(const std::string& key)
    {
        return key == "enc" ? 4 : numbered(key, "knob", 4);
    }

    static std::string dialKey(const int d)
    {
        return d == 4 ? std::string("enc") : "knob" + std::to_string(d + 1);
    }

    // How far a knob or the encoder moved since the value last set.
    double turn(const int d, const double v)
    {
        Dial& dial = dials[static_cast<std::size_t>(d)];
        const double moved = v - dial.last;
        dial.last = v;
        dial.lastSet = std::chrono::steady_clock::now();
        dial.resting = false;
        return moved;
    }

    // A fresh engine copy with `card` on its SD card, booted.
    void start(const std::string& card)
    {
        machine = nullptr;
        status.clear();
        copy = std::make_unique<EngineCopy>();
        const char* forced = std::getenv("VOLTKIT_ENGINE");
        if (!copy->load(forced != nullptr && *forced != '\0' ? forced : defaultEnginePath()))
        {
            status = "NO ENGINE " + copy->error();
            markAllChanged();
            return;
        }
        const VoltMachineApi* m = copy->api();
        if (card.empty())
            status = "NO CARD (engine/card.vkc)";
        else if (!card::unpack(card, *m))
            status = "CARD UNREADABLE";
        if (m->boot() != 0)
        {
            status = "FIRMWARE DID NOT START";
            markAllChanged();
            return;
        }
        machine = m;
        for (std::size_t i = 0; i < knobs.size(); ++i)
        {
            knobs[i] = Knob {};
            const auto w = wipers(knobs[i].angle);
            machine->adc(static_cast<int>(2 * i), w[0]);
            machine->adc(static_cast<int>(2 * i + 1), w[1]);
        }
        for (std::size_t i = 0; i < faders.size(); ++i)
            machine->adc(VOLT_ADC_KNOBS + static_cast<int>(i), faderRaw(faders[i]));
        down.fill(false);
        latched.fill(false);
        releaseIn.fill(0);
        holdNext = false;
        encoderRelease = 0;
        wasPlaying = false;
        markAllChanged();
    }

    void markAllChanged()
    {
        lcdSeen = ledSeen = ~0u;
        lcdShown.fill(-1);
        ledShown.fill(false);
        for (int c = 0; c < kLcdCells; ++c)
            changed.insert(indexedKey("lcd_", c));
        for (int l = 0; l < kButtons; ++l)
            changed.insert(indexedKey("led_", l));
        for (const char* k : { "shift", "hold", "sync", "master" })
            changed.insert(k);
        for (int i = 1; i <= VOLT_ADC_FADERS; ++i)
            changed.insert("fader" + std::to_string(i));
    }

    // Once per firmware block: keys let go, knobs turned a step, the clocks due.
    void beforeBlock(const int frames)
    {
        for (int b = 0; b < kButtons; ++b)
        {
            int& left = releaseIn[static_cast<std::size_t>(b)];
            if (left > 0 && (left -= frames) <= 0)
                setHeld(b, false);
        }
        if (encoderRelease > 0 && (encoderRelease -= frames) <= 0)
            machine->encoder_button(0);
        for (std::size_t i = 0; i < knobs.size(); ++i)
        {
            Knob& kn = knobs[i];
            const double diff = kn.target - kn.angle;
            if (std::abs(diff) < 1e-6)
                continue;
            kn.angle += std::clamp(diff, -kKnobStep, kKnobStep);
            const auto w = wipers(kn.angle);
            machine->adc(static_cast<int>(2 * i), w[0]);
            machine->adc(static_cast<int>(2 * i + 1), w[1]);
        }
        const int end = clockFrame + frames;
        for (const Clock& c : clocks)
            if (c.frame >= clockFrame && c.frame < end)
                machine->midi(&c.byte, 1);
        clockFrame = end;
    }

    // A tap on a panel key: pressed now, let go after kTapFrames; with HOLD,
    // held until tapped again.
    void tapKey(const int b)
    {
        if (bouncing(indexedKey("btn_", b)))
            return;
        const auto i = static_cast<std::size_t>(b);
        if (latched[i])
        {
            setHeld(b, false);
            return;
        }
        setHeld(b, true);
        if (holdNext)
        {
            latched[i] = true;
            holdNext = false;
            changed.insert("hold");
            return;
        }
        releaseIn[i] = kTapFrames;
    }

    void setHeld(const int b, const bool isDown)
    {
        const auto i = static_cast<std::size_t>(b);
        releaseIn[i] = 0;
        if (!isDown)
            latched[i] = false;
        if (down[i] == isDown)
            return;
        down[i] = isDown;
        if (machine)
            machine->button(b, isDown ? 1 : 0);
        if (b == kShiftButton)
            changed.insert("shift");
    }

    bool bouncing(const std::string& key)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto it = lastPress.find(key);
        if (it != lastPress.end() && now - it->second < std::chrono::milliseconds(debounceMs))
            return true;
        lastPress[key] = now;
        return false;
    }

    // The character at display cell c (row * 16 + column): printable ASCII,
    // anything else (the splash's and screensaver's own characters) blank.
    // Without a running engine, the display spells out why.
    int cellCode(const int c) const
    {
        if (!machine)
        {
            const auto i = static_cast<std::size_t>(c);
            const unsigned char ch = i < status.size() ? static_cast<unsigned char>(status[i]) : ' ';
            return ch >= 32 && ch < 127 ? ch : ' ';
        }
        const std::uint8_t ch = machine->lcd()[(c / 16) * 0x40 + c % 16];
        return ch >= 32 && ch < 127 ? ch : ' ';
    }

    bool ledOn(const int l) const
    {
        if (!machine)
            return false;
        const std::uint8_t* leds = machine->leds();
        return l == 40 ? leds[5] != 0 : ((leds[l >> 3] >> (l & 7)) & 1) != 0;
    }

    // ---- state: the card, and what the plug-in itself keeps

    std::string saveState() const
    {
        std::string s = "v=2;sync=" + std::to_string(sync ? 1 : 0) + ";master=" + formatNum(masterDb, 2) + ";faders=";
        for (std::size_t i = 0; i < faders.size(); ++i)
            s += (i ? "," : "") + formatNum(faders[i], 4);
        if (machine)
            s += ";card=" + card::base64(card::pack(*machine));
        return s;
    }

    void restore(const std::string& state)
    {
        std::string cardText;
        std::stringstream items(state);
        std::string item;
        while (std::getline(items, item, ';'))
        {
            const auto eq = item.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string k = item.substr(0, eq), v = item.substr(eq + 1);
            if (k == "sync")
                sync = parseDouble(v.c_str(), 1.0) != 0.0;
            else if (k == "master")
                masterDb = std::clamp(parseDouble(v.c_str(), 0.0), -24.0, 6.0);
            else if (k == "faders")
            {
                std::stringstream ss(v);
                std::string f;
                for (std::size_t i = 0; i < faders.size() && std::getline(ss, f, ','); ++i)
                    faders[i] = std::clamp(parseDouble(f.c_str(), 1.0), 0.0, 1.0);
            }
            else if (k == "card")
                cardText = v;
        }
        std::string card;
        start(!cardText.empty() && card::unbase64(cardText, card) ? card : defaultCard);
    }

    std::unique_ptr<EngineCopy> copy;
    const VoltMachineApi* machine = nullptr;
    std::string defaultCard;
    std::string status;

    std::array<bool, kButtons> down {};
    std::array<bool, kButtons> latched {};
    std::array<int, kButtons> releaseIn {};
    bool holdNext = false;
    std::array<Knob, 4> knobs {};
    std::array<Dial, kDials> dials {};
    double encoderRest = 0.0;
    int encoderRelease = 0;
    std::array<double, VOLT_ADC_FADERS> faders { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
    double masterDb = 0.0;

    bool sync = true;
    bool wasPlaying = false;
    double lastPpq = 0.0;
    std::vector<Clock> clocks;   // this host block's MIDI clock bytes, by frame
    int clockFrame = 0;          // frames of this host block rendered so far

    std::uint32_t lcdSeen = ~0u, ledSeen = ~0u;
    std::array<int, kLcdCells> lcdShown {};
    std::array<bool, kButtons> ledShown {};
    std::set<std::string> changed;   // keys to report to MPC (mpc_engine_changed)

    std::optional<std::chrono::steady_clock::time_point> lastSuspend;
    std::map<std::string, std::chrono::steady_clock::time_point> lastPress;
    // VOLTKIT_DEBOUNCE_MS, VOLTKIT_RECENTRE_MS: the offline test turns them off.
    const int debounceMs = std::getenv("VOLTKIT_DEBOUNCE_MS") ? std::atoi(std::getenv("VOLTKIT_DEBOUNCE_MS")) : kDebounceMs;
    const int recentreMs = std::getenv("VOLTKIT_RECENTRE_MS") ? std::atoi(std::getenv("VOLTKIT_RECENTRE_MS")) : kRecentreMs;
};

void* engineCreate(const char* dataDir)
{
    try
    {
        return new Instance(dataDir);
    }
    catch (...)
    {
        return nullptr;
    }
}

void engineDestroy(void* inst) { delete static_cast<Instance*>(inst); }
void engineMidi(void* inst, const std::uint8_t* msg, const int len) { static_cast<Instance*>(inst)->midi(msg, len); }
void engineSetParam(void* inst, const char* key, const char* val) { static_cast<Instance*>(inst)->setParam(key, val); }
int engineGetParam(void* inst, const char* key, char* buf, const int len)
{
    return static_cast<Instance*>(inst)->getParam(key, buf, len);
}
void engineRender(void* inst, std::int16_t* out, const int frames) { static_cast<Instance*>(inst)->render(out, frames); }
void engineSongPos(void* inst, const double ppq, const double bpm, const int playing, const int frames)
{
    static_cast<Instance*>(inst)->songPos(ppq, bpm, playing, frames);
}

const mpc_engine_t kEngine { engineCreate, engineDestroy, engineMidi, engineSetParam, engineGetParam, engineRender, nullptr,
                             engineSongPos };
}

extern "C" const mpc_engine_t* mpc_engine(void)
{
    return &kEngine;
}

extern "C" int mpc_engine_changed(void* inst, char* key, const int len)
{
    return static_cast<Instance*>(inst)->takeChangedKey(key, len) ? 1 : 0;
}
