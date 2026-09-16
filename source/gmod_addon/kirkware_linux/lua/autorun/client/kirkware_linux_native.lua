-- Optional supported native-module bridge for Kirkware Linux.
-- The binary is installed separately into garrysmod/lua/bin/ by kirkware.py.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local ok, detail = pcall(require, "kirkware_native")
if not ok then
    KW.NativeAvailable = false
    KW.NativeError = tostring(detail)
    print("[kirkware linux] native module unavailable: " .. KW.NativeError)
    return
end

local native = _G.kirkware_native
if not istable(native) then
    KW.NativeAvailable = false
    KW.NativeError = "module loaded without kirkware_native table"
    print("[kirkware linux] native module unavailable: " .. KW.NativeError)
    return
end

KW.NativeAvailable = native.loaded and native.loaded() == true or false
KW.NativeVersion = native.version and native.version() or "unknown"
KW.NativePlatform = native.platform and native.platform() or "unknown"
KW.NativeAbi = native.abi and native.abi() or 0
KW.NativePayloadCount = native.payload_count and native.payload_count() or 0
KW.NativeEmbeddedLoaded = native.embedded_loaded and native.embedded_loaded() == true or false

print(string.format(
    "[kirkware linux] native module loaded: %s platform=%s abi=%s payloads=%s embedded=%s",
    tostring(KW.NativeVersion), tostring(KW.NativePlatform), tostring(KW.NativeAbi),
    tostring(KW.NativePayloadCount), tostring(KW.NativeEmbeddedLoaded)))
