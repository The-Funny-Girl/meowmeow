#pragma once

#include "component_loader.hpp"

#include "imgui.h"

#include <array>
#include <filesystem>
#include <string>

namespace kirkware::linux_ui {

class LinuxKirkwareUi {
public:
    LinuxKirkwareUi(ImFont* font13,
                    ImFont* font14,
                    ImFont* font12_bold,
                    ImTextureID garrys_mod_texture,
                    std::filesystem::path component_path);
    ~LinuxKirkwareUi();

    LinuxKirkwareUi(const LinuxKirkwareUi&) = delete;
    LinuxKirkwareUi& operator=(const LinuxKirkwareUi&) = delete;

    void Update(float delta_seconds);
    void Render();

    int ClientWidth() const noexcept;
    int ClientHeight() const noexcept;
    float CurrentWidth() const noexcept { return current_width_; }
    float CurrentHeight() const noexcept { return current_height_; }
    bool CloseRequested() const noexcept { return close_requested_; }

private:
    enum class State {
        Connecting,
        Login,
        Logging,
        Home,
        LoadingComponent,
        LocalError,
    };

    void Enter(State state, float target_width, float target_height);
    void AdvanceTransition();
    bool SizeSettled() const noexcept;
    bool UsernameValid() const noexcept;
    void SubmitLogin();
    void SubmitLoad();
    void FinishComponentLoad();
    void RenderShell(ImDrawList* draw, float width, float height);
    void RenderStatus(ImDrawList* draw,
                      const char* text,
                      const char* child_id,
                      const char* spinner_id,
                      bool spinning = true);
    void RenderLogin();
    void RenderHome();

    ImFont* font13_ = nullptr;
    ImFont* font14_ = nullptr;
    ImFont* font12_bold_ = nullptr;
    ImTextureID garrys_mod_texture_ = nullptr;
    std::filesystem::path component_path_;
    platform::ComponentSession component_;
    KirkwareComponentStatus component_status_{};

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
    std::string failure_stage_;
    std::string failure_detail_;
    bool transition_pending_ = false;
    bool offline_connection_ready_ = true;
    bool offline_login_ready_ = false;
    bool pending_component_load_ = false;
    bool clean_workspace_ = false;
    float clean_workspace_check_visibility_ = 0.0f;
    float clean_workspace_hint_hover_started_ = -1.0f;
    float clean_workspace_hint_visibility_ = 0.0f;
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

} // namespace kirkware::linux_ui
