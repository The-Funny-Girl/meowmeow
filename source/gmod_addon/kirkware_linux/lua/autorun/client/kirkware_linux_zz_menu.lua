-- Enhanced dynamic menu for the Kirkware Linux Garry's Mod addon.
-- Loaded after the module files so every registered module appears automatically.

if not KIRKWARE_LINUX then
    return
end

local KW = KIRKWARE_LINUX
local settingsPath = "kirkware_linux/modules.json"

local theme = {
    window = Color(18, 18, 18, 250),
    panel = Color(27, 27, 27, 255),
    panelHover = Color(35, 35, 35, 255),
    border = Color(53, 53, 53, 255),
    accent = Color(43, 151, 250, 255),
    text = Color(255, 255, 255, 255),
    muted = Color(153, 153, 153, 255),
    unsafe = Color(227, 126, 129, 255),
}

local categories = {
    {id = "aim", label = "aim"},
    {id = "visuals", label = "visuals"},
    {id = "misc", label = "misc"},
    {id = "tuning", label = "tuning"},
}

local tuning = {
    {label = "legit fov", convar = "kirkware_legit_fov", min = 0.25, max = 45, decimals = 2},
    {label = "legit smoothing", convar = "kirkware_legit_smoothing", min = 1, max = 40, decimals = 1},
    {label = "legit max distance", convar = "kirkware_legit_max_distance", min = 100, max = 50000, decimals = 0},
    {label = "trigger delay", convar = "kirkware_trigger_delay", min = 0, max = 1, decimals = 2},
    {label = "rage fov", convar = "kirkware_rage_fov", min = 1, max = 180, decimals = 1},
    {label = "rage max distance", convar = "kirkware_rage_max_distance", min = 100, max = 50000, decimals = 0},
    {label = "camera fov", convar = "kirkware_fov", min = 60, max = 130, decimals = 0},
    {label = "zoom fov", convar = "kirkware_zoom_fov", min = 10, max = 100, decimals = 0},
    {label = "thirdperson distance", convar = "kirkware_thirdperson_distance", min = 30, max = 300, decimals = 0},
    {label = "entity distance", convar = "kirkware_entity_distance", min = 250, max = 10000, decimals = 0},
}

local tuningChecks = {
    {label = "aim at teammates", convar = "kirkware_aim_teammates"},
    {label = "legit only while attacking", convar = "kirkware_legit_require_attack"},
}

local function enabled(id)
    return KW.ModuleEnabled and KW.ModuleEnabled(id) == true
end

local function saveAllSettings()
    file.CreateDir("kirkware_linux")
    file.Write(settingsPath, util.TableToJSON(KW.Settings, true))
end

local function closeMenu()
    if IsValid(KW.Frame) then
        KW.Frame:SetVisible(false)
    end
    gui.EnableScreenClicker(false)
end

