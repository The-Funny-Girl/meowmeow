// kirkware_lua_loader.cpp
//
// Injected / preloaded .so that runs ALL embedded Kirkware Lua features directly
// inside GMod's already-running client Lua state. No autorun, no require(), no
// dependence on the addon loading — which is what breaks on 64-bit / multiplayer.
// Every existing Lua feature (the full 76) keeps working; this just delivers them.
//
// Mechanism:
//   1. Hook SDL_GL_SwapWindow (a per-frame heartbeat on the game's main thread).
//   2. Each frame, until it succeeds once: find the client Lua interface via
//      ILuaShared and run every embedded payload through it (same RunString path
//      the native module already uses).
//
// Attach modes (build-time):
//   * LD_PRELOAD (default): exports SDL_GL_SwapWindow; loader interposes it.
//   * INJECT (-DKIRKWARE_INJECT_MODE): constructor GOT-hooks it via plthook.
//
// Reuses the generated embedded header from embed_lua.py, so build it in-tree
// with the gmod_native CMake (the header target is already there).

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/Lua/LuaInterface.h>
#include <GarrysMod/Lua/LuaShared.h>   // ILuaShared, GMOD_LUASHARED_INTERFACE, State::CLIENT

#include "kirkware_embedded_lua.hpp"

#include <dlfcn.h>
#include <link.h>        // dl_iterate_phdr
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>
#include <cstddef>

using namespace GarrysMod::Lua;

// Source engine module interface factory.
using CreateInterfaceFn = void *(*)(const char *name, int *returnCode);

// SDL_Window* is opaque to us; treat it as void* so we need not link SDL.
using SwapFn = void (*)(void *);
static SwapFn real_SDL_GL_SwapWindow = nullptr;
static std::atomic<bool> g_loaded{false};
// Index of the next payload still to run. Persisted across frames so a transient
// failure retries only the payload that failed, never re-running earlier ones
// (which would re-register hooks/commands every frame). Touched only from the
// game's main thread via SDL_GL_SwapWindow, so a plain size_t is sufficient.
static std::size_t g_nextPayload = 0;

// Identical to the helper in kirkware_native.cpp: run each embedded payload in
// order through GMod's global RunString. Leaves the Lua stack balanced.
static bool LoadEmbeddedPayloads(ILuaBase *LUA, std::string &outError)
{
    for (; g_nextPayload < KirkwareEmbedded::kPayloadCount; ++g_nextPayload) {
        const auto &payload = KirkwareEmbedded::kPayloads[g_nextPayload];
        const int stackBase = LUA->Top();

        LUA->GetField(INDEX_GLOBAL, "RunString");
        if (!LUA->IsType(-1, Type::Function)) {
            LUA->Pop(1);
            outError = "global RunString unavailable";
            return false;
        }

        LUA->PushString(reinterpret_cast<const char *>(payload.data),
                        static_cast<unsigned int>(payload.size));
        const std::string id = std::string("kirkware_embedded/") + payload.name;
        LUA->PushString(id.c_str());
        LUA->PushBool(true);

        if (LUA->PCall(3, 1, 0) != 0) {
            const char *msg = LUA->GetString(-1);
            outError = msg ? msg
                           : std::string("payload failed: ") + payload.name;
            const int now = LUA->Top();
            if (now > stackBase) LUA->Pop(now - stackBase);
            return false;
        }

        const int now = LUA->Top();
        if (now > stackBase) LUA->Pop(now - stackBase);
    }
    return true;
}

// Locate the already-mapped lua_shared module's full path by scanning the
// process's loaded objects. Robust to whatever soname it was loaded under.
namespace {
struct PhdrFind { const char *needle; char path[4096]; bool found; };
int PhdrScan(struct dl_phdr_info *info, size_t, void *data)
{
    auto *fd = static_cast<PhdrFind *>(data);
    if (info->dlpi_name && std::strstr(info->dlpi_name, fd->needle)) {
        std::strncpy(fd->path, info->dlpi_name, sizeof(fd->path) - 1);
        fd->found = true;
        return 1; // stop iterating
    }
    return 0;
}
} // namespace

