-- Desktop control-panel profile bridge for Kirkware Linux.
--
-- This file does not implement any feature logic. It only applies values to
-- modules/convars that were already registered by the earlier addon files.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local profilePath = "kirkware_linux/desktop_profile.conf"
local timerName = "KirkwareLinux.DesktopProfile"
local lastRaw = nil

-- Keep the desktop profile constrained to convars that already exist in the
-- supported addon. Imported/hand-edited profiles are validated independently
-- of the desktop UI so an invalid string cannot accidentally force a numeric
-- convar to an unexpected value.
local allowedConVars = {
    kirkware_legit_fov = {minimum = 0.25, maximum = 45},
    kirkware_legit_smoothing = {minimum = 1, maximum = 40},
    kirkware_legit_max_distance = {minimum = 100, maximum = 50000},
    kirkware_legit_sort = {minimum = 0, maximum = 2, integer = true},
    kirkware_trigger_delay = {minimum = 0, maximum = 1},
    kirkware_rage_fov = {minimum = 1, maximum = 180},
    kirkware_rage_max_distance = {minimum = 100, maximum = 50000},
    kirkware_rage_sort = {minimum = 0, maximum = 2, integer = true},
    kirkware_aim_teammates = {minimum = 0, maximum = 1, integer = true},
    kirkware_legit_require_attack = {minimum = 0, maximum = 1, integer = true},
    kirkware_fov = {minimum = 60, maximum = 130},
    kirkware_zoom_fov = {minimum = 10, maximum = 100},
    kirkware_thirdperson_distance = {minimum = 30, maximum = 300},
    kirkware_entity_distance = {minimum = 250, maximum = 10000},
    kirkware_freecam_speed = {minimum = 50, maximum = 4000},
    kirkware_freecam_boost = {minimum = 1, maximum = 10},
    kirkware_tracer_time = {minimum = 0.05, maximum = 5},
}

local function parseBoolean(value)
    value = string.lower(string.Trim(value or ""))
    if value == "true" or value == "1" then
        return true
    end
    if value == "false" or value == "0" then
        return false
    end
    return nil
end

local function applyModule(id, value)
    if not KW.Modules or not KW.Modules[id] or not KW.SetModuleEnabled then
        return
    end
    local parsed = parseBoolean(value)
    if parsed == nil then
        return
    end
    local current = KW.ModuleEnabled and KW.ModuleEnabled(id) == true or false
    if current ~= parsed then
        KW.SetModuleEnabled(id, parsed)
    end
end

local function applyConVar(name, value)
    local definition = allowedConVars[name]
    if not definition then
        return
    end
    local convar = GetConVar(name)
    if not convar then
        return
    end

    local numeric = tonumber(string.Trim(value or ""))
    if numeric == nil then
        return
    end
    numeric = math.Clamp(numeric, definition.minimum, definition.maximum)
    if definition.integer then
        numeric = math.floor(numeric + 0.5)
    end

    if math.abs(convar:GetFloat() - numeric) <= 0.0001 then
        return
    end
    RunConsoleCommand(name, tostring(numeric))
end

local function applyProfile(raw)
    for line in string.gmatch((raw or "") .. "\n", "([^\n]*)\n") do
        line = string.Trim(line)
        if line ~= "" and string.sub(line, 1, 1) ~= "#" then
            local equals = string.find(line, "=", 1, true)
            if equals then
                local key = string.Trim(string.sub(line, 1, equals - 1))
                local value = string.Trim(string.sub(line, equals + 1))
                if string.StartWith(key, "module.") then
                    applyModule(string.sub(key, 8), value)
                elseif string.StartWith(key, "convar.") then
                    applyConVar(string.sub(key, 8), value)
                end
            end
        end
    end
end

local function refresh(force)
    local raw = file.Read(profilePath, "DATA")
    if not raw or raw == "" then
        return false
    end
    if not force and raw == lastRaw then
        return true
    end
    lastRaw = raw
    applyProfile(raw)
    hook.Run("KirkwareLinuxDesktopProfileApplied", profilePath)
    return true
end

concommand.Add("kirkware_desktop_profile_apply", function()
    if refresh(true) then
        print("[kirkware linux] desktop profile applied")
    else
        print("[kirkware linux] no desktop profile found at data/" .. profilePath)
    end
end)

timer.Simple(0, function()
    refresh(true)
end)

timer.Create(timerName, 0.75, 0, function()
    refresh(false)
end)

hook.Add("ShutDown", "KirkwareLinux.DesktopProfileCleanup", function()
    timer.Remove(timerName)
end)

print("[kirkware linux] desktop profile bridge loaded")
