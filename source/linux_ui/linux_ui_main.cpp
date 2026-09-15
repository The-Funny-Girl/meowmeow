#include "linux_kirkware_ui.hpp"

#include "imgui.h"

#include <SDL.h>
#include <SDL_image.h>
#include <GL/gl.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

#ifndef KIRKWARE_SOURCE_ASSET_DIR
#define KIRKWARE_SOURCE_ASSET_DIR ""
#endif

namespace {

GLuint g_font_texture = 0;
char* g_clipboard_text = nullptr;

constexpr std::uint32_t kStyleColorBits[ImGuiCol_COUNT][4] = {
    {0x3F2DADAE, 0x3F1A9A9B, 0x3F20A0A1, 0x3F800000},
    {0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000},
    {0x3F0A8A8B, 0x3F0A8A8B, 0x3F0A8A8B, 0x3F800000},
    {0x3DC8C8CA, 0x3DC0C0C2, 0x3DC8C8CA, 0x3F800000},
    {0x3D888889, 0x3D888889, 0x3D888889, 0x3F800000},
    {0x3DA3D70A, 0x3DA3D70A, 0x3DA3D70A, 0x3F70A3D7},
    {0x3F800000, 0x3F800000, 0x3F800000, 0x3D8F5C29},
    {0x00000000, 0x00000000, 0x00000000, 0x3E19999A},
    {0x3DA8A8A9, 0x3DA8A8A9, 0x3DA8A8A9, 0x3F800000},
    {0x3DE0E0E2, 0x3DE0E0E2, 0x3DE0E0E2, 0x3F800000},
    {0x3E0C8C8D, 0x3E0C8C8D, 0x3E0C8C8D, 0x3F800000},
    {0x3D23D70A, 0x3D23D70A, 0x3D23D70A, 0x3F800000},
    {0x3E23D70A, 0x3E947AE1, 0x3EF5C28F, 0x3F800000},
    {0x00000000, 0x00000000, 0x00000000, 0x3F028F5C},
    {0x3E0F5C29, 0x3E0F5C29, 0x3E0F5C29, 0x3F800000},
    {0x3CA3D70A, 0x3CA3D70A, 0x3CA3D70A, 0x00000000},
    {0x3F2DADAE, 0x3F1A9A9B, 0x3F20A0A1, 0x3F800000},
    {0x3F37B7B8, 0x3F24A4A5, 0x3F2AAAAB, 0x3F800000},
    {0x3F41C1C3, 0x3F2EAEAF, 0x3F34B4B5, 0x3F800000},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x3F800000},
    {0x3E75C28F, 0x3F051EB8, 0x3F6147AE, 0x3F800000},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x3F800000},
    {0x3DA8A8A9, 0x3DA8A8A9, 0x3DA8A8A9, 0x3F800000},
    {0x3DE0E0E2, 0x3DE0E0E2, 0x3DE0E0E2, 0x3F800000},
    {0x3E0C8C8D, 0x3E0C8C8D, 0x3E0C8C8D, 0x3F800000},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x3E9EB852},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x3F4CCCCD},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x3F800000},
    {0x3F800000, 0x3F800000, 0x3F800000, 0x3D8F5C29},
    {0x3DCCCCCD, 0x3ECCCCCD, 0x3F400000, 0x3F47AE14},
    {0x3DCCCCCD, 0x3ECCCCCD, 0x3F400000, 0x3F800000},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x00000000},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x00000000},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x00000000},
    {0x3E3851EB, 0x3EB33332, 0x3F147AE2, 0x3F5CAC08},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x3F4CCCCD},
    {0x3E4CCCCC, 0x3ED1EB84, 0x3F2E147B, 0x3F800000},
    {0x3D8B4396, 0x3DD0E55E, 0x3E178D52, 0x3F78EF35},
    {0x3E0B4394, 0x3E8624DC, 0x3ED91687, 0x3F800000},
    {0x3F1C28F6, 0x3F1C28F6, 0x3F1C28F6, 0x3F800000},
    {0x3F800000, 0x3EDC28F6, 0x3EB33333, 0x3F800000},
    {0x3F666666, 0x3F333333, 0x00000000, 0x3F800000},
    {0x3F800000, 0x3F19999A, 0x00000000, 0x3F800000},
    {0x3E428F5C, 0x3E428F5C, 0x3E4CCCCD, 0x3F800000},
    {0x3E9EB852, 0x3E9EB852, 0x3EB33333, 0x3F800000},
    {0x3E6B851F, 0x3E6B851F, 0x3E800000, 0x3F800000},
    {0x00000000, 0x00000000, 0x00000000, 0x00000000},
    {0x3F800000, 0x3F800000, 0x3F800000, 0x3D75C28F},
    {0x3F2DADAE, 0x3F1A9A9B, 0x3F20A0A1, 0x3F800000},
    {0x3F800000, 0x3F800000, 0x00000000, 0x3F666666},
    {0x3E851EB8, 0x3F170A3D, 0x3F7AE148, 0x3F800000},
    {0x3F800000, 0x3F800000, 0x3F800000, 0x3F333333},
    {0x3F4CCCCD, 0x3F4CCCCD, 0x3F4CCCCD, 0x3E4CCCCD},
    {0x3F4CCCCD, 0x3F4CCCCD, 0x3F4CCCCD, 0x3EB33333},
};

