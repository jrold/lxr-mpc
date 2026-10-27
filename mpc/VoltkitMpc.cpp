// MPC OS (Open MPC VST2 wrapper) engine for VOLTKIT: the LXR-02's seven
// voices (engine/volt_machine.h) as a voice module behind the MPC's
// touchscreen. The MPC's sequencer plays them; the LXR's own sequencer has
// no keys and no clock.
//
// The panel's controls are parameters (mpc/gen_layout.py):
//   btn_NN           the voice and page keys (the firmware's button numbers). A
//                    tap is down for one of the firmware's key scans and up on
//                    the next; they read 0 (their LEDs show the state)
//   knob1..knob4     the four knobs (and the MPC's four Q-Links): each reads the
//                    value of the parameter the display shows above it, and
//                    setting it turns the knob until the firmware's value is there
//   all              ALL, latched: a knob turned moves its parameter on every
//                    voice that has it in the same place (by as much, or to the
//                    same setting for a choice of a few); off, every parameter
//                    it moved goes back to where it was. A knob on a global or
//                    a modulation target (MIX's second screen: track length,
//                    MIDI channel and note; LFO's targets) turns as without
//                    ALL. knobN_all (not a control) reads whether ALL takes knob N
//   fader1..fader6   the six faders; master the output level
//   learn, pads_clear, pads_info   PAD LEARN: the pads that play the voices
//   led_NN, lcd_00..lcd_31   the keys' LEDs and the 2x16 display's characters:
//                    shown, never set (the engine reports them)
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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace voltkit;
using Clock = std::chrono::steady_clock;

constexpr int kMaxFrames = 128;                 // render() gets 1..128 frames
constexpr int kDebounceMs = 150;                // a skin key's touch sets it twice (test/StressTest.cpp)
constexpr auto kKnobSettle = std::chrono::milliseconds(300);   // a knob still turning reads its target
constexpr int kButtons = VOLT_BUTTONS;          // 41
// The panel's keys (the firmware's button numbers): the seven voices, then
// OSC AEG MOD FM CLICK FILTER LFO MIX
constexpr std::array<int, 15> kPanelKeys { 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 8, 9, 10, 11 };
constexpr int kLcdCells = 32;
constexpr int kVoices = 7;
constexpr int kVoiceNote = 36;                  // the card's voice notes: 36..42 (tools/make_card.py)
constexpr int kChoiceRange = 16;                // ALL: a parameter with fewer settings is a choice, set alike
constexpr const char* kCardFile = "card.vkc";   // the default card, beside the engine library
constexpr const char* kPadMapFile = "padmap.txt";   // the learned pads, for new tracks

constexpr std::array<const char*, kVoices> kVoiceName { "DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CLOSED HAT",
                                                        "OPEN HAT" };
constexpr std::array<const char*, kVoices> kVoiceShort { "D1", "D2", "D3", "SN", "CY", "CH", "OH" };

// Pads play these unless PAD LEARN has mapped others: General MIDI drum
// notes, a few per voice, so most pad layouts reach every voice.
const std::map<int, int> kDefaultVoiceNotes {
    { 35, 0 }, { 36, 0 },
    { 41, 1 }, { 43, 1 }, { 45, 1 },
    { 47, 2 }, { 48, 2 }, { 50, 2 },
    { 37, 3 }, { 38, 3 }, { 39, 3 }, { 40, 3 },
    { 49, 4 }, { 51, 4 }, { 52, 4 }, { 55, 4 }, { 57, 4 },
    { 42, 5 }, { 44, 5 },
    { 46, 6 },
};

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

std::string noteName(const int n)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string(names[n % 12]) + std::to_string(n / 12 - 2);   // MPC's octaves: 36 = C1
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

