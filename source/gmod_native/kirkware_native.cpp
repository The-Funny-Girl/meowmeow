#include <GarrysMod/Lua/Interface.h>

using namespace GarrysMod::Lua;

namespace {
constexpr const char *kVersion = "kirkware-native-linux64-v1";

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
    LUA->PushNumber(1);
    return 1;
}

LUA_FUNCTION_STATIC(KirkwareNativeLoaded)
{
    LUA->PushBool(true);
    return 1;
}
} // namespace

GMOD_MODULE_OPEN()
{
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

    LUA->SetField(-2, "kirkware_native");
    LUA->Pop(1);
    return 0;
}

GMOD_MODULE_CLOSE()
{
    return 0;
}
