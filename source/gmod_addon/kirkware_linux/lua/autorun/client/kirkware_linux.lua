-- Kirkware Linux in-game menu.
-- This client-side addon uses Garry's Mod's supported Lua/addon APIs only.

KIRKWARE_LINUX = KIRKWARE_LINUX or {}
local KW = KIRKWARE_LINUX

KW.Version = "1.0"
KW.Frame = KW.Frame or nil
KW.ActiveCategory = KW.ActiveCategory or "visuals"

local dataDirectory = "kirkware_linux"
local dataFile = dataDirectory .. "/modules.json"

local theme = {
    window = Color(18, 18, 18, 248),
    panel = Color(27, 27, 27, 255),
    panelHover = Color(35, 35, 35, 255),
    border = Color(53, 53, 53, 255),
    accent = Color(43, 151, 250, 255),
    text = Color(255, 255, 255, 255),
    muted = Color(153, 153, 153, 255),
    unsafe = Color(227, 126, 129, 255),
}

local modules = {
    menu_watermark = {
        name = "watermark",
        description = "show the kirkware linux watermark",
        category = "misc",
        default = true,
    },
    esp_other_crosshair = {
        name = "crosshair",
        description = "draw a client-side center crosshair",
        category = "visuals",
        default = false,
    },
    esp_player_enable = {
        name = "player visuals",
        description = "master switch for player visual modules",
        category = "visuals",
        default = false,
    },
    esp_player_name = {
        name = "player names",
        description = "show player names above player models",
        category = "visuals",
        default = true,
    },
    esp_player_box = {
        name = "player boxes",
        description = "draw simple 2D boxes around players",
        category = "visuals",
        default = true,
    },
    misc_thirdperson = {
        name = "third person",
        description = "use a collision-aware third-person camera",
        category = "misc",
        default = false,
    },
}

KW.Modules = modules
KW.Settings = KW.Settings or {}

local function copyDefaults()
    local result = {}
    for id, definition in pairs(modules) do
        result[id] = definition.default == true
    end
    return result
end

local function loadSettings()
    file.CreateDir(dataDirectory)
    local loaded = copyDefaults()
    local raw = file.Read(dataFile, "DATA")
    if raw and raw ~= "" then
        local decoded = util.JSONToTable(raw)
        if istable(decoded) then
            for id in pairs(modules) do
                if isbool(decoded[id]) then
                    loaded[id] = decoded[id]
                end
            end
        end
    end
    KW.Settings = loaded
end

local function saveSettings()
    file.CreateDir(dataDirectory)
    file.Write(dataFile, util.TableToJSON(KW.Settings, true))
end

local function settingEnabled(id)
    return KW.Settings[id] == true
end

local function setSetting(id, enabled)
    if not modules[id] then
        return
    end
    KW.Settings[id] = enabled == true
    saveSettings()
    hook.Run("KirkwareLinuxModuleChanged", id, KW.Settings[id])
end

KW.SetModuleEnabled = setSetting
KW.ModuleEnabled = settingEnabled

surface.CreateFont("KirkwareLinuxTitle", {
    font = "Tahoma",
    size = 18,
    weight = 600,
    antialias = true,
})

surface.CreateFont("KirkwareLinuxText", {
    font = "Tahoma",
    size = 14,
    weight = 500,
    antialias = true,
})

surface.CreateFont("KirkwareLinuxSmall", {
    font = "Tahoma",
    size = 12,
    weight = 500,
    antialias = true,
})