std::int16_t toPcm(const float v)
{
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

std::vector<int> parseNotes(const std::string& text, const std::size_t most)
{
    std::vector<int> notes;
    std::stringstream ss(text);
    std::string item;
    while (notes.size() < most && std::getline(ss, item, ','))
    {
        const double n = parseDouble(item.c_str(), -1.0);
        if (n >= 0.0 && n <= 127.0)
            notes.push_back(static_cast<int>(n));
    }
    return notes;
}

std::string joinNotes(const std::vector<int>& notes)
{
    std::string s;
    for (std::size_t i = 0; i < notes.size(); ++i)
        s += (i ? "," : "") + std::to_string(notes[i]);
    return s;
}

class Instance
{
public:
    explicit Instance(const char* dataDir)
    {
        // the default card ships beside the engine (VOLTKIT_CARD: another one)
        const char* forced = std::getenv("VOLTKIT_CARD");
        const auto path = forced != nullptr && *forced != '\0'
                              ? std::filesystem::path(forced)
                              : std::filesystem::path(defaultEnginePath()).parent_path() / kCardFile;
        std::ifstream in(path, std::ios::binary);
        defaultCard.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (dataDir != nullptr && *dataDir != '\0')
        {
            pluginData = dataDir;
            readPadMap();
        }
        start(defaultCard);
        saver = std::thread([this] { saveLoop(); });
    }

    ~Instance()
    {
        {
            std::scoped_lock lock(saverMutex);
            quit = true;
        }
        saverCv.notify_all();
        saver.join();
    }

    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    // ---- wrapper calls ----

    void midi(const std::uint8_t* msg, const int len)
    {
        if (msg == nullptr || len < 1 || !machine)
            return;
        logMidi(msg, len);
        const int status = msg[0] & 0xF0;
        if (msg[0] >= 0xF8)
            return;   // realtime: no clock, start or stop reaches the LXR's sequencer
        if ((status == 0x90 || status == 0x80) && len >= 3)
        {
            const int note = msg[1] & 0x7F, vel = msg[2] & 0x7F;
            const bool on = status == 0x90 && vel > 0;
            if (on && learning)
                learnNote(note);
            const int v = voiceFor(note);
            if (v >= 0)
            {
                const std::uint8_t out[3] = { static_cast<std::uint8_t>(on ? 0x90 : 0x80),
                                              static_cast<std::uint8_t>(kVoiceNote + v),
                                              static_cast<std::uint8_t>(on ? vel : 0) };
                machine->midi(out, 3);
            }
            return;
        }
        machine->midi(msg, len);   // anything else (CCs, pitch bend...) as it comes
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
        if (const int b = indexed(k, "btn_", kButtons); b >= 0 && isPanelKey(b))
        {
            if (touched(k, v != 0.0) && machine)
                keyTap(b);
            if (v != 0.0)
                changed.insert(k);   // a key reads 0: its LED shows what it did
            return;
        }
        if (indexed(k, "lcd_", kLcdCells) >= 0 || indexed(k, "led_", kButtons) >= 0)
        {
            changed.insert(k);   // shown, never set: put back what the machine shows
            return;
        }
        if (const int i = numbered(k, "knob", VOLT_KNOBS); i >= 0)
        {
            turnKnob(i, v);
            return;
        }
        if (const int i = numbered(k, "fader", VOLT_FADERS); i >= 0)
        {
            faders[static_cast<std::size_t>(i)] = std::clamp(v, 0.0, 1.0);
            if (machine)
                machine->fader(i, static_cast<int>(std::lround(faders[static_cast<std::size_t>(i)] * 4095.0)));
            return;
        }
        if (k == "master")
            masterDb = std::clamp(v, -24.0, 6.0);
        else if (k == "all")
            setAll(v != 0.0);
        else if (k == "learn")
            setLearning(v != 0.0);
        else if (k == "pads_clear" && v != 0.0)
            clearPads();
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
            return num(cellCode(c) + 0.3);   // frame n of the skin's character strip shows character n
        if (const int i = numbered(k, "knob", VOLT_KNOBS); i >= 0)
            return num(knobShown(i));
        if (k.size() == 13 && k.compare(0, 4, "knob") == 0 && k.compare(5, 8, "_display") == 0)
        {
            const int i = numbered(k.substr(0, 5), "knob", VOLT_KNOBS);
            return i >= 0 && knobs[static_cast<std::size_t>(i)].param != -1000
                       ? writeText(buf, len, std::to_string(knobValueShown(i)))
                       : writeText(buf, len, "-");
        }
        if (k == "pads_clear")
            return num(0.0);
        if (const int i = numbered(k, "fader", VOLT_FADERS); i >= 0)
            return num(faders[static_cast<std::size_t>(i)]);
        if (k.size() > 8 && k.compare(k.size() - 8, 8, "_display") == 0)
        {
            const std::string base = k.substr(0, k.size() - 8);
            if (const int i = numbered(base, "fader", VOLT_FADERS); i >= 0)
                return writeText(buf, len, formatNum(faders[static_cast<std::size_t>(i)] * 100.0, 0) + "%");
            if (base == "master")
                return writeText(buf, len, (masterDb > 0.05 ? "+" : "") + formatNum(masterDb, 1) + " dB");
            return 0;
        }
        if (k == "master")
            return num(masterDb);
        if (k == "all")
            return num(allOn ? 1.0 : 0.0);
        if (k.size() == 9 && k.compare(0, 4, "knob") == 0 && k.compare(5, 4, "_all") == 0)
        {
            VoltKnob o;
            const int i = numbered(k.substr(0, 5), "knob", VOLT_KNOBS);
            return num(i >= 0 && machine && machine->voice_knob(machine->voice(), i, &o) ? 1.0 : 0.0);
        }
        if (k == "learn")
            return num(learning ? 1.0 : 0.0);
        if (k == "pads_info")
            return writeText(buf, len, padsText());
        if (k == "status")
            return writeText(buf, len, status);
        return 0;
    }

    void render(std::int16_t* out, const int frames)
    {
        if (out == nullptr || frames <= 0)
            return;
        const float gain = static_cast<float>(std::pow(10.0, masterDb / 20.0));
        framesRendered += frames;
        float l[kMaxFrames], r[kMaxFrames];
        for (int done = 0; done < frames;)
        {
            const int n = std::min(kMaxFrames, frames - done);
            if (machine)
                machine->render(l, r, n);
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
            // what the firmware changed on its display, LEDs and knobs since last asked
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
                for (const int l : kPanelKeys)
                    if (ledOn(l) != ledShown[static_cast<std::size_t>(l)])
                        changed.insert(indexedKey("led_", l));
            }
            for (int i = 0; i < VOLT_KNOBS; ++i)
                pollKnob(i);
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
    // every key change sent to the firmware (VOLTKIT_KEYLOG: printed, with the time)
    void keyTap(const int b)
    {
        if (keyLog)
            std::fprintf(stderr, "%8.3f tap %d\n", framesRendered / double(VOLT_SAMPLE_RATE), b);
        machine->tap(b);
    }

    // What a knob shows: the firmware's parameter under it, or, while it is
    // still turning there, where it was sent.
    struct Knob
    {
        int param = -1000;        // -1000: nothing under it
        int value = 0, min = 0, max = 127;
        int target = 0;
        bool turning = false;
        Clock::time_point sentAt {};
    };

    // A fresh engine copy with `card` on its SD card, booted. A blank card is
    // formatted by the firmware, which then asks for a reboot: it gets one.
    void start(const std::string& card, const int tries = 2)
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
        if (!card.empty() && !card::unpack(card, *m))
            status = "CARD UNREADABLE";
        if (m->boot() != 0)
        {
            if (tries > 1 && m->card_version() != 0)
            {
                const std::string prepared = card::pack(*m);   // what the firmware wrote: boot on it
                start(prepared, tries - 1);
                return;
            }
            status = "FIRMWARE DID NOT START";
            markAllChanged();
            return;
        }
        machine = m;
        for (std::size_t i = 0; i < faders.size(); ++i)
            machine->fader(static_cast<int>(i), static_cast<int>(std::lround(faders[i] * 4095.0)));
        knobs = {};
        allOn = false;
        allFrom.clear();
        markAllChanged();
    }

    void markAllChanged()
    {
        lcdSeen = ledSeen = ~0u;
        lcdShown.fill(-1);
        ledShown.fill(false);
        for (int c = 0; c < kLcdCells; ++c)
            changed.insert(indexedKey("lcd_", c));
        for (const int l : kPanelKeys)
            changed.insert(indexedKey("led_", l));
        for (const char* k : { "all", "master", "learn", "pads_info" })
            changed.insert(k);
        for (int i = 1; i <= VOLT_FADERS; ++i)
            changed.insert("fader" + std::to_string(i));
    }

    // ---- keys

    // A skin key's touch sets its parameter twice: its Mouse Down flips it, and
    // its Button sets it to 1 (which, the key shown unlit again in between, is
    // another press). Sets less than debounceMs of audio apart are one touch,
    // and a touch is one press: the first of its sets that `presses` (a 1).
    bool touched(const std::string& key, const bool presses)
    {
        Touch& t = touches[key];
        const bool fresh = framesRendered - t.lastSet >= static_cast<long long>(debounceMs) * VOLT_SAMPLE_RATE / 1000;
        t.lastSet = framesRendered;
        if (fresh)
            t.pressed = false;
        if (!presses || t.pressed)
            return false;
        t.pressed = true;
        return true;
    }

    // ---- knobs: the firmware's values

    void turnKnob(const int i, const double v)
    {
        VoltKnob info;
        if (!machine || !machine->knob(i, &info))
            return;
        Knob& kn = knobs[static_cast<std::size_t>(i)];
        const int target = static_cast<int>(std::lround(info.min + std::clamp(v, 0.0, 1.0) * (info.max - info.min)));
        if (target == info.value && !kn.turning)
            return;   // the host reading back what it was shown
        if (allOn)
            turnAll(i, info, target);
        machine->knob_to(i, target);
        kn.target = target;
        kn.turning = true;
        kn.sentAt = Clock::now();
    }

    void pollKnob(const int i)
    {
        Knob& kn = knobs[static_cast<std::size_t>(i)];
        VoltKnob info;
        if (!machine->knob(i, &info))
            info = VoltKnob { -1000, 0, 0, 127, 0 };
        bool report = false;
        if (kn.turning
            && (info.value == kn.target || info.param != kn.param || Clock::now() - kn.sentAt > kKnobSettle))
        {
            kn.turning = false;
            report = true;   // where it got to, which may be short of where it was sent
        }
        if (info.param != kn.param || info.value != kn.value || info.min != kn.min || info.max != kn.max)
        {
            kn.param = info.param;
            kn.value = info.value;
            kn.min = info.min;
            kn.max = info.max;
            report |= !kn.turning;
        }
        if (report)
        {
            changed.insert("knob" + std::to_string(i + 1));
            changed.insert("knob" + std::to_string(i + 1) + "_display");
        }
    }

    int knobValueShown(const int i) const
    {
        const Knob& kn = knobs[static_cast<std::size_t>(i)];
        return kn.turning ? kn.target : kn.value;
    }

    double knobShown(const int i) const
    {
        const Knob& kn = knobs[static_cast<std::size_t>(i)];
        return kn.max > kn.min ? static_cast<double>(knobValueShown(i) - kn.min) / (kn.max - kn.min) : 0.0;
    }

    // ---- ALL

    // The parameter a voice had before ALL first moved it: kept until ALL is off.
    int allOriginal(const int param, const int value)
    {
        return allFrom.try_emplace(param, value).first->second;
    }

    // Knob i turned to `target` with ALL on: every other voice with the same
    // parameter in that place (by name) moves by as much from where it was, or,
    // for a choice of a few settings, to the same one. The voice shown moves
    // with the knob itself.
    void turnAll(const int i, const VoltKnob& shown, const int target)
    {
        VoltKnob me;
        if (!machine->voice_knob(machine->voice(), i, &me))
            return;   // not a sound parameter: this voice's alone
        const int delta = target - allOriginal(shown.param, shown.value);
        const bool choice = shown.max - shown.min < kChoiceRange;
        for (int v = 0; v < kVoices; ++v)
        {
            VoltKnob o;
            if (!machine->voice_knob(v, i, &o) || o.param == shown.param || o.name != shown.name)
                continue;
            const int from = allOriginal(o.param, o.value);
            machine->set_param(o.param, std::clamp(choice ? target : from + delta, o.min, o.max));
        }
    }

    // ALL off: everything it moved back as it was.
    void setAll(const bool on)
    {
        if (on == allOn)
            return;
        allOn = on;
        changed.insert("all");
        if (!on && machine)
        {
            for (int i = 0; i < VOLT_KNOBS; ++i)
            {
                // a knob still on its way to where ALL sent it: back instead
                VoltKnob info;
                Knob& kn = knobs[static_cast<std::size_t>(i)];
                if (kn.turning && machine->knob(i, &info))
                    if (const auto it = allFrom.find(info.param); it != allFrom.end())
                    {
                        machine->knob_to(i, it->second);
                        kn.target = it->second;
                    }
            }
            for (const auto& [param, value] : allFrom)
                machine->set_param(param, value);
        }
        allFrom.clear();
    }

    // ---- the display and LEDs

    static bool isPanelKey(const int b) { return std::find(kPanelKeys.begin(), kPanelKeys.end(), b) != kPanelKeys.end(); }

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

    // ---- the pads

    int voiceFor(const int note) const
    {
        if (!voiceNotes.empty())
        {
            const auto it = std::find(voiceNotes.begin(), voiceNotes.end(), note);
            return it == voiceNotes.end() ? -1 : static_cast<int>(it - voiceNotes.begin());
        }
        const auto it = kDefaultVoiceNotes.find(note);
        return it == kDefaultVoiceNotes.end() ? -1 : it->second;
    }

    // PAD LEARN: each new note becomes the next pad (D1 to OH).
    void learnNote(const int note)
    {
        std::vector<int>& notes = voiceNotes;
        if (std::find(notes.begin(), notes.end(), note) != notes.end())
            return;
        notes.push_back(note);
        changed.insert("pads_info");
        if (notes.size() == kVoices)
        {
            learning = false;
            changed.insert("learn");
            queuePadMapSave();
        }
    }

    void setLearning(const bool on)
    {
        if (on == learning)
            return;
        learning = on;
        changed.insert("learn");
        changed.insert("pads_info");
        if (on)
            voiceNotes.clear();   // start over: the next pad is the first
        else
            queuePadMapSave();
    }

    void clearPads()
    {
        learning = false;
        voiceNotes.clear();
        changed.insert("learn");
        changed.insert("pads_info");
        queuePadMapSave();
    }

    std::string padsText() const
    {
        if (!status.empty())
            return status;
        const std::vector<int>& notes = voiceNotes;
        if (learning)
        {
            const std::size_t n = notes.size();
            return "LEARN: HIT THE PAD FOR " + std::string(kVoiceName[std::min<std::size_t>(n, kVoices - 1)]) + "  ("
                   + std::to_string(n + 1) + " OF 7)";
        }
        if (!notes.empty())
        {
            std::string s = "PADS: LEARNED ";
            for (std::size_t i = 0; i < notes.size(); ++i)
                s += std::string("  ") + kVoiceShort[i] + " " + noteName(notes[i]);
            return s;
        }
        return "PADS: GM DRUM NOTES (D1 36, D2 45, D3 48, SN 38, CY 49, CH 42, OH 46)";
    }

    // What MIDI came in, the last kMidiLog messages: written to data/midi-log.txt
    // with each save, to see where a sound came from.
    void logMidi(const std::uint8_t* msg, const int len)
    {
        MidiLogEntry& e = midiLog[midiLogNext++ % midiLog.size()];
        e.frame = framesRendered;
        e.len = static_cast<std::uint8_t>(std::min(len, 3));
        std::memcpy(e.bytes, msg, e.len);
    }

    std::string midiLogText() const
    {
        std::string s = "# the MIDI VOLTKIT received last: seconds of audio rendered, bytes\n";
        const std::size_t n = std::min<std::size_t>(midiLogNext, midiLog.size());
        for (std::size_t k = midiLogNext - n; k < midiLogNext; ++k)
        {
            const MidiLogEntry& e = midiLog[k % midiLog.size()];
            char line[64];
            std::snprintf(line, sizeof line, "%10.3f ", static_cast<double>(e.frame) / VOLT_SAMPLE_RATE);
            s += line;
            for (int i = 0; i < e.len; ++i)
            {
                std::snprintf(line, sizeof line, " %02X", e.bytes[i]);
                s += line;
            }
            s += "\n";
        }
        return s;
    }

    // The learned pads, for new tracks: data/padmap.txt, written off the audio thread.
    void readPadMap()
    {
        std::ifstream in(std::filesystem::path(pluginData) / kPadMapFile);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.rfind("voices ", 0) == 0)
                voiceNotes = parseNotes(line.substr(7), kVoices);
        }
    }

    void queuePadMapSave()
    {
        std::scoped_lock lock(saverMutex);
        toSave = "voices " + joinNotes(voiceNotes) + "\n";
        savePending = true;
        saverCv.notify_all();
    }

    void queueMidiLogSave() const
    {
        std::scoped_lock lock(saverMutex);
        logToSave = midiLogText();
        savePending = true;
        saverCv.notify_all();
    }

    void saveLoop()
    {
        std::unique_lock lock(saverMutex);
        while (!quit)
        {
            saverCv.wait(lock, [this] { return quit || savePending; });
            if (!savePending || pluginData.empty())
            {
                savePending = false;
                continue;
            }
            const std::string text = toSave, log = logToSave;
            toSave.clear();
            logToSave.clear();
            savePending = false;
            lock.unlock();
            if (!text.empty())
            {
                std::ofstream out(std::filesystem::path(pluginData) / kPadMapFile, std::ios::trunc);
                out << text;
            }
            if (!log.empty())
            {
                std::ofstream out(std::filesystem::path(pluginData) / "midi-log.txt", std::ios::trunc);
                out << log;
            }
            lock.lock();
        }
    }

    // ---- state: the card, and what the plug-in itself keeps

    std::string saveState() const
    {
        std::string s = "v=4;master=" + formatNum(masterDb, 2) + ";faders=";
        for (std::size_t i = 0; i < faders.size(); ++i)
            s += (i ? "," : "") + formatNum(faders[i], 4);
        s += ";voices=" + joinNotes(voiceNotes);
        queueMidiLogSave();
        if (machine)
        {
            machine->flush();   // what the firmware would autosave in a moment: on the card now
            s += ";card=" + card::base64(card::pack(*machine));
        }
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
            if (k == "master")
                masterDb = std::clamp(parseDouble(v.c_str(), 0.0), -24.0, 6.0);
            else if (k == "faders")
            {
                std::stringstream ss(v);
                std::string f;
                for (std::size_t i = 0; i < faders.size() && std::getline(ss, f, ','); ++i)
                    faders[i] = std::clamp(parseDouble(f.c_str(), 1.0), 0.0, 1.0);
            }
            else if (k == "voices")
                voiceNotes = parseNotes(v, kVoices);
            else if (k == "card")
                cardText = v;
        }
        learning = false;
        std::string card;
        start(!cardText.empty() && card::unbase64(cardText, card) ? card : defaultCard);
    }

    std::unique_ptr<EngineCopy> copy;
    const VoltMachineApi* machine = nullptr;
    std::string defaultCard;
    std::string status;

    long long framesRendered = 0;
    std::array<Knob, VOLT_KNOBS> knobs {};
    std::array<double, VOLT_FADERS> faders { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
    double masterDb = 0.0;

    bool allOn = false;
    std::map<int, int> allFrom;      // ALL: each parameter it moved, as it was
    bool learning = false;
    std::vector<int> voiceNotes;     // learned: voiceNotes[v] plays voice v
    std::string pluginData;

    std::uint32_t lcdSeen = ~0u, ledSeen = ~0u;
    std::array<int, kLcdCells> lcdShown {};
    std::array<bool, kButtons> ledShown {};
    std::set<std::string> changed;   // keys to report to MPC (mpc_engine_changed)

    struct Touch
    {
        long long lastSet = -(1LL << 40);   // frames rendered at the key's last set
        bool pressed = false;               // this touch has pressed it
    };
    std::map<std::string, Touch> touches;
    // VOLTKIT_DEBOUNCE_MS: the offline test turns it off (its taps set a key once).
    const int debounceMs = std::getenv("VOLTKIT_DEBOUNCE_MS") ? std::atoi(std::getenv("VOLTKIT_DEBOUNCE_MS")) : kDebounceMs;
    const bool keyLog = std::getenv("VOLTKIT_KEYLOG") != nullptr;

    struct MidiLogEntry
    {
        long long frame = 0;
        std::uint8_t len = 0;
        std::uint8_t bytes[3] {};
    };
    std::array<MidiLogEntry, 256> midiLog {};
    std::size_t midiLogNext = 0;

    mutable std::mutex saverMutex;
    mutable std::condition_variable saverCv;
    mutable std::string toSave, logToSave;
    mutable bool savePending = false;
    bool quit = false;
    std::thread saver;   // last: started after everything it uses
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
const mpc_engine_t kEngine { engineCreate, engineDestroy, engineMidi, engineSetParam, engineGetParam, engineRender, nullptr,
                             nullptr };
}

extern "C" const mpc_engine_t* mpc_engine(void)
{
    return &kEngine;
}

extern "C" int mpc_engine_changed(void* inst, char* key, const int len)
{
    return static_cast<Instance*>(inst)->takeChangedKey(key, len) ? 1 : 0;
}
