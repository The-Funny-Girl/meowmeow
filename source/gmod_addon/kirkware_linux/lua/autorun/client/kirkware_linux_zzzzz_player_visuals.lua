-- Final player-visual composition for Kirkware Linux.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"
local modules = {
    esp_show_health = {
        name = "health number",
        description = "show numeric player health",
        category = "visuals",
        default = false,
    },
    esp_show_armor = {
        name = "armor number",
        description = "show numeric player armor",
        category = "visuals",
        default = false,
    },
    esp_player_flag_cloaked = {
        name = "cloaked flag",
        description = "flag players using no-draw or translucent rendering",
        category = "visuals",
        default = false,
    },
    playerlist_highlight_staff = {
        name = "staff flag",
        description = "flag players whose usergroup is not the default user group",
        category = "visuals",
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
for id, definition in pairs(modules) do
    KW.Modules[id] = definition
    if isbool(persisted[id]) then
        KW.Settings[id] = persisted[id]
    elseif KW.Settings[id] == nil then
        KW.Settings[id] = definition.default == true
    end
end

local function enabled(id)
    return KW.ModuleEnabled and KW.ModuleEnabled(id) == true
end

local function playerTop(playerEntity)
    local maxs = playerEntity:OBBMaxs()
    return playerEntity:LocalToWorld(Vector(0, 0, maxs.z + 8)):ToScreen()
end

hook.Add("HUDPaint", "KirkwareLinux.PlayerStatusDetails", function()
    if not enabled("esp_player_enable") then
        return
    end
    if not enabled("esp_show_health") and not enabled("esp_show_armor") and
       not enabled("esp_player_flag_cloaked") and
       not enabled("playerlist_highlight_staff") then
        return
    end

    local localPlayer = LocalPlayer()
    for _, playerEntity in ipairs(player.GetAll()) do
        if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
            local screen = playerTop(playerEntity)
            if screen.visible ~= false then
                local details = {}
                if enabled("esp_show_health") then
                    details[#details + 1] = tostring(playerEntity:Health()) .. " hp"
                end
                if enabled("esp_show_armor") and playerEntity:Armor() > 0 then
                    details[#details + 1] = tostring(playerEntity:Armor()) .. " ar"
                end
                if enabled("esp_player_flag_cloaked") then
                    local color = playerEntity:GetColor()
                    if playerEntity:GetNoDraw() or (color and color.a < 245) then
                        details[#details + 1] = "CLOAKED"
                    end
                end
                if enabled("playerlist_highlight_staff") then
                    local group = playerEntity:GetUserGroup()
                    if group and group ~= "" and group ~= "user" then
                        details[#details + 1] = "STAFF:" .. string.upper(group)
                    end
                end

                if #details > 0 then
                    draw.SimpleText(table.concat(details, " | "),
                                    "KirkwareLinuxSmall", screen.x, screen.y - 43,
                                    Color(210, 210, 210),
                                    TEXT_ALIGN_CENTER, TEXT_ALIGN_BOTTOM)
                end
            end
        end
    end
end)

-- Replace the generic outline hook with rule-aware groups. Ignored players are
-- intentionally omitted; priority/friend colors match the Players page.
hook.Remove("PreDrawHalos", "KirkwareLinux.PlayerOutlines")
hook.Add("PreDrawHalos", "KirkwareLinux.PlayerOutlines", function()
    if not enabled("chams_enable") or not enabled("esp_player_enable") then
        return
    end

    local localPlayer = LocalPlayer()
    local normal = {}
    local priority = {}
    local friends = {}

    for _, playerEntity in ipairs(player.GetAll()) do
        if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
            local rule = KW.GetPlayerRule and KW.GetPlayerRule(playerEntity) or "normal"
            if rule == "priority" then
                priority[#priority + 1] = playerEntity
            elseif rule == "friend" then
                friends[#friends + 1] = playerEntity
            elseif rule ~= "ignore" then
                normal[#normal + 1] = playerEntity
            end
        end
    end

    if #normal > 0 then
        halo.Add(normal, Color(43, 151, 250), 1, 1, 1, true, true)
    end
    if #priority > 0 then
        halo.Add(priority, Color(255, 105, 105), 1, 1, 1, true, true)
    end
    if #friends > 0 then
        halo.Add(friends, Color(90, 210, 130), 1, 1, 1, true, true)
    end
end)

print("[kirkware linux] player visual parity loaded")
