-- Additional supported Kirkware Linux client modules.
-- Loaded after kirkware_linux.lua from lua/autorun/client.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"

local extraModules = {
    chams_enable = {
        name = "player outlines",
        description = "draw player outlines through world geometry",
        category = "visuals",
        default = false,
    },
    esp_player_hpbar = {
        name = "health bar",
        description = "show player health beside the player box",
        category = "visuals",
        default = true,
    },
    esp_player_arbar = {
        name = "armor bar",
        description = "show player armor beside the player box",
        category = "visuals",
        default = false,
    },
    esp_player_distance = {
        name = "player distance",
        description = "show distance to other players",
        category = "visuals",
        default = false,
    },
    esp_player_weapon = {
        name = "player weapon",
        description = "show each player's active weapon class",
        category = "visuals",
        default = false,
    },
    esp_player_skeleton = {
        name = "skeleton",
        description = "draw a simple player-model skeleton",
        category = "visuals",
        default = false,
    },
    esp_player_velocity = {
        name = "velocity",
        description = "show player movement speed",
        category = "visuals",
        default = false,
    },
    entities_enable = {
        name = "entity visuals",
        description = "master switch for nearby entity information",
        category = "visuals",
        default = false,
    },
    entities_name = {
        name = "entity names",
        description = "show nearby entity class names",
        category = "visuals",
        default = true,
    },
    entities_distance = {
        name = "entity distance",
        description = "show distance to nearby entities",
        category = "visuals",
        default = false,
    },
    menu_spectators = {
        name = "spectator list",
        description = "show players currently spectating you",
        category = "misc",
        default = false,
    },
    misc_fov_changer = {
        name = "fov changer",
        description = "use kirkware_fov as the normal camera field of view",
        category = "misc",
        default = false,
    },
    misc_zoom = {
        name = "zoom",
        description = "use kirkware_zoom_fov while this module is enabled",
        category = "misc",
        default = false,
    },
    misc_bunnyhop = {
        name = "bunnyhop",
        description = "release jump while airborne so held jump can retrigger",
        category = "misc",
        default = false,
    },
    misc_auto_strafe = {
        name = "auto strafe",
        description = "apply air strafe input in the direction of mouse movement",
        category = "misc",
        default = false,
    },
    misc_auto_pistol = {
        name = "auto pistol",
        description = "alternate held primary attack input for semi-auto weapons",
        category = "misc",
        default = false,
    },
}

local persisted = {}
local rawSettings = file.Read(settingsPath, "DATA")
if rawSettings and rawSettings ~= "" then
    local decoded = util.JSONToTable(rawSettings)
    if istable(decoded) then
        persisted = decoded
    end
end

for id, definition in pairs(extraModules) do
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

local fovConVar = CreateClientConVar("kirkware_fov", "100", true, false,
                                    "Kirkware normal camera FOV", 60, 130)
local zoomFovConVar = CreateClientConVar("kirkware_zoom_fov", "40", true, false,
                                        "Kirkware zoom camera FOV", 10, 100)
local thirdpersonDistanceConVar = CreateClientConVar(
    "kirkware_thirdperson_distance", "110", true, false,
    "Kirkware third-person camera distance", 30, 300)
local entityDistanceConVar = CreateClientConVar(
    "kirkware_entity_distance", "2500", true, false,
    "Maximum range for Kirkware entity visuals", 250, 10000)

local function playerScreenBounds(playerEntity)
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
    return {
        left = top.x - width * 0.5,
        top = math.min(top.y, bottom.y),
        width = width,
        height = height,
        center = top.x,
    }
end

local skeletonPairs = {
    {"ValveBiped.Bip01_Head1", "ValveBiped.Bip01_Neck1"},
    {"ValveBiped.Bip01_Neck1", "ValveBiped.Bip01_Spine2"},
    {"ValveBiped.Bip01_Spine2", "ValveBiped.Bip01_Spine"},
    {"ValveBiped.Bip01_Spine", "ValveBiped.Bip01_Pelvis"},
    {"ValveBiped.Bip01_Spine2", "ValveBiped.Bip01_L_UpperArm"},
    {"ValveBiped.Bip01_L_UpperArm", "ValveBiped.Bip01_L_Forearm"},
    {"ValveBiped.Bip01_L_Forearm", "ValveBiped.Bip01_L_Hand"},
    {"ValveBiped.Bip01_Spine2", "ValveBiped.Bip01_R_UpperArm"},
    {"ValveBiped.Bip01_R_UpperArm", "ValveBiped.Bip01_R_Forearm"},
    {"ValveBiped.Bip01_R_Forearm", "ValveBiped.Bip01_R_Hand"},
    {"ValveBiped.Bip01_Pelvis", "ValveBiped.Bip01_L_Thigh"},
    {"ValveBiped.Bip01_L_Thigh", "ValveBiped.Bip01_L_Calf"},
    {"ValveBiped.Bip01_L_Calf", "ValveBiped.Bip01_L_Foot"},
    {"ValveBiped.Bip01_Pelvis", "ValveBiped.Bip01_R_Thigh"},
    {"ValveBiped.Bip01_R_Thigh", "ValveBiped.Bip01_R_Calf"},
    {"ValveBiped.Bip01_R_Calf", "ValveBiped.Bip01_R_Foot"},
}

