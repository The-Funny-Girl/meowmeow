#include "d3d9_texture.h"
#include "resource_ids.h"
#include "kirkware_ui.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <d3d9.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace
{

IDirect3D9* g_direct3d = nullptr;
IDirect3DDevice9* g_device = nullptr;
D3DPRESENT_PARAMETERS g_present{};
bool g_resize_active = false;
float g_resize_center_x = 0.0f;
float g_resize_center_y = 0.0f;
int g_drag_x = 0;
int g_drag_y = 0;

static_assert(sizeof(ImGuiStyle) == 0x458);
static_assert(offsetof(ImGuiStyle, Colors) == 0xE4);
static_assert(ImGuiCol_COUNT == 54);

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
    {0x3F4CCCCD, 0x3F4CCCCD, 0x3F4CCCCD, 0x3EB33333}
};

constexpr float FloatFromBits(std::uint32_t bits)
{
    return std::bit_cast<float>(bits);
}

std::uint32_t g_legacy_rand_state = 1;

int LegacyRand()
{
    g_legacy_rand_state =
        g_legacy_rand_state * 0x343fdu + 0x269ec3u;
    return static_cast<int>((g_legacy_rand_state >> 16u) & 0x7fffu);
}

std::string RandomWindowName()
{
    constexpr char alphabet[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string value;
    value.reserve(12);
    for (int index = 0; index < 12; ++index)
        value.push_back(alphabet[LegacyRand() % 62]);
    return value;
}

bool CreateDevice(HWND window)
{
    g_direct3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (g_direct3d == nullptr)
        return false;
    ZeroMemory(&g_present, sizeof(g_present));
    g_present.Windowed = TRUE;
    g_present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_present.BackBufferFormat = D3DFMT_UNKNOWN;
    g_present.EnableAutoDepthStencil = TRUE;
    g_present.AutoDepthStencilFormat = D3DFMT_D16;
    g_present.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    constexpr DWORD vertex_processing[] = {
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        D3DCREATE_MIXED_VERTEXPROCESSING,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING
    };
    for (int cycle = 0; cycle < 3; ++cycle)
    {
        for (DWORD behavior : vertex_processing)
        {
            if (SUCCEEDED(g_direct3d->CreateDevice(
                    D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                    behavior, &g_present, &g_device)))
                return true;
        }
        Sleep(150);
    }
    return false;
}

void CleanupDevice()
{
    if (g_device != nullptr)
    {
        g_device->Release();
        g_device = nullptr;
    }
    if (g_direct3d != nullptr)
    {
        g_direct3d->Release();
        g_direct3d = nullptr;
    }
}

bool ResetDevice()
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    const HRESULT result = g_device->Reset(&g_present);
    if (FAILED(result))
        return false;
    return ImGui_ImplDX9_CreateDeviceObjects();
}

void ApplyRoundedRegion(HWND window,
                        int width,
                        int height,
                        int rounding)
{
    HRGN region = CreateRoundRectRgn(
        0, 0, width, height, rounding, rounding);
    if (region == nullptr)
        return;
    if (!SetWindowRgn(window, region, TRUE))
        DeleteObject(region);
}

void ResizeAroundCenter(HWND window,
                        float current_width,
                        float current_height,
                        float target_width,
                        float target_height,
                        int rounding)
{
    RECT outer{};
    GetWindowRect(window, &outer);
    const float width_distance = std::abs(target_width - current_width);
    const float height_distance = std::abs(target_height - current_height);
    if ((width_distance > 0.1f || height_distance > 0.1f) &&
        !g_resize_active)
    {
        g_resize_active = true;
        g_resize_center_x = static_cast<float>(outer.left) +
            static_cast<float>(outer.right - outer.left) * 0.5f;
        g_resize_center_y = static_cast<float>(outer.top) +
            static_cast<float>(outer.bottom - outer.top) * 0.5f;
    }
    if (width_distance < 0.001f && height_distance < 0.001f)
        g_resize_active = false;
    float left = static_cast<float>(outer.left);
    float top = static_cast<float>(outer.top);
    if (g_resize_active)
    {
        left = g_resize_center_x - current_width * 0.5f;
        top = g_resize_center_y - current_height * 0.5f;
    }
    const int width = static_cast<int>(current_width);
    const int height = static_cast<int>(current_height);
    SetWindowPos(
        window, nullptr, static_cast<int>(left), static_cast<int>(top),
        width, height, SWP_NOZORDER);
    ApplyRoundedRegion(window, width, height, rounding);
}

}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam);