constexpr float FloatFromBits(std::uint32_t bits)
{
    return std::bit_cast<float>(bits);
}

fs::path ExecutableDirectory()
{
    std::array<char, 4096> buffer{};
    const ssize_t length =
        ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0)
        return {};
    buffer[static_cast<std::size_t>(length)] = '\0';
    return fs::path(buffer.data()).parent_path();
}

bool IsRegularFile(const fs::path& path)
{
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(path, ec) && !ec;
}

fs::path FirstExisting(const std::vector<fs::path>& candidates)
{
    for (const fs::path& candidate : candidates) {
        if (IsRegularFile(candidate))
            return candidate;
    }
    return {};
}

fs::path ResolveComponentPath(const fs::path& override_path)
{
    if (!override_path.empty())
        return IsRegularFile(override_path) ? override_path : fs::path{};
    if (const char* configured = std::getenv("KIRKWARE_COMPONENT_PATH");
        configured && *configured) {
        const fs::path path(configured);
        if (IsRegularFile(path))
            return path;
    }
    const fs::path executable = ExecutableDirectory();
    return FirstExisting({
        executable / "libkirkware_component.so",
        executable / ".." / "lib" / "kirkware" /
            "libkirkware_component.so",
        executable / ".." / "lib64" / "kirkware" /
            "libkirkware_component.so",
    });
}

fs::path ResolveAssetPath(std::string_view name)
{
    std::vector<fs::path> candidates;
    if (const char* configured = std::getenv("KIRKWARE_ASSET_DIR");
        configured && *configured) {
        candidates.emplace_back(fs::path(configured) / name);
    }
    const fs::path executable = ExecutableDirectory();
    candidates.emplace_back(executable / ".." / "share" / "kirkware" / name);
    if (std::string_view(KIRKWARE_SOURCE_ASSET_DIR).size() != 0)
        candidates.emplace_back(fs::path(KIRKWARE_SOURCE_ASSET_DIR) / name);
    return FirstExisting(candidates);
}

