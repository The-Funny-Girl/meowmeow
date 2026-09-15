#include "linux_kirkware_ui.hpp"

#include "game_integration.hpp"
#include "linux_paths.hpp"
#include "workspace.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace kirkware::linux_ui {
namespace {

ImU32 Color(unsigned red,
            unsigned green,
            unsigned blue,
            unsigned alpha = 255,
            float opacity = 1.0f)
{
    const unsigned adjusted = static_cast<unsigned>(std::clamp(
        std::lround(static_cast<float>(alpha) * opacity), 0l, 255l));
    return IM_COL32(red, green, blue, adjusted);
}

void SecureClear(char* data, std::size_t size) noexcept
{
    volatile char* cursor = data;
    while (size-- != 0)
        *cursor++ = '\0';
}

void AddText(ImDrawList* draw,
             ImFont* font,
             float size,
             ImVec2 position,
             ImU32 color,
             const char* text)
{
    draw->AddText(font, size, position, color, text);
}

void AddButtonHoverGlowExact(ImDrawList* draw,
                             ImVec2 bounds_min,
                             ImVec2 bounds_max,
                             float visibility)
{
    if (visibility <= 0.02f || bounds_min.x == 0.0f)
        return;
    const ImVec4 scheme = ImGui::GetStyle().Colors[ImGuiCol_Scheme];
    ImVec4 color(scheme.x, scheme.y, scheme.z, 0.14f * visibility);
    while (color.w >= 0.0019f) {
        draw->AddRectFilled(bounds_min, bounds_max,
                            ImGui::ColorConvertFloat4ToU32(color), 4.0f, 0);
        color.w -= color.w / 2.5f;
        bounds_min.x -= 1.0f;
        bounds_min.y -= 1.0f;
        bounds_max.x += 1.0f;
        bounds_max.y += 1.0f;
    }
}

void BeginGroupBoxExact(const char* label, ImVec2 size)
{
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(27, 27, 27, 255));
    ImGui::BeginChild(label, size, false, ImGuiWindowFlags_NoBackground);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 position = ImGui::GetWindowPos();
    const ImVec2 window_size = ImGui::GetWindowSize();
    const float rounding = ImGui::GetStyle().ChildRounding;
    const ImVec2 frame_min(position.x, position.y + 6.0f);
    const ImVec2 frame_max(position.x + window_size.x,
                           position.y + window_size.y);
    draw->AddRectFilled(frame_min, frame_max,
                        ImGui::GetColorU32(ImGuiCol_ChildBg), rounding);
    draw->AddRect(frame_min, frame_max,
                  ImGui::GetColorU32(ImGuiCol_Border), rounding, 0, 1.0f);
    draw->AddLine({position.x + 13.0f, position.y + 6.0f},
                  {position.x + 22.0f + label_size.x, position.y + 6.0f},
                  ImGui::GetColorU32(ImGuiCol_ChildBg), 1.0f);
    draw->AddText({position.x + 18.0f, position.y},
                  ImGui::GetColorU32(ImGuiCol_Text), label,
                  ImGui::FindRenderedTextEnd(label));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10.0f, 10.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f, 8.0f});
    ImGui::SetCursorPos({0.0f, 10.0f});
    const std::string content_name = std::string(label) + "##content";
    ImGui::BeginChild(content_name.c_str(),
                      {window_size.x, window_size.y - 10.0f}, true,
                      ImGuiWindowFlags_NoBackground |
                          ImGuiWindowFlags_NoScrollbar);
}

void EndGroupBoxExact()
{
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::EndChild();
}

