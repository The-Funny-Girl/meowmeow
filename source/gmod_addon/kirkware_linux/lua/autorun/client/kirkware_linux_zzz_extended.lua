-- Additional supported visual/misc modules for Kirkware Linux.
-- Loaded last so the camera implementation can compose all Linux camera features.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"

local modules = {
    esp_player_team = {
        name = "team info",
        description = "show each player's team name",
        category = "visuals",
        default = false,
    },
    esp_player_usergroup = {
        name = "usergroup",
        description = "show each player's advertised user group",
        category = "visuals",
        default = false,
    },
    esp_player_flag_noclip = {
        name = "noclip flag",
        description = "flag players currently using noclip movement",
        category = "visuals",
        default = false,
    },
    esp_player_oof = {
        name = "offscreen arrows",
        description = "show direction arrows for offscreen players",
        category = "visuals",
        default = false,
    },
    esp_player_hit_notify = {
        name = "hit marker",
        description = "show a hit marker after damaging another player",
        category = "visuals",
        default = false,
    },
    esp_player_hitsound = {
        name = "hit sound",
        description = "play a local sound after damaging another player",
        category = "visuals",
        default = false,
    },
    entities_box = {
        name = "entity boxes",
        description = "draw simple screen-space boxes around supported entities",
        category = "visuals",
        default = false,
    },
    entities_index = {
        name = "entity index",
        description = "show entity indexes with entity visuals",
        category = "visuals",
        default = false,
    },
    menu_hotkeys = {
        name = "enabled module list",
        description = "show currently enabled modules on the HUD",
        category = "misc",
        default = false,
    },
    misc_freecam = {
        name = "freecam",
        description = "move the camera locally without moving the player",
        category = "misc",
        default = false,
    },
    misc_faststop = {
        name = "fast stop",
        description = "counter ground velocity when movement keys are released",
        category = "misc",
        default = false,
    },
    misc_use_spam = {
        name = "use spam",
        description = "pulse the normal +use input while enabled",
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

local freecamSpeed = CreateClientConVar(
    "kirkware_freecam_speed", "650", true, false,
    "Kirkware freecam speed", 50, 4000)
local freecamBoost = CreateClientConVar(
    "kirkware_freecam_boost", "3", true, false,
    "Kirkware freecam shift multiplier", 1, 10)

local freecamPosition = nil
local hitMarkerUntil = 0

local function playerHeadScreen(playerEntity)
    local bone = playerEntity:LookupBone("ValveBiped.Bip01_Head1")
    local position = playerEntity:EyePos()
    if bone ~= nil then
        local matrix = playerEntity:GetBoneMatrix(bone)
        if matrix then
            position = matrix:GetTranslation()
        end
    end
    return position:ToScreen(), position
end

local function drawPlayerMetadata(localPlayer)
    if not enabled("esp_player_enable") then
        return
    end
    if not enabled("esp_player_team") and
       not enabled("esp_player_usergroup") and
       not enabled("esp_player_flag_noclip") then
        return
    end

    for _, playerEntity in ipairs(player.GetAll()) do
        if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
            local screen = playerHeadScreen(playerEntity)
            if screen.visible ~= false then
                local parts = {}
                if enabled("esp_player_team") then
                    parts[#parts + 1] = team.GetName(playerEntity:Team()) or "team"
                end
                if enabled("esp_player_usergroup") then
                    parts[#parts + 1] = playerEntity:GetUserGroup() or "user"
                end
                if enabled("esp_player_flag_noclip") and
                   playerEntity:GetMoveType() == MOVETYPE_NOCLIP then
                    parts[#parts + 1] = "NOCLIP"
                end
                if #parts > 0 then
                    draw.SimpleText(table.concat(parts, " | "), "KirkwareLinuxSmall",
                                    screen.x, screen.y - 28,
                                    Color(190, 190, 190),
                                    TEXT_ALIGN_CENTER, TEXT_ALIGN_BOTTOM)
                end
            end
        end
    end
end

local function drawOffscreenArrows(localPlayer)
    if not enabled("esp_player_enable") or not enabled("esp_player_oof") then
        return
    end

    local centerX = ScrW() * 0.5
    local centerY = ScrH() * 0.5
    local radius = math.min(ScrW(), ScrH()) * 0.38
    local eyeAngles = EyeAngles()
    local forward = eyeAngles:Forward()
    forward.z = 0
    forward:Normalize()
    local right = eyeAngles:Right()
    right.z = 0
    right:Normalize()

    for _, playerEntity in ipairs(player.GetAll()) do
        if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
            local screen = playerEntity:EyePos():ToScreen()
            local offscreen = screen.visible == false or screen.x < 0 or
                              screen.x > ScrW() or screen.y < 0 or screen.y > ScrH()
            if offscreen then
                local delta = playerEntity:GetPos() - localPlayer:GetPos()
                delta.z = 0
                if delta:LengthSqr() > 1 then
                    delta:Normalize()
                    local xComponent = delta:Dot(right)
                    local yComponent = delta:Dot(forward)
                    local angle = math.atan2(xComponent, yComponent)
                    local x = centerX + math.sin(angle) * radius
                    local y = centerY - math.cos(angle) * radius
                    local size = 9
                    local directionX = math.sin(angle)
                    local directionY = -math.cos(angle)
                    local perpendicularX = -directionY
                    local perpendicularY = directionX
                    surface.SetDrawColor(43, 151, 250, 225)
                    surface.DrawPoly({
                        {x = x + directionX * size, y = y + directionY * size},
                        {x = x - directionX * size * 0.65 + perpendicularX * size * 0.7,
                         y = y - directionY * size * 0.65 + perpendicularY * size * 0.7},
                        {x = x - directionX * size * 0.65 - perpendicularX * size * 0.7,
                         y = y - directionY * size * 0.65 - perpendicularY * size * 0.7},
                    })
                end
            end
        end
    end
end

local function supportedEntity(entity, localPlayer)
    if not IsValid(entity) or entity == localPlayer or entity:IsPlayer() or
       entity == game.GetWorld() then
        return false
    end
    local className = entity:GetClass() or ""
    return string.StartWith(className, "prop_") or
           string.StartWith(className, "weapon_") or
           string.StartWith(className, "npc_") or
           string.StartWith(className, "sent_") or
           string.StartWith(className, "gmod_")
end

local function entityBounds(entity)
    local mins, maxs = entity:WorldSpaceAABB()
    if not mins or not maxs then
        return nil
    end
    local corners = {
        Vector(mins.x, mins.y, mins.z), Vector(mins.x, mins.y, maxs.z),
        Vector(mins.x, maxs.y, mins.z), Vector(mins.x, maxs.y, maxs.z),
        Vector(maxs.x, mins.y, mins.z), Vector(maxs.x, mins.y, maxs.z),
        Vector(maxs.x, maxs.y, mins.z), Vector(maxs.x, maxs.y, maxs.z),
    }
    local left, top = math.huge, math.huge
    local right, bottom = -math.huge, -math.huge
    local visible = false
    for _, corner in ipairs(corners) do
        local screen = corner:ToScreen()
        visible = visible or screen.visible ~= false
        left = math.min(left, screen.x)
        top = math.min(top, screen.y)
        right = math.max(right, screen.x)
        bottom = math.max(bottom, screen.y)
    end
    if not visible or right <= left or bottom <= top then
        return nil
    end
    return left, top, right - left, bottom - top
end

local function drawEntityExtras(localPlayer)
    if not enabled("entities_enable") or
       (not enabled("entities_box") and not enabled("entities_index")) then
        return
    end
    local distanceConVar = GetConVar("kirkware_entity_distance")
    local maximumDistance = distanceConVar and distanceConVar:GetFloat() or 2500
    for _, entity in ipairs(ents.FindInSphere(localPlayer:GetPos(), maximumDistance)) do
        if supportedEntity(entity, localPlayer) then
            if enabled("entities_box") then
                local x, y, width, height = entityBounds(entity)
                if x then
                    surface.SetDrawColor(43, 151, 250, 210)
                    surface.DrawOutlinedRect(x, y, width, height, 1)
                end
            end
            if enabled("entities_index") then
                local screen = entity:WorldSpaceCenter():ToScreen()
                if screen.visible ~= false then
                    draw.SimpleText("#" .. entity:EntIndex(), "KirkwareLinuxSmall",
                                    screen.x, screen.y + 14,
                                    Color(170, 170, 170),
                                    TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP)
                end
            end
        end
    end
end

local function drawEnabledModules()
    if not enabled("menu_hotkeys") then
        return
    end
    local names = {}
    for id, definition in pairs(KW.Modules or {}) do
        if KW.Settings[id] == true and id ~= "menu_hotkeys" then
            names[#names + 1] = definition.name or id
        end
    end
    table.sort(names)
    if #names == 0 then
        return
    end
    local x = 12
    local y = ScrH() * 0.35
    draw.RoundedBox(4, x, y, 190, 26 + #names * 15, Color(18, 18, 18, 205))
    draw.SimpleText("enabled", "KirkwareLinuxText", x + 8, y + 6,
                    Color(255, 255, 255), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    for index, name in ipairs(names) do
        draw.SimpleText(name, "KirkwareLinuxSmall", x + 8,
                        y + 24 + (index - 1) * 15,
                        Color(185, 185, 185), TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    end
end

hook.Add("HUDPaint", "KirkwareLinux.ExtendedVisuals", function()
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) then
        return
    end
    drawPlayerMetadata(localPlayer)
    drawOffscreenArrows(localPlayer)
    drawEntityExtras(localPlayer)
    drawEnabledModules()

    if enabled("esp_player_hit_notify") and CurTime() < hitMarkerUntil then
        local x = math.floor(ScrW() * 0.5)
        local y = math.floor(ScrH() * 0.5)
        surface.SetDrawColor(255, 255, 255, 235)
        surface.DrawLine(x - 9, y - 9, x - 3, y - 3)
        surface.DrawLine(x + 9, y - 9, x + 3, y - 3)
        surface.DrawLine(x - 9, y + 9, x - 3, y + 3)
        surface.DrawLine(x + 9, y + 9, x + 3, y + 3)
    end
end)

gameevent.Listen("player_hurt")
hook.Add("player_hurt", "KirkwareLinux.HitFeedback", function(data)
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) or data.attacker ~= localPlayer:UserID() or
       data.userid == localPlayer:UserID() then
        return
    end
    if enabled("esp_player_hit_notify") then
        hitMarkerUntil = CurTime() + 0.18
    end
    if enabled("esp_player_hitsound") then
        surface.PlaySound("buttons/button15.wav")
    end
end)

hook.Add("Think", "KirkwareLinux.FreecamMove", function()
    if not enabled("misc_freecam") then
        freecamPosition = nil
        return
    end
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) or (IsValid(KW.Frame) and KW.Frame:IsVisible()) then
        return
    end
    if not freecamPosition then
        freecamPosition = localPlayer:EyePos()
    end

    local angle = EyeAngles()
    local speed = freecamSpeed:GetFloat() * FrameTime()
    if input.IsKeyDown(KEY_LSHIFT) or input.IsKeyDown(KEY_RSHIFT) then
        speed = speed * freecamBoost:GetFloat()
    end
    if input.IsKeyDown(KEY_W) then freecamPosition = freecamPosition + angle:Forward() * speed end
    if input.IsKeyDown(KEY_S) then freecamPosition = freecamPosition - angle:Forward() * speed end
    if input.IsKeyDown(KEY_D) then freecamPosition = freecamPosition + angle:Right() * speed end
    if input.IsKeyDown(KEY_A) then freecamPosition = freecamPosition - angle:Right() * speed end
    if input.IsKeyDown(KEY_SPACE) then freecamPosition = freecamPosition + Vector(0, 0, speed) end
    if input.IsKeyDown(KEY_LCONTROL) or input.IsKeyDown(KEY_RCONTROL) then
        freecamPosition = freecamPosition - Vector(0, 0, speed)
    end
end)

-- Compose freecam with the earlier third-person/FOV implementation.
hook.Remove("CalcView", "KirkwareLinux.Camera")
hook.Add("CalcView", "KirkwareLinux.Camera", function(playerEntity, position, angles, fov)
    if not IsValid(playerEntity) then
        return
    end

    local fovConVar = GetConVar("kirkware_fov")
    local zoomConVar = GetConVar("kirkware_zoom_fov")
    local thirdpersonConVar = GetConVar("kirkware_thirdperson_distance")
    local targetFov = fov
    if enabled("misc_zoom") and zoomConVar then
        targetFov = zoomConVar:GetFloat()
    elseif enabled("misc_fov_changer") and fovConVar then
        targetFov = fovConVar:GetFloat()
    end

    if enabled("misc_freecam") then
        freecamPosition = freecamPosition or position
        return {
            origin = freecamPosition,
            angles = angles,
            fov = targetFov,
            drawviewer = true,
        }
    end

    if enabled("misc_thirdperson") then
        local distance = thirdpersonConVar and thirdpersonConVar:GetFloat() or 110
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

hook.Add("CreateMove", "KirkwareLinux.ExtendedMovement", function(command)
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) or not localPlayer:Alive() then
        return
    end
    if IsValid(KW.Frame) and KW.Frame:IsVisible() then
        return
    end

    if enabled("misc_freecam") then
        command:SetForwardMove(0)
        command:SetSideMove(0)
        command:SetUpMove(0)
        command:RemoveKey(IN_FORWARD)
        command:RemoveKey(IN_BACK)
        command:RemoveKey(IN_MOVELEFT)
        command:RemoveKey(IN_MOVERIGHT)
        command:RemoveKey(IN_JUMP)
        command:RemoveKey(IN_DUCK)
    elseif enabled("misc_faststop") and localPlayer:OnGround() and
           not command:KeyDown(IN_FORWARD) and not command:KeyDown(IN_BACK) and
           not command:KeyDown(IN_MOVELEFT) and not command:KeyDown(IN_MOVERIGHT) then
        local velocity = localPlayer:GetVelocity()
        local flatVelocity = Vector(velocity.x, velocity.y, 0)
        if flatVelocity:Length2D() > 5 then
            local angle = command:GetViewAngles()
            command:SetForwardMove(math.Clamp(-flatVelocity:Dot(angle:Forward()), -450, 450))
            command:SetSideMove(math.Clamp(-flatVelocity:Dot(angle:Right()), -450, 450))
        end
    end

    if enabled("misc_use_spam") and command:CommandNumber() ~= 0 and
       command:CommandNumber() % 2 == 1 then
        command:AddKey(IN_USE)
    end
end)

print("[kirkware linux] extended visual/misc modules loaded")
