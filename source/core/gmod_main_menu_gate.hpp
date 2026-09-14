#pragma once

#include <windows.h>

#include <cstdint>

namespace kirkware::gmod_main_menu_gate
{
enum class Status
{
    Ready,
    NotFound,
    Ambiguous,
    RuntimeUnavailable,
    WindowUnavailable,
    StateUnavailable,
    NotMainMenu,
    IdentityChanged
};

struct Target
{
    DWORD process_id = 0;
    std::uint64_t creation_time = 0;
    std::uintptr_t window = 0;
};

Status Wait(Target& target, DWORD timeout_ms);
Status Validate(const Target& target, unsigned samples,
                DWORD interval_ms);
}
