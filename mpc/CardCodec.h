#pragma once

// The LXR's SD card packed for VOLTKIT: the default card shipped with the
// plug-in (data/card.vkc) and each track's card saved in the MPC project.
//
//   "VKC1", then per sector holding data: sector (u32 LE), packed length
//   (u16 LE), packed bytes. Packing: a zero byte is followed by the length of
//   its run of zeros (1..255); any other byte stands for itself.
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

#include <cstdint>
#include <string>
#include <vector>

#include "../engine/volt_machine.h"

namespace voltkit::card
{

inline void putLe(std::string& out, const uint32_t v, const int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// One sector, packed.
inline std::string packSector(const uint8_t* d)
{
    std::string out;
    for (uint32_t i = 0; i < VOLT_SECTOR_BYTES;)
    {
        if (d[i] != 0)
        {
            out.push_back(static_cast<char>(d[i++]));
            continue;
        }
        uint32_t run = 0;
        while (i < VOLT_SECTOR_BYTES && d[i] == 0 && run < 255)
        {
            ++i;
            ++run;
        }
        out.push_back(0);
        out.push_back(static_cast<char>(run));
    }
    return out;
}

// The machine's whole card.
inline std::string pack(const VoltMachineApi& m)
{
    std::string out = "VKC1";
    uint8_t sector[VOLT_SECTOR_BYTES];
    for (uint32_t s = m.card_next(0); s < VOLT_CARD_SECTORS; s = m.card_next(s + 1))
    {
        m.card_read(s, sector);
        const std::string p = packSector(sector);
        putLe(out, s, 4);
        putLe(out, static_cast<uint32_t>(p.size()), 2);
        out += p;
    }
    return out;
}

// Into the machine (before boot). False on anything malformed, having
// written nothing past the fault.
inline bool unpack(const std::string& in, const VoltMachineApi& m)
{
    if (in.size() < 4 || in.compare(0, 4, "VKC1") != 0)
        return false;
    const auto byte = [&](size_t i) { return static_cast<uint8_t>(in[i]); };
    size_t at = 4;
    while (at < in.size())
    {
        if (at + 6 > in.size())
            return false;
        const uint32_t s = byte(at) | byte(at + 1) << 8 | byte(at + 2) << 16 | static_cast<uint32_t>(byte(at + 3)) << 24;
        const size_t n = byte(at + 4) | byte(at + 5) << 8;
        at += 6;
        if (s >= VOLT_CARD_SECTORS || at + n > in.size())
            return false;
        uint8_t sector[VOLT_SECTOR_BYTES] = {};
        uint32_t o = 0;
        for (size_t i = at; i < at + n; ++i)
        {
            if (byte(i) != 0)
            {
                if (o >= VOLT_SECTOR_BYTES)
                    return false;
                sector[o++] = byte(i);
                continue;
            }
            if (++i >= at + n)
                return false;
            o += byte(i);   // zeros: already there
            if (o > VOLT_SECTOR_BYTES)
                return false;
        }
        if (o != VOLT_SECTOR_BYTES)
            return false;
        m.card_write(s, sector);
        at += n;
    }
    return true;
}

// ---- base64, for the card inside the project's text state

inline std::string base64(const std::string& in)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3)
    {
        const uint32_t a = static_cast<uint8_t>(in[i]);
        const uint32_t b = i + 1 < in.size() ? static_cast<uint8_t>(in[i + 1]) : 0;
        const uint32_t c = i + 2 < in.size() ? static_cast<uint8_t>(in[i + 2]) : 0;
        const uint32_t v = a << 16 | b << 8 | c;
        out.push_back(t[(v >> 18) & 63]);
        out.push_back(t[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? t[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < in.size() ? t[v & 63] : '=');
    }
    return out;
}

inline bool unbase64(const std::string& in, std::string& out)
{
    const auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    if (in.size() % 4 != 0)
        return false;
    for (size_t i = 0; i < in.size(); i += 4)
    {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k)
        {
            if (in[i + k] == '=' && i + 4 == in.size() && k >= 2)
            {
                v[k] = 0;
                ++pad;
                continue;
            }
            if (pad || (v[k] = value(in[i + k])) < 0)
                return false;
        }
        const uint32_t w = static_cast<uint32_t>(v[0] << 18 | v[1] << 12 | v[2] << 6 | v[3]);
        out.push_back(static_cast<char>(w >> 16));
        if (pad < 2)
            out.push_back(static_cast<char>((w >> 8) & 0xFF));
        if (pad < 1)
            out.push_back(static_cast<char>(w & 0xFF));
    }
    return true;
}

}
