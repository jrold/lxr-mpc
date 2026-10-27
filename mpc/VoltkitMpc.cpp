// MPC OS (Open MPC VST2 wrapper) engine for VOLTKIT: the whole LXR-02
// (engine/volt_machine.h) behind the MPC's touchscreen.
//
// The panel's controls are parameters (mpc/gen_layout.py):
//   btn_00..btn_40   the panel keys (the firmware's button numbers; 40 = PAGE DOWN).
//                    A tap is down for one of the firmware's key scans and up on
//                    the next. They read 0 (their LEDs show the state), but a
//                    step key held for a lock reads 1: it shows lit
//   shift            SHIFT, latched
//   lock_NN          a double tap on step key NN (the skin's Double Click): the
//                    step is held as a finger would hold it (the firmware arms
//                    its lock after 0.5 s; every knob turned then locks to it)
//                    until the key is tapped again. Its on/off state is kept.
//                    Another key tapped meanwhile (a page key) is tapped with the
//                    step lifted, and the step held again after: on the LXR a
//                    page key pressed while a step is held edits its sub-steps.
//                    Knobs turned while it re-arms wait for it
//   knob1..knob4     the four knobs: each reads the value of the parameter the
//                    display shows above it, and setting it turns the knob until
//                    the firmware's value is there
//   enc, enc_push    the encoder: endless, counted from the value last set; its push
//   fader1..fader6   the six faders; master the output level
//   sync             MIDI clock, start and stop from the MPC's transport (the
//                    card's tempo is MID for it; off, set a tempo on the LXR)
//   pad_mode         VOICES: the pads play the voices (learned, or General MIDI
//                    drum notes); STEPS: the pads are the 16 step keys, held
//                    while held (hold one and turn a knob: a parameter lock)
//   learn, pads_clear, pads_info   PAD LEARN for the mode's pads
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
#include <optional>
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
constexpr int kClockLead = 260;                 // frames the MIDI clock goes early: steps then sound on the
                                                // beat, +-0.8 ms (the LXR's 64-frame blocks; test/AdapterTest.cpp)
constexpr int kDebounceMs = 150;                // a skin key fires its press twice
constexpr auto kDoubleStopWindow = std::chrono::milliseconds(1200);
constexpr auto kKnobSettle = std::chrono::milliseconds(300);   // a knob still turning reads its target
constexpr int kButtons = VOLT_BUTTONS;          // 41
constexpr int kShiftButton = 32;                // BUT_SHIFT
constexpr int kLcdCells = 32;
constexpr int kVoices = 7;
constexpr int kSteps = 16;
constexpr int kVoiceNote = 36;                  // the card's voice notes: 36..42 (tools/make_card.py)
constexpr double kEncoderDetentsPerUnit = 128.0;
constexpr int kRecentreMs = 250;
constexpr int kDoubleTap = VOLT_SAMPLE_RATE * 6 / 10;         // frames: a double tap's own taps
constexpr int kGrabTapGuard = VOLT_SAMPLE_RATE * 4 / 10;     // frames: its second tap's echoes
constexpr int kArmFrames = VOLT_SAMPLE_RATE * 6 / 10;        // a held step's lock is armed by then (0.5 s)
constexpr const char* kCardFile = "card.vkc";   // the default card, beside the engine library
constexpr const char* kPadMapFile = "padmap.txt";   // the learned pads, for new tracks

