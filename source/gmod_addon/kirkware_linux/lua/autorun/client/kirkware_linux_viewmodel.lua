-- Supported viewmodel and tracer visuals for Kirkware Linux.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"
local modules = {
    chams_hands_enable = {
        name = "hand chams",
        description = "apply a local material tint to first-person hands",
        category = "visuals",
        default = false,
    },
    chams_weapon_enable = {
        name = "weapon chams",
        description = "apply a local material tint to the first-person weapon",
        category = "visuals",
        default = false,
    },
    esp_other_tracer = {
        name = "bullet tracers",
        description = "draw short-lived local bullet trajectory lines",
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

local debugMaterial = Material("models/debug/debugwhite")
local tracerLifetime = CreateClientConVar(
    "kirkware_tracer_time", "0.8", true, false,
    "Kirkware local bullet tracer lifetime", 0.05, 5)
local tracers = {}

hook.Add("PreDrawViewModel", "KirkwareLinux.WeaponChams", function()
    if not enabled("chams_weapon_enable") then
        return
    end
    render.MaterialOverride(debugMaterial)
    render.SetColorModulation(0.17, 0.59, 0.98)
end)

hook.Add("PostDrawViewModel", "KirkwareLinux.WeaponChamsReset", function()
    if not enabled("chams_weapon_enable") then
        return
    end
    render.MaterialOverride(nil)
    render.SetColorModulation(1, 1, 1)
end)

hook.Add("PreDrawPlayerHands", "KirkwareLinux.HandChams", function()
    if not enabled("chams_hands_enable") then
        return
    end
    render.MaterialOverride(debugMaterial)
    render.SetColorModulation(0.35, 0.8, 1)
end)

hook.Add("PostDrawPlayerHands", "KirkwareLinux.HandChamsReset", function()
    if not enabled("chams_hands_enable") then
        return
    end
    render.MaterialOverride(nil)
    render.SetColorModulation(1, 1, 1)
end)

hook.Add("PostEntityFireBullets", "KirkwareLinux.BulletTracerCapture", function(entity, data)
    if not enabled("esp_other_tracer") then
        return
    end
    local localPlayer = LocalPlayer()
    if not IsValid(localPlayer) then
        return
    end
    local attacker = data.Attacker
    if entity ~= localPlayer and attacker ~= localPlayer then
        return
    end
    if not data.Trace or not data.Trace.HitPos then
        return
    end

    tracers[#tracers + 1] = {
        start = data.Trace.StartPos or localPlayer:GetShootPos(),
        finish = data.Trace.HitPos,
        expire = CurTime() + tracerLifetime:GetFloat(),
    }
end)

hook.Add("PostDrawTranslucentRenderables", "KirkwareLinux.BulletTracerDraw",
         function(_, drawingSkybox)
    if drawingSkybox or not enabled("esp_other_tracer") then
        return
    end
    local now = CurTime()
    local nextTracers = {}
    for _, tracer in ipairs(tracers) do
        if tracer.expire > now then
            local alpha = math.Clamp((tracer.expire - now) /
                                     math.max(0.05, tracerLifetime:GetFloat()), 0, 1)
            render.DrawLine(tracer.start, tracer.finish,
                            Color(43, 151, 250, math.floor(alpha * 255)), true)
            nextTracers[#nextTracers + 1] = tracer
        end
    end
    tracers = nextTracers
end)

print("[kirkware linux] viewmodel/tracer modules loaded")