static ILuaShared *ResolveLuaShared()
{
    void *mod = nullptr;

    // First try the usual sonames without loading a new copy.
    static const char *candidates[] = { "lua_shared_client.so", "lua_shared.so" };
    for (const char *name : candidates) {
        mod = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
        if (mod) break;
    }

    // Fallback: find whatever object has "lua_shared" in its path and reuse it.
    if (!mod) {
        PhdrFind fd{ "lua_shared", {0}, false };
        dl_iterate_phdr(PhdrScan, &fd);
        if (fd.found) mod = dlopen(fd.path, RTLD_NOW | RTLD_NOLOAD);
    }
    if (!mod) return nullptr;

    auto CreateInterface =
        reinterpret_cast<CreateInterfaceFn>(dlsym(mod, "CreateInterface"));
    if (!CreateInterface) return nullptr;

    return reinterpret_cast<ILuaShared *>(
        CreateInterface(GMOD_LUASHARED_INTERFACE, nullptr));
}

static ILuaBase *ResolveClientLua()
{
    ILuaShared *shared = ResolveLuaShared();
    if (!shared) return nullptr;
    // State::CLIENT selects the in-game client interface. It only exists once a
    // map/server is loaded, so this returns null at the main menu — we retry.
    ILuaBase *lua = shared->GetLuaInterface(
        static_cast<unsigned char>(State::CLIENT));
    if (!lua) return nullptr;
    // Guard against a registered-but-not-started state.
    if (lua->GetState() == nullptr) return nullptr;
    return lua;
}

static void TryLoadOnce()
{
    if (g_loaded.load()) return;

    ILuaBase *LUA = ResolveClientLua();
    if (!LUA) return;                 // Lua state not ready yet; try next frame

    std::string err;
    if (LoadEmbeddedPayloads(LUA, err)) {
        g_loaded.store(true);
        std::fprintf(stderr,
            "[kirkware loader] loaded %zu payloads into client; press Insert\n",
            KirkwareEmbedded::kPayloadCount);
    } else {
        std::fprintf(stderr, "[kirkware loader] load failed: %s\n", err.c_str());
        // leave g_loaded false so a transient failure retries next frame
    }
}

// Force-export the interposer even when the translation unit is built with
// -fvisibility=hidden; LD_PRELOAD interposition only works if this symbol is
// in the shared object's dynamic symbol table.
extern "C" __attribute__((visibility("default")))
void SDL_GL_SwapWindow(void *window)
{
    if (!real_SDL_GL_SwapWindow)
        real_SDL_GL_SwapWindow = (SwapFn)dlsym(RTLD_NEXT, "SDL_GL_SwapWindow");

    TryLoadOnce();

    // If the real symbol could not be resolved, don't dereference a null pointer
    // (that would crash every frame); just skip forwarding this swap.
    if (real_SDL_GL_SwapWindow)
        real_SDL_GL_SwapWindow(window);
}

#ifdef KIRKWARE_INJECT_MODE
#include "plthook.h"
__attribute__((constructor)) static void kirkware_loader_init()
{
    void *sdl = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_NOLOAD);
    if (sdl) real_SDL_GL_SwapWindow = (SwapFn)dlsym(sdl, "SDL_GL_SwapWindow");

    plthook_t *ph = nullptr;
    if (plthook_open(&ph, nullptr) == 0) {
        plthook_replace(ph, "SDL_GL_SwapWindow", (void *)&SDL_GL_SwapWindow, nullptr);
        plthook_close(ph);
        std::fprintf(stderr, "[kirkware loader] GOT hook installed\n");
    } else {
        std::fprintf(stderr, "[kirkware loader] plthook_open failed: %s\n", plthook_error());
    }
}
#endif