LRESULT WINAPI WndProc(HWND window,
                       UINT message,
                       WPARAM wparam,
                       LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam))
        return TRUE;
    switch (message)
    {
    case WM_SIZE:
        if (g_device != nullptr && wparam != SIZE_MINIMIZED)
        {
            g_present.BackBufferWidth = static_cast<UINT>(LOWORD(lparam));
            g_present.BackBufferHeight = static_cast<UINT>(HIWORD(lparam));
            ResetDevice();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0u) == SC_KEYMENU)
            return 0;
        break;
    case WM_MOUSEMOVE:
    {
        if (wparam == MK_LBUTTON)
        {
            RECT outer{};
            GetWindowRect(window, &outer);
            const int width = outer.right - outer.left;
            if (g_drag_x >= 0 && g_drag_x <= width &&
                g_drag_y >= 0 && g_drag_y <= 20)
            {
                const int x = static_cast<short>(LOWORD(lparam));
                const int y = static_cast<short>(HIWORD(lparam));
                SetWindowPos(
                    window, nullptr,
                    outer.left + x - g_drag_x,
                    outer.top + y - g_drag_y,
                    0, 0, 0x45u);
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        g_drag_x = static_cast<short>(LOWORD(lparam));
        g_drag_y = static_cast<short>(HIWORD(lparam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE,
                    HINSTANCE,
                    wchar_t*,
                    int)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
    constexpr int initial_width = 360;
    constexpr int initial_height = 270;
    HINSTANCE module_instance = GetModuleHandleA(nullptr);
    const std::string title = RandomWindowName();
    const std::string class_name = RandomWindowName();
    const HICON large_icon = static_cast<HICON>(LoadImageA(
        module_instance, MAKEINTRESOURCEA(kKirkwareWindowIcon),
        IMAGE_ICON, GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    const HICON small_icon = static_cast<HICON>(LoadImageA(
        module_instance, MAKEINTRESOURCEA(kKirkwareWindowIcon),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    const WNDCLASSEXA window_class = {
        sizeof(WNDCLASSEXA),
        CS_CLASSDC,
        WndProc,
        0,
        0,
        module_instance,
        large_icon,
        nullptr,
        nullptr,
        nullptr,
        class_name.c_str(),
        small_icon
    };
    if (!RegisterClassExA(&window_class))
        return 1;

    const int left =
        (GetSystemMetrics(SM_CXSCREEN) - initial_width) / 2;
    const int top =
        (GetSystemMetrics(SM_CYSCREEN) - initial_height) / 2;
    HWND window = CreateWindowExA(
        0,
        window_class.lpszClassName,
        title.c_str(),
        WS_POPUP,
        left, top, initial_width, initial_height,
        nullptr, nullptr, module_instance, nullptr);
    if (window == nullptr || !CreateDevice(window))
    {
        CleanupDevice();
        if (window != nullptr)
            DestroyWindow(window);
        UnregisterClassA(window_class.lpszClassName, module_instance);
        return 2;
    }
    if (large_icon != nullptr)
        SendMessageA(window, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(large_icon));
    if (small_icon != nullptr)
        SendMessageA(window, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(small_icon));
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE)
    {
        MessageBoxW(window, L"Windows Imaging Component is unavailable",
                    L"kirkware", MB_ICONERROR | MB_OK);
        CleanupDevice();
        DestroyWindow(window);
        UnregisterClassA(window_class.lpszClassName, module_instance);
        return 3;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    font_config.FontBuilderFlags |= 0x90;
    const ImWchar* glyph_ranges = io.Fonts->GetGlyphRangesCyrillic();
    ImFont* font13 = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\tahoma.ttf", 13.0f,
        &font_config, glyph_ranges);
    ImFont* font14 = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\tahoma.ttf", 14.0f,
        &font_config, glyph_ranges);
    ImFont* font12_bold = io.Fonts->AddFontFromFileTTF(
        "C:\\Windows\\Fonts\\tahomabd.ttf", 12.0f,
        &font_config, glyph_ranges);
    if (font13 == nullptr || font14 == nullptr || font12_bold == nullptr)
    {
        MessageBoxW(window, L"Tahoma could not be loaded",
                    L"kirkware", MB_ICONERROR | MB_OK);
        ImGui::DestroyContext();
        if (uninitialize_com)
            CoUninitialize();
        CleanupDevice();
        DestroyWindow(window);
        UnregisterClassA(window_class.lpszClassName, module_instance);
        return 4;
    }
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
    for (int index = 0; index < ImGuiCol_COUNT; ++index)
    {
        style.Colors[index] = {
            FloatFromBits(kStyleColorBits[index][0]),
            FloatFromBits(kStyleColorBits[index][1]),
            FloatFromBits(kStyleColorBits[index][2]),
            FloatFromBits(kStyleColorBits[index][3])};
    }

    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX9_Init(g_device))
    {
        MessageBoxW(window, L"The Direct3D 9 interface could not start",
                    L"kirkware", MB_ICONERROR | MB_OK);
        ImGui::DestroyContext();
        if (uninitialize_com)
            CoUninitialize();
        CleanupDevice();
        DestroyWindow(window);
        UnregisterClassA(window_class.lpszClassName, module_instance);
        return 5;
    }

    std::wstring texture_error;
    kirkware::D3d9Texture garrys_mod;
    if (!garrys_mod.Load(g_device, module_instance, kKirkwareGarrysModPng,
                         false, texture_error))
    {
        MessageBoxW(window, texture_error.c_str(),
                    L"kirkware", MB_ICONERROR | MB_OK);
        garrys_mod.Reset();
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        if (uninitialize_com)
            CoUninitialize();
        CleanupDevice();
        DestroyWindow(window);
        UnregisterClassA(window_class.lpszClassName, module_instance);
        return 6;
    }

    kirkware::KirkwareUi ui(
        font13, font14, font12_bold, &garrys_mod);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    auto previous = std::chrono::steady_clock::now();
    bool done = false;
    int exit_code = 0;
    while (!done)
    {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        const auto now = std::chrono::steady_clock::now();
        const float delta = std::chrono::duration<float>(now - previous).count();
        previous = now;
        ui.Update(delta);
        if (ui.CloseRequested())
        {
            PostMessageW(window, WM_CLOSE, 0, 0);
            continue;
        }
        ResizeAroundCenter(
            window, ui.CurrentWidth(), ui.CurrentHeight(),
            ui.TargetWidth(), ui.TargetHeight(),
            static_cast<int>(ImGui::GetStyle().WindowRounding));

        const HRESULT cooperative = g_device->TestCooperativeLevel();
        if (cooperative == D3DERR_DEVICELOST)
        {
            Sleep(10);
            continue;
        }
        if (cooperative == D3DERR_DEVICENOTRESET && !ResetDevice())
        {
            exit_code = 7;
            break;
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ui.Render();

        g_device->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        g_device->Clear(
            0, nullptr, D3DCLEAR_TARGET,
            D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
        if (SUCCEEDED(g_device->BeginScene()))
        {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_device->EndScene();
        }
        const HRESULT presented =
            g_device->Present(nullptr, nullptr, nullptr, nullptr);
        if (presented != D3DERR_DEVICELOST && FAILED(presented))
        {
            exit_code = 8;
            break;
        }
    }

    garrys_mod.Reset();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (uninitialize_com)
        CoUninitialize();
    CleanupDevice();
    if (IsWindow(window))
        DestroyWindow(window);
    UnregisterClassA(window_class.lpszClassName, module_instance);
    return exit_code;
}