bool ButtonExact(const char* label, ImVec2 requested_size)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 position = window->DC.CursorPos;
    const ImVec2 size = ImGui::CalcItemSize(
        requested_size,
        label_size.x + style.FramePadding.x * 2.0f,
        label_size.y + style.FramePadding.y * 2.0f);
    const ImRect bounds(position,
                        {position.x + size.x, position.y + size.y});
    ImGui::ItemSize(size, style.FramePadding.y);
    if (!ImGui::ItemAdd(bounds, id))
        return false;
    bool hovered = false;
    bool held = false;
    const bool pressed = ImGui::ButtonBehavior(
        bounds, id, &hovered, &held, ImGuiButtonFlags_None);
    const ImU32 background = ImGui::GetColorU32(
        held && hovered ? ImGuiCol_ButtonActive
                        : hovered ? ImGuiCol_ButtonHovered
                                  : ImGuiCol_Button);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(bounds.Min, bounds.Max, background, 3.0f);
    draw->AddRect(bounds.Min, bounds.Max,
                  ImGui::GetColorU32(ImGuiCol_Border), 3.0f, 0, 1.0f);
    ImGui::RenderTextClipped(bounds.Min, bounds.Max, label, nullptr,
                             &label_size, style.ButtonTextAlign, &bounds);
    return pressed;
}

bool CheckboxExact(const char* label,
                   bool& value,
                   float& visibility,
                   float delta_seconds)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, nullptr, true);
    const ImVec2 position = window->DC.CursorPos;
    const ImRect check_bounds(position,
                              {position.x + 13.0f, position.y + 13.0f});
    const ImRect total_bounds(
        position, {position.x + 19.0f + label_size.x, position.y + 13.0f});
    ImGui::ItemAdd(total_bounds, id);
    ImGui::ItemSize(total_bounds, -1.0f);
    bool hovered = false;
    bool held = false;
    const bool pressed = ImGui::ButtonBehavior(
        total_bounds, id, &hovered, &held, ImGuiButtonFlags_None);
    if (pressed) {
        value = !value;
        ImGui::MarkItemEdited(id);
    }
    const float blend = 0.09f * (1.0f - delta_seconds);
    visibility += ((value ? 1.0f : 0.0f) - visibility) * blend;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(
        check_bounds.Min, check_bounds.Max,
        ImGui::GetColorU32(
            hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg),
        3.0f);
    draw->AddRectFilled(check_bounds.Min, check_bounds.Max,
                        ImGui::GetColorU32(ImGuiCol_Scheme, visibility), 3.0f);
    draw->AddRect(check_bounds.Min, check_bounds.Max,
                  ImGui::GetColorU32(ImGuiCol_Border), 3.0f, 0, 1.0f);
    const ImVec2 text_position(
        check_bounds.Max.x + 6.0f,
        check_bounds.GetCenter().y - label_size.y * 0.5f);
    draw->AddText(
        text_position,
        ImGui::GetColorU32(value ? ImGuiCol_Text : ImGuiCol_TextDisabled),
        label, ImGui::FindRenderedTextEnd(label));
    return pressed;
}

} // namespace

LinuxKirkwareUi::LinuxKirkwareUi(ImFont* font13,
                                 ImFont* font14,
                                 ImFont* font12_bold,
                                 ImTextureID garrys_mod_texture,
                                 std::filesystem::path component_path)
    : font13_(font13),
      font14_(font14),
      font12_bold_(font12_bold),
      garrys_mod_texture_(garrys_mod_texture),
      component_path_(std::move(component_path))
{
}

LinuxKirkwareUi::~LinuxKirkwareUi()
{
    SecureClear(password_.data(), password_.size());
    component_.Close();
}

void LinuxKirkwareUi::Enter(State state,
                            float target_width,
                            float target_height)
{
    pending_state_ = state;
    target_width_ = target_width;
    target_height_ = target_height;
    transition_pending_ = true;
}

void LinuxKirkwareUi::AdvanceTransition()
{
    const float target = transition_pending_ ? 0.0f : 1.0f;
    content_alpha_ += (target - content_alpha_) * 0.09f;
    if (transition_pending_ && content_alpha_ < 0.0001f) {
        state_ = pending_state_;
        transition_pending_ = false;
    }
}

