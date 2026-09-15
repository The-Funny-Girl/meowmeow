-- Supported navmesh path walking for Kirkware Linux.
-- This consumes an existing map navmesh; it never generates/edits nav data.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"

local modules = {
    misc_pathfinder_enable = {
        name = "pathfinder",
        description = "allow navmesh route calculation and walking",
        category = "misc",
        default = false,
    },
    misc_pathfinder_walk_path = {
        name = "walk path",
        description = "automatically follow the current navmesh route",
        category = "misc",
        default = false,
    },
    misc_pathfinder_visualize = {
        name = "draw path",
        description = "draw the current navmesh route in the world",
        category = "visuals",
        default = true,
    },
    misc_pathfinder_aim = {
        name = "path aim",
        description = "turn the view toward the next route point while walking",
        category = "misc",
        default = false,
    },
    misc_pathfinder_jump = {
        name = "path jump",
        description = "jump when the next path point is substantially higher",
        category = "misc",
        default = true,
    },
    misc_pathfinder_crouch = {
        name = "path crouch",
        description = "honor crouch nav-area attributes when available",
        category = "misc",
        default = true,
    },
    misc_pathfinder_run = {
        name = "path run",
        description = "run instead of using the normal walk modifier",
        category = "misc",
        default = true,
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

local goalTolerance = CreateClientConVar(
    "kirkware_path_tolerance", "45", true, false,
    "Distance at which a path node is considered reached", 15, 200)
local maxSearchAreas = CreateClientConVar(
    "kirkware_path_max_areas", "4096", true, false,
    "Maximum nav areas inspected by one path search", 64, 20000)

local route = {}
local routeAreas = {}
local routeIndex = 1
local goalPosition = nil
local routeStatus = "none"

local function navmeshAvailable()
    return navmesh ~= nil and navmesh.IsLoaded ~= nil and
           navmesh.GetNearestNavArea ~= nil
end

local function areaValid(area)
    return area ~= nil and area.GetID ~= nil and area:GetID() ~= 0
end

local function heuristic(area, goal)
    return area:GetCenter():Distance(goal:GetCenter())
end

local function reconstruct(cameFrom, byId, currentId)
    local reversed = {}
    while currentId do
        local area = byId[currentId]
        if not area then
            break
        end
        reversed[#reversed + 1] = area
        currentId = cameFrom[currentId]
    end

    local areas = {}
    for index = #reversed, 1, -1 do
        areas[#areas + 1] = reversed[index]
    end
    return areas
end

local function findLowest(openSet, fScore)
    local bestId = nil
    local bestScore = math.huge
    for id in pairs(openSet) do
        local score = fScore[id] or math.huge
        if score < bestScore then
            bestScore = score
            bestId = id
        end
    end
    return bestId
end

local function aStar(startArea, goalArea)
    local startId = startArea:GetID()
    local goalId = goalArea:GetID()
    local openSet = {[startId] = true}
    local cameFrom = {}
    local byId = {[startId] = startArea, [goalId] = goalArea}
    local gScore = {[startId] = 0}
    local fScore = {[startId] = heuristic(startArea, goalArea)}
    local inspected = 0
    local maximum = math.floor(maxSearchAreas:GetFloat())

    while next(openSet) ~= nil and inspected < maximum do
        inspected = inspected + 1
        local currentId = findLowest(openSet, fScore)
        if not currentId then
            break
        end
        local current = byId[currentId]
        if currentId == goalId then
            return reconstruct(cameFrom, byId, currentId), inspected
        end
        openSet[currentId] = nil

        for _, neighbor in ipairs(current:GetAdjacentAreas()) do
            if areaValid(neighbor) and not neighbor:IsBlocked() then
                local neighborId = neighbor:GetID()
                byId[neighborId] = neighbor
                local stepCost = current:GetCenter():Distance(neighbor:GetCenter())
                local tentative = (gScore[currentId] or math.huge) + stepCost
                if tentative < (gScore[neighborId] or math.huge) then
                    cameFrom[neighborId] = currentId
                    gScore[neighborId] = tentative
                    fScore[neighborId] = tentative + heuristic(neighbor, goalArea)
                    openSet[neighborId] = true
                end
            end
        end
    end

    return nil, inspected
end

local function buildRoute(destination)
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) then
        return false, "local player is unavailable"
    end
    if not navmeshAvailable() then
        routeStatus = "navmesh api unavailable"
        return false, routeStatus
    end
    if not navmesh.IsLoaded() then
        routeStatus = "no loaded navmesh"
        return false, "this map has no loaded navmesh"
    end

    local startArea = navmesh.GetNearestNavArea(localPlayer:GetPos(), false, 5000,
                                                false, true)
    local goalArea = navmesh.GetNearestNavArea(destination, false, 5000,
                                               false, true)
    if not areaValid(startArea) or not areaValid(goalArea) then
        return false, "unable to resolve start/goal nav areas"
    end

    local areas, inspected = aStar(startArea, goalArea)
    if not areas or #areas == 0 then
        route = {}
        routeAreas = {}
        routeIndex = 1
        routeStatus = "no route"
        return false, "no route found after inspecting " .. tostring(inspected) .. " areas"
    end

    routeAreas = areas
    route = {}
    for _, area in ipairs(areas) do
        route[#route + 1] = area:GetCenter()
    end
    route[#route + 1] = destination
    routeIndex = math.min(2, #route)
    goalPosition = destination
    routeStatus = string.format("ready (%d nodes)", #route)
    return true, routeStatus
end

local function setGoalFromCrosshair()
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) then
        return false, "local player is unavailable"
    end
    local trace = localPlayer:GetEyeTrace()
    if not trace or not trace.Hit then
        return false, "crosshair did not hit the world"
    end
    return buildRoute(trace.HitPos)
end

local function clearRoute()
    route = {}
    routeAreas = {}
    routeIndex = 1
    goalPosition = nil
    routeStatus = "none"
end

KW.PathfinderSetGoal = buildRoute
KW.PathfinderClear = clearRoute
KW.PathfinderRoute = function()
    return route, routeIndex, goalPosition, routeStatus
end

concommand.Add("kirkware_path_set", function()
    if not enabled("misc_pathfinder_enable") then
        print("[kirkware linux] enable the pathfinder module first")
        return
    end
    local ok, detail = setGoalFromCrosshair()
    print("[kirkware linux] path: " .. tostring(detail))
    if ok and KW.SetModuleEnabled then
        KW.SetModuleEnabled("misc_pathfinder_walk_path", true)
    end
end)

concommand.Add("kirkware_path_clear", function()
    clearRoute()
    if KW.SetModuleEnabled then
        KW.SetModuleEnabled("misc_pathfinder_walk_path", false)
    end
    print("[kirkware linux] path cleared")
end)

concommand.Add("kirkware_path_status", function()
    print(string.format("[kirkware linux] path status=%s node=%d/%d",
                        routeStatus, routeIndex, #route))
end)

hook.Add("CreateMove", "KirkwareLinux.PathfinderWalk", function(command)
    if not enabled("misc_pathfinder_enable") or
       not enabled("misc_pathfinder_walk_path") or #route == 0 then
        return
    end
    if IsValid(KW.Frame) and KW.Frame:IsVisible() then
        return
    end

    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) or not localPlayer:Alive() then
        return
    end

    local tolerance = goalTolerance:GetFloat()
    local position = localPlayer:GetPos()
    while routeIndex <= #route and
          position:DistToSqr(route[routeIndex]) <= tolerance * tolerance do
        routeIndex = routeIndex + 1
    end

    if routeIndex > #route then
        routeStatus = "arrived"
        if KW.SetModuleEnabled then
            KW.SetModuleEnabled("misc_pathfinder_walk_path", false)
        end
        return
    end

    local target = route[routeIndex]
    local delta = target - position
    local flat = Vector(delta.x, delta.y, 0)
    if flat:Length2D() < 1 then
        return
    end
    flat:Normalize()

    local view = command:GetViewAngles()
    local forward = view:Forward()
    forward.z = 0
    forward:Normalize()
    local right = view:Right()
    right.z = 0
    right:Normalize()

    command:SetForwardMove(math.Clamp(flat:Dot(forward) * 450, -450, 450))
    command:SetSideMove(math.Clamp(flat:Dot(right) * 450, -450, 450))

    if enabled("misc_pathfinder_aim") then
        local desired = delta:Angle()
        desired.p = math.Clamp(desired.p, -30, 30)
        desired.y = math.NormalizeAngle(desired.y)
        desired.r = 0
        command:SetViewAngles(desired)
    end

    if enabled("misc_pathfinder_jump") and delta.z > 28 and localPlayer:OnGround() then
        command:AddKey(IN_JUMP)
    end

    local area = routeAreas[math.min(routeIndex, #routeAreas)]
    if enabled("misc_pathfinder_crouch") and areaValid(area) and
       area:HasAttributes(NAV_MESH_CROUCH) then
        command:AddKey(IN_DUCK)
    end

    if enabled("misc_pathfinder_run") then
        command:RemoveKey(IN_WALK)
    else
        command:AddKey(IN_WALK)
    end
end)

hook.Add("PostDrawTranslucentRenderables", "KirkwareLinux.PathfinderVisualize",
         function(_, drawingSkybox)
    if drawingSkybox or not enabled("misc_pathfinder_enable") or
       not enabled("misc_pathfinder_visualize") or #route == 0 then
        return
    end

    for index = math.max(1, routeIndex - 1), #route - 1 do
        local first = route[index] + Vector(0, 0, 6)
        local second = route[index + 1] + Vector(0, 0, 6)
        local color = index < routeIndex and Color(100, 100, 100, 160)
                                           or Color(43, 151, 250, 230)
        render.DrawLine(first, second, color, true)
    end
end)

print("[kirkware linux] navmesh pathfinder loaded")