local function boneScreenPosition(entity, boneName)
    local bone = entity:LookupBone(boneName)
    if bone == nil then
        return nil
    end

    local matrix = entity:GetBoneMatrix(bone)
    local position = nil
    if matrix then
        position = matrix:GetTranslation()
    end
    if not position then
        position = entity:GetBonePosition(bone)
    end
    if not position then
        return nil
    end
    return position:ToScreen()
end

local function drawSkeleton(playerEntity)
    surface.SetDrawColor(43, 151, 250, 220)
    for _, pair in ipairs(skeletonPairs) do
        local first = boneScreenPosition(playerEntity, pair[1])
        local second = boneScreenPosition(playerEntity, pair[2])
        if first and second and (first.visible ~= false or second.visible ~= false) then
            surface.DrawLine(first.x, first.y, second.x, second.y)
        end
    end
end

local function drawHealthAndArmor(playerEntity, bounds)
    if enabled("esp_player_hpbar") then
        local maximum = math.max(1, playerEntity:GetMaxHealth())
        local fraction = math.Clamp(playerEntity:Health() / maximum, 0, 1)
        local filled = math.floor(bounds.height * fraction)
        local x = bounds.left - 6
        surface.SetDrawColor(0, 0, 0, 220)
        surface.DrawRect(x - 1, bounds.top - 1, 4, bounds.height + 2)
        surface.SetDrawColor(70, 210, 105, 255)
        surface.DrawRect(x, bounds.top + bounds.height - filled, 2, filled)
    end

    if enabled("esp_player_arbar") then
        local armor = math.Clamp(playerEntity:Armor(), 0, 100)
        local fraction = armor / 100
        local filled = math.floor(bounds.height * fraction)
        local x = bounds.left + bounds.width + 4
        surface.SetDrawColor(0, 0, 0, 220)
        surface.DrawRect(x - 1, bounds.top - 1, 4, bounds.height + 2)
        surface.SetDrawColor(90, 150, 255, 255)
        surface.DrawRect(x, bounds.top + bounds.height - filled, 2, filled)
    end
end

