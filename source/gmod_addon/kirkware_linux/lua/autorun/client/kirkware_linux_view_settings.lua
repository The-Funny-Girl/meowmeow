-- Supported local view settings for Kirkware Linux.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"
local definition = {
    name = "viewmodel fov",
    description = "override the normal client viewmodel_fov while enabled",
    category = "misc",
    default = false,
}

local persisted = {}
local raw = file.Read(settingsPath, "DATA")
if raw and raw ~= "" then
    local decoded = util.JSONToTable(raw)
    if istable(decoded) then
        persisted = decoded
    end
end

KW.Modules.misc_fov_viewmodel = definition
if isbool(persisted.misc_fov_viewmodel) then
    KW.Settings.misc_fov_viewmodel = persisted.misc_fov_viewmodel
elseif KW.Settings.misc_fov_viewmodel == nil then
    KW.Settings.misc_fov_viewmodel = false
end

local requestedFov = CreateClientConVar(
    "kirkware_viewmodel_fov", "54", true, false,
    "Kirkware viewmodel FOV", 20, 140)
local engineConVar = GetConVar("viewmodel_fov")
local originalValue = engineConVar and engineConVar:GetString() or "54"
local wasEnabled = false
local nextApply = 0

local function enabled()
    return KW.ModuleEnabled and KW.ModuleEnabled("misc_fov_viewmodel") == true
end

local function setEngineValue(value)
    if not engineConVar then
        return
    end
    if engineConVar:GetString() ~= tostring(value) then
        RunConsoleCommand("viewmodel_fov", tostring(value))
    end
end

hook.Add("Think", "KirkwareLinux.ViewmodelFov", function()
    if CurTime() < nextApply then
        return
    end
    nextApply = CurTime() + 0.25

    if not engineConVar then
        engineConVar = GetConVar("viewmodel_fov")
        if not engineConVar then
            return
        end
        originalValue = engineConVar:GetString()
    end

    local active = enabled()
    if active then
        if not wasEnabled then
            originalValue = engineConVar:GetString()
        end
        setEngineValue(requestedFov:GetFloat())
    elseif wasEnabled then
        setEngineValue(originalValue)
    else
        -- If Kirkware is not controlling the convar, follow the user's normal value.
        originalValue = engineConVar:GetString()
    end
    wasEnabled = active
end)

hook.Add("ShutDown", "KirkwareLinux.RestoreViewmodelFov", function()
    if wasEnabled then
        setEngineValue(originalValue)
    end
end)

print("[kirkware linux] viewmodel FOV module loaded")
