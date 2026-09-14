#pragma once

#include "d3d9_texture.h"
#include "native_chain_bridge.h"

#include "imgui.h"

#include <array>
#include <string>

namespace kirkware
{

class KirkwareUi
{
public:
    KirkwareUi(ImFont* font13,
               ImFont* font14,
               ImFont* font12_bold,
               D3d9Texture* garrys_mod);
    ~KirkwareUi();

    void Update(float delta_seconds);
    void Render();

    int ClientWidth() const noexcept;
    int ClientHeight() const noexcept;
    float CurrentWidth() const noexcept { return current_width_; }
    float CurrentHeight() const noexcept { return current_height_; }
    float TargetWidth() const noexcept { return target_width_; }
    float TargetHeight() const noexcept { return target_height_; }
    bool CloseRequested() const noexcept { return close_requested_; }

private:
    enum class State
    {
        Connecting,
        Login,
        Logging,
        Home,
        CleaningTraces,
        WaitingForGame,
        PleaseWait,
        Injecting,
        LocalError
    };

    void Enter(State state, float target_width, float target_height);
    void AdvanceTransition();
    bool SizeSettled() const noexcept;
    bool UsernameValid() const noexcept;
    void SubmitLogin();
    void SubmitLoad();
    void RenderShell(ImDrawList* draw, float width, float height);
    void RenderStatus(ImDrawList* draw,
                      float width,
                      float height,
                      const char* text,
                      const char* child_id,
                      const char* spinner_id,
                      bool spinning = true,
                      float alpha = 1.0f);
    void RenderLogin(ImDrawList* draw,
                     float width,
                     float height,
                     float alpha);
    void RenderHome(ImDrawList* draw,
                    float width,
                    float height,
                    float alpha);
    ImFont* font13_ = nullptr;
    ImFont* font14_ = nullptr;
    ImFont* font12_bold_ = nullptr;
    D3d9Texture* garrys_mod_ = nullptr;
    State state_ = State::Connecting;
    State pending_state_ = State::Connecting;
    float current_width_ = 360.0f;
    float current_height_ = 270.0f;
    float target_width_ = 300.0f;
    float target_height_ = 210.0f;
    float frame_delta_seconds_ = 0.0f;
    float content_alpha_ = 1.0f;
    std::array<char, 21> username_{};
    std::array<char, 255> password_{};
    NativeChainBridge bridge_{};
    NativeChainSnapshot native_{};
    bool transition_pending_ = false;
    bool offline_connection_ready_ = true;
    bool offline_login_ready_ = false;
    bool clean_traces_ = false;
    bool cleaning_state_presented_ = false;
    float clean_traces_check_visibility_ = 0.0f;
    float clean_traces_hint_hover_started_ = -1.0f;
    float clean_traces_hint_visibility_ = 0.0f;
    bool run_garrys_mod_ = false;
    float run_garrys_mod_check_visibility_ = 0.0f;
    float game_tile_selection_visibility_ = 0.0f;
    bool login_submit_hovered_ = false;
    ImVec2 login_submit_bounds_min_{};
    ImVec2 login_submit_bounds_max_{};
    float login_submit_hover_visibility_ = 0.0f;
    bool home_load_hovered_ = false;
    ImVec2 home_load_bounds_min_{};
    ImVec2 home_load_bounds_max_{};
    float home_load_hover_visibility_ = 0.0f;
    float close_hover_visibility_ = 0.0f;
    bool root_open_ = true;
    bool close_requested_ = false;
};

}
