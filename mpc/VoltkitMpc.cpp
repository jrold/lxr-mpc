// MPC OS (Open MPC VST2 wrapper) engine for VOLTKIT.
//
// Each instance loads its own copy of the LXR firmware engine
// (EngineLoader.*) and maps the plug-in's parameters (Controls.inc, from
// gen_controls.py) onto the firmware's. The wrapper calls this one call at a
// time per instance, under its own lock: set_param and get_param from MPC's
// screen thread, midi, song_pos and render from the audio thread.
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

extern "C" {
#include "engine.h"
}

#include "EngineLoader.h"
#include "FirmwareParams.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

constexpr int kMaxFrames = 128;                 // wrapper/engine.h: render() gets 1..128 frames
constexpr const char* kPadMapFile = "padmap.txt";
constexpr auto kDoubleStopWindow = std::chrono::milliseconds(1200);
constexpr int kDebounceMs = 350;
constexpr int kPads = 7;                        // D1, D2, D3, SN, CY, CH, OH
constexpr int kFxTypes = 5;                     // off, drive, ring, comp, delay

// The firmware's parameters VOLTKIT touches outside Controls.inc (P1..P4,
// and LFO 1..6, follow one another).
constexpr int kParFxP1 = kParFxP1_;
constexpr int kParFxType = kParFxType_;
constexpr int kParFxDryWet = kParFxDryWet_;
constexpr int kParVoiceLfo1 = kParVoiceLfo1_;
constexpr int kParTargetLfo1 = kParTargetLfo1_;
constexpr int kAudioOutFx = 6;                  // outputNames: "FX"

// Pad slots, in PAD LEARN order, and the engine voice each one plays.
constexpr std::array<const char*, kPads> kPadName { "DRUM 1", "DRUM 2", "DRUM 3", "SNARE", "CYMBAL", "CLOSED HAT", "OPEN HAT" };
constexpr std::array<const char*, kPads> kPadShort { "D1", "D2", "D3", "SN", "CY", "CH", "OH" };

// Pads play these notes unless PAD LEARN has mapped others: General MIDI
// drum notes, a few per voice, so most pad layouts reach every voice.
const std::map<int, int> kDefaultNotes {
    { 35, 0 }, { 36, 0 },
    { 41, 1 }, { 43, 1 }, { 45, 1 },
    { 47, 2 }, { 48, 2 }, { 50, 2 },
    { 37, 3 }, { 38, 3 }, { 39, 3 }, { 40, 3 },
    { 49, 4 }, { 51, 4 }, { 52, 4 }, { 55, 4 }, { 57, 4 },
    { 42, 5 }, { 44, 5 },
    { 46, 6 },
};

constexpr int kDefaultNote = 60;   // the firmware's SEQ_DEFAULT_NOTE: the kit's own pitch

enum class Kind
{
    page, knob, pm60, pm63, menu, out, veldest, lfovoice, lfodest, fxp, master, learn, padsClear, initKit, padsInfo
};

struct Control
{
    const char* key;
    int par;      // firmware parameter, -1 for none
    Kind kind;
    int voice;    // 1..6; for fxp the effect (1..4)
};

const Control kControls[] = {
#include "Controls.inc"
};

struct Targets
{
    int lo, hi;   // this voice's range in the firmware's modTargets
    std::vector<const char*> names;
};

const Targets kTargets[6] = {
#include "Targets.inc"
};

int parseInt(const char* text, const int fallback)
{
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    return end == text || !std::isfinite(v) ? fallback : static_cast<int>(std::lround(v));
}

