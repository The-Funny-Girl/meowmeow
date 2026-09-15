-- Local join/leave notifications for Kirkware Linux.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"
local id = "misc_anonymous_join_leave_notify"
local definition = {
    name = "join / leave notices",
    description = "show local player join and leave notifications",
    category = "misc",
    default = false,
}

local raw = file.Read(settingsPath, "DATA")
local persisted = raw and util.JSONToTable(raw) or nil
KW.Modules[id] = definition
if istable(persisted) and isbool(persisted[id]) then
    KW.Settings[id] = persisted[id]
elseif KW.Settings[id] == nil then
    KW.Settings[id] = false
end

local function enabled(moduleId)
    return KW.ModuleEnabled and KW.ModuleEnabled(moduleId) == true
end

local function displayName(name)
    if enabled("misc_anonymous_mode") then
        return "a player"
    end
    if not isstring(name) or name == "" then
        return "a player"
    end
    return name
end

local function notice(prefix, name, suffixColor)
    if not enabled(id) then
        return
    end
    chat.AddText(Color(43, 151, 250), "[kirkware] ",
                 Color(220, 220, 220), displayName(name),
                 suffixColor or Color(180, 180, 180), " " .. prefix)
end

gameevent.Listen("player_connect_client")
hook.Add("player_connect_client", "KirkwareLinux.JoinNotice", function(data)
    notice("joined", data and data.name, Color(90, 210, 130))
end)

gameevent.Listen("player_disconnect")
hook.Add("player_disconnect", "KirkwareLinux.LeaveNotice", function(data)
    notice("left", data and data.name, Color(227, 126, 129))
end)

print("[kirkware linux] join/leave notifications loaded")
