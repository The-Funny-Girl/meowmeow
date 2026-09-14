#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static const char kConfig[] =
    "engine_dll = engine.dll\r\n"
    "engine_iface = VEngineClient015\r\n"
    "lua_shared_dll = lua_shared.dll\r\n"
    "lua_shared_iface = LUASHARED003\r\n"
    "\r\n"
    "materialsystem_dll = materialsystem.dll\r\n"
    "materialsystem_iface = VMaterialSystem080\r\n"
    "\r\n"
    "engine_model_dll = engine.dll\r\n"
    "engine_model_iface = VEngineModel016\r\n"
    "\r\n"
    "modelinfo_dll = engine.dll\r\n"
    "modelinfo_iface = VModelInfoClient006\r\n"
    "\r\n"
    "engine_trace_dll = engine.dll\r\n"
    "engine_trace_iface = EngineTraceClient003\r\n"
    "\r\n"
    "client_entitylist_dll = client.dll\r\n"
    "client_entitylist_iface = VClientEntityList003\r\n"
    "\r\n"
    "client_dll = client.dll\r\n"
    "client_iface = VClient017\r\n"
    "\r\n"
    "gamemovement_dll = client.dll\r\n"
    "gamemovement_iface = GameMovement001\r\n"
    "\r\n"
    "prediction_dll = client.dll\r\n"
    "prediction_iface = VClientPrediction001\r\n"
    "\r\n"
    "inputsystem_dll = inputsystem.dll\r\n"
    "inputsystem_iface = InputSystemVersion001\r\n"
    "\r\n"
    "cvar_dll = vstdlib.dll\r\n"
    "cvar_iface = VEngineCvar007\r\n"
    "\r\n"
    "vguimatsurface_dll = vguimatsurface.dll\r\n"
    "vguimatsurface_iface = VGUI_Surface030\r\n"
    "\r\n"
    "gameeventmanager_dll = engine.dll\r\n"
    "gameeventmanager_iface = GAMEEVENTSMANAGER002\r\n"
    "\r\n"
    "engine_vgui_dll = engine.dll\r\n"
    "engine_vgui_iface = VEngineVGui001\r\n"
    "\r\n"
    "renderview_dll = engine.dll\r\n"
    "renderview_iface = VEngineRenderView014\r\n"
    "\r\n"
    "model_render_dll = engine.dll\r\n"
    "model_render_iface = VEngineModel016\r\n"
    "\r\n"
    "studiorender_dll = studiorender.dll\r\n"
    "studiorender_iface = VStudioRender025\r\n"
    "\r\n"
    "vgui2_panel_dll = vgui2.dll\r\n"
    "vgui2_panel_iface = VGUI_Panel009";

template <typename T>
static void put(std::vector<std::uint8_t>& out, std::size_t offset, T value) {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}

static std::string base64url(const std::string& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string output;
    output.reserve((input.size() * 4 + 2) / 3);
    std::size_t index = 0;
    while (index + 3 <= input.size()) {
        const auto value =
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(input[index])) << 16) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(input[index + 1])) << 8) |
            static_cast<std::uint32_t>(
                static_cast<unsigned char>(input[index + 2]));
        output.push_back(alphabet[(value >> 18) & 63]);
        output.push_back(alphabet[(value >> 12) & 63]);
        output.push_back(alphabet[(value >> 6) & 63]);
        output.push_back(alphabet[value & 63]);
        index += 3;
    }
    const auto remaining = input.size() - index;
    if (remaining == 1) {
        const auto value = static_cast<std::uint32_t>(
                               static_cast<unsigned char>(input[index]))
                           << 16;
        output.push_back(alphabet[(value >> 18) & 63]);
        output.push_back(alphabet[(value >> 12) & 63]);
    } else if (remaining == 2) {
        const auto value =
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(input[index])) << 16) |
            (static_cast<std::uint32_t>(
                 static_cast<unsigned char>(input[index + 1])) << 8);
        output.push_back(alphabet[(value >> 18) & 63]);
        output.push_back(alphabet[(value >> 12) & 63]);
        output.push_back(alphabet[(value >> 6) & 63]);
    }
    return output;
}

static std::string json_string(const std::string& input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(input.size());
    for (const unsigned char character : input) {
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(static_cast<char>(character));
        } else if (character < 0x20) {
            output += "\\u00";
            output.push_back(hex[character >> 4]);
            output.push_back(hex[character & 15]);
        } else {
            output.push_back(static_cast<char>(character));
        }
    }
    return output;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: build_kirkware_offline_handoff.exe output.bin username\n";
        return 2;
    }
    const std::string username = argv[2];
    if (username.empty() || username.size() > 20) {
        std::cerr << "username must contain 1 to 20 bytes\n";
        return 3;
    }
    if (std::strlen(kConfig) != 0x554) {
        std::cerr << "internal config length mismatch: " << std::strlen(kConfig) << "\n";
        return 4;
    }
    std::vector<std::uint8_t> out(0x30240, 0);
    std::memcpy(out.data() + 4, username.data(), username.size());
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    const std::string header = base64url("{\"alg\":\"HS512\"}");
    const std::string payload = base64url(
        "{\"sub\":\"" + json_string(username) + "\",\"exp\":" +
        std::to_string(static_cast<std::uint64_t>(now) + 604800) + "}");
    const std::string signature(86, 'A');
    const std::string token = header + "." + payload + "." + signature;
    if (token.size() >= 0x21c - 0x1a) {
        std::cerr << "internal token length mismatch\n";
        return 4;
    }
    std::memcpy(out.data() + 0x1a, token.data(), token.size());
    put<std::uint16_t>(out, 0x21c, 0x2c2c);
    constexpr std::uint16_t nonce = 0xD537;
    put<std::uint16_t>(out, 0x220, nonce);
    put<std::uint32_t>(out, 0x228, now);
    put<std::uint32_t>(out, 0x234, 0x554);
    std::memcpy(out.data() + 0x1023c, kConfig, 0x554);
    std::ofstream file(argv[1], std::ios::binary | std::ios::trunc);
    if (!file) return 5;
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return file ? 0 : 6;
}