fs::path ResolveFont(bool bold)
{
    const char* variable = bold ? "KIRKWARE_UI_FONT_BOLD" : "KIRKWARE_UI_FONT";
    if (const char* configured = std::getenv(variable); configured && *configured) {
        const fs::path path(configured);
        if (IsRegularFile(path))
            return path;
    }
    if (bold) {
        return FirstExisting({
            "/usr/share/fonts/truetype/msttcorefonts/tahomabd.ttf",
            "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        });
    }
    return FirstExisting({
        "/usr/share/fonts/truetype/msttcorefonts/tahoma.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    });
}

const char* GetClipboardText(void*)
{
    if (g_clipboard_text != nullptr) {
        SDL_free(g_clipboard_text);
        g_clipboard_text = nullptr;
    }
    g_clipboard_text = SDL_GetClipboardText();
    return g_clipboard_text != nullptr ? g_clipboard_text : "";
}

void SetClipboardText(void*, const char* text)
{
    SDL_SetClipboardText(text != nullptr ? text : "");
}

ImGuiKey SdlKeyToImGuiKey(SDL_Keycode key)
{
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_INSERT: return ImGuiKey_Insert;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_a: return ImGuiKey_A;
    case SDLK_c: return ImGuiKey_C;
    case SDLK_v: return ImGuiKey_V;
    case SDLK_x: return ImGuiKey_X;
    case SDLK_y: return ImGuiKey_Y;
    case SDLK_z: return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

void UpdateModifiers(SDL_Keymod modifiers)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, (modifiers & KMOD_CTRL) != 0);
    io.AddKeyEvent(ImGuiMod_Shift, (modifiers & KMOD_SHIFT) != 0);
    io.AddKeyEvent(ImGuiMod_Alt, (modifiers & KMOD_ALT) != 0);
    io.AddKeyEvent(ImGuiMod_Super, (modifiers & KMOD_GUI) != 0);
}

void ProcessEvent(const SDL_Event& event)
{
    ImGuiIO& io = ImGui::GetIO();
    switch (event.type) {
    case SDL_MOUSEMOTION:
        io.AddMousePosEvent(static_cast<float>(event.motion.x),
                            static_cast<float>(event.motion.y));
        break;
    case SDL_MOUSEWHEEL:
        io.AddMouseWheelEvent(static_cast<float>(event.wheel.x),
                              static_cast<float>(event.wheel.y));
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int button = -1;
        if (event.button.button == SDL_BUTTON_LEFT)
            button = 0;
        else if (event.button.button == SDL_BUTTON_RIGHT)
            button = 1;
        else if (event.button.button == SDL_BUTTON_MIDDLE)
            button = 2;
        else if (event.button.button == SDL_BUTTON_X1)
            button = 3;
        else if (event.button.button == SDL_BUTTON_X2)
            button = 4;
        if (button >= 0)
            io.AddMouseButtonEvent(button, event.type == SDL_MOUSEBUTTONDOWN);
        break;
    }
    case SDL_TEXTINPUT:
        io.AddInputCharactersUTF8(event.text.text);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const bool down = event.type == SDL_KEYDOWN;
        UpdateModifiers(static_cast<SDL_Keymod>(event.key.keysym.mod));
        const ImGuiKey key = SdlKeyToImGuiKey(event.key.keysym.sym);
        if (key != ImGuiKey_None)
            io.AddKeyEvent(key, down);
        break;
    }
    case SDL_WINDOWEVENT:
        if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            io.AddFocusEvent(true);
        else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            io.AddFocusEvent(false);
        break;
    default:
        break;
    }
}

void ConfigureStyle()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.DisabledAlpha = 0.6f;
    style.WindowPadding = {0.0f, 0.0f};
    style.WindowRounding = 5.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowMinSize = {32.0f, 32.0f};
    style.WindowTitleAlign = {0.0f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_Left;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupRounding = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = {4.0f, 3.0f};
    style.FrameRounding = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = {8.0f, 4.0f};
    style.ItemInnerSpacing = {4.0f, 4.0f};
    style.CellPadding = {4.0f, 2.0f};
    style.TouchExtraPadding = {0.0f, 0.0f};
    style.IndentSpacing = 21.0f;
    style.ColumnsMinSpacing = 6.0f;
    style.ScrollbarSize = 7.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabMinSize = 3.0f;
    style.GrabRounding = 0.0f;
    style.LogSliderDeadzone = 4.0f;
    style.TabRounding = 4.0f;
    style.TabBorderSize = 0.0f;
    style.TabMinWidthForCloseButton = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TableAngledHeadersAngle = FloatFromBits(0x3F1C61AA);
    style.ColorButtonPosition = ImGuiDir_Right;
    style.ButtonTextAlign = {0.5f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.0f};
    style.SeparatorTextBorderSize = 3.0f;
    style.SeparatorTextAlign = {0.0f, 0.5f};
    style.SeparatorTextPadding = {20.0f, 3.0f};
    style.DisplayWindowPadding = {19.0f, 19.0f};
    style.DisplaySafeAreaPadding = {3.0f, 3.0f};
    style.MouseCursorScale = 1.0f;
    style.AntiAliasedLines = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill = true;
    style.CurveTessellationTol = 1.25f;
    style.CircleTessellationMaxError = 0.30f;
    for (int index = 0; index < ImGuiCol_COUNT; ++index) {
        style.Colors[index] = {
            FloatFromBits(kStyleColorBits[index][0]),
            FloatFromBits(kStyleColorBits[index][1]),
            FloatFromBits(kStyleColorBits[index][2]),
            FloatFromBits(kStyleColorBits[index][3]),
        };
    }
}

bool CreateFontTexture(std::string& error)
{
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        error = "ImGui font atlas is empty";
        return false;
    }

    glGenTextures(1, &g_font_texture);
    glBindTexture(GL_TEXTURE_2D, g_font_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    io.Fonts->SetTexID(
        reinterpret_cast<ImTextureID>(static_cast<intptr_t>(g_font_texture)));
    return true;
}

void DestroyFontTexture()
{
    if (g_font_texture == 0)
        return;
    ImGui::GetIO().Fonts->SetTexID(nullptr);
    glDeleteTextures(1, &g_font_texture);
    g_font_texture = 0;
}

bool LoadTexture(const fs::path& path, GLuint& texture, std::string& error)
{
    SDL_Surface* loaded = IMG_Load(path.c_str());
    if (loaded == nullptr) {
        error = "unable to load " + path.string() + ": " + IMG_GetError();
        return false;
    }
    SDL_Surface* surface =
        SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(loaded);
    if (surface == nullptr) {
        error = "unable to convert image: " + std::string(SDL_GetError());
        return false;
    }

    std::vector<unsigned char> packed;
    const void* pixels = surface->pixels;
    const int packed_pitch = surface->w * 4;
    if (surface->pitch != packed_pitch) {
        packed.resize(static_cast<std::size_t>(packed_pitch) *
                      static_cast<std::size_t>(surface->h));
        const auto* source = static_cast<const unsigned char*>(surface->pixels);
        for (int y = 0; y < surface->h; ++y) {
            std::copy_n(source + y * surface->pitch, packed_pitch,
                        packed.data() + y * packed_pitch);
        }
        pixels = packed.data();
    }

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surface->w, surface->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    SDL_FreeSurface(surface);
    return true;
}

void SetupRenderState(ImDrawData* draw_data, int framebuffer_width,
                      int framebuffer_height)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_TEXTURE_2D);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glViewport(0, 0, framebuffer_width, framebuffer_height);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    const float left = draw_data->DisplayPos.x;
    const float right = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
    const float top = draw_data->DisplayPos.y;
    const float bottom = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
    glOrtho(left, right, bottom, top, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
}

