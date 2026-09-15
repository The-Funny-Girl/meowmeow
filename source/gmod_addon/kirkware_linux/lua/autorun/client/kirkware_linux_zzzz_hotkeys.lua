-- Persistent per-module hotkeys for the supported Kirkware Linux addon.
-- Insert remains reserved for opening/closing the menu.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local dataDirectory = "kirkware_linux"
local dataFile = dataDirectory .. "/binds.json"

KW.ModuleBinds = KW.ModuleBinds or {}
local wasDown = {}

local function saveBinds()
    file.CreateDir(dataDirectory)
    file.Write(dataFile, util.TableToJSON(KW.ModuleBinds, true))
end

local function loadBinds()
    file.CreateDir(dataDirectory)
    local raw = file.Read(dataFile, "DATA")
    if not raw or raw == "" then
        return
    end
    local decoded = util.JSONToTable(raw)
    if not istable(decoded) then
        return
    end
    for moduleId, code in pairs(decoded) do
        code = tonumber(code)
        if isstring(moduleId) and code and code >= 0 and code ~= KEY_INSERT then
            KW.ModuleBinds[moduleId] = code
        end
    end
end

function KW.SetModuleBind(moduleId, code)
    if not KW.Modules[moduleId] then
        return false, "unknown module"
    end
    code = tonumber(code)
    if code == nil or code < 0 then
        KW.ModuleBinds[moduleId] = nil
        wasDown[moduleId] = nil
        saveBinds()
        return true
    end
    if code == KEY_INSERT then
        return false, "Insert is reserved for the Kirkware menu"
    end
    KW.ModuleBinds[moduleId] = code
    wasDown[moduleId] = false
    saveBinds()
    return true
end

function KW.GetModuleBind(moduleId)
    return KW.ModuleBinds[moduleId]
end

function KW.GetModuleBindName(moduleId)
    local code = KW.ModuleBinds[moduleId]
    if code == nil then
        return "none"
    end
    return input.GetKeyName(code) or tostring(code)
end

concommand.Add("kirkware_bind", function(_, _, arguments)
    local moduleId = arguments[1]
    local keyName = string.lower(arguments[2] or "")
    if not moduleId or not KW.Modules[moduleId] or keyName == "" then
        print("usage: kirkware_bind <module_id> <key name|none>")
        return
    end

    if keyName == "none" or keyName == "clear" or keyName == "unbind" then
        KW.SetModuleBind(moduleId, -1)
        print("[kirkware linux] cleared bind for " .. moduleId)
        return
    end

    local code = input.GetKeyCode(keyName)
    if code == nil or code < 0 then
        print("[kirkware linux] unknown key name: " .. keyName)
        return
    end
    local ok, errorText = KW.SetModuleBind(moduleId, code)
    if not ok then
        print("[kirkware linux] unable to bind: " .. tostring(errorText))
        return
    end
    print(string.format("[kirkware linux] %s -> %s", moduleId,
                        KW.GetModuleBindName(moduleId)))
end)

concommand.Add("kirkware_binds", function()
    print("[kirkware linux] module hotkeys")
    local ids = {}
    for moduleId in pairs(KW.ModuleBinds) do
        ids[#ids + 1] = moduleId
    end
    table.sort(ids)
    if #ids == 0 then
        print("  no module hotkeys configured")
        return
    end
    for _, moduleId in ipairs(ids) do
        print(string.format("  %s = %s", moduleId,
                            KW.GetModuleBindName(moduleId)))
    end
end)

concommand.Add("kirkware_binds_clear", function()
    KW.ModuleBinds = {}
    wasDown = {}
    saveBinds()
    print("[kirkware linux] cleared all module hotkeys")
end)

hook.Add("Think", "KirkwareLinux.ModuleHotkeys", function()
    if IsValid(KW.Frame) and KW.Frame:IsVisible() then
        return
    end

    for moduleId, code in pairs(KW.ModuleBinds) do
        if KW.Modules[moduleId] and code ~= KEY_INSERT then
            local down = input.IsButtonDown(code)
            if down and not wasDown[moduleId] and KW.SetModuleEnabled then
                KW.SetModuleEnabled(moduleId, not KW.ModuleEnabled(moduleId))
            end
            wasDown[moduleId] = down
        end
    end
end)

loadBinds()
print("[kirkware linux] module hotkeys loaded")