bool LinuxKirkwareUi::SizeSettled() const noexcept
{
    return std::abs(current_width_ - target_width_) < 0.001f &&
           std::abs(current_height_ - target_height_) < 0.001f;
}

bool LinuxKirkwareUi::UsernameValid() const noexcept
{
    return username_[0] != '\0';
}

void LinuxKirkwareUi::SubmitLogin()
{
    if (!UsernameValid())
        return;
    SecureClear(password_.data(), password_.size());
    offline_login_ready_ = true;
    Enter(State::Logging, 300.0f, 210.0f);
}

void LinuxKirkwareUi::SubmitLoad()
{
    if (!UsernameValid())
        return;
    pending_component_load_ = true;
    failure_stage_.clear();
    failure_detail_.clear();
    Enter(State::LoadingComponent, 300.0f, 210.0f);
}

void LinuxKirkwareUi::FinishComponentLoad()
{
    pending_component_load_ = false;
    component_.Close();

    if (clean_workspace_) {
        platform::AppPaths paths;
        std::string cleanup_error;
        if (!platform::DiscoverAppPaths(paths, &cleanup_error)) {
            failure_stage_ = "workspace_paths";
            failure_detail_ = cleanup_error.empty()
                                  ? "unable to resolve Linux workspace paths"
                                  : cleanup_error;
            Enter(State::LocalError, 300.0f, 210.0f);
            return;
        }
        platform::CleanupStaleWorkspaces(
            paths.workspaces, std::chrono::hours(0), &cleanup_error);
        if (!cleanup_error.empty()) {
            failure_stage_ = "workspace_cleanup";
            failure_detail_ = cleanup_error;
            Enter(State::LocalError, 300.0f, 210.0f);
            return;
        }
    }

    if (component_path_.empty()) {
        failure_stage_ = "component_path";
        failure_detail_ = "libkirkware_component.so was not found";
        Enter(State::LocalError, 300.0f, 210.0f);
        return;
    }

    std::string error;
    if (!component_.Open(component_path_, &error)) {
        failure_stage_ = "component_open";
        failure_detail_ = error.empty() ? "unable to load Linux component" : error;
        Enter(State::LocalError, 300.0f, 210.0f);
        return;
    }
    component_status_ = component_.initial_status();

    if (run_garrys_mod_ && !platform::LaunchGarrysMod(&error)) {
        component_.Close();
        failure_stage_ = "game_launch";
        failure_detail_ = error.empty() ? "unable to start Garry's Mod" : error;
        Enter(State::LocalError, 300.0f, 210.0f);
        return;
    }

    Enter(State::Home, 380.0f, 290.0f);
}

void LinuxKirkwareUi::Update(float delta_seconds)
{
    const float delta = std::clamp(delta_seconds, 0.0f, 0.1f);
    frame_delta_seconds_ = delta;
    const auto approach = [](float current, float target) {
        return current + (target - current) * 0.09f;
    };
    current_width_ = approach(current_width_, target_width_);
    current_height_ = approach(current_height_, target_height_);

    if (transition_pending_)
        return;

    switch (state_) {
    case State::Connecting:
        if (SizeSettled() && offline_connection_ready_)
            Enter(State::Login, 360.0f, 270.0f);
        break;
    case State::Logging:
        if (offline_login_ready_)
            Enter(State::Home, 380.0f, 290.0f);
        break;
    case State::LoadingComponent:
        if (pending_component_load_)
            FinishComponentLoad();
        break;
    case State::Home:
        if (component_.active()) {
            std::string error;
            KirkwareComponentStatus status{};
            status.abi_version = KIRKWARE_COMPONENT_ABI_VERSION;
            status.struct_size = sizeof(status);
            if (!component_.Poll(&status, &error)) {
                component_.Close();
                failure_stage_ = "component_poll";
                failure_detail_ = error.empty() ? "component poll failed" : error;
                Enter(State::LocalError, 300.0f, 210.0f);
            } else {
                component_status_ = status;
            }
        }
        break;
    case State::Login:
    case State::LocalError:
        break;
    }
}

