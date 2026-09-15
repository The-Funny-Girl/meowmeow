-- One-shot menu/hotkey actions for the supported Linux pathfinder.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"
local definitions = {
    misc_pathfinder_set_target = {
        name = "set path target",
        description = "use the current crosshair world position as the path goal",
        category = "misc",
        default = false,
    },
    misc_pathfinder_clear_target = {
        name = "clear path",
        description = "clear the current pathfinder route",
        category = "misc",
        default = false,
    },
}

local persisted = {}
local raw = file.Read(settingsPath, "DATA")
if raw and raw ~= "" then
    local decoded = util.JSONToTable(raw)
    if istable(decoded) then
        persisted = decoded
    end
end
for id, definition in pairs(definitions) do
    KW.Modules[id] = definition
    -- Action modules always start false; a stale true value should never replay an action.
    KW.Settings[id] = false
end

local busy = false
local function resetAction(id)
    timer.Simple(0, function()
        if KW.SetModuleEnabled and KW.Settings[id] == true then
            KW.SetModuleEnabled(id, false)
        end
    end)
end

hook.Add("KirkwareLinuxModuleChanged", "KirkwareLinux.PathfinderActions",
         function(id, active)
    if busy or not active then
        return
    end

    if id == "misc_pathfinder_set_target" then
        busy = true
        local localPlayer = LocalPlayer()
        if not KW.ModuleEnabled("misc_pathfinder_enable") then
            print("[kirkware linux] enable pathfinder before setting a target")
        elseif not IsValid(localPlayer) or not KW.PathfinderSetGoal then
            print("[kirkware linux] pathfinder is not ready")
        else
            local trace = localPlayer:GetEyeTrace()
            if trace and trace.Hit then
                local ok, detail = KW.PathfinderSetGoal(trace.HitPos)
                print("[kirkware linux] path: " .. tostring(detail))
                if ok and KW.SetModuleEnabled then
                    KW.SetModuleEnabled("misc_pathfinder_walk_path", true)
                end
            else
                print("[kirkware linux] crosshair did not hit a path target")
            end
        end
        resetAction(id)
        busy = false
        return
    end

    if id == "misc_pathfinder_clear_target" then
        busy = true
        if KW.PathfinderClear then
            KW.PathfinderClear()
        end
        if KW.SetModuleEnabled then
            KW.SetModuleEnabled("misc_pathfinder_walk_path", false)
        end
        print("[kirkware linux] path cleared")
        resetAction(id)
        busy = false
    end
end)

print("[kirkware linux] pathfinder menu actions loaded")
