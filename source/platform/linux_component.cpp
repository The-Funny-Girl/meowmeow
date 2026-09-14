#include "component_api.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

std::atomic<uint64_t> g_heartbeat{0};

int ReadTracerPid()
{
    std::ifstream input("/proc/self/status");
    std::string key;
    while (input >> key) {
        if (key == "TracerPid:") {
            int tracer = 0;
            input >> tracer;
            return tracer;
        }
        std::string rest;
        std::getline(input, rest);
    }
    return -1;
}

uint32_t CountMappedFiles()
{
    std::ifstream input("/proc/self/maps");
    uint32_t count = 0;
    std::string line;
    while (std::getline(input, line)) {
        const auto path = line.find('/');
        if (path != std::string::npos && count != UINT32_MAX)
            ++count;
    }
    return count;
}

void FillStatus(KirkwareComponentStatus* status, const char* detail)
{
    std::memset(status, 0, sizeof(*status));
    status->abi_version = KIRKWARE_COMPONENT_ABI_VERSION;
    status->struct_size = static_cast<uint32_t>(sizeof(*status));
    status->tracer_pid = ReadTracerPid();
    status->mapped_file_count = CountMappedFiles();
    status->heartbeat = ++g_heartbeat;
    std::snprintf(status->detail, sizeof(status->detail), "%s", detail);
}

} // namespace

extern "C" int kirkware_component_initialize(KirkwareComponentStatus* status)
{
    if (status == nullptr)
        return -1;

    FillStatus(status, "linux in-process component initialized");
    return 0;
}

extern "C" int kirkware_component_poll(KirkwareComponentStatus* status)
{
    if (status == nullptr)
        return -1;

    FillStatus(status, "linux in-process component healthy");
    return 0;
}

extern "C" void kirkware_component_shutdown(void)
{
    g_heartbeat.store(0);
}