int LinuxKirkwareUi::ClientWidth() const noexcept
{
    return std::clamp(static_cast<int>(current_width_), 300, 379);
}

int LinuxKirkwareUi::ClientHeight() const noexcept
{
    return std::clamp(static_cast<int>(current_height_), 210, 289);
}

void LinuxKirkwareUi::RenderShell(ImDrawList* draw,
                                  float,
                                  float)
{
    if (font14_ == nullptr)
        font14_ = font13_;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {15.0f, 0.0f});
    ImGui::BeginChild("header", {ImGui::GetWindowWidth(), 45.0f}, false,
                      ImGuiWindowFlags_NoBackground |
                          ImGuiWindowFlags_NoNavInputs);
    draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();

    const char* first = "kirkware";
    const ImVec2 first_size = font14_->CalcTextSizeA(
        14.0f, 1000.0f, 0.0f, first);
    const float title_y = origin.y + ImGui::GetWindowHeight() * 0.5f -
                          first_size.y * 0.5f + 1.0f;
    AddText(draw, font14_, 14.0f,
            {origin.x + ImGui::GetStyle().WindowPadding.x, title_y},
            Color(255, 255, 255), first);

    const float close_x = ImGui::GetWindowWidth() -
                          ImGui::GetStyle().WindowPadding.x - 10.0f;
    const float close_y = ImGui::GetWindowHeight() * 0.5f - 5.0f;
    ImGui::SetCursorPos({close_x, close_y});
    ImGui::InvisibleButton("exit_btn##exit_btn", {10.0f, 10.0f});
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        close_requested_ = true;
    const float close_blend = 0.09f * (1.0f - frame_delta_seconds_);
    close_hover_visibility_ +=
        (((hovered || held) ? 1.0f : 0.0f) - close_hover_visibility_) *
        close_blend;
    const auto close_channel = [this](unsigned normal, unsigned active) {
        return static_cast<unsigned>(std::clamp(
            std::lround(static_cast<float>(normal) +
                        (static_cast<float>(active) -
                         static_cast<float>(normal)) *
                            close_hover_visibility_),
            0l, 255l));
    };
    const ImU32 close_color = Color(close_channel(255, 173),
                                    close_channel(255, 154),
                                    close_channel(255, 160));
    const ImVec2 center(origin.x + close_x + 5.0f,
                        origin.y + close_y + 5.0f);
    float glow_alpha = 0.05f * close_hover_visibility_;
    float glow_expansion = 0.0f;
    while (glow_alpha > 0.0019f) {
        draw->AddRectFilled(
            {origin.x + close_x - glow_expansion,
             origin.y + close_y - glow_expansion},
            {origin.x + close_x + 10.0f + glow_expansion,
             origin.y + close_y + 10.0f + glow_expansion},
            Color(138, 138, 138, 255, glow_alpha), 4.0f);
        glow_alpha *= 0.5f;
        glow_expansion += 1.0f;
    }
    const int close_vertex_start = draw->VtxBuffer.Size;
    draw->AddLine({center.x, center.y - 5.25f},
                  {center.x, center.y + 5.25f}, close_color, 0.21f);
    draw->AddLine({center.x - 5.25f, center.y},
                  {center.x + 5.25f, center.y}, close_color, 0.21f);
    constexpr float sine = 0.7071067691f;
    constexpr float cosine = 0.7071067691f;
    for (int index = close_vertex_start; index < draw->VtxBuffer.Size; ++index) {
        ImVec2& position = draw->VtxBuffer[index].pos;
        const float x = position.x - center.x;
        const float y = position.y - center.y;
        position.x = center.x + x * cosine - y * sine;
        position.y = center.y + x * sine + y * cosine;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void LinuxKirkwareUi::RenderStatus(ImDrawList* draw,
                                   const char* text,
                                   const char* child_id,
                                   const char* spinner_id,
                                   bool spinning)
{
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const float window_width = ImGui::GetWindowWidth();
    const float window_height = ImGui::GetWindowHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    float child_width = text_size.x + spacing + 22.0f;
    float child_height = 16.0f;
    float child_x = (window_width - child_width) * 0.5f;
    float child_y = window_height * 0.5f - 7.0f;
    float text_offset = 7.0f;

    if (state_ == State::Connecting) {
        child_width = text_size.x + spacing + 13.0f;
        child_x = (window_width - child_width) * 0.5f;
        text_offset = 0.0f;
    } else if (state_ == State::Logging) {
        child_width = text_size.x + spacing + 20.0f;
        child_x = (window_width - child_width) * 0.5f;
        text_offset = 6.0f;
    } else if (state_ == State::LocalError) {
        child_width = (std::min)(text_size.x + spacing + 13.0f,
                                 window_width - 10.0f);
        child_height = (std::max)(16.0f, text_size.y);
        child_x = (window_width - child_width) * 0.5f;
        child_y = (window_height - child_height) * 0.5f;
        text_offset = 0.0f;
    }

    ImGui::SetCursorPos({child_x, child_y});
    ImGui::BeginChild(child_id, {child_width, child_height}, false,
                      ImGuiWindowFlags_None);
    draw = ImGui::GetWindowDrawList();
    if (text_offset != 0.0f)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + text_offset);
    ImGui::TextUnformatted(text);

    if (spinning) {
        ImGui::SameLine(0.0f, -1.0f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2.0f);
        constexpr float pi = 3.14159265358979323846f;
        constexpr float radius = 6.0f;
        ImGuiWindow* spinner_window = ImGui::GetCurrentWindow();
        const ImGuiID spinner_item_id = spinner_window->GetID(spinner_id);
        const ImVec2 spinner_min = spinner_window->DC.CursorPos;
        const ImVec2 spinner_max(
            spinner_min.x + radius * 2.0f,
            spinner_min.y +
                (radius + ImGui::GetStyle().FramePadding.y) * 2.0f);
        const ImRect spinner_bounds(spinner_min, spinner_max);
        ImGui::ItemSize(spinner_bounds, ImGui::GetStyle().FramePadding.y);
        if (ImGui::ItemAdd(spinner_bounds, spinner_item_id)) {
            const float time = static_cast<float>(ImGui::GetTime()) * 2.8f;
            const int segments = draw->_CalcCircleAutoSegmentCount(radius);
            const ImVec2 center = spinner_bounds.GetCenter();
            for (int index = 0; index < 3; ++index) {
                const float ring_alpha =
                    0.1f + 0.9f / 3.0f * static_cast<float>(index);
                const float y_offset = radius * 0.5f * std::sin(
                    time + pi / 3.0f * static_cast<float>(index));
                draw->PathClear();
                for (int segment = 0; segment < segments; ++segment) {
                    const float angle =
                        static_cast<float>(segment) * 2.0f * pi /
                        static_cast<float>(segments - 1);
                    draw->PathLineTo({center.x + std::cos(angle) * radius,
                                      center.y + y_offset +
                                          std::sin(angle) * radius * 0.5f});
                }
                draw->PathStroke(
                    ImGui::GetColorU32(ImGuiCol_Scheme, ring_alpha),
                    ImDrawFlags_None, 3.0f);
            }
        }
    }
    ImGui::EndChild();
}