void RenderDrawData(ImDrawData* draw_data)
{
    const int framebuffer_width = static_cast<int>(
        draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    const int framebuffer_height = static_cast<int>(
        draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    if (framebuffer_width <= 0 || framebuffer_height <= 0)
        return;

    GLint last_matrix_mode = GL_MODELVIEW;
    glGetIntegerv(GL_MATRIX_MODE, &last_matrix_mode);
    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT |
                 GL_VIEWPORT_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT |
                 GL_POLYGON_BIT);
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();

    SetupRenderState(draw_data, framebuffer_width, framebuffer_height);

    const ImVec2 clip_offset = draw_data->DisplayPos;
    const ImVec2 clip_scale = draw_data->FramebufferScale;
    for (int list_index = 0; list_index < draw_data->CmdListsCount;
         ++list_index) {
        const ImDrawList* command_list = draw_data->CmdLists[list_index];
        const ImDrawVert* vertices = command_list->VtxBuffer.Data;
        const ImDrawIdx* indices = command_list->IdxBuffer.Data;
        glVertexPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                        reinterpret_cast<const char*>(vertices) +
                            offsetof(ImDrawVert, pos));
        glTexCoordPointer(2, GL_FLOAT, sizeof(ImDrawVert),
                          reinterpret_cast<const char*>(vertices) +
                              offsetof(ImDrawVert, uv));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ImDrawVert),
                       reinterpret_cast<const char*>(vertices) +
                           offsetof(ImDrawVert, col));

        for (const ImDrawCmd& command : command_list->CmdBuffer) {
            if (command.UserCallback != nullptr) {
                if (command.UserCallback == ImDrawCallback_ResetRenderState)
                    SetupRenderState(draw_data, framebuffer_width,
                                     framebuffer_height);
                else
                    command.UserCallback(command_list, &command);
                continue;
            }

            ImVec2 clip_min(
                (command.ClipRect.x - clip_offset.x) * clip_scale.x,
                (command.ClipRect.y - clip_offset.y) * clip_scale.y);
            ImVec2 clip_max(
                (command.ClipRect.z - clip_offset.x) * clip_scale.x,
                (command.ClipRect.w - clip_offset.y) * clip_scale.y);
            clip_min.x = (std::max)(clip_min.x, 0.0f);
            clip_min.y = (std::max)(clip_min.y, 0.0f);
            clip_max.x = (std::min)(clip_max.x,
                                    static_cast<float>(framebuffer_width));
            clip_max.y = (std::min)(clip_max.y,
                                    static_cast<float>(framebuffer_height));
            if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                continue;

            glScissor(static_cast<int>(clip_min.x),
                      static_cast<int>(
                          static_cast<float>(framebuffer_height) - clip_max.y),
                      static_cast<int>(clip_max.x - clip_min.x),
                      static_cast<int>(clip_max.y - clip_min.y));
            const GLuint texture = static_cast<GLuint>(reinterpret_cast<intptr_t>(
                command.GetTexID()));
            glBindTexture(GL_TEXTURE_2D, texture);
            const GLenum index_type = sizeof(ImDrawIdx) == 2
                                          ? GL_UNSIGNED_SHORT
                                          : GL_UNSIGNED_INT;
            glDrawElements(
                GL_TRIANGLES, static_cast<GLsizei>(command.ElemCount), index_type,
                indices + command.IdxOffset);
        }
    }

    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopClientAttrib();
    glPopAttrib();
    glMatrixMode(static_cast<GLenum>(last_matrix_mode));
}