local function createModuleRow(parent, id, definition)
    local row = vgui.Create("DPanel", parent)
    row:Dock(TOP)
    row:SetTall(54)
    row:DockMargin(0, 0, 0, 7)
    row.Paint = function(self, width, height)
        local background = self:IsHovered() and theme.panelHover or theme.panel
        draw.RoundedBox(4, 0, 0, width, height, background)
        surface.SetDrawColor(theme.border)
        surface.DrawOutlinedRect(0, 0, width, height, 1)
        draw.SimpleText(definition.name or id, "KirkwareLinuxText", 12, 10,
                        theme.text, TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
        draw.SimpleText(definition.description or id, "KirkwareLinuxSmall", 12, 31,
                        theme.muted, TEXT_ALIGN_LEFT, TEXT_ALIGN_TOP)
    end

    local toggle = vgui.Create("DButton", row)
    toggle:SetText("")
    toggle:SetSize(46, 22)
    toggle.Think = function(self)
        self:SetPos(math.max(0, row:GetWide() - 58), 16)
    end
    toggle.Paint = function(self, width, height)
        local state = enabled(id)
        draw.RoundedBox(height * 0.5, 0, 0, width, height,
                        state and theme.accent or Color(64, 64, 64, 255))
        local knob = height - 6
        local x = state and (width - knob - 3) or 3
        draw.RoundedBox(knob * 0.5, x, 3, knob, knob, theme.text)
    end
    toggle.DoClick = function()
        if KW.SetModuleEnabled then
            KW.SetModuleEnabled(id, not enabled(id))
        end
    end
end

local function addTuningControls(parent)
    for _, entry in ipairs(tuning) do
        if GetConVar(entry.convar) then
            local slider = vgui.Create("DNumSlider", parent)
            slider:Dock(TOP)
            slider:DockMargin(6, 3, 6, 5)
            slider:SetTall(38)
            slider:SetText(entry.label)
            slider:SetTextColor(theme.text)
            slider:SetMinMax(entry.min, entry.max)
            slider:SetDecimals(entry.decimals)
            slider:SetConVar(entry.convar)
        end
    end

    for _, entry in ipairs(tuningChecks) do
        if GetConVar(entry.convar) then
            local check = vgui.Create("DCheckBoxLabel", parent)
            check:Dock(TOP)
            check:DockMargin(12, 5, 6, 5)
            check:SetTall(24)
            check:SetText(entry.label)
            check:SetTextColor(theme.text)
            check:SetConVar(entry.convar)
        end
    end
end

local function rebuild(frame)
    if not IsValid(frame) or not IsValid(frame.ModuleList) then
        return
    end

    frame.ModuleList:Clear()
    if KW.ActiveCategory == "tuning" then
        addTuningControls(frame.ModuleList)
        return
    end

    local ids = {}
    for id, definition in pairs(KW.Modules or {}) do
        if definition.category == KW.ActiveCategory then
            ids[#ids + 1] = id
        end
    end
    table.sort(ids, function(left, right)
        local leftName = KW.Modules[left].name or left
        local rightName = KW.Modules[right].name or right
        return leftName < rightName
    end)

    for _, id in ipairs(ids) do
        createModuleRow(frame.ModuleList, id, KW.Modules[id])
    end
end

local function createCategoryButton(parent, frame, category, label, y)
    local button = vgui.Create("DButton", parent)
    button:SetText("")
    button:SetPos(10, y)
    button:SetSize(118, 31)
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
        rebuild(frame)
    end
end

local function openEnhancedMenu()
    if IsValid(KW.Frame) then
        KW.Frame:SetVisible(true)
        KW.Frame:MakePopup()
        gui.EnableScreenClicker(true)
        return
    end

    local frame = vgui.Create("DFrame")
    KW.Frame = frame
    frame:SetSize(690, 470)
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
        draw.SimpleText("Insert toggles menu", "KirkwareLinuxSmall",
                        width - 50, 19, theme.muted,
                        TEXT_ALIGN_RIGHT, TEXT_ALIGN_TOP)
    end

    local close = vgui.Create("DButton", frame)
    close:SetText("")
    close:SetSize(24, 24)
    close:SetPos(656, 10)
    close.Paint = function(self, width, height)
        local color = self:IsHovered() and theme.unsafe or theme.muted
        surface.SetDrawColor(color)
        surface.DrawLine(7, 7, width - 7, height - 7)
        surface.DrawLine(width - 7, 7, 7, height - 7)
    end
    close.DoClick = closeMenu

    local sidebar = vgui.Create("DPanel", frame)
    sidebar:SetPos(12, 48)
    sidebar:SetSize(138, 410)
    sidebar.Paint = function(self, width, height)
        draw.RoundedBox(4, 0, 0, width, height, theme.panel)
        surface.SetDrawColor(theme.border)
        surface.DrawOutlinedRect(0, 0, width, height, 1)
    end

    for index, category in ipairs(categories) do
        createCategoryButton(sidebar, frame, category.id, category.label,
                             12 + (index - 1) * 39)
    end

    local reset = vgui.Create("DButton", sidebar)
    reset:SetText("reset defaults")
    reset:SetTextColor(theme.muted)
    reset:SetPos(10, 364)
    reset:SetSize(118, 30)
    reset.Paint = function(self, width, height)
        draw.RoundedBox(4, 0, 0, width, height,
                        self:IsHovered() and theme.panelHover or theme.panel)
        surface.SetDrawColor(theme.border)
        surface.DrawOutlinedRect(0, 0, width, height, 1)
    end
    reset.DoClick = function()
        for id, definition in pairs(KW.Modules or {}) do
            KW.Settings[id] = definition.default == true
        end
        saveAllSettings()
        rebuild(frame)
    end

    local content = vgui.Create("DPanel", frame)
    content:SetPos(160, 48)
    content:SetSize(518, 410)
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
    list:SetSize(494, 358)

    if KW.ActiveCategory ~= "aim" and KW.ActiveCategory ~= "visuals" and
       KW.ActiveCategory ~= "misc" and KW.ActiveCategory ~= "tuning" then
        KW.ActiveCategory = "aim"
    end
    rebuild(frame)
    gui.EnableScreenClicker(true)
end

KW.ToggleMenu = function()
    if IsValid(KW.Frame) and KW.Frame:IsVisible() then
        closeMenu()
    else
        openEnhancedMenu()
    end
end

KW.RebuildModuleList = function()
    if IsValid(KW.Frame) then
        rebuild(KW.Frame)
    end
end

-- Replace the first-stage reset command so modules registered by later files are
-- also reset, not just the six modules from the initial menu implementation.
concommand.Add("kirkware_reset_modules", function()
    for id, definition in pairs(KW.Modules or {}) do
        KW.Settings[id] = definition.default == true
    end
    saveAllSettings()
    if IsValid(KW.Frame) then
        rebuild(KW.Frame)
    end
end)

print("[kirkware linux] enhanced dynamic menu loaded")
