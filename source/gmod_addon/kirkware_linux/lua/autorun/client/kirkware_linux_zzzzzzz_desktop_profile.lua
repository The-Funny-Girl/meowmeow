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

local allowedConVars = {
    kirkware_legit_fov = true,
    kirkware_legit_smoothing = true,
    kirkware_legit_max_distance = true,
    kirkware_legit_sort = true,
    kirkware_trigger_delay = true,
    kirkware_rage_fov = true,
    kirkware_rage_max_distance = true,
    kirkware_rage_sort = true,
    kirkware_aim_teammates = true,
    kirkware_legit_require_attack = true,
    kirkware_fov = true,
    kirkware_zoom_fov = true,
    kirkware_thirdperson_distance = true,
    kirkware_entity_distance = true,
    kirkware_freecam_speed = true,
    kirkware_freecam_boost = true,
    kirkware_tracer_time = true,
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
    if not allowedConVars[name] then
        return
    end
    local convar = GetConVar(name)
    if not convar then
        return
    end
    value = string.Trim(value or "")
    if value == "" then
        return
    end

    local numeric = tonumber(value)
    if numeric ~= nil then
        if math.abs(convar:GetFloat() - numeric) <= 0.0001 then
            return
        end
    elseif convar:GetString() == value then
        return
    end
    RunConsoleCommand(name, value)
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