void ApplyWindowSizeAroundCenter(SDL_Window* window, int width, int height)
{
    int current_width = 0;
    int current_height = 0;
    SDL_GetWindowSize(window, &current_width, &current_height);
    if (current_width == width && current_height == height)
        return;

    int x = 0;
    int y = 0;
    SDL_GetWindowPosition(window, &x, &y);
    const int center_x = x + current_width / 2;
    const int center_y = y + current_height / 2;
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, center_x - width / 2,
                          center_y - height / 2);
}

void PrintUsage(const char* program)
{
    std::cout << "Usage: " << program << " [--component PATH] [--smoke-test]\n";
}

} // namespace

int main(int argc, char** argv)
{
    bool smoke_test = false;
    fs::path component_override;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--smoke-test") {
            smoke_test = true;
        } else if (argument == "--component") {
            if (index + 1 >= argc) {
                std::cerr << "--component requires a path\n";
                return 2;
            }
            component_override = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option: " << argument << '\n';
            PrintUsage(argv[0]);
            return 2;
        }
    }

    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
        return 3;
    }
    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
        std::cerr << "SDL_image PNG support is unavailable: "
                  << IMG_GetError() << '\n';
        SDL_Quit();
        return 4;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

    Uint32 window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_BORDERLESS |
                          SDL_WINDOW_ALLOW_HIGHDPI;
    window_flags |= smoke_test ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN;
    SDL_Window* window = SDL_CreateWindow(
        "kirkware", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        360, 270, window_flags);
    if (window == nullptr) {
        std::cerr << "window creation failed: " << SDL_GetError() << '\n';
        IMG_Quit();
        SDL_Quit();
        return 5;
    }

    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == nullptr || SDL_GL_MakeCurrent(window, context) != 0) {
        std::cerr << "OpenGL context creation failed: " << SDL_GetError() << '\n';
        if (context != nullptr)
            SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 6;
    }
    SDL_GL_SetSwapInterval(smoke_test ? 0 : 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "kirkware_sdl2";
    io.BackendRendererName = "kirkware_opengl2";
    io.GetClipboardTextFn = GetClipboardText;
    io.SetClipboardTextFn = SetClipboardText;
    io.ClipboardUserData = nullptr;

    const fs::path regular_font = ResolveFont(false);
    const fs::path bold_font = ResolveFont(true);
    ImFontConfig font_config;
    font_config.FontBuilderFlags |= 0x90;
    const ImWchar* glyph_ranges = io.Fonts->GetGlyphRangesCyrillic();
    ImFont* font13 = nullptr;
    ImFont* font14 = nullptr;
    ImFont* font12_bold = nullptr;
    if (!regular_font.empty()) {
        font13 = io.Fonts->AddFontFromFileTTF(
            regular_font.c_str(), 13.0f, &font_config, glyph_ranges);
        font14 = io.Fonts->AddFontFromFileTTF(
            regular_font.c_str(), 14.0f, &font_config, glyph_ranges);
    }
    if (!bold_font.empty()) {
        font12_bold = io.Fonts->AddFontFromFileTTF(
            bold_font.c_str(), 12.0f, &font_config, glyph_ranges);
    }
    if (font13 == nullptr)
        font13 = io.Fonts->AddFontDefault();
    if (font14 == nullptr)
        font14 = font13;
    if (font12_bold == nullptr)
        font12_bold = font13;

    ConfigureStyle();

    std::string renderer_error;
    if (!CreateFontTexture(renderer_error)) {
        std::cerr << renderer_error << '\n';
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 7;
    }

    GLuint garrys_mod_texture = 0;
    const fs::path image_path = ResolveAssetPath("garrys-mod.png");
    if (!image_path.empty()) {
        std::string texture_error;
        if (!LoadTexture(image_path, garrys_mod_texture, texture_error))
            std::cerr << "warning: " << texture_error << '\n';
    }

    const fs::path component_path = ResolveComponentPath(component_override);
    const ImTextureID game_texture = garrys_mod_texture != 0
        ? reinterpret_cast<ImTextureID>(
              static_cast<intptr_t>(garrys_mod_texture))
        : nullptr;
    kirkware::linux_ui::LinuxKirkwareUi ui(
        font13, font14, font12_bold, game_texture, component_path);

    SDL_StartTextInput();
    bool done = false;
    bool dragging = false;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
    auto previous = std::chrono::steady_clock::now();
    unsigned smoke_frames = 0;

    while (!done) {
        SDL_Event event{};
        while (SDL_PollEvent(&event) != 0) {
            ProcessEvent(event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT &&
                event.button.y <= 20) {
                int global_x = 0;
                int global_y = 0;
                SDL_GetGlobalMouseState(&global_x, &global_y);
                int window_x = 0;
                int window_y = 0;
                SDL_GetWindowPosition(window, &window_x, &window_y);
                drag_offset_x = global_x - window_x;
                drag_offset_y = global_y - window_y;
                dragging = true;
            } else if (event.type == SDL_MOUSEBUTTONUP &&
                       event.button.button == SDL_BUTTON_LEFT) {
                dragging = false;
            } else if (event.type == SDL_MOUSEMOTION && dragging) {
                int global_x = 0;
                int global_y = 0;
                SDL_GetGlobalMouseState(&global_x, &global_y);
                SDL_SetWindowPosition(window,
                                      global_x - drag_offset_x,
                                      global_y - drag_offset_y);
            }
        }
        if (done)
            break;

        int window_width = 0;
        int window_height = 0;
        int drawable_width = 0;
        int drawable_height = 0;
        SDL_GetWindowSize(window, &window_width, &window_height);
        SDL_GL_GetDrawableSize(window, &drawable_width, &drawable_height);
        io.DisplaySize = {static_cast<float>(window_width),
                          static_cast<float>(window_height)};
        if (window_width > 0 && window_height > 0) {
            io.DisplayFramebufferScale = {
                static_cast<float>(drawable_width) /
                    static_cast<float>(window_width),
                static_cast<float>(drawable_height) /
                    static_cast<float>(window_height),
            };
        }

        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - previous).count();
        previous = now;
        io.DeltaTime = (std::max)(delta, 1.0f / 1000.0f);

        ImGui::NewFrame();
        ui.Update(delta);
        ui.Render();
        ImGui::Render();

        ApplyWindowSizeAroundCenter(window, ui.ClientWidth(), ui.ClientHeight());

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        if (ui.CloseRequested())
            done = true;
        if (smoke_test && ++smoke_frames >= 12)
            done = true;
    }

    SDL_StopTextInput();
    if (garrys_mod_texture != 0)
        glDeleteTextures(1, &garrys_mod_texture);
    DestroyFontTexture();
    if (g_clipboard_text != nullptr) {
        SDL_free(g_clipboard_text);
        g_clipboard_text = nullptr;
    }
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
