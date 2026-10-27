#include "EngineLoader.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace voltkit
{
namespace
{

bool readFile(const std::string& path, std::vector<char>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return !out.empty();
}

bool writeAll(const int fd, const std::vector<char>& data)
{
    size_t done = 0;
    while (done < data.size())
    {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n <= 0)
            return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

}

EngineCopy::~EngineCopy()
{
    engine = nullptr;
    if (handle)
        dlclose(handle);
    if (memfd >= 0)
        ::close(memfd);
}

bool EngineCopy::load(const std::string& libraryPath)
{
    std::vector<char> bytes;
    if (!readFile(libraryPath, bytes))
    {
        message = "can't read " + libraryPath;
        return false;
    }

#if defined(__linux__)
    memfd = memfd_create("voltkit-engine", MFD_CLOEXEC);
    if (memfd >= 0 && writeAll(memfd, bytes))
    {
        const std::string path = "/proc/self/fd/" + std::to_string(memfd);
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
#endif
    if (!handle)
    {
        // A temporary copy, removed once loaded (the mapping stays).
        const char* tmp = std::getenv("TMPDIR");
        std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/voltkit-engine-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end());
        name.push_back('\0');
        const int fd = mkstemp(name.data());
        if (fd < 0)
        {
            message = "can't make a temporary engine copy";
            return false;
        }
        const bool written = writeAll(fd, bytes);
        ::close(fd);
        if (written)
            handle = dlopen(name.data(), RTLD_NOW | RTLD_LOCAL);
        ::unlink(name.data());
    }
    if (!handle)
    {
        const char* e = dlerror();
        message = e ? e : "can't load the engine";
        return false;
    }

    using Entry = const VoltMachineApi* (*)();
    const auto entry = reinterpret_cast<Entry>(dlsym(handle, "volt_machine_api"));
    engine = entry ? entry() : nullptr;
    if (!engine || engine->version != VOLT_MACHINE_API_VERSION)
    {
        message = "engine library doesn't match this plug-in";
        engine = nullptr;
        return false;
    }
    return true;
}

std::string defaultEnginePath()
{
    Dl_info info {};
    if (!dladdr(reinterpret_cast<const void*>(&defaultEnginePath), &info) || !info.dli_fname)
        return "voltkit-engine.so";
    std::string self = info.dli_fname;
    const auto slash = self.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : self.substr(0, slash);
#if defined(__APPLE__)
    return dir + "/libvoltkit-engine.dylib";
#else
    return dir + "/engine/voltkit-engine.so";   // the package's engine/ folder, next to the plug-in
#endif
}

}
