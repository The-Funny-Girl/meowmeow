#pragma once

#include <windows.h>

#include <string_view>

using KirkwareReadyCallback = void (*)(void*) noexcept;

enum class KirkwareNativePhase
{
    CleaningTraces,
    WaitingForGame,
    PleaseWait,
    Injecting
};

using KirkwarePhaseCallback =
    void (*)(void*, KirkwareNativePhase) noexcept;

struct KirkwareNativeResult
{
    int code = 0;
    const char* stage = "none";
    DWORD process_id = 0;
};

KirkwareNativeResult RunKirkwareNativeChain(
    HINSTANCE instance,
    std::wstring_view username,
    KirkwareReadyCallback ready_callback,
    void* ready_context,
    KirkwarePhaseCallback phase_callback,
    void* phase_context,
    bool clean_traces,
    bool run_garrys_mod);
