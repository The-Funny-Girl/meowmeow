#include "native_chain_bridge.h"

#include "kirkware_native_chain.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <thread>

namespace kirkware
{
namespace
{

void AppendFailureLog(int code, const char* stage, DWORD target_process_id)
{
    wchar_t temporary_path[MAX_PATH + 1]{};
    const DWORD temporary_path_length =
        GetTempPathW(MAX_PATH, temporary_path);
    if (temporary_path_length == 0 || temporary_path_length > MAX_PATH)
        return;
    const std::wstring log_path =
        std::wstring(temporary_path, temporary_path_length) +
        L"kirkware-injection-errors.log";
    HANDLE file = CreateFileW(
        log_path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char line[512]{};
    const int length = std::snprintf(
        line, sizeof(line),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u loader_pid=%lu "
        "target_pid=%lu code=%d stage=%s\r\n",
        static_cast<unsigned>(time.wYear),
        static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay),
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(target_process_id), code,
        stage ? stage : "unknown");
    if (length > 0)
    {
        const DWORD bytes = static_cast<DWORD>(
            (std::min)(static_cast<std::size_t>(length), sizeof(line) - 1));
        DWORD written = 0;
        WriteFile(file, line, bytes, &written, nullptr);
    }
    CloseHandle(file);
}

}

struct NativeChainBridge::SharedState
{
    std::atomic<NativeChainStatus> status{NativeChainStatus::Idle};
    std::atomic<NativeChainPhase> phase{NativeChainPhase::WaitingForGame};
    std::atomic<int> exit_code{0};
    std::atomic<const char*> failure_stage{"none"};
    std::atomic<DWORD> target_process_id{0};
    std::atomic<bool> run_garrys_mod_requested{false};
};

NativeChainBridge::NativeChainBridge()
    : state_(std::make_shared<SharedState>())
{
}

bool NativeChainBridge::Start(std::wstring_view username,
                              bool clean_traces,
                              bool run_garrys_mod)
{
    NativeChainStatus expected = NativeChainStatus::Idle;
    if (!state_->status.compare_exchange_strong(
            expected, NativeChainStatus::Injecting,
            std::memory_order_acq_rel))
    {
        return false;
    }

    const std::wstring copied_username(username);
    const std::shared_ptr<SharedState> state = state_;
    state_->run_garrys_mod_requested.store(
        run_garrys_mod, std::memory_order_release);
    state_->phase.store(
        clean_traces ? NativeChainPhase::CleaningTraces
                     : NativeChainPhase::WaitingForGame,
        std::memory_order_release);
    try
    {
        std::thread([state, copied_username, clean_traces, run_garrys_mod]() {
            const KirkwareNativeResult result =
                RunKirkwareNativeChain(
                GetModuleHandleW(nullptr), copied_username,
                nullptr, nullptr,
                [](void* context, KirkwareNativePhase phase) noexcept {
                    auto* shared = static_cast<SharedState*>(context);
                    NativeChainPhase mapped = NativeChainPhase::WaitingForGame;
                    switch (phase)
                    {
                    case KirkwareNativePhase::CleaningTraces:
                        mapped = NativeChainPhase::CleaningTraces;
                        break;
                    case KirkwareNativePhase::WaitingForGame:
                        mapped = NativeChainPhase::WaitingForGame;
                        break;
                    case KirkwareNativePhase::PleaseWait:
                        mapped = NativeChainPhase::PleaseWait;
                        break;
                    case KirkwareNativePhase::Injecting:
                        mapped = NativeChainPhase::Injecting;
                        break;
                    }
                    shared->phase.store(mapped, std::memory_order_release);
                },
                state.get(), clean_traces, run_garrys_mod);
            state->failure_stage.store(
                result.stage ? result.stage : "unknown",
                std::memory_order_relaxed);
            state->target_process_id.store(
                result.process_id, std::memory_order_relaxed);
            state->exit_code.store(result.code, std::memory_order_relaxed);
            if (result.code == 0)
            {
                state->status.store(
                    NativeChainStatus::Succeeded,
                    std::memory_order_release);
            }
            else
            {
                AppendFailureLog(
                    result.code, result.stage, result.process_id);
                NativeChainStatus expected = NativeChainStatus::Injecting;
                state->status.compare_exchange_strong(
                    expected, NativeChainStatus::Failed,
                    std::memory_order_acq_rel);
            }
        }).detach();
    }
    catch (...)
    {
        state_->failure_stage.store(
            "bridge_thread_create", std::memory_order_relaxed);
        state_->target_process_id.store(0, std::memory_order_relaxed);
        state_->exit_code.store(30, std::memory_order_relaxed);
        AppendFailureLog(30, "bridge_thread_create", 0);
        state_->status.store(
            NativeChainStatus::Failed, std::memory_order_release);
        return false;
    }
    return true;
}

NativeChainSnapshot NativeChainBridge::Poll() const noexcept
{
    NativeChainSnapshot result;
    result.status = state_->status.load(std::memory_order_acquire);
    result.phase = state_->phase.load(std::memory_order_acquire);
    result.exit_code = state_->exit_code.load(std::memory_order_acquire);
    result.failure_stage =
        state_->failure_stage.load(std::memory_order_acquire);
    result.target_process_id =
        state_->target_process_id.load(std::memory_order_acquire);
    result.run_garrys_mod_requested =
        state_->run_garrys_mod_requested.load(std::memory_order_acquire);
    return result;
}

}
