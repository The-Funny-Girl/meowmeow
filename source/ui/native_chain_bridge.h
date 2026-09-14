#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace kirkware
{

enum class NativeChainStatus
{
    Idle,
    Injecting,
    Succeeded,
    Failed
};

enum class NativeChainPhase
{
    CleaningTraces,
    WaitingForGame,
    PleaseWait,
    Injecting
};

struct NativeChainSnapshot
{
    NativeChainStatus status = NativeChainStatus::Idle;
    NativeChainPhase phase = NativeChainPhase::WaitingForGame;
    int exit_code = 0;
    std::string failure_stage = "none";
    unsigned long target_process_id = 0;
    bool run_garrys_mod_requested = false;
};

class NativeChainBridge
{
public:
    NativeChainBridge();
    NativeChainBridge(const NativeChainBridge&) = delete;
    NativeChainBridge& operator=(const NativeChainBridge&) = delete;

    bool Start(std::wstring_view username,
               bool clean_traces,
               bool run_garrys_mod);
    NativeChainSnapshot Poll() const noexcept;

private:
    struct SharedState;
    std::shared_ptr<SharedState> state_;
};

}