void LinuxKirkwareUi::RenderLogin()
{
    login_submit_hover_visibility_ +=
        ((login_submit_hovered_ ? 1.0f : 0.0f) -
         login_submit_hover_visibility_) *
        (0.09f * (1.0f - frame_delta_seconds_));
    AddButtonHoverGlowExact(ImGui::GetWindowDrawList(),
                            login_submit_bounds_min_,
                            login_submit_bounds_max_,
                            login_submit_hover_visibility_);
    ImGui::SetCursorPos({ImGui::GetWindowWidth() * 0.5f - 100.0f,
                         ImGui::GetWindowHeight() * 0.5f - 52.0f});
    BeginGroupBoxExact("login", {200.0f, 105.0f});
    ImGui::InputText("name", username_.data(), username_.size());
    ImGui::InputText("password", password_.data(), password_.size(),
                     ImGuiInputTextFlags_Password);
    const float button_width = ImGui::GetContentRegionAvail().x;
    const bool clicked = ButtonExact("submit", {button_width, 20.0f});
    login_submit_bounds_min_ = ImGui::GetItemRectMin();
    login_submit_bounds_max_ = ImGui::GetItemRectMax();
    login_submit_hovered_ = ImGui::IsItemHovered();
    if (clicked && state_ == State::Login)
        SubmitLogin();
    EndGroupBoxExact();
}

