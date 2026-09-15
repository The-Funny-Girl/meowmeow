-- Persistent per-player state for the supported Kirkware Linux addon.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local dataDirectory = "kirkware_linux"
local dataFile = dataDirectory .. "/players.json"

KW.PlayerRules = KW.PlayerRules or {}

local validRules = {
    normal = true,
    ignore = true,
    priority = true,
    friend = true,
}
local ruleOrder = {"normal", "ignore", "priority", "friend"}

local function playerKey(playerEntity)
    if not IsValid(playerEntity) or not playerEntity:IsPlayer() then
        return nil
    end
    local steamId = playerEntity:SteamID64()
    if not steamId or steamId == "" or steamId == "0" then
        return "uid:" .. tostring(playerEntity:UserID())
    end
    return steamId
end

local function loadRules()
    file.CreateDir(dataDirectory)
    local raw = file.Read(dataFile, "DATA")
    if not raw or raw == "" then
        return
    end
    local decoded = util.JSONToTable(raw)
    if not istable(decoded) then
        return
    end
    for key, rule in pairs(decoded) do
        if isstring(key) and validRules[rule] then
            KW.PlayerRules[key] = rule
        end
    end
end

local function saveRules()
    file.CreateDir(dataDirectory)
    file.Write(dataFile, util.TableToJSON(KW.PlayerRules, true))
end

function KW.GetPlayerRule(playerEntity)
    local key = playerKey(playerEntity)
    if not key then
        return "normal"
    end
    local rule = KW.PlayerRules[key]
    return validRules[rule] and rule or "normal"
end

function KW.SetPlayerRule(playerEntity, rule)
    local key = playerKey(playerEntity)
    if not key or not validRules[rule] then
        return false
    end
    if rule == "normal" then
        KW.PlayerRules[key] = nil
    else
        KW.PlayerRules[key] = rule
    end
    saveRules()
    hook.Run("KirkwareLinuxPlayerRuleChanged", playerEntity, rule)
    if KW.RebuildModuleList then
        KW.RebuildModuleList()
    end
    return true
end

function KW.CyclePlayerRule(playerEntity)
    local current = KW.GetPlayerRule(playerEntity)
    local index = 1
    for position, rule in ipairs(ruleOrder) do
        if rule == current then
            index = position
            break
        end
    end
    local nextRule = ruleOrder[index % #ruleOrder + 1]
    KW.SetPlayerRule(playerEntity, nextRule)
    return nextRule
end

function KW.PlayerRuleColor(rule)
    if rule == "ignore" then
        return Color(150, 150, 150)
    elseif rule == "priority" then
        return Color(255, 105, 105)
    elseif rule == "friend" then
        return Color(90, 210, 130)
    end
    return Color(43, 151, 250)
end

concommand.Add("kirkware_player_rules_clear", function()
    KW.PlayerRules = {}
    saveRules()
    if KW.RebuildModuleList then
        KW.RebuildModuleList()
    end
end)

loadRules()
print("[kirkware linux] persistent player rules loaded")