float parseFloat(const char* text, const float fallback)
{
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    return end == text || !std::isfinite(v) ? fallback : static_cast<float>(v);
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

std::string signedText(const int v)
{
    return v > 0 ? "+" + std::to_string(v) : std::to_string(v);
}

std::string noteName(const int note)
{
    static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string(names[((note % 12) + 12) % 12]) + std::to_string(note / 12 - 2);   // MPC: 60 = C3
}

const std::unordered_map<std::string, const Control*>& controlsByKey()
{
    static const auto map = [] {
        std::unordered_map<std::string, const Control*> m;
        for (const auto& c : kControls)
            m.emplace(c.key, &c);
        return m;
    }();
    return map;
}

class Instance
{
public:
    explicit Instance(const char* dataDir)
    {
        learnedNotes.reserve(kPads);   // learned on the audio thread: no allocation there
        const char* override = std::getenv("VOLTKIT_ENGINE");
        if (engineCopy.load(override && *override ? override : voltkit::defaultEnginePath()))
        {
            engine = engineCopy.api();
            engine->init();
        }
        else
        {
            std::fprintf(stderr, "VOLTKIT: %s\n", engineCopy.error().c_str());
        }
        captureFxValues();
        if (dataDir != nullptr && *dataDir != '\0')
        {
            pluginData = dataDir;
            std::ifstream in(std::filesystem::path(dataDir) / kPadMapFile);
            std::vector<int> notes;
            for (int n; in >> n && notes.size() < static_cast<std::size_t>(kPads);)
                notes.push_back(n);
            learnedNotes.assign(notes.begin(), notes.end());
        }
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
        if (msg == nullptr || len < 1)
            return;
        const auto status = msg[0] & 0xF0;
        const int d1 = len > 1 ? msg[1] & 0x7F : 0;
        const int d2 = len > 2 ? msg[2] & 0x7F : 0;
        if (status == 0x90 && d2 > 0)
        {
            // PAD LEARN: each new note becomes the next pad slot, D1 to OH.
            if (learning && std::find(learnedNotes.begin(), learnedNotes.end(), d1) == learnedNotes.end())
            {
                learnedNotes.push_back(d1);
                changed.push_back("learn");
                if (static_cast<int>(learnedNotes.size()) == kPads)
                {
                    learning = false;
                    for (int i = 0; i < kPads; ++i)
                        toSave[static_cast<std::size_t>(i)] = learnedNotes[static_cast<std::size_t>(i)];
                    savePending.store(true, std::memory_order_release);
                }
            }
            const int slot = slotFor(d1);
            if (slot >= 0 && engine)
                engine->trigger(slot, kDefaultNote, d2);
        }
        else if (status == 0xB0 && (d1 == 120 || d1 == 123))
        {
            if (engine)
                engine->all_off();
        }
    }

    void songPos(const double bpm, const int playing)
    {
        if (engine)
            engine->set_tempo(bpm > 0 ? static_cast<float>(bpm) : 0.0f, playing);
    }

    void setParam(const char* key, const char* val)
    {
        if (key == nullptr)
            return;
        const std::string k(key);
        if (k == "state")
        {
            restoreState(val != nullptr ? val : "");
            return;
        }
        const int v = parseInt(val, 0);
        if (k == "mains")
        {
            // MPC suspends and resumes plug-ins on every STOP press (our patched
            // wrapper passes it on): two close together stop all sound.
            if (v == 0)
            {
                const auto now = std::chrono::steady_clock::now();
                if (lastSuspend && now - *lastSuspend < kDoubleStopWindow && engine)
                    engine->all_off();
                lastSuspend = now;
            }
            return;
        }
        const auto& map = controlsByKey();
        const auto it = map.find(k);
        if (it == map.end())
            return;
        apply(*it->second, v, val);
    }

    int getParam(const char* key, char* buf, const int len)
    {
        if (key == nullptr)
            return 0;
        std::string k(key);
        if (k == "state")
            return writeText(buf, len, saveState());
        const bool display = k.size() > 8 && k.compare(k.size() - 8, 8, "_display") == 0;
        if (display)
            k.resize(k.size() - 8);
        const auto& map = controlsByKey();
        const auto it = map.find(k);
        if (it == map.end())
            return 0;
        const Control& c = *it->second;
        if (c.kind == Kind::padsInfo)
            return writeText(buf, len, padsText());
        if (c.kind == Kind::master)
        {
            if (display)
            {
                char b[32];
                std::snprintf(b, sizeof b, "%+.1f dB", static_cast<double>(volumeDb));
                return writeText(buf, len, b);
            }
            char b[32];
            std::snprintf(b, sizeof b, "%.2f", static_cast<double>(volumeDb));
            return writeText(buf, len, b);
        }
        const int v = value(c);
        return writeText(buf, len, display ? displayText(c, v) : std::to_string(v));
    }

    void render(std::int16_t* out, const int frames)
    {
        if (out == nullptr || frames <= 0)
            return;
        const float gain = std::pow(10.0f, volumeDb / 20.0f);
        int done = 0;
        while (done < frames)
        {
            const int n = std::min(kMaxFrames, frames - done);
            if (engine)
                engine->render(left.data(), right.data(), n);
            else
                std::fill(left.begin(), left.end(), 0.0f), std::fill(right.begin(), right.end(), 0.0f);
            for (int i = 0; i < n; ++i)
            {
                out[(done + i) * 2] = toPcm(left[static_cast<std::size_t>(i)] * gain);
                out[(done + i) * 2 + 1] = toPcm(right[static_cast<std::size_t>(i)] * gain);
            }
            done += n;
        }
    }

    bool takeChangedKey(char* key, const int len)
    {
        if (changed.empty())
            return false;
        writeText(key, len, changed.front());
        changed.pop_front();
        return true;
    }

    // For the tests.
    const VoltEngineApi* engineApi() const { return engine; }
    std::size_t pendingChanges() const { return changed.size(); }

private:
    static std::int16_t toPcm(const float v)
    {
        return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }

    int slotFor(const int note) const
    {
        if (!learnedNotes.empty())
        {
            const auto it = std::find(learnedNotes.begin(), learnedNotes.end(), note);
            return it == learnedNotes.end() ? -1 : static_cast<int>(it - learnedNotes.begin());
        }
        const auto it = kDefaultNotes.find(note);
        return it == kDefaultNotes.end() ? -1 : it->second;
    }

    bool bouncing(const std::string& key)
    {
        const auto now = std::chrono::steady_clock::now();
        auto& last = lastPress[key];
        if (last.time_since_epoch().count() != 0 && now - last < std::chrono::milliseconds(debounceMs))
            return true;
        last = now;
        return false;
    }

    int raw(const int par) const { return engine ? engine->get_param(par) : 0; }
    void send(const int par, const int v) { if (engine) engine->set_param(par, v); }

    static const Targets& targetsOf(const int voice) { return kTargets[std::clamp(voice, 1, 6) - 1]; }

    int lfoVoice(const int lfo) const { return std::clamp(raw(kParVoiceLfo1 + lfo - 1), 1, 6); }

    // ---- parameters ----

    void apply(const Control& c, const int v, const char* text)
    {
        switch (c.kind)
        {
        case Kind::page:
            pages[c.key] = v;
            return;
        case Kind::knob:
        case Kind::pm60:
        case Kind::pm63:
        case Kind::menu:
            send(c.par, v);
            if (c.par == kParFxType)
                sendFxValues();
            return;
        case Kind::out:
            send(c.par, v != 0 ? kAudioOutFx : 0);
            return;
        case Kind::veldest:
        {
            const auto& t = targetsOf(c.voice);
            send(c.par, v <= 0 ? 0 : std::min(t.lo + v - 1, t.hi));
            return;
        }
        case Kind::lfovoice:
        {
            // A new target voice keeps the LFO on the same kind of target,
            // the way the LXR's menu does.
            const int lfo = c.voice;
            const int voice = std::clamp(v + 1, 1, 6);
            const int target = raw(kParTargetLfo1 + lfo - 1);
            send(c.par, voice);
            if (engine)
                send(kParTargetLfo1 + lfo - 1, engine->retarget(target, voice));
            changed.push_back(std::string(c.key).substr(0, 3) + "ldest");
            return;
        }
        case Kind::lfodest:
            sendLfoTarget(c.voice, v);
            return;
        case Kind::fxp:
        {
            const int slot = c.par - kParFxP1;
            fxValues[static_cast<std::size_t>(c.voice)][static_cast<std::size_t>(slot)] = std::clamp(v, 0, 127);
            if (raw(kParFxType) == c.voice)
                send(c.par, v);
            return;
        }
        case Kind::master:
            volumeDb = std::clamp(parseFloat(text, 0.0f), -24.0f, 6.0f);
            return;
        case Kind::learn:
            setLearning(v != 0);
            return;
        case Kind::padsClear:
            if (v != 0 && !bouncing(c.key))
            {
                learning = false;
                learnedNotes.clear();
                std::error_code ec;
                if (!pluginData.empty())
                    std::filesystem::remove(std::filesystem::path(pluginData) / kPadMapFile, ec);
                changed.push_back("learn");
            }
            return;
        case Kind::initKit:
            if (v != 0 && !bouncing(c.key) && engine)
            {
                engine->init();
                captureFxValues();
                markAllChanged();
            }
            return;
        case Kind::padsInfo:
            return;
        }
    }

    int value(const Control& c) const
    {
        switch (c.kind)
        {
        case Kind::page:
        {
            const auto it = pages.find(c.key);
            return it == pages.end() ? 0 : it->second;
        }
        case Kind::knob:
        case Kind::pm60:
        case Kind::pm63:
        case Kind::menu:
            return raw(c.par);
        case Kind::out:
            return raw(c.par) == kAudioOutFx ? 1 : 0;
        case Kind::veldest:
        {
            const auto& t = targetsOf(c.voice);
            const int abs = raw(c.par);
            return abs >= t.lo && abs <= t.hi ? abs - t.lo + 1 : 0;
        }
        case Kind::lfovoice:
            return lfoVoice(c.voice) - 1;
        case Kind::lfodest:
            return lfoRelative(c.voice);
        case Kind::fxp:
            return fxValues[static_cast<std::size_t>(c.voice)][static_cast<std::size_t>(c.par - kParFxP1)];
        case Kind::learn:
            return learning ? 1 : 0;
        default:
            return 0;
        }
    }

    std::string displayText(const Control& c, const int v) const
    {
        switch (c.kind)
        {
        case Kind::pm60: return signedText(v - 60);
        case Kind::pm63: return signedText(v - 63);
        case Kind::lfodest:
        {
            if (v <= 0)
                return "Off";
            const auto& t = targetsOf(lfoVoice(c.voice));
            const auto i = static_cast<std::size_t>(v - 1);
            static const char* const voiceName[6] = { "D1", "D2", "D3", "SN", "CY", "HH" };
            return i < t.names.size() ? std::string(voiceName[lfoVoice(c.voice) - 1]) + " " + t.names[i] : "Off";
        }
        default:
            if (c.par == kParFxDryWet)   // the firmware's 0..100
                return std::to_string(v) + "%";
            return std::to_string(v);
        }
    }

    // The LFO's target as a position in its target voice's list (0 = off).
    int lfoRelative(const int lfo) const
    {
        const int abs = raw(kParTargetLfo1 + lfo - 1);
        const auto& t = targetsOf(lfoVoice(lfo));
        return abs >= t.lo && abs <= t.hi ? abs - t.lo + 1 : 0;
    }

    void sendLfoTarget(const int lfo, const int rel)
    {
        const auto& t = targetsOf(lfoVoice(lfo));
        const int count = t.hi - t.lo + 1;
        send(kParTargetLfo1 + lfo - 1, rel <= 0 ? 0 : t.lo + std::min(rel, count) - 1);
    }

    // Each effect remembers its own P1..P4; the firmware holds the active one's.
    void captureFxValues()
    {
        for (auto& set : fxValues)
            for (int j = 0; j < 4; ++j)
                set[static_cast<std::size_t>(j)] = raw(kParFxP1 + j);
    }

    void sendFxValues()
    {
        const int type = std::clamp(raw(kParFxType), 0, kFxTypes - 1);
        for (int j = 0; j < 4; ++j)
            send(kParFxP1 + j, fxValues[static_cast<std::size_t>(type)][static_cast<std::size_t>(j)]);
    }

    void markAllChanged()
    {
        for (const auto& c : kControls)
            if (c.par >= 0 || c.kind == Kind::lfodest)
                changed.push_back(c.key);
    }

    // ---- PAD LEARN ----

    void setLearning(const bool on)
    {
        if (on == learning)
            return;
        learning = on;
        changed.push_back("learn");
        if (on)
        {
            learnedNotes.clear();   // start over: the next pad is DRUM 1
            return;
        }
        writePadMap(learnedNotes);
    }

    void writePadMap(const std::vector<int>& notes) const
    {
        if (notes.empty() || pluginData.empty())
            return;
        std::ofstream out(std::filesystem::path(pluginData) / kPadMapFile, std::ios::trunc);
        for (const int n : notes)
            out << n << "\n";
    }

    void saveLoop()
    {
        std::unique_lock lock(saverMutex);
        while (!quit)
        {
            saverCv.wait_for(lock, std::chrono::milliseconds(250));
            if (savePending.exchange(false, std::memory_order_acq_rel))
                writePadMap(std::vector<int>(toSave.begin(), toSave.end()));
        }
    }

    std::string padsText() const
    {
        if (!engine)
            return "ENGINE NOT LOADED: " + engineCopy.error();
        if (learning)
        {
            const auto n = learnedNotes.size();
            return n < static_cast<std::size_t>(kPads)
                ? "LEARN: HIT THE PAD FOR " + std::string(kPadName[n]) + "  (" + std::to_string(n + 1) + " OF 7)"
                : "LEARNED ALL SEVEN";
        }
        if (!learnedNotes.empty())
        {
            std::string s = "PADS: LEARNED ";
            for (std::size_t i = 0; i < learnedNotes.size(); ++i)
                s += std::string("  ") + kPadShort[i] + " " + noteName(learnedNotes[i]);
            return s;
        }
        return "PADS: GM DRUM NOTES (D1 36, D2 45, D3 48, SN 38, CY 49, CH 42, OH 46)";
    }

    // ---- state ----

    // v=1;kit=<hex of the firmware's sound parameters>;fx=<hex>;pages=..;padmap=..;volume=..
    std::string saveState() const
    {
        static const char* const hex = "0123456789abcdef";
        std::string kit;
        for (int p = 1; p < VOLT_PARAMS; ++p)
        {
            const int v = raw(p) & 0xff;
            kit += hex[v >> 4];
            kit += hex[v & 15];
        }
        std::string fx;
        for (const auto& set : fxValues)
            for (const int v : set)
            {
                fx += hex[(v & 0xff) >> 4];
                fx += hex[v & 15];
            }
        std::string s = "v=1;kit=" + kit + ";fx=" + fx;
        std::string pg;
        for (const auto& [k, v] : pages)
            pg += (pg.empty() ? "" : ".") + k + ":" + std::to_string(v);
        s += ";pages=" + pg;
        std::string map;
        for (const int n : learnedNotes)
            map += (map.empty() ? "" : ".") + std::to_string(n);
        s += ";padmap=" + map;
        char b[32];
        std::snprintf(b, sizeof b, ";volume=%.2f", static_cast<double>(volumeDb));
        return s + b;
    }

    static std::vector<int> fromHex(const std::string& h)
    {
        std::vector<int> out;
        for (std::size_t i = 0; i + 1 < h.size(); i += 2)
            out.push_back(static_cast<int>(std::strtol(h.substr(i, 2).c_str(), nullptr, 16)));
        return out;
    }

    void restoreState(const std::string& state)
    {
        std::size_t pos = 0;
        while (pos <= state.size())
        {
            const auto end = std::min(state.find(';', pos), state.size());
            const auto item = state.substr(pos, end - pos);
            pos = end + 1;
            const auto eq = item.find('=');
            if (eq == std::string::npos)
                continue;
            const auto k = item.substr(0, eq);
            const auto v = item.substr(eq + 1);
            if (k == "kit" && engine)
            {
                // As a kit file: 8 name bytes, then parameters from 0.
                std::vector<std::uint8_t> bytes(8 + VOLT_PARAMS, 0);
                const auto vals = fromHex(v);
                for (std::size_t i = 0; i < vals.size() && i + 1 < static_cast<std::size_t>(VOLT_PARAMS); ++i)
                    bytes[8 + 1 + i] = static_cast<std::uint8_t>(vals[i]);
                engine->load_kit(bytes.data(), static_cast<int>(bytes.size()));
            }
            else if (k == "fx")
            {
                const auto vals = fromHex(v);
                for (std::size_t i = 0; i < vals.size() && i < kFxTypes * 4; ++i)
                    fxValues[i / 4][i % 4] = vals[i];
                sendFxValues();
            }
            else if (k == "pages")
            {
                std::size_t at = 0;
                while (at < v.size())
                {
                    const auto dot = std::min(v.find('.', at), v.size());
                    const auto entry = v.substr(at, dot - at);
                    const auto colon = entry.find(':');
                    if (colon != std::string::npos)
                        pages[entry.substr(0, colon)] = parseInt(entry.substr(colon + 1).c_str(), 0);
                    at = dot + 1;
                }
            }
            else if (k == "padmap")
            {
                std::vector<int> notes;
                for (std::size_t at = 0; at < v.size() && notes.size() < static_cast<std::size_t>(kPads);)
                {
                    const auto dot = std::min(v.find('.', at), v.size());
                    notes.push_back(parseInt(v.substr(at, dot - at).c_str(), -1));
                    at = dot + 1;
                }
                learning = false;
                learnedNotes.assign(notes.begin(), notes.end());
            }
            else if (k == "volume")
            {
                volumeDb = std::clamp(parseFloat(v.c_str(), 0.0f), -24.0f, 6.0f);
            }
        }
        markAllChanged();
    }

    voltkit::EngineCopy engineCopy;
    const VoltEngineApi* engine = nullptr;
    std::array<float, kMaxFrames> left {};
    std::array<float, kMaxFrames> right {};
    std::array<std::array<int, 4>, kFxTypes> fxValues {};
    std::map<std::string, int> pages;
    float volumeDb = 0.0f;
    std::deque<std::string> changed;   // keys to report to MPC (mpc_engine_changed)

    std::string pluginData;            // the plug-in's data/ folder (pad map)
    bool learning = false;
    std::vector<int> learnedNotes;     // learnedNotes[slot] plays that pad slot
    std::array<int, kPads> toSave {};
    std::atomic<bool> savePending { false };
    std::mutex saverMutex;
    std::condition_variable saverCv;
    bool quit = false;
    std::optional<std::chrono::steady_clock::time_point> lastSuspend;
    std::map<std::string, std::chrono::steady_clock::time_point> lastPress;
    // VOLTKIT_DEBOUNCE_MS: the offline test turns it off to press keys quickly.
    const int debounceMs = std::getenv("VOLTKIT_DEBOUNCE_MS") ? std::atoi(std::getenv("VOLTKIT_DEBOUNCE_MS")) : kDebounceMs;
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
void engineSongPos(void* inst, double, const double bpm, const int playing, int)
{
    static_cast<Instance*>(inst)->songPos(bpm, playing);
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
