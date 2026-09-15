-- Supported combat/aiming modules for the Kirkware Linux Garry's Mod addon.
-- Uses only normal client Lua APIs. No injection, remote hooks, net exploits,
-- anti-cheat bypasses, or server-setting bypasses are used here.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"

local combatModules = {
    legit_enable = {
        name = "legit aim",
        description = "smooth aim assistance while attacking",
        category = "aim",
        default = false,
    },
    legit_fov_circle = {
        name = "legit fov circle",
        description = "show the legit aim field of view",
        category = "aim",
        default = true,
    },
    legit_hitscan = {
        name = "legit hitscan",
        description = "choose the best visible head/chest/pelvis point",
        category = "aim",
        default = false,
    },
    legit_triggerbot = {
        name = "triggerbot",
        description = "fire when the crosshair is over a valid player",
        category = "aim",
        default = false,
    },
    legit_triggerbot_ammo = {
        name = "triggerbot ammo check",
        description = "do not trigger an empty magazine",
        category = "aim",
        default = true,
    },
    legit_triggerbot_canshoot = {
        name = "triggerbot fire-ready check",
        description = "wait until the active weapon can primary fire",
        category = "aim",
        default = true,
    },
    legit_recoil = {
        name = "legit recoil compensation",
        description = "compensate view punch while legit aim is active",
        category = "aim",
        default = true,
    },
    legit_visible_check = {
        name = "legit visible check",
        description = "only select visible targets for legit aim",
        category = "aim",
        default = true,
    },
    rage_enable = {
        name = "rage aim",
        description = "select the best target inside the rage fov",
        category = "aim",
        default = false,
    },
    rage_autofire = {
        name = "rage autofire",
        description = "fire automatically while rage aim has a target",
        category = "aim",
        default = false,
    },
    rage_autofire_ammo = {
        name = "rage ammo check",
        description = "do not autofire an empty magazine",
        category = "aim",
        default = true,
    },
    rage_can_fire = {
        name = "rage fire-ready check",
        description = "wait until the active weapon can primary fire",
        category = "aim",
        default = true,
    },
    rage_fix_movement = {
        name = "rage movement correction",
        description = "preserve intended movement when aim changes view angle",
        category = "aim",
        default = true,
    },
    rage_fov_circle = {
        name = "rage fov circle",
        description = "show the rage aim field of view",
        category = "aim",
        default = false,
    },
    rage_hitscan = {
        name = "rage hitscan",
        description = "choose the best visible head/chest/pelvis point",
        category = "aim",
        default = true,
    },
    rage_norecoil = {
        name = "rage recoil compensation",
        description = "compensate view punch while rage aim is active",
        category = "aim",
        default = true,
    },
    rage_target_lock = {
        name = "target lock",
        description = "keep the current rage target while it remains valid",
        category = "aim",
        default = true,
    },
    rage_visible_check = {
        name = "rage visible check",
        description = "only select visible targets for rage aim",
        category = "aim",
        default = true,
    },
    esp_target_line = {
        name = "target line",
        description = "draw a line from screen center to the current aim target",
        category = "visuals",
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

for id, definition in pairs(combatModules) do
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

local legitFov = CreateClientConVar(
    "kirkware_legit_fov", "6", true, false,
    "Legit aim field of view in degrees", 0.25, 45)
local legitSmoothing = CreateClientConVar(
    "kirkware_legit_smoothing", "8", true, false,
    "Legit aim smoothing factor", 1, 40)
local legitMaxDistance = CreateClientConVar(
    "kirkware_legit_max_distance", "10000", true, false,
    "Maximum legit aim target distance", 100, 50000)
local legitSort = CreateClientConVar(
    "kirkware_legit_sort", "0", true, false,
    "Legit target sort: 0=fov, 1=distance, 2=health", 0, 2)
local triggerDelay = CreateClientConVar(
    "kirkware_trigger_delay", "0.03", true, false,
    "Triggerbot delay in seconds", 0, 1)
local rageFov = CreateClientConVar(
    "kirkware_rage_fov", "35", true, false,
    "Rage aim field of view in degrees", 1, 180)
local rageMaxDistance = CreateClientConVar(
    "kirkware_rage_max_distance", "20000", true, false,
    "Maximum rage aim target distance", 100, 50000)
local rageSort = CreateClientConVar(
    "kirkware_rage_sort", "0", true, false,
    "Rage target sort: 0=fov, 1=distance, 2=health", 0, 2)
local aimTeammates = CreateClientConVar(
    "kirkware_aim_teammates", "0", true, false,
    "Allow aim modules to target teammates", 0, 1)
local legitRequireAttack = CreateClientConVar(
    "kirkware_legit_require_attack", "1", true, false,
    "Only run legit aim while primary attack is held", 0, 1)

local currentTarget = nil
local currentTargetPosition = nil
local lockedRageTarget = nil
local nextTriggerTime = 0

local aimBones = {
    "ValveBiped.Bip01_Head1",
    "ValveBiped.Bip01_Spine2",
    "ValveBiped.Bip01_Pelvis",
}

local function bonePosition(playerEntity, boneName)
    local bone = playerEntity:LookupBone(boneName)
    if bone == nil then
        return nil
    end
    local matrix = playerEntity:GetBoneMatrix(bone)
    if matrix then
        return matrix:GetTranslation()
    end
    return playerEntity:GetBonePosition(bone)
end

local function targetPosition(playerEntity)
    return bonePosition(playerEntity, aimBones[1]) or playerEntity:EyePos()
end

local function angularDistance(fromAngle, toAngle)
    local pitch = math.AngleDifference(toAngle.p, fromAngle.p)
    local yaw = math.AngleDifference(toAngle.y, fromAngle.y)
    return math.sqrt(pitch * pitch + yaw * yaw)
end

local function visibleTo(localPlayer, target, position)
    local trace = util.TraceLine({
        start = localPlayer:GetShootPos(),
        endpos = position,
        filter = localPlayer,
        mask = MASK_SHOT,
    })
    return trace.Fraction >= 0.995 or trace.Entity == target
end

local function playerRuleWeight(target)
    if KW.PlayerTargetWeight then
        return KW.PlayerTargetWeight(target)
    end
    return 0
end

local function basicTargetAllowed(localPlayer, target, maxDistance)
    if not IsValid(target) or target == localPlayer or not target:IsPlayer() then
        return false
    end
    if not target:Alive() or target:GetObserverMode() ~= OBS_MODE_NONE then
        return false
    end
    if playerRuleWeight(target) == nil then
        return false
    end
    if aimTeammates:GetBool() == false and localPlayer:Team() == target:Team() then
        return false
    end
    if localPlayer:GetShootPos():DistToSqr(target:GetPos()) > maxDistance * maxDistance then
        return false
    end
    return true
end

local function bestPointForTarget(localPlayer, target, viewAngles, maximumFov,
                                  requireVisible, useHitscan)
    local shootPosition = localPlayer:GetShootPos()
    local positions = {}
    if useHitscan then
        for _, boneName in ipairs(aimBones) do
            local position = bonePosition(target, boneName)
            if position then
                positions[#positions + 1] = position
            end
        end
    else
        positions[1] = targetPosition(target)
    end

    local bestPosition = nil
    local bestFov = maximumFov
    for _, position in ipairs(positions) do
        if not requireVisible or visibleTo(localPlayer, target, position) then
            local desired = (position - shootPosition):Angle()
            local delta = angularDistance(viewAngles, desired)
            if delta <= bestFov then
                bestPosition = position
                bestFov = delta
            end
        end
    end
    return bestPosition, bestFov
end

local function targetScore(localPlayer, target, fovDelta, sortMode)
    local score = fovDelta
    if sortMode == 1 then
        score = localPlayer:GetShootPos():Distance(target:GetPos())
    elseif sortMode == 2 then
        score = math.max(0, target:Health())
    end

    local ruleWeight = playerRuleWeight(target) or 0
    if ruleWeight < 0 then
        score = score - 1000000000
    end
    return score
end

local function bestTarget(localPlayer, viewAngles, maximumFov, maxDistance,
                          requireVisible, useHitscan, sortMode)
    local best = nil
    local bestPosition = nil
    local bestScore = math.huge
    local bestFov = maximumFov

    for _, candidate in ipairs(player.GetAll()) do
        if basicTargetAllowed(localPlayer, candidate, maxDistance) then
            local position, delta = bestPointForTarget(
                localPlayer, candidate, viewAngles, maximumFov,
                requireVisible, useHitscan)
            if position then
                local score = targetScore(localPlayer, candidate, delta, sortMode)
                if score < bestScore then
                    best = candidate
                    bestPosition = position
                    bestFov = delta
                    bestScore = score
                end
            end
        end
    end

    return best, bestPosition, bestFov
end

local function compensatedAim(localPlayer, desired, compensate)
    local output = Angle(desired.p, desired.y, 0)
    if compensate then
        local punch = localPlayer:GetViewPunchAngles()
        output.p = output.p - punch.p
        output.y = output.y - punch.y
    end
    output.p = math.Clamp(output.p, -89, 89)
    output.y = math.NormalizeAngle(output.y)
    return output
end

local function smoothAim(current, desired, smoothing)
    local factor = math.Clamp(1 / math.max(1, smoothing), 0.025, 1)
    local pitch = current.p + math.AngleDifference(desired.p, current.p) * factor
    local yaw = current.y + math.AngleDifference(desired.y, current.y) * factor
    return Angle(math.Clamp(pitch, -89, 89), math.NormalizeAngle(yaw), 0)
end

local function weaponReady(localPlayer, requireAmmo, requireCooldown)
    local weapon = localPlayer:GetActiveWeapon()
    if not IsValid(weapon) then
        return false
    end

    if requireAmmo then
        local maximum = weapon:GetMaxClip1()
        if maximum and maximum > 0 and weapon:Clip1() <= 0 then
            return false
        end
    end

    if requireCooldown and weapon:GetNextPrimaryFire() > CurTime() then
        return false
    end
    return true
end

local function correctMovement(command, oldAngles, newAngles)
    local oldForward = oldAngles:Forward()
    local oldRight = oldAngles:Right()
    oldForward.z = 0
    oldRight.z = 0
    oldForward:Normalize()
    oldRight:Normalize()

    local worldMove = oldForward * command:GetForwardMove() +
                      oldRight * command:GetSideMove()

    local newForward = newAngles:Forward()
    local newRight = newAngles:Right()
    newForward.z = 0
    newRight.z = 0
    newForward:Normalize()
    newRight:Normalize()

    command:SetForwardMove(math.Clamp(worldMove:Dot(newForward), -10000, 10000))
    command:SetSideMove(math.Clamp(worldMove:Dot(newRight), -10000, 10000))
end

local function validLockedRageTarget(localPlayer, viewAngles)
    if not enabled("rage_target_lock") or
       not basicTargetAllowed(localPlayer, lockedRageTarget,
                              rageMaxDistance:GetFloat()) then
        return nil, nil
    end

    local position = bestPointForTarget(
        localPlayer,
        lockedRageTarget,
        viewAngles,
        rageFov:GetFloat(),
        enabled("rage_visible_check"),
        enabled("rage_hitscan"))
    if not position then
        return nil, nil
    end
    return lockedRageTarget, position
end

hook.Add("CreateMove", "KirkwareLinux.CombatModules", function(command)
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) or not localPlayer:Alive() then
        currentTarget = nil
        currentTargetPosition = nil
        lockedRageTarget = nil
        return
    end
    if IsValid(KW.Frame) and KW.Frame:IsVisible() then
        currentTarget = nil
        currentTargetPosition = nil
        return
    end

    local viewAngles = command:GetViewAngles()
    local target = nil
    local position = nil

    if enabled("rage_enable") then
        target, position = validLockedRageTarget(localPlayer, viewAngles)
        if not IsValid(target) then
            target, position = bestTarget(
                localPlayer,
                viewAngles,
                rageFov:GetFloat(),
                rageMaxDistance:GetFloat(),
                enabled("rage_visible_check"),
                enabled("rage_hitscan"),
                math.floor(rageSort:GetFloat()))
        end

        if IsValid(target) and position then
            lockedRageTarget = target
            local desired = compensatedAim(
                localPlayer,
                (position - localPlayer:GetShootPos()):Angle(),
                enabled("rage_norecoil"))
            if enabled("rage_fix_movement") then
                correctMovement(command, viewAngles, desired)
            end
            command:SetViewAngles(desired)
            if enabled("rage_autofire") and
               weaponReady(localPlayer,
                           enabled("rage_autofire_ammo"),
                           enabled("rage_can_fire")) then
                command:AddKey(IN_ATTACK)
            end
        else
            lockedRageTarget = nil
        end
    elseif enabled("legit_enable") and
           (not legitRequireAttack:GetBool() or command:KeyDown(IN_ATTACK)) then
        target, position = bestTarget(
            localPlayer,
            viewAngles,
            legitFov:GetFloat(),
            legitMaxDistance:GetFloat(),
            enabled("legit_visible_check"),
            enabled("legit_hitscan"),
            math.floor(legitSort:GetFloat()))

        if IsValid(target) and position then
            local desired = compensatedAim(
                localPlayer,
                (position - localPlayer:GetShootPos()):Angle(),
                enabled("legit_recoil"))
            command:SetViewAngles(
                smoothAim(viewAngles, desired, legitSmoothing:GetFloat()))
        end
        lockedRageTarget = nil
    else
        lockedRageTarget = nil
    end

    currentTarget = IsValid(target) and target or nil
    currentTargetPosition = position

    if enabled("legit_triggerbot") and CurTime() >= nextTriggerTime then
        local direction = command:GetViewAngles():Forward()
        local trace = util.TraceLine({
            start = localPlayer:GetShootPos(),
            endpos = localPlayer:GetShootPos() + direction * 32768,
            filter = localPlayer,
            mask = MASK_SHOT,
        })
        local hit = trace.Entity
        if IsValid(hit) and hit:IsPlayer() and hit:Alive() and
           basicTargetAllowed(localPlayer, hit, legitMaxDistance:GetFloat()) and
           weaponReady(localPlayer,
                       enabled("legit_triggerbot_ammo"),
                       enabled("legit_triggerbot_canshoot")) then
            command:AddKey(IN_ATTACK)
            nextTriggerTime = CurTime() + triggerDelay:GetFloat()
        end
    end
end)

local function fovRadius(degrees)
    local localPlayer = LocalPlayer()
    local cameraFov = IsValid(localPlayer) and localPlayer:GetFOV() or 90
    cameraFov = math.Clamp(cameraFov, 1, 179)
    local ratio = math.tan(math.rad(degrees) * 0.5) /
                  math.tan(math.rad(cameraFov) * 0.5)
    return math.Clamp(ratio * ScrW() * 0.5, 2, ScrW())
end

local function drawCircle(x, y, radius, segments, color)
    surface.SetDrawColor(color)
    local previousX = x + radius
    local previousY = y
    for index = 1, segments do
        local angle = math.rad(index / segments * 360)
        local nextX = x + math.cos(angle) * radius
        local nextY = y + math.sin(angle) * radius
        surface.DrawLine(previousX, previousY, nextX, nextY)
        previousX = nextX
        previousY = nextY
    end
end

hook.Add("HUDPaint", "KirkwareLinux.CombatVisuals", function()
    local centerX = math.floor(ScrW() * 0.5)
    local centerY = math.floor(ScrH() * 0.5)

    if enabled("legit_enable") and enabled("legit_fov_circle") then
        drawCircle(centerX, centerY, fovRadius(legitFov:GetFloat()), 72,
                   Color(100, 190, 255, 190))
    end
    if enabled("rage_enable") and enabled("rage_fov_circle") then
        drawCircle(centerX, centerY, fovRadius(rageFov:GetFloat()), 72,
                   Color(255, 115, 115, 190))
    end

    if enabled("esp_target_line") and IsValid(currentTarget) then
        local worldPosition = currentTargetPosition or targetPosition(currentTarget)
        local position = worldPosition:ToScreen()
        if position.visible ~= false then
            surface.SetDrawColor(255, 105, 105, 220)
            surface.DrawLine(centerX, centerY, position.x, position.y)
        end
    end
end)

KW.CombatTarget = function()
    return currentTarget
end

print("[kirkware linux] combat modules loaded")
