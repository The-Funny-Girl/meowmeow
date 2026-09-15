-- Runtime compatibility fixes for Kirkware Linux.
-- Loaded last so all feature modules have registered themselves in KW.Modules.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local dataDirectory = "kirkware_linux"
local dataFile = dataDirectory .. "/modules.json"

local function saveSettings()
    file.CreateDir(dataDirectory)
    file.Write(dataFile, util.TableToJSON(KW.Settings or {}, true))
end

-- The original base setter closes over only the handful of modules declared in
-- kirkware_linux.lua. Later combat/visual/misc modules extend KW.Modules, so the
-- public setter must consult the live registry instead of that original table.
function KW.SetModuleEnabled(id, enabled)
    if not KW.Modules or not KW.Modules[id] then
        return false
    end

    KW.Settings = KW.Settings or {}
    local nextValue = enabled == true
    if KW.Settings[id] == nextValue then
        return true
    end

    KW.Settings[id] = nextValue
    saveSettings()
    hook.Run("KirkwareLinuxModuleChanged", id, nextValue)
    return true
end

KW.SaveModuleSettings = saveSettings

print("[kirkware linux] dynamic module setter loaded")