// SEQ1..SEQ16: the firmware's button numbers of the step keys.
constexpr std::array<int, kSteps> kStepButtons { 39, 38, 37, 36, 31, 30, 29, 28, 23, 22, 21, 20, 15, 14, 13, 12 };
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
constexpr int kDefaultStepNote = 36;            // STEPS without a learned map: pads from C1 up

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
            return;   // realtime: the transport's clock is ours (sync)
        if ((status == 0x90 || status == 0x80) && len >= 3)
        {
            const int note = msg[1] & 0x7F, vel = msg[2] & 0x7F;
            const bool on = status == 0x90 && vel > 0;
            if (on && learning)
                learnNote(note);
            if (padMode == 0)
            {
                const int v = voiceFor(note);
                if (v >= 0)
                {
                    const std::uint8_t out[3] = { static_cast<std::uint8_t>(on ? 0x90 : 0x80),
                                                  static_cast<std::uint8_t>(kVoiceNote + v),
                                                  static_cast<std::uint8_t>(on ? vel : 0) };
                    machine->midi(out, 3);
                }
            }
            else
            {
                const int s = stepFor(note);
                if (s >= 0)
                    machine->key(kStepButtons[static_cast<std::size_t>(s)], on ? 1 : 0);
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
        if (k == "mains")
        {
            // MPC suspends and resumes its plug-ins on every STOP press (our
            // patched wrapper passes it on): two close together stop the LXR.
            if (v == 0.0)
            {
                const auto now = Clock::now();
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
            if (v != 0.0 || b == grab.button)   // the held key reads 1: its tap may set 0
                tapKey(b);
            return;
        }
        if (indexed(k, "lcd_", kLcdCells) >= 0 || indexed(k, "led_", kButtons) >= 0)
        {
            changed.insert(k);   // shown, never set: put back what the machine shows
            return;
        }
        if (k == "shift")
        {
            setHeld(kShiftButton, v != 0.0);
            return;
        }
        if (const int b = indexed(k, "lock_", kButtons); b >= 0)
        {
            if (v != 0.0)
                grabStep(b);
            return;
        }
        if (const int i = numbered(k, "knob", VOLT_KNOBS); i >= 0)
        {
            turnKnob(i, v);
            return;
        }
        if (k == "enc")
        {
            encoderRest += turnEncoder(v) * kEncoderDetentsPerUnit;
            // whole detents, allowing for the host's rounding (0.507812 for 0.5 + 1/128)
            const int detents = static_cast<int>(encoderRest + (encoderRest > 0 ? 0.01 : -0.01));
            encoderRest -= detents;
            if (detents != 0 && machine)
                machine->encoder(detents);
            return;
        }
        if (k == "enc_push")
        {
            if (v != 0.0 && machine && !bouncing(k))
                machine->encoder_push();
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
        else if (k == "sync")
            sync = v != 0.0;
        else if (k == "pad_mode")
            setPadMode(v >= 0.5 ? 1 : 0);
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
        if (const int b = indexed(k, "btn_", kButtons); b >= 0)
            return num(b == grab.button ? 1.0 : 0.0);
        if (indexed(k, "lock_", kButtons) >= 0)
            return num(0.0);
        if (const int l = indexed(k, "led_", kButtons); l >= 0)
            return num(ledOn(l) ? 1.0 : 0.0);
        if (const int c = indexed(k, "lcd_", kLcdCells); c >= 0)
            return num(cellCode(c) + 0.3);   // frame n of the skin's character strip shows character n
        if (k == "shift")
            return num(down[kShiftButton] ? 1.0 : 0.0);
        if (const int i = numbered(k, "knob", VOLT_KNOBS); i >= 0)
            return num(knobShown(i));
        if (k.size() == 13 && k.compare(0, 4, "knob") == 0 && k.compare(5, 8, "_display") == 0)
        {
            const int i = numbered(k.substr(0, 5), "knob", VOLT_KNOBS);
            return i >= 0 && knobs[static_cast<std::size_t>(i)].param != -1000
                       ? writeText(buf, len, std::to_string(knobValueShown(i)))
                       : writeText(buf, len, "-");
        }
        if (k == "enc")
            return num(encoder.last);
        if (k == "enc_push" || k == "pads_clear")
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
        if (k == "sync")
            return num(sync ? 1.0 : 0.0);
        if (k == "pad_mode")
            return num(padMode);
        if (k == "learn")
            return num(learning ? 1.0 : 0.0);
        if (k == "pads_info")
            return writeText(buf, len, padsText());
        if (k == "status")
            return writeText(buf, len, status);
        return 0;
    }

    // Called before each render: the MIDI clocks (24 per beat), start and stop
    // for the coming block, each at its frame. The LXR sounds a step
    // clockLead frames after the clock that starts it (its own 64-frame audio
    // blocks, and the DACs' buffer), so each clock goes that much early and
    // the step sounds on the MPC's beat.
    void songPos(const double ppq, const double bpm, const int playing, const int frames)
    {
        if (!machine || !sync)
        {
            wasPlaying = false;
            return;
        }
        const bool isPlaying = playing != 0 && ppq >= 0.0 && bpm > 0.0;
        const bool started = isPlaying && (!wasPlaying || ppq + 1e-6 < lastPpq);   // or the MPC jumped back (a loop)
        if (started)
            sendAt(0xFA, 0);
        if (!isPlaying && wasPlaying)
            sendAt(0xFC, 0);
        if (isPlaying)
        {
            const double framesPerBeat = 60.0 / bpm * VOLT_SAMPLE_RATE;
            const double lead = clockLead / framesPerBeat;   // in beats
            // the ticks that sound in [ppq + lead, ppq + lead + frames); on a start, also
            // those before, which can only come now
            const double from = started ? ppq : ppq + lead, to = ppq + lead + frames / framesPerBeat;
            for (double tick = std::ceil(from * 24.0 - 1e-9); tick / 24.0 < to; tick += 1.0)
                sendAt(0xF8, static_cast<int>(std::lround((tick / 24.0 - ppq - lead) * framesPerBeat)));
            lastPpq = ppq;
        }
        wasPlaying = isPlaying;
    }

    void render(std::int16_t* out, const int frames)
    {
        if (out == nullptr || frames <= 0)
            return;
        const float gain = static_cast<float>(std::pow(10.0, masterDb / 20.0));
        framesRendered += frames;
        if (grab.button >= 0 && framesRendered >= grab.armedAt)
            for (int i = 0; i < VOLT_KNOBS; ++i)
                if (grab.waiting[static_cast<std::size_t>(i)] >= 0 && machine)
                {
                    // where it was turned, on the parameter shown now
                    VoltKnob info;
                    if (machine->knob(i, &info))
                        machine->knob_to(i, static_cast<int>(std::lround(
                                                info.min + grab.waiting[static_cast<std::size_t>(i)] * (info.max - info.min))));
                    grab.waiting[static_cast<std::size_t>(i)] = -1;
                    knobs[static_cast<std::size_t>(i)].sentAt = Clock::now();
                }
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
                for (int l = 0; l < kButtons; ++l)
                    if (ledOn(l) != ledShown[static_cast<std::size_t>(l)])
                        changed.insert(indexedKey("led_", l));
            }
            for (int i = 0; i < VOLT_KNOBS; ++i)
                pollKnob(i);
        }
        if (!encoder.resting && Clock::now() - encoder.lastSet >= std::chrono::milliseconds(recentreMs))
        {
            encoder.resting = true;
            encoder.last = 0.5;
            changed.insert("enc");
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
    void sendAt(const std::uint8_t byte, const int frame)
    {
        machine->midi_at(&byte, 1, std::max(frame, 0));
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

    struct Grab
    {
        int button = -1;            // the step key held for a lock, or -1
        long long guardUntil = 0;   // frames: the double tap's own late taps and echoes
        long long armedAt = 0;      // frames: when the firmware will have armed the lock
        std::array<double, VOLT_KNOBS> waiting { -1, -1, -1, -1 };   // knob positions held back until then
    };

    struct Dial
    {
        double last = 0.5;   // the value the host last set (and reads back)
        Clock::time_point lastSet {};
        bool resting = true;
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
        down.fill(false);
        grab = Grab {};
        wasPlaying = false;
        knobs = {};
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
        for (const char* k : { "shift", "sync", "master", "pad_mode", "learn", "pads_info" })
            changed.insert(k);
        for (int i = 1; i <= VOLT_FADERS; ++i)
            changed.insert("fader" + std::to_string(i));
    }

    // ---- keys

    // A tap on a panel key: down for one scan, up on the next; on a step key
    // held for a lock, the end of the lock.
    void tapKey(const int b)
    {
        if (bouncing(indexedKey("btn_", b)) || !machine)
            return;
        const auto i = static_cast<std::size_t>(b);
        if (b == grab.button)
        {
            if (framesRendered < grab.guardUntil)
                return;   // the double tap's own second tap, arriving late
            letGo();
            return;
        }
        if (grab.button >= 0)
        {
            // the step lifted, the key tapped, the step held again (its lock re-arms)
            machine->key(grab.button, 0);
            machine->tap(b);
            machine->key(grab.button, 1);
            grab.armedAt = framesRendered + kArmFrames;
            return;
        }
        machine->tap(b);
        taps[i].push_back(framesRendered);
    }

    // A double tap on a step key: its two taps toggled the step twice, unless
    // one came too soon after the other to count; then hold it down.
    void grabStep(const int b)
    {
        if (!machine || b == grab.button)
            return;
        if (grab.button >= 0)
            letGo();
        const auto i = static_cast<std::size_t>(b);
        auto& t = taps[i];
        t.erase(std::remove_if(t.begin(), t.end(), [&](const long long at) { return framesRendered - at > kDoubleTap; }),
                t.end());
        if (t.size() % 2 == 1)
            machine->tap(b);   // back as it was before the double tap
        t.clear();
        setHeld(b, true);
        grab = Grab {};
        grab.button = b;
        grab.guardUntil = framesRendered + kGrabTapGuard;
        grab.armedAt = framesRendered + kArmFrames;
        changed.insert(indexedKey("btn_", b));   // lit
    }

    void letGo()
    {
        if (grab.button < 0)
            return;
        const int b = grab.button;
        const bool armed = framesRendered >= grab.armedAt;
        grab.button = -1;
        setHeld(b, false);
        if (!armed && machine)
            machine->tap(b);   // let go before the lock armed: that release toggled the step
        changed.insert(indexedKey("btn_", b));
    }

    void setHeld(const int b, const bool isDown)
    {
        const auto i = static_cast<std::size_t>(b);
        if (down[i] == isDown)
            return;
        down[i] = isDown;
        if (machine)
            machine->key(b, isDown ? 1 : 0);
        if (b == kShiftButton)
            changed.insert("shift");
    }

    bool bouncing(const std::string& key)
    {
        const auto now = Clock::now();
        const auto it = lastPress.find(key);
        if (it != lastPress.end() && now - it->second < std::chrono::milliseconds(debounceMs))
            return true;
        lastPress[key] = now;
        return false;
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
        if (grab.button >= 0 && framesRendered < grab.armedAt)
            grab.waiting[static_cast<std::size_t>(i)] = std::clamp(v, 0.0, 1.0);   // locks once the step is armed
        else
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
            info = VoltKnob { -1000, 0, 0, 127 };
        bool report = false;
        const bool waiting = grab.button >= 0 && grab.waiting[static_cast<std::size_t>(i)] >= 0;
        if (kn.turning && !waiting
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

    // The encoder: how far it moved since the value last set.
    double turnEncoder(const double v)
    {
        const double moved = v - encoder.last;
        encoder.last = v;
        encoder.lastSet = Clock::now();
        encoder.resting = false;
        return moved;
    }

    // ---- the display and LEDs

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

    int stepFor(const int note) const
    {
        if (!stepNotes.empty())
        {
            const auto it = std::find(stepNotes.begin(), stepNotes.end(), note);
            return it == stepNotes.end() ? -1 : static_cast<int>(it - stepNotes.begin());
        }
        const int s = note - kDefaultStepNote;
        return s >= 0 && s < kSteps ? s : -1;
    }

    std::vector<int>& learnTarget() { return padMode == 0 ? voiceNotes : stepNotes; }
    std::size_t learnCount() const { return padMode == 0 ? kVoices : kSteps; }

    // PAD LEARN: each new note becomes the next pad (D1 to OH, or step 1 to 16).
    void learnNote(const int note)
    {
        std::vector<int>& notes = learnTarget();
        if (std::find(notes.begin(), notes.end(), note) != notes.end())
            return;
        notes.push_back(note);
        changed.insert("pads_info");
        if (notes.size() == learnCount())
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
            learnTarget().clear();   // start over: the next pad is the first
        else
            queuePadMapSave();
    }

    void clearPads()
    {
        learning = false;
        learnTarget().clear();
        changed.insert("learn");
        changed.insert("pads_info");
        queuePadMapSave();
    }

    void setPadMode(const int mode)
    {
        if (mode == padMode)
            return;
        if (padMode == 1 && machine)   // steps held by pads let go
            for (const int b : kStepButtons)
                machine->key(b, 0);
        padMode = mode;
        learning = false;
        changed.insert("pad_mode");
        changed.insert("learn");
        changed.insert("pads_info");
    }

    std::string padsText() const
    {
        if (!status.empty())
            return status;
        const std::vector<int>& notes = padMode == 0 ? voiceNotes : stepNotes;
        if (learning)
        {
            const std::size_t n = notes.size();
            if (padMode == 0)
                return "LEARN: HIT THE PAD FOR " + std::string(kVoiceName[std::min<std::size_t>(n, kVoices - 1)]) + "  ("
                       + std::to_string(n + 1) + " OF 7)";
            return "LEARN: HIT THE PAD FOR STEP " + std::to_string(n + 1) + "  (" + std::to_string(n + 1) + " OF 16)";
        }
        if (padMode == 1)
            return notes.empty() ? "PADS ARE THE STEP KEYS: " + noteName(kDefaultStepNote) + " UP IS STEP 1 TO 16. HOLD ONE "
                                       "AND TURN A KNOB TO LOCK"
                                 : "PADS ARE THE STEP KEYS (LEARNED). HOLD ONE AND TURN A KNOB TO LOCK";
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
            else if (line.rfind("steps ", 0) == 0)
                stepNotes = parseNotes(line.substr(6), kSteps);
        }
    }

    void queuePadMapSave()
    {
        std::scoped_lock lock(saverMutex);
        toSave = "voices " + joinNotes(voiceNotes) + "\nsteps " + joinNotes(stepNotes) + "\n";
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
        std::string s = "v=3;sync=" + std::to_string(sync ? 1 : 0) + ";master=" + formatNum(masterDb, 2) + ";faders=";
        for (std::size_t i = 0; i < faders.size(); ++i)
            s += (i ? "," : "") + formatNum(faders[i], 4);
        s += ";pads=" + std::to_string(padMode) + ";voices=" + joinNotes(voiceNotes) + ";steps=" + joinNotes(stepNotes);
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
            else if (k == "pads")
                padMode = parseDouble(v.c_str(), 0.0) >= 0.5 ? 1 : 0;
            else if (k == "voices")
                voiceNotes = parseNotes(v, kVoices);
            else if (k == "steps")
                stepNotes = parseNotes(v, kSteps);
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

    std::array<bool, kButtons> down {};
    Grab grab;
    std::array<std::vector<long long>, kButtons> taps;   // recent taps by key, as frames rendered (double taps)
    long long framesRendered = 0;
    std::array<Knob, VOLT_KNOBS> knobs {};
    Dial encoder;
    double encoderRest = 0.0;
    std::array<double, VOLT_FADERS> faders { 1.0, 1.0, 1.0, 1.0, 1.0, 1.0 };
    double masterDb = 0.0;

    bool sync = true;
    bool wasPlaying = false;
    double lastPpq = 0.0;
    // VOLTKIT_CLOCK_LEAD (frames): another lead, for measuring it.
    const int clockLead = std::getenv("VOLTKIT_CLOCK_LEAD") ? std::atoi(std::getenv("VOLTKIT_CLOCK_LEAD")) : kClockLead;

    int padMode = 0;                 // 0: VOICES, 1: STEPS
    bool learning = false;
    std::vector<int> voiceNotes;     // learned: voiceNotes[v] plays voice v
    std::vector<int> stepNotes;      // learned: stepNotes[s] is step s + 1
    std::string pluginData;

    std::uint32_t lcdSeen = ~0u, ledSeen = ~0u;
    std::array<int, kLcdCells> lcdShown {};
    std::array<bool, kButtons> ledShown {};
    std::set<std::string> changed;   // keys to report to MPC (mpc_engine_changed)

    std::optional<Clock::time_point> lastSuspend;
    std::map<std::string, Clock::time_point> lastPress;
    // VOLTKIT_DEBOUNCE_MS, VOLTKIT_RECENTRE_MS: the offline test turns them off.
    const int debounceMs = std::getenv("VOLTKIT_DEBOUNCE_MS") ? std::atoi(std::getenv("VOLTKIT_DEBOUNCE_MS")) : kDebounceMs;
    const int recentreMs = std::getenv("VOLTKIT_RECENTRE_MS") ? std::atoi(std::getenv("VOLTKIT_RECENTRE_MS")) : kRecentreMs;

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