local function drawPlayerDetails(localPlayer, playerEntity, bounds)
    local rightX = bounds.left + bounds.width + 8
    local line = 0

    local function detail(text)
        draw.SimpleText(text, "KirkwareLinuxSmall", rightX,
                        bounds.top + line * 13, Color(225, 225, 225),
                        TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
        line = line + 1
    end

    if enabled("esp_player_distance") then
        detail(string.format("%.0f u", localPlayer:GetPos():Distance(playerEntity:GetPos())))
    end

    if enabled("esp_player_weapon") then
        local weapon = playerEntity:GetActiveWeapon()
        if IsValid(weapon) then
            detail(weapon:GetClass())
        end
    end

    if enabled("esp_player_velocity") then
        detail(string.format("%.0f u/s", playerEntity:GetVelocity():Length()))
    end
end

local function entityScreenCenter(entity)
    local mins, maxs = entity:WorldSpaceAABB()
    if not mins or not maxs then
        return nil
    end
    local center = (mins + maxs) * 0.5
    local screen = center:ToScreen()
    if screen.visible == false then
        return nil
    end
    return screen
end

local function shouldDrawEntity(entity, localPlayer)
    if not IsValid(entity) or entity == localPlayer or entity:IsPlayer() then
        return false
    end
    if entity == game.GetWorld() then
        return false
    end
    local className = entity:GetClass() or ""
    return string.StartWith(className, "prop_") or
           string.StartWith(className, "weapon_") or
           string.StartWith(className, "npc_") or
           string.StartWith(className, "sent_") or
           string.StartWith(className, "gmod_")
end

hook.Add("HUDPaint", "KirkwareLinux.ExpandedVisuals", function()
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) then
        return
    end

    if enabled("esp_player_enable") then
        for _, playerEntity in ipairs(player.GetAll()) do
            if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
                local bounds = playerScreenBounds(playerEntity)
                if bounds then
                    drawHealthAndArmor(playerEntity, bounds)
                    drawPlayerDetails(localPlayer, playerEntity, bounds)
                    if enabled("esp_player_skeleton") then
                        drawSkeleton(playerEntity)
                    end
                end
            end
        end
    end

    if enabled("entities_enable") then
        local maximumDistance = entityDistanceConVar:GetFloat()
        for _, entity in ipairs(ents.FindInSphere(localPlayer:GetPos(), maximumDistance)) do
            if shouldDrawEntity(entity, localPlayer) then
                local screen = entityScreenCenter(entity)
                if screen then
                    local line = 0
                    if enabled("entities_name") then
                        draw.SimpleText(entity:GetClass(), "KirkwareLinuxSmall",
                                        screen.x, screen.y,
                                        Color(235, 235, 235),
                                        TEXT_ALIGN_CENTER, TEXT_ALIGN_BOTTOM)
                        line = line + 1
                    end
                    if enabled("entities_distance") then
                        local distance = localPlayer:GetPos():Distance(entity:GetPos())
                        draw.SimpleText(string.format("%.0f u", distance),
                                        "KirkwareLinuxSmall", screen.x,
                                        screen.y + line * 13,
                                        Color(170, 170, 170),
                                        TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP)
                    end
                end
            end
        end
    end

    if enabled("menu_spectators") then
        local names = {}
        for _, playerEntity in ipairs(player.GetAll()) do
            if IsValid(playerEntity) and playerEntity ~= localPlayer and
               playerEntity:GetObserverMode() ~= OBS_MODE_NONE and
               playerEntity:GetObserverTarget() == localPlayer then
                table.insert(names, playerEntity:Nick())
            end
        end

        if #names > 0 then
            table.sort(names)
            local width = 180
            local x = ScrW() - width - 12
            local y = 38
            draw.RoundedBox(4, x, y, width, 26 + #names * 16,
                            Color(18, 18, 18, 220))
            draw.SimpleText("spectators", "KirkwareLinuxText",
                            x + 8, y + 6, Color(255, 255, 255),
                            TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
            for index, name in ipairs(names) do
                draw.SimpleText(name, "KirkwareLinuxSmall",
                                x + 8, y + 24 + (index - 1) * 16,
                                Color(190, 190, 190),
                                TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
            end
        end
    end
end)

hook.Add("PreDrawHalos", "KirkwareLinux.PlayerOutlines", function()
    if not enabled("chams_enable") or not enabled("esp_player_enable") then
        return
    end

    local localPlayer = LocalPlayer()
    local targets = {}
    for _, playerEntity in ipairs(player.GetAll()) do
        if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
            targets[#targets + 1] = playerEntity
        end
    end

    if #targets > 0 then
        halo.Add(targets, Color(43, 151, 250), 1, 1, 1, true, true)
    end
end)

-- Replace the first-stage camera hook with one combined camera implementation so
-- third person, normal FOV and zoom compose instead of competing for CalcView.
hook.Remove("CalcView", "KirkwareLinux.ThirdPerson")
hook.Add("CalcView", "KirkwareLinux.Camera", function(playerEntity,
                                                       position,
                                                       angles,
                                                       fov)
    if not IsValid(playerEntity) then
        return
    end

    local targetFov = fov
    if enabled("misc_zoom") then
        targetFov = zoomFovConVar:GetFloat()
    elseif enabled("misc_fov_changer") then
        targetFov = fovConVar:GetFloat()
    end

    if enabled("misc_thirdperson") then
        local distance = thirdpersonDistanceConVar:GetFloat()
        local desired = position - angles:Forward() * distance + Vector(0, 0, 8)
        local trace = util.TraceHull({
            start = position,
            endpos = desired,
            mins = Vector(-4, -4, -4),
            maxs = Vector(4, 4, 4),
            filter = playerEntity,
            mask = MASK_SOLID,
        })

        return {
            origin = trace.HitPos + trace.HitNormal * 4,
            angles = angles,
            fov = targetFov,
            drawviewer = true,
        }
    end

    if targetFov ~= fov then
        return {
            origin = position,
            angles = angles,
            fov = targetFov,
            drawviewer = false,
        }
    end
end)

hook.Add("CreateMove", "KirkwareLinux.MovementModules", function(command)
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) or not localPlayer:Alive() then
        return
    end
    if IsValid(KW.Frame) and KW.Frame:IsVisible() then
        return
    end

    if enabled("misc_bunnyhop") and command:KeyDown(IN_JUMP) and
       not localPlayer:OnGround() then
        command:RemoveKey(IN_JUMP)
    end

    if enabled("misc_auto_strafe") and not localPlayer:OnGround() then
        local mouseX = command:GetMouseX()
        if mouseX > 1 then
            command:SetSideMove(450)
        elseif mouseX < -1 then
            command:SetSideMove(-450)
        end
    end

    if enabled("misc_auto_pistol") and command:KeyDown(IN_ATTACK) and
       command:CommandNumber() ~= 0 and command:CommandNumber() % 2 == 0 then
        command:RemoveKey(IN_ATTACK)
    end
end)

print("[kirkware linux] expanded client modules loaded")
