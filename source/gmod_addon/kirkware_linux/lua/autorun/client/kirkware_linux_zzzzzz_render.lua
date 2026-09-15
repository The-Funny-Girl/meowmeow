-- Final base HUD renderer for Kirkware Linux.
-- Replaces the small first-stage HUD hook so later settings compose correctly.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"
local modules = {
    esp_player_teambased = {
        name = "team colors",
        description = "color normal player ESP using team colors",
        category = "visuals",
        default = false,
    },
    esp_player_name_steam = {
        name = "Steam names",
        description = "prefer Steam profile names for player labels",
        category = "visuals",
        default = false,
    },
    esp_other_crosshair_outline = {
        name = "crosshair outline",
        description = "draw a dark outline around the custom crosshair",
        category = "visuals",
        default = true,
    },
    esp_other_crosshair_rainbow = {
        name = "rainbow crosshair",
        description = "cycle the custom crosshair hue over time",
        category = "visuals",
        default = false,
    },
    misc_anonymous_mode = {
        name = "anonymous ESP names",
        description = "replace player names in Kirkware ESP with anonymous labels",
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

local crosshairSize = CreateClientConVar(
    "kirkware_crosshair_size", "7", true, false,
    "Length of each Kirkware crosshair arm", 2, 30)
local crosshairGap = CreateClientConVar(
    "kirkware_crosshair_gap", "2", true, false,
    "Gap in the center of the Kirkware crosshair", 0, 15)
local crosshairThickness = CreateClientConVar(
    "kirkware_crosshair_thickness", "1", true, false,
    "Kirkware crosshair line thickness", 1, 6)
local crosshairRainbowSpeed = CreateClientConVar(
    "kirkware_crosshair_rainbow_speed", "90", true, false,
    "Kirkware rainbow crosshair hue speed", 10, 360)

local steamNames = {}
local steamPending = {}

local function requestSteamName(playerEntity)
    if not IsValid(playerEntity) then
        return
    end
    local steamId = playerEntity:SteamID64()
    if not steamId or steamId == "" or steamId == "0" or
       steamNames[steamId] or steamPending[steamId] then
        return
    end
    steamPending[steamId] = true
    steamworks.RequestPlayerInfo(steamId, function(name)
        steamPending[steamId] = nil
        if isstring(name) and name ~= "" then
            steamNames[steamId] = name
        end
    end)
end

local function displayName(playerEntity)
    if enabled("misc_anonymous_mode") then
        return "player " .. tostring(playerEntity:EntIndex())
    end
    if enabled("esp_player_name_steam") then
        local steamId = playerEntity:SteamID64()
        requestSteamName(playerEntity)
        if steamId and steamNames[steamId] then
            return steamNames[steamId]
        end
    end
    return playerEntity:Nick()
end

local function playerColor(playerEntity)
    local rule = KW.GetPlayerRule and KW.GetPlayerRule(playerEntity) or "normal"
    if rule == "priority" then
        return Color(255, 105, 105)
    elseif rule == "friend" then
        return Color(90, 210, 130)
    elseif rule == "ignore" then
        return Color(145, 145, 145)
    end
    if enabled("esp_player_teambased") then
        local color = team.GetColor(playerEntity:Team())
        if color then
            return color
        end
    end
    return Color(43, 151, 250)
end

local function playerBounds(playerEntity)
    local mins = playerEntity:OBBMins()
    local maxs = playerEntity:OBBMaxs()
    local bottom = playerEntity:LocalToWorld(Vector(0, 0, mins.z)):ToScreen()
    local top = playerEntity:LocalToWorld(Vector(0, 0, maxs.z + 6)):ToScreen()
    if bottom.visible == false and top.visible == false then
        return nil
    end
    local height = math.abs(bottom.y - top.y)
    if height < 2 then
        return nil
    end
    local width = math.max(10, height * 0.42)
    return top.x - width * 0.5, math.min(top.y, bottom.y), width, height, top.x
end

local function drawOutlinedBox(x, y, width, height, color)
    surface.SetDrawColor(0, 0, 0, 220)
    surface.DrawOutlinedRect(x - 1, y - 1, width + 2, height + 2, 1)
    surface.SetDrawColor(color)
    surface.DrawOutlinedRect(x, y, width, height, 1)
end

local function crosshairColor()
    if enabled("esp_other_crosshair_rainbow") then
        return HSVToColor((CurTime() * crosshairRainbowSpeed:GetFloat()) % 360, 1, 1)
    end
    return Color(43, 151, 250)
end

local function drawCrosshairArm(x, y, width, height, color)
    if enabled("esp_other_crosshair_outline") then
        surface.SetDrawColor(0, 0, 0, 230)
        surface.DrawRect(x - 1, y - 1, width + 2, height + 2)
    end
    surface.SetDrawColor(color)
    surface.DrawRect(x, y, width, height)
end

local function drawCrosshair()
    if not enabled("esp_other_crosshair") then
        return
    end
    local centerX = math.floor(ScrW() * 0.5)
    local centerY = math.floor(ScrH() * 0.5)
    local size = math.floor(crosshairSize:GetFloat())
    local gap = math.floor(crosshairGap:GetFloat())
    local thickness = math.max(1, math.floor(crosshairThickness:GetFloat()))
    local halfThickness = math.floor(thickness * 0.5)
    local color = crosshairColor()

    drawCrosshairArm(centerX - gap - size, centerY - halfThickness,
                     size, thickness, color)
    drawCrosshairArm(centerX + gap, centerY - halfThickness,
                     size, thickness, color)
    drawCrosshairArm(centerX - halfThickness, centerY - gap - size,
                     thickness, size, color)
    drawCrosshairArm(centerX - halfThickness, centerY + gap,
                     thickness, size, color)
end

-- The initial implementation provided this hook. All of its functionality is
-- retained here while allowing the later module/player systems to affect it.
hook.Remove("HUDPaint", "KirkwareLinux.HUDModules")
hook.Add("HUDPaint", "KirkwareLinux.HUDModules", function()
    if enabled("menu_watermark") then
        draw.SimpleText("kirkware linux", "KirkwareLinuxText",
                        ScrW() - 12, 10, Color(255, 255, 255),
                        TEXT_ALIGN_RIGHT, TEXT_ALIGN_TOP)
    end

    drawCrosshair()

    if not enabled("esp_player_enable") then
        return
    end

    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) then
        return
    end

    for _, playerEntity in ipairs(player.GetAll()) do
        if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
            local x, y, width, height, center = playerBounds(playerEntity)
            if x then
                local color = playerColor(playerEntity)
                if enabled("esp_player_box") then
                    drawOutlinedBox(x, y, width, height, color)
                end
                if enabled("esp_player_name") then
                    draw.SimpleText(displayName(playerEntity), "KirkwareLinuxSmall",
                                    center, y - 14, color,
                                    TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP)
                end
            end
        end
    end
end)

print("[kirkware linux] composed ESP/crosshair renderer loaded")