void LinuxKirkwareUi::RenderHome()
{
    const ImVec2 parent_size = ImGui::GetWindowSize();
    ImGui::BeginChild("home_child",
                      {parent_size.x - 26.0f, parent_size.y - 26.0f},
                      false, ImGuiWindowFlags_None);
    const float home_width = ImGui::GetWindowWidth();
    const float home_height = ImGui::GetWindowHeight();

    BeginGroupBoxExact("games", {130.0f, home_height});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImGuiWindow* game_window = ImGui::GetCurrentWindow();
    const ImGuiID game_id = game_window->GetID("garry's mod");
    const ImVec2 game_size(
        ImGui::GetWindowWidth() -
            ImGui::GetStyle().WindowPadding.x * 2.0f,
        30.0f);
    const ImVec2 game_min = game_window->DC.CursorPos;
    const ImRect game_bounds(game_min,
                             {game_min.x + game_size.x,
                              game_min.y + game_size.y});
    ImGui::ItemAdd(game_bounds, game_id);
    ImGui::ItemSize(game_bounds, -1.0f);
    bool game_hovered = false;
    bool game_held = false;
    ImGui::ButtonBehavior(game_bounds, game_id, &game_hovered, &game_held,
                          ImGuiButtonFlags_None);
    game_tile_selection_visibility_ +=
        (1.0f - game_tile_selection_visibility_) *
        (0.09f * (1.0f - frame_delta_seconds_));
    draw->AddRectFilled(
        game_bounds.Min, game_bounds.Max,
        ImGui::GetColorU32(ImGuiCol_FrameBg,
                           game_tile_selection_visibility_),
        3.0f);
    if (garrys_mod_texture_ != nullptr) {
        draw->AddImageRounded(
            garrys_mod_texture_, game_bounds.Min,
            {game_bounds.Min.x + 30.0f, game_bounds.Min.y + 30.0f},
            {0.0f, 0.0f}, {1.0f, 1.0f},
            ImGui::GetColorU32(ImGuiCol_Text), 3.0f);
    }
    const ImVec4 title_color = ImLerp(
        ImGui::GetStyle().Colors[ImGuiCol_TextDisabled],
        ImGui::GetStyle().Colors[ImGuiCol_Scheme],
        game_tile_selection_visibility_);
    draw->AddText({game_bounds.Min.x + 35.0f, game_bounds.Min.y + 2.0f},
                  ImGui::GetColorU32(title_color), "garry's mod");
    const char* tile_status = component_.active() ? "linux active" : "linux ready";
    const ImVec2 game_title_size = ImGui::CalcTextSize(tile_status);
    draw->AddText(
        {game_bounds.Min.x + 35.0f,
         game_bounds.Min.y + game_bounds.GetHeight() -
             game_title_size.y - 2.0f},
        ImGui::GetColorU32(ImGuiCol_TextDisabled), tile_status);
    EndGroupBoxExact();

    ImGui::SameLine(0.0f, -1.0f);
    BeginGroupBoxExact(
        "garry's mod",
        {home_width - 130.0f - ImGui::GetStyle().ItemSpacing.x,
         home_height});
    ImGui::Text("game: %s", "garry's mod");
    ImGui::Text("status: %s", component_.active() ? "active" : "ready");

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 77.0f);
    CheckboxExact("clean workspace", clean_workspace_,
                  clean_workspace_check_visibility_, frame_delta_seconds_);
    ImGui::SameLine(0.0f, -1.0f);

    const char* hint_text =
        "remove inactive kirkware linux workspaces before loading.\n"
        "active sessions are protected by their workspace lock.";
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImGuiID hint_id = window->GetID(hint_text);
    const ImVec2 hint_min = window->DC.CursorPos;
    const ImVec2 hint_max(hint_min.x + 13.0f, hint_min.y + 13.0f);
    const ImRect hint_bounds(hint_min, hint_max);
    ImGui::ItemSize(hint_bounds);
    ImGui::ItemAdd(hint_bounds, hint_id);
    bool hint_hovered = false;
    bool hint_held = false;
    ImGui::ButtonBehavior(hint_bounds, hint_id, &hint_hovered, &hint_held,
                          ImGuiButtonFlags_None);
    const float milliseconds =
        static_cast<float>(ImGui::GetTime() * 1000.0);
    float hint_target = 0.0f;
    if (hint_hovered) {
        if (clean_workspace_hint_hover_started_ < 0.0f)
            clean_workspace_hint_hover_started_ = milliseconds;
        if (milliseconds - clean_workspace_hint_hover_started_ > 300.0f)
            hint_target = 1.0f;
    } else {
        clean_workspace_hint_hover_started_ = -1.0f;
    }
    clean_workspace_hint_visibility_ +=
        (hint_target - clean_workspace_hint_visibility_) * 0.09f;
    const ImVec2 hint_center = hint_bounds.GetCenter();
    const ImVec2 hint_size = ImGui::CalcTextSize(hint_text, nullptr, true);
    if (clean_workspace_hint_visibility_ > 0.001f) {
        const ImVec2 popup_min(hint_min.x - hint_size.x * 0.5f,
                               hint_min.y - hint_size.y - 5.0f);
        const ImVec2 popup_max(hint_max.x + hint_size.x * 0.5f,
                               hint_min.y - 2.0f);
        const ImVec2 text_position(hint_center.x - hint_size.x * 0.5f,
                                   hint_min.y - hint_size.y - 4.0f);
        ImDrawList* foreground = ImGui::GetForegroundDrawList();
        foreground->PushClipRectFullScreen();
        foreground->AddRectFilled(
            popup_min, popup_max,
            ImGui::GetColorU32(ImGuiCol_WindowBg,
                               clean_workspace_hint_visibility_),
            ImGui::GetStyle().FrameRounding);
        foreground->AddRect(
            popup_min, popup_max,
            ImGui::GetColorU32(ImGuiCol_Border,
                               clean_workspace_hint_visibility_),
            ImGui::GetStyle().FrameRounding, 0, 1.0f);
        foreground->AddText(
            text_position,
            ImGui::GetColorU32(ImGuiCol_Text,
                               clean_workspace_hint_visibility_),
            hint_text, ImGui::FindRenderedTextEnd(hint_text));
        foreground->PopClipRect();
    }
    draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(hint_center, 6.5f,
                          ImGui::GetColorU32(ImGuiCol_WindowBg));
    draw->AddCircle(hint_center, 6.5f,
                    ImGui::GetColorU32(ImGuiCol_Border), 0, 1.0f);
    ImFont* question_font = font12_bold_ != nullptr
                                ? font12_bold_
                                : ImGui::GetFont();
    const float question_font_size = font12_bold_ != nullptr
                                         ? 12.0f
                                         : ImGui::GetFontSize();
    const ImVec2 question_size = question_font->CalcTextSizeA(
        question_font_size, 1000.0f, 0.0f, "?");
    AddText(draw, question_font, question_font_size,
            {hint_center.x - question_size.x * 0.5f + 1.0f,
             hint_center.y - question_size.y * 0.5f},
            ImGui::GetColorU32(ImGuiCol_TextDisabled), "?");

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 57.0f);
    ImGui::BeginGroup();
    ImGui::PushID(1);
    CheckboxExact("run garry's mod", run_garrys_mod_,
                  run_garrys_mod_check_visibility_, frame_delta_seconds_);
    ImGui::PopID();
    home_load_hover_visibility_ +=
        ((home_load_hovered_ ? 1.0f : 0.0f) -
         home_load_hover_visibility_) *
        (0.09f * (1.0f - frame_delta_seconds_));
    AddButtonHoverGlowExact(ImGui::GetWindowDrawList(),
                            home_load_bounds_min_, home_load_bounds_max_,
                            home_load_hover_visibility_);
    const ImVec2 load_size(
        ImGui::GetWindowWidth() -
            ImGui::GetStyle().WindowPadding.x * 2.0f,
        25.0f);
    const bool clicked = ButtonExact(component_.active() ? "reload" : "load",
                                     load_size);
    home_load_bounds_min_ = ImGui::GetItemRectMin();
    home_load_bounds_max_ = ImGui::GetItemRectMax();
    home_load_hovered_ = ImGui::IsItemHovered();
    ImGui::EndGroup();
    if (clicked && state_ == State::Home)
        SubmitLoad();
    EndGroupBoxExact();
    ImGui::EndChild();
}