local function createModuleRow(parent, id, definition)
    local row = vgui.Create("DPanel", parent)
    row:Dock(TOP)
    row:SetTall(52)
    row:DockMargin(0, 0, 0, 7)
    row.Paint = function(self, width, height)
        local background = self:IsHovered() and theme.panelHover or theme.panel
        draw.RoundedBox(4, 0, 0, width, height, background)
        surface.SetDrawColor(theme.border)
        surface.DrawOutlinedRect(0, 0, width, height, 1)
        draw.SimpleText(definition.name, "KirkwareLinuxText", 12, 10,
                        theme.text, TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
        draw.SimpleText(definition.description, "KirkwareLinuxSmall", 12, 30,
                        theme.muted, TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    end

    local toggle = vgui.Create("DButton", row)
    toggle:SetText("")
    toggle:SetSize(46, 22)
    toggle:SetPos(0, 15)
    toggle.Think = function(self)
        local width = row:GetWide()
        self:SetPos(math.max(0, width - 58), 15)
    end
    toggle.Paint = function(self, width, height)
        local enabled = settingEnabled(id)
        draw.RoundedBox(height * 0.5, 0, 0, width, height,
                        enabled and theme.accent or Color(64, 64, 64, 255))
        local knob = height - 6
        local x = enabled and (width - knob - 3) or 3
        draw.RoundedBox(knob * 0.5, x, 3, knob, knob, theme.text)
    end
    toggle.DoClick = function()
        setSetting(id, not settingEnabled(id))
    end

    return row
end

local function rebuildModuleList(frame)
    if not IsValid(frame.ModuleList) then
        return
    end

    frame.ModuleList:Clear()
    local ids = {}
    for id, definition in pairs(modules) do
        if definition.category == KW.ActiveCategory then
            table.insert(ids, id)
        end
    end
    table.sort(ids, function(left, right)
        return modules[left].name < modules[right].name
    end)

    for _, id in ipairs(ids) do
        createModuleRow(frame.ModuleList, id, modules[id])
    end
end

local function createCategoryButton(parent, frame, category, label, y)
    local button = vgui.Create("DButton", parent)
    button:SetText("")
    button:SetPos(10, y)
    button:SetSize(112, 30)
    button.Paint = function(self, width, height)
        local selected = KW.ActiveCategory == category
        local background = selected and Color(43, 151, 250, 42)
                            or (self:IsHovered() and theme.panelHover or theme.panel)
        draw.RoundedBox(4, 0, 0, width, height, background)
        if selected then
            draw.RoundedBox(2, 0, 5, 3, height - 10, theme.accent)
        end
        draw.SimpleText(label, "KirkwareLinuxText", 12, height * 0.5,
                        selected and theme.accent or theme.text,
                        TEXT_ALIGN_LEFT, TEXT_ALIGN_CENTER)
    end
    button.DoClick = function()
        KW.ActiveCategory = category
        rebuildModuleList(frame)
    end
end

local function closeMenu()
    if IsValid(KW.Frame) then
        KW.Frame:SetVisible(false)
    end
    gui.EnableScreenClicker(false)
end

local function openMenu()
    if IsValid(KW.Frame) then
        KW.Frame:SetVisible(true)
        KW.Frame:MakePopup()
        gui.EnableScreenClicker(true)
        return
    end

    local frame = vgui.Create("DFrame")
    KW.Frame = frame
    frame:SetSize(560, 380)
    frame:Center()
    frame:SetTitle("")
    frame:ShowCloseButton(false)
    frame:SetDraggable(true)
    frame:SetSizable(false)
    frame:SetDeleteOnClose(false)
    frame:MakePopup()
    frame.Paint = function(self, width, height)
        draw.RoundedBox(6, 0, 0, width, height, theme.window)
        surface.SetDrawColor(theme.border)
        surface.DrawOutlinedRect(0, 0, width, height, 1)
        draw.SimpleText("kirkware", "KirkwareLinuxTitle", 16, 14,
                        theme.text, TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
        draw.SimpleText("linux", "KirkwareLinuxSmall", 92, 18,
                        theme.accent, TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
        draw.SimpleText("Insert toggles this menu", "KirkwareLinuxSmall",
                        width - 48, 19, theme.muted,
                        TEXT_ALIGN_RIGHT, TEXT_ALIGN_TOP)
    end
    frame.OnClose = function()
        gui.EnableScreenClicker(false)
    end

    local close = vgui.Create("DButton", frame)
    close:SetText("")
    close:SetSize(24, 24)
    close:SetPos(526, 10)
    close.Paint = function(self, width, height)
        local color = self:IsHovered() and theme.unsafe or theme.muted
        surface.SetDrawColor(color)
        surface.DrawLine(7, 7, width - 7, height - 7)
        surface.DrawLine(width - 7, 7, 7, height - 7)
    end
    close.DoClick = closeMenu

    local sidebar = vgui.Create("DPanel", frame)
    sidebar:SetPos(12, 48)
    sidebar:SetSize(132, 320)
    sidebar.Paint = function(self, width, height)
        draw.RoundedBox(4, 0, 0, width, height, theme.panel)
        surface.SetDrawColor(theme.border)
        surface.DrawOutlinedRect(0, 0, width, height, 1)
    end

    createCategoryButton(sidebar, frame, "visuals", "visuals", 12)
    createCategoryButton(sidebar, frame, "misc", "misc", 50)

    local content = vgui.Create("DPanel", frame)
    content:SetPos(154, 48)
    content:SetSize(394, 320)
    content.Paint = function(self, width, height)
        draw.RoundedBox(4, 0, 0, width, height, Color(22, 22, 22, 255))
        surface.SetDrawColor(theme.border)
        surface.DrawOutlinedRect(0, 0, width, height, 1)
    end

    local heading = vgui.Create("DLabel", content)
    heading:SetFont("KirkwareLinuxTitle")
    heading:SetTextColor(theme.text)
    heading:SetText("modules")
    heading:SetPos(12, 10)
    heading:SizeToContents()

    local list = vgui.Create("DScrollPanel", content)
    frame.ModuleList = list
    list:SetPos(12, 40)
    list:SetSize(370, 268)

    rebuildModuleList(frame)
    gui.EnableScreenClicker(true)
end

function KW.ToggleMenu()
    if IsValid(KW.Frame) and KW.Frame:IsVisible() then
        closeMenu()
    else
        openMenu()
    end
end

concommand.Add("kirkware_menu", function()
    KW.ToggleMenu()
end)

concommand.Add("kirkware_reset_modules", function()
    KW.Settings = copyDefaults()
    saveSettings()
    if IsValid(KW.Frame) then
        rebuildModuleList(KW.Frame)
    end
end)

local insertWasDown = false
hook.Add("Think", "KirkwareLinux.InsertMenuToggle", function()
    local insertDown = input.IsKeyDown(KEY_INSERT)
    if insertDown and not insertWasDown then
        KW.ToggleMenu()
    end
    insertWasDown = insertDown
end)

hook.Add("HUDPaint", "KirkwareLinux.HUDModules", function()
    if settingEnabled("menu_watermark") then
        draw.SimpleText("kirkware linux", "KirkwareLinuxText",
                        ScrW() - 12, 10, theme.text,
                        TEXT_ALIGN_RIGHT, TEXT_ALIGN_TOP)
    end

    if settingEnabled("esp_other_crosshair") then
        local x = math.floor(ScrW() * 0.5)
        local y = math.floor(ScrH() * 0.5)
        surface.SetDrawColor(theme.accent)
        surface.DrawLine(x - 6, y, x - 2, y)
        surface.DrawLine(x + 2, y, x + 6, y)
        surface.DrawLine(x, y - 6, x, y - 2)
        surface.DrawLine(x, y + 2, x, y + 6)
    end

    if not settingEnabled("esp_player_enable") then
        return
    end

    local localPlayer = LocalPlayer()
    for _, playerEntity in ipairs(player.GetAll()) do
        if IsValid(playerEntity) and playerEntity ~= localPlayer and playerEntity:Alive() then
            local mins = playerEntity:OBBMins()
            local maxs = playerEntity:OBBMaxs()
            local bottom = playerEntity:LocalToWorld(Vector(0, 0, mins.z)):ToScreen()
            local top = playerEntity:LocalToWorld(Vector(0, 0, maxs.z + 6)):ToScreen()

            if bottom.visible ~= false or top.visible ~= false then
                local height = math.abs(bottom.y - top.y)
                local width = math.max(10, height * 0.42)
                local left = top.x - width * 0.5
                local upper = math.min(top.y, bottom.y)

                if settingEnabled("esp_player_box") then
                    surface.SetDrawColor(0, 0, 0, 220)
                    surface.DrawOutlinedRect(left - 1, upper - 1,
                                             width + 2, height + 2, 1)
                    surface.SetDrawColor(theme.accent)
                    surface.DrawOutlinedRect(left, upper, width, height, 1)
                end

                if settingEnabled("esp_player_name") then
                    draw.SimpleText(playerEntity:Nick(), "KirkwareLinuxSmall",
                                    top.x, upper - 14, theme.text,
                                    TEXT_ALIGN_CENTER, TEXT_ALIGN_TOP)
                end
            end
        end
    end
end)

hook.Add("CalcView", "KirkwareLinux.ThirdPerson", function(playerEntity,
                                                             position,
                                                             angles,
                                                             fov)
    if not settingEnabled("misc_thirdperson") or not IsValid(playerEntity) then
        return
    end

    local desired = position - angles:Forward() * 110 + Vector(0, 0, 8)
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
        fov = fov,
        drawviewer = true,
    }
end)

hook.Add("ShutDown", "KirkwareLinux.SaveModules", saveSettings)

loadSettings()
print("[kirkware linux] loaded; press Insert to toggle the menu")
