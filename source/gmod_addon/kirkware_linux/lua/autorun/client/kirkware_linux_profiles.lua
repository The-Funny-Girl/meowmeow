-- Named profiles for supported Kirkware Linux module/tuning state.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local dataDirectory = "kirkware_linux"
local profilesDirectory = dataDirectory .. "/profiles"
local moduleSettingsPath = dataDirectory .. "/modules.json"

local profileConVars = {
    "kirkware_legit_fov",
    "kirkware_legit_smoothing",
    "kirkware_legit_max_distance",
    "kirkware_trigger_delay",
    "kirkware_rage_fov",
    "kirkware_rage_max_distance",
    "kirkware_aim_teammates",
    "kirkware_legit_require_attack",
    "kirkware_fov",
    "kirkware_zoom_fov",
    "kirkware_viewmodel_fov",
    "kirkware_thirdperson_distance",
    "kirkware_entity_distance",
    "kirkware_freecam_speed",
    "kirkware_freecam_boost",
    "kirkware_tracer_time",
    "kirkware_path_tolerance",
    "kirkware_path_max_areas",
    "kirkware_crosshair_size",
    "kirkware_crosshair_gap",
    "kirkware_crosshair_thickness",
    "kirkware_crosshair_rainbow_speed",
}

local function safeName(name)
    name = string.Trim(string.lower(tostring(name or "")))
    name = string.gsub(name, "[^a-z0-9_%-]", "_")
    name = string.gsub(name, "_+", "_")
    name = string.Trim(name, "_")
    if #name > 48 then
        name = string.sub(name, 1, 48)
    end
    return name
end

local function profilePath(name)
    local safe = safeName(name)
    if safe == "" then
        return nil, "profile name is empty"
    end
    return profilesDirectory .. "/" .. safe .. ".json", safe
end

local function captureConVars()
    local result = {}
    for _, name in ipairs(profileConVars) do
        local variable = GetConVar(name)
        if variable then
            result[name] = variable:GetString()
        end
    end
    return result
end

local function captureModules()
    local result = {}
    for id in pairs(KW.Modules or {}) do
        result[id] = KW.Settings[id] == true
    end
    return result
end

function KW.SaveProfile(name)
    local path, safe = profilePath(name)
    if not path then
        return false, safe
    end
    file.CreateDir(dataDirectory)
    file.CreateDir(profilesDirectory)
    local payload = {
        version = 1,
        name = safe,
        modules = captureModules(),
        convars = captureConVars(),
    }
    file.Write(path, util.TableToJSON(payload, true))
    return true, safe
end

function KW.LoadProfile(name)
    local path, safe = profilePath(name)
    if not path then
        return false, safe
    end
    local raw = file.Read(path, "DATA")
    if not raw or raw == "" then
        return false, "profile not found: " .. safe
    end
    local payload = util.JSONToTable(raw)
    if not istable(payload) then
        return false, "profile JSON is invalid"
    end

    local changed = {}
    if istable(payload.modules) then
        for id, definition in pairs(KW.Modules or {}) do
            if isbool(payload.modules[id]) and
               KW.Settings[id] ~= payload.modules[id] then
                -- One-shot action modules must never replay from a profile.
                if definition.default == false and
                   (id == "misc_pathfinder_set_target" or
                    id == "misc_pathfinder_clear_target") then
                    KW.Settings[id] = false
                else
                    KW.Settings[id] = payload.modules[id]
                    changed[#changed + 1] = id
                end
            end
        end
    end

    file.CreateDir(dataDirectory)
    file.Write(moduleSettingsPath, util.TableToJSON(KW.Settings, true))

    if istable(payload.convars) then
        for _, convarName in ipairs(profileConVars) do
            local value = payload.convars[convarName]
            if value ~= nil and GetConVar(convarName) then
                RunConsoleCommand(convarName, tostring(value))
            end
        end
    end

    for _, id in ipairs(changed) do
        hook.Run("KirkwareLinuxModuleChanged", id, KW.Settings[id] == true)
    end
    if KW.RebuildModuleList then
        KW.RebuildModuleList()
    end
    return true, safe
end

function KW.DeleteProfile(name)
    local path, safe = profilePath(name)
    if not path then
        return false, safe
    end
    if not file.Exists(path, "DATA") then
        return false, "profile not found: " .. safe
    end
    file.Delete(path)
    return true, safe
end

function KW.ListProfiles()
    file.CreateDir(dataDirectory)
    file.CreateDir(profilesDirectory)
    local files = file.Find(profilesDirectory .. "/*.json", "DATA") or {}
    local names = {}
    for _, filename in ipairs(files) do
        names[#names + 1] = string.StripExtension(filename)
    end
    table.sort(names)
    return names
end

concommand.Add("kirkware_profile_save", function(_, _, arguments)
    local ok, detail = KW.SaveProfile(table.concat(arguments, "_"))
    print("[kirkware linux] " .. (ok and "saved profile " or "profile error: ") .. detail)
end)

concommand.Add("kirkware_profile_load", function(_, _, arguments)
    local ok, detail = KW.LoadProfile(table.concat(arguments, "_"))
    print("[kirkware linux] " .. (ok and "loaded profile " or "profile error: ") .. detail)
end)

concommand.Add("kirkware_profile_delete", function(_, _, arguments)
    local ok, detail = KW.DeleteProfile(table.concat(arguments, "_"))
    print("[kirkware linux] " .. (ok and "deleted profile " or "profile error: ") .. detail)
end)

concommand.Add("kirkware_profiles", function()
    local names = KW.ListProfiles()
    print("[kirkware linux] profiles")
    if #names == 0 then
        print("  none")
        return
    end
    for _, name in ipairs(names) do
        print("  " .. name)
    end
end)

print("[kirkware linux] profile manager loaded")