void LinuxKirkwareUi::Render()
{
    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({current_width_, current_height_});
    ImGui::Begin(
        "kirkware linux", &root_open_,
        ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoSavedSettings);
    const float width = ImGui::GetWindowWidth();
    const float height = ImGui::GetWindowHeight();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 root_min = ImGui::GetWindowPos();
    const ImVec2 root_size = ImGui::GetWindowSize();
    const ImVec2 root_max(root_min.x + root_size.x - 1.0f,
                          root_min.y + root_size.y - 1.0f);
    draw->AddRect(root_min, root_max, ImGui::GetColorU32(ImGuiCol_Border),
                  ImGui::GetStyle().WindowRounding);
    RenderShell(draw, width, height);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {13.0f, 13.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10.0f, 10.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,
                        ImGui::GetStyle().WindowRounding);
    ImGui::SetCursorPosX(15.0f);
    ImGui::BeginChild("main", {width - 30.0f, height - 70.0f}, true,
                      ImGuiWindowFlags_NoNavInputs);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, content_alpha_);
    draw = ImGui::GetWindowDrawList();

    switch (state_) {
    case State::Connecting:
        RenderStatus(draw, "connecting..", "initializing_child",
                     "##initializing_spinner", true);
        break;
    case State::Login:
        RenderLogin();
        break;
    case State::Logging:
        RenderStatus(draw, "logging in..", "loading_child",
                     "##logging_spinner", true);
        break;
    case State::Home:
        RenderHome();
        break;
    case State::LoadingComponent:
        RenderStatus(draw, "please wait..", "component_child",
                     "##component_spinner", true);
        break;
    case State::LocalError: {
        char failure_text[512]{};
        const std::string stage =
            failure_stage_.empty() ? "unknown" : failure_stage_;
        const std::string detail =
            failure_detail_.empty() ? "unknown error" : failure_detail_;
        std::snprintf(failure_text, sizeof(failure_text),
                      "load failed\nstage: %s\n%s",
                      stage.c_str(), detail.c_str());
        RenderStatus(draw, failure_text, "error_child",
                     "##error_spinner", false);
        break;
    }
    }

    ImGui::PopStyleVar(2);
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::End();
    AdvanceTransition();

    if (!root_open_)
        close_requested_ = true;
}

} // namespace kirkware::linux_ui
