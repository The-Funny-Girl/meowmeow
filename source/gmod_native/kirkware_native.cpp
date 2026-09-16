#include <GarrysMod/Lua/Interface.h>

#include "kirkware_embedded_lua.hpp"

#include <cstring>
#include <string>

using namespace GarrysMod::Lua;

namespace {
constexpr const char *kVersion = "kirkware-native-linux64-v2";
constexpr const char *kGlobalName = "kirkware_native";
bool gEmbeddedLoaded = false;

const KirkwareEmbedded::LuaPayload *FindPayload(const char *name)
{
    if (name == nullptr) {
        return nullptr;
    }

    for (std::size_t i = 0; i < KirkwareEmbedded::kPayloadCount; ++i) {
        const auto &payload = KirkwareEmbedded::kPayloads[i];
        if (std::strcmp(payload.name, name) == 0) {
            return &payload;
        }
    }
    return nullptr;
}

LUA_FUNCTION_STATIC(KirkwareNativeVersion)
{
    LUA->PushString(kVersion);
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativePlatform)
{
    LUA->PushString("linux64");
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativeAbi)
{
    LUA->PushNumber(2);
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativeLoaded)
{
    LUA->PushBool(true);
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativePayloadCount)
{
    LUA->PushNumber(static_cast<double>(KirkwareEmbedded::kPayloadCount));
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativePayloadNames)
{
    LUA->CreateTable();
    for (std::size_t i = 0; i < KirkwareEmbedded::kPayloadCount; ++i) {
        LUA->PushNumber(static_cast<double>(i + 1));
        LUA->PushString(KirkwareEmbedded::kPayloads[i].name);
        LUA->SetTable(-3);
    }
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativePayload)
{
    const char *name = LUA->CheckString(1);
    const auto *payload = FindPayload(name);
    if (payload == nullptr) {
        LUA->PushNil();
        return 1;
    }

    LUA->PushString(
        reinterpret_cast<const char *>(payload->data),
        static_cast<unsigned int>(payload->size));
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativeEmbeddedLoaded)
{
    LUA->PushBool(gEmbeddedLoaded);
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativeLoadEmbedded)
{
    if (gEmbeddedLoaded) {
        LUA->PushBool(true);
        LUA->PushNumber(0);
        return 2;
    }

    for (std::size_t i = 0; i < KirkwareEmbedded::kPayloadCount; ++i) {
        const auto &payload = KirkwareEmbedded::kPayloads[i];
        const int stackBase = LUA->Top();

        LUA->GetField(INDEX_GLOBAL, "RunString");
        if (!LUA->IsType(-1, Type::Function)) {
            LUA->Pop(1);
            LUA->PushBool(false);
            LUA->PushString("GMod global RunString is unavailable");
            return 2;
        }

        LUA->PushString(
            reinterpret_cast<const char *>(payload.data),
            static_cast<unsigned int>(payload.size));
        const std::string identifier =
            std::string("kirkware_embedded/") + payload.name;
        LUA->PushString(identifier.c_str());
        LUA->PushBool(true);

        const int result = LUA->PCall(3, 1, 0);
        if (result != 0) {
            const char *message = LUA->GetString(-1);
            const std::string error = message != nullptr
                ? message
                : std::string("failed to run embedded payload: ") + payload.name;
            const int stackNow = LUA->Top();
            if (stackNow > stackBase) {
                LUA->Pop(stackNow - stackBase);
            }
            LUA->PushBool(false);
            LUA->PushString(error.c_str());
            return 2;
        }

        const int stackNow = LUA->Top();
        if (stackNow > stackBase) {
            LUA->Pop(stackNow - stackBase);
        }
    }

    gEmbeddedLoaded = true;
    LUA->PushBool(true);
    LUA->PushNumber(static_cast<double>(KirkwareEmbedded::kPayloadCount));
    return 2;
}
} // namespace

GMOD_MODULE_OPEN()
{
    gEmbeddedLoaded = false;

    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->CreateTable();

    LUA->PushCFunction(KirkwareNativeVersion);
    LUA->SetField(-2, "version");

    LUA->PushCFunction(KirkwareNativePlatform);
    LUA->SetField(-2, "platform");

    LUA->PushCFunction(KirkwareNativeAbi);
    LUA->SetField(-2, "abi");

    LUA->PushCFunction(KirkwareNativeLoaded);
    LUA->SetField(-2, "loaded");

    LUA->PushCFunction(KirkwareNativePayloadCount);
    LUA->SetField(-2, "payload_count");

    LUA->PushCFunction(KirkwareNativePayloadNames);
    LUA->SetField(-2, "payload_names");

    LUA->PushCFunction(KirkwareNativePayload);
    LUA->SetField(-2, "payload");

    LUA->PushCFunction(KirkwareNativeEmbeddedLoaded);
    LUA->SetField(-2, "embedded_loaded");

    LUA->PushCFunction(KirkwareNativeLoadEmbedded);
    LUA->SetField(-2, "load_embedded");

    LUA->SetField(-2, kGlobalName);
    LUA->Pop(1);
    return 0;
}

GMOD_MODULE_CLOSE()
{
    gEmbeddedLoaded = false;

    // Do not leave a Lua table containing C function pointers behind if the
    // module is unloaded. A stale table could otherwise point at unmapped
    // native code after gmod13_close returns.
    LUA->PushSpecial(SPECIAL_GLOB);
    LUA->PushNil();
    LUA->SetField(-2, kGlobalName);
    LUA->Pop(1);
    return 0;
}
