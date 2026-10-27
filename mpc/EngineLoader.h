#pragma once

// Loads a private copy of the VOLTKIT engine library for one plug-in
// instance. The LXR firmware keeps its state in globals, and dlopen of the
// same file twice returns the same copy, so each instance loads the library
// from its own file: an in-memory file on Linux (memfd), a temporary copy
// elsewhere. Every copy has its own globals, so instances never share sound.
//
// Copyright (C) 2026 Jason Rold. GPL-3.0-only.

#include <string>

#include "../engine/volt_engine.h"

namespace voltkit
{

class EngineCopy
{
public:
    EngineCopy() = default;
    ~EngineCopy();
    EngineCopy(const EngineCopy&) = delete;
    EngineCopy& operator=(const EngineCopy&) = delete;

    // Loads `libraryPath` as a new copy. False with error() set on failure.
    bool load(const std::string& libraryPath);

    const VoltEngineApi* api() const { return engine; }
    const std::string& error() const { return message; }

private:
    void* handle = nullptr;
    int memfd = -1;
    const VoltEngineApi* engine = nullptr;
    std::string message;
};

// The engine library: engine/voltkit-engine.so next to the plug-in.
std::string defaultEnginePath();

}
