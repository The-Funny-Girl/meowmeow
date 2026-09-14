param(
    [string]$Toolchain = "",
    [string]$OutputName = "kirkware.exe"
)

$ErrorActionPreference = "Stop"
if ($args.Count -ne 0)
{
    throw "Unsupported build argument"
}
$ProjectRoot = $PSScriptRoot
$ResolvedProjectRoot = [IO.Path]::GetFullPath($ProjectRoot).TrimEnd('\')
$PathMapFlags = @(
    "-ffile-prefix-map=$ResolvedProjectRoot=.",
    "-fdebug-prefix-map=$ResolvedProjectRoot=."
)
$SourceRoot = Join-Path $ProjectRoot "source"
$CoreRoot = Join-Path $SourceRoot "core"
$UiRoot = Join-Path $SourceRoot "ui"
$ImGuiRoot = Join-Path $SourceRoot "third_party\imgui-1.90-wip-18995"
$FreeTypeRoot = Join-Path $SourceRoot "third_party\freetype-2.13.0"
$FreeTypeInclude = Join-Path $FreeTypeRoot "include"
$FreeTypeLibrary = Join-Path $FreeTypeRoot "lib\libfreetype.a"
$AssetRoot = Join-Path $SourceRoot "assets"
$BuildRoot = Join-Path $ProjectRoot "build"
$ObjectRoot = Join-Path $BuildRoot "obj"

if ([IO.Path]::GetFileName($OutputName) -cne $OutputName)
{
    throw "OutputName must be a file name"
}

function Resolve-Tool([string]$Requested, [string]$Name)
{
    $Candidates = [System.Collections.Generic.List[string]]::new()
    if (-not [string]::IsNullOrWhiteSpace($Requested))
    {
        if ([IO.Path]::GetFileName($Requested) -ieq $Name)
        {
            $Candidates.Add($Requested)
        }
        else
        {
            $Candidates.Add((Join-Path $Requested $Name))
            $Candidates.Add((Join-Path $Requested ("bin\" + $Name)))
        }
    }
    $Found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $Found)
    {
        $Candidates.Add($Found.Source)
    }
    foreach ($Candidate in $Candidates)
    {
        if (Test-Path -LiteralPath $Candidate -PathType Leaf)
        {
            return (Resolve-Path -LiteralPath $Candidate).Path
        }
    }
    throw "$Name was not found; pass -Toolchain with an LLVM-MinGW root"
}

function Assert-Hash([string]$Relative, [string]$Expected)
{
    $Path = Join-Path $AssetRoot $Relative
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
    {
        throw "Missing asset: $Relative"
    }
    $Actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($Actual -cne $Expected)
    {
        throw "Asset hash mismatch: $Relative"
    }
}

function New-FixedBaseBootstrapCopy([string]$Relative, [string]$Destination,
                                    [UInt64]$ExpectedImageBase,
                                    [string]$ExpectedHash)
{
    $Source = Join-Path $AssetRoot $Relative
    $Bytes = [IO.File]::ReadAllBytes($Source)
    if ($Bytes.Length -lt 0x200 -or
        [BitConverter]::ToUInt16($Bytes, 0) -ne 0x5A4D)
    {
        throw "Invalid bootstrap DOS image: $Relative"
    }
    $PeOffset = [BitConverter]::ToInt32($Bytes, 0x3C)
    if ($PeOffset -lt 0x40 -or $PeOffset -gt $Bytes.Length - 0x108 -or
        [BitConverter]::ToUInt32($Bytes, $PeOffset) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($Bytes, $PeOffset + 4) -ne 0x8664)
    {
        throw "Invalid bootstrap PE64 image: $Relative"
    }
    $SectionCount = [BitConverter]::ToUInt16($Bytes, $PeOffset + 6)
    $OptionalSize = [BitConverter]::ToUInt16($Bytes, $PeOffset + 20)
    $OptionalOffset = $PeOffset + 24
    if ($SectionCount -ne 9 -or $OptionalSize -lt 0xF0 -or
        $OptionalOffset + $OptionalSize + 40 * $SectionCount -gt
            $Bytes.Length -or
        [BitConverter]::ToUInt16($Bytes, $OptionalOffset) -ne 0x20B -or
        [BitConverter]::ToUInt64($Bytes, $OptionalOffset + 24) -ne
            $ExpectedImageBase -or
        [BitConverter]::ToUInt32($Bytes, $OptionalOffset + 16) -ne
            0x1162000 -or
        [BitConverter]::ToUInt32($Bytes, $OptionalOffset + 56) -ne
            0x1163000 -or
        [BitConverter]::ToUInt32($Bytes, $OptionalOffset + 152) -ne
            0xBBA000 -or
        [BitConverter]::ToUInt32($Bytes, $OptionalOffset + 156) -ne
            0x4F60)
    {
        throw "Unexpected bootstrap PE layout: $Relative"
    }
    $CharacteristicsOffset = $OptionalOffset + 70
    $OriginalCharacteristics =
        [BitConverter]::ToUInt16($Bytes, $CharacteristicsOffset)
    if (($OriginalCharacteristics -band 0x60) -ne 0x60)
    {
        throw "Bootstrap ASLR flags are not present: $Relative"
    }
    $FixedCharacteristics =
        [UInt16]($OriginalCharacteristics -band 0xFF9F)
    [BitConverter]::GetBytes($FixedCharacteristics).CopyTo(
        $Bytes, $CharacteristicsOffset)
    [IO.File]::WriteAllBytes($Destination, $Bytes)
    $Written = [IO.File]::ReadAllBytes($Destination)
    if ($Written.Length -ne $Bytes.Length -or
        [BitConverter]::ToUInt64($Written, $OptionalOffset + 24) -ne
            $ExpectedImageBase -or
        [BitConverter]::ToUInt16($Written, $CharacteristicsOffset) -ne
            $FixedCharacteristics -or
        ([BitConverter]::ToUInt16($Written, $CharacteristicsOffset) -band
            0x60) -ne 0)
    {
        throw "Fixed-base bootstrap verification failed: $Relative"
    }
    $ActualHash =
        (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($ActualHash -cne $ExpectedHash)
    {
        throw "Fixed-base bootstrap hash mismatch: $Relative"
    }
}

function Invoke-Checked([string]$Name, [string]$Executable,
                        [string[]]$Arguments)
{
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

function Compile-Entry([string]$Compiler, [string[]]$Common,
                       [string]$Source, [string]$Macro,
                       [string]$Entry, [string]$Output)
{
    $Arguments = [System.Collections.Generic.List[string]]::new()
    $Arguments.AddRange($Common)
    $Arguments.Add("-D$Macro=$Entry")
    $Arguments.Add("-c")
    $Arguments.Add((Join-Path $CoreRoot $Source))
    $Arguments.Add("-o")
    $Arguments.Add((Join-Path $ObjectRoot $Output))
    Invoke-Checked "Compile $Source" $Compiler $Arguments
}

Assert-Hash "active-image-b1.bin" "22679533585D62DC8BAD322938823D6E89A7B4A92DEF46AA0EBB9F00F48E56AC"
Assert-Hash "bootstrap-b1.dll" "45F1D09BEA088ECF75BB2AB8DF0EF56180086F96BA534DCDD51B0E5E0299790B"
Assert-Hash "bootstrap-b2.dll" "F20754893504955B9CD1C831AF66A7623552DB8F0D6CBF6C496C4567044FE8DA"
Assert-Hash "reconstructed-bss.bin" "91A018E36198AB386D087E491F22699D630A77E02665E3262E7349080F542E0F"
Assert-Hash "reconstructed-bss.mask.bin" "BE398E8D90596AA32A6E816CA17D79CC5B1EEC891B8CF98BB14CFDD8BBD37666"
Assert-Hash "mbcinfo-template.bin" "5961C808737FB64BEB30CBE80838DEB4C2806F4AB9CDE4DD98CEAFEE921134B7"
Assert-Hash "garrys-mod.png" "FE08F972935A461F5A1D8B103B185B0D685DEDA71572AAF084FAD94CCD7610B1"
Assert-Hash "kirkware-icon.ico" "637D753177E11917737C35AF1A0BC127E233111EA3F4CFA6034BABC1874BEA62"

$Image = [IO.File]::ReadAllBytes((Join-Path $AssetRoot "active-image-b1.bin"))
$ExpectedOriginal = @{
    0xB81210 = [byte[]]::new(20)
    0x58EC90 = [byte[]]::new(0x121)
    0xDEF726 = [byte[]](0xCD,0x29)
    0xE8150 = [byte[]](0xE8,0x1B,0x2A,0x3C,0x00)
    0x349544 = [byte[]](0x48,0x85,0xC9)
    0x3495B5 = [byte[]](0x48,0x85,0xC9)
    0xBC336D = [byte[]](0x49,0x81,0xEB,0xFA,0x00,0x00,0x00)
    0xBC3501 = [byte[]](
        0x41,0x80,0xFA,0x66,0x0F,0x85,0x6B,0x00,
        0x00,0x00,0x81,0xE7,0x40,0x00,0x00,0x00,
        0x0F,0x85,0x4E,0x00,0x00,0x00,0x48,0x81,
        0xE2,0xF7,0x00,0x00,0x00,0x49,0x21,0xF0,
        0x4C,0x01,0xCA,0x49,0x89,0xEF,0x4C,0x31,
        0xC2,0x49,0x81,0xCC,0x04,0x00,0x00,0x00,
        0x48,0xC7,0xC6,0x00,0x00,0x00,0x00,0x48,
        0xC7,0xC6,0x12,0x00,0x00,0x00,0x48,0xC7,
        0xC6,0x00,0x00,0x00,0x00,0x49,0x81,0xC7,
        0x53,0x01,0x00,0x00,0x4C,0x09,0xC2,0x48,
        0xC7,0xC2,0x01,0x00,0x00,0x00,0x49,0x21,
        0xF4,0x41,0xC6,0x07,0x01,0x49,0x81,0xC8,
        0xF1,0x00,0x00,0x00,0x49,0x09,0xFC)
    0xBC3F03 = [byte[]](0x0F,0x84,0xB0,0x00,0x00,0x00)
    0xD0F15E = [byte[]](0x45,0x8B,0x00,0x41,0x80,0xFC,0x04)
    0xCDF166 = [byte[]](0x0F,0x85,0x02,0x00,0x00,0x00)
    0xD23E92 = [byte[]](
        0x48,0x89,0x19,0x48,0xC7,0xC0,0x01,0x00,0x00,0x00)
    0xCE463E = [byte[]](
        0x49,0x89,0x30,0x48,0x81,0xEA,0x1D,0x00,0x00,0x00)
}
foreach ($Offset in $ExpectedOriginal.Keys)
{
    $Expected = $ExpectedOriginal[$Offset]
    for ($Index = 0; $Index -lt $Expected.Length; ++$Index)
    {
        if ($Image[$Offset + $Index] -ne $Expected[$Index])
        {
            throw ("Unapproved active-image patch at 0x{0:X}" -f $Offset)
        }
    }
}

$Compiler = Resolve-Tool $Toolchain "clang++.exe"
$ToolBin = Split-Path -Parent $Compiler
$Windres = Resolve-Tool $ToolBin "x86_64-w64-mingw32-windres.exe"
$Objdump = Resolve-Tool $ToolBin "llvm-objdump.exe"
$Nm = Resolve-Tool $ToolBin "llvm-nm.exe"
$ReadObj = Resolve-Tool $ToolBin "llvm-readobj.exe"
$Strip = Resolve-Tool $ToolBin "llvm-strip.exe"

if (Test-Path -LiteralPath $ObjectRoot)
{
    Remove-Item -LiteralPath $ObjectRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $ObjectRoot -Force | Out-Null

$BootstrapB1Resource = Join-Path $ObjectRoot "bootstrap-b1-fixed-base.dll"
$BootstrapB2Resource = Join-Path $ObjectRoot "bootstrap-b2-fixed-base.dll"
New-FixedBaseBootstrapCopy `
    "bootstrap-b1.dll" $BootstrapB1Resource 0x1E5DCC00000 `
    "0C24F933A1FF93E7B12CB24DDFC1C8D6BB7B6CE6B6AC60031EBC65E5847B6D1E"
New-FixedBaseBootstrapCopy `
    "bootstrap-b2.dll" $BootstrapB2Resource 0x1E5DF940000 `
    "5281D7D5F6D6D70F2F912281DD9E05169683898A2735452E87C07B0E82FDF143"

$RcPath = Join-Path $ObjectRoot "resources.rc"
$ResourceObject = Join-Path $ObjectRoot "resources.o"
$IconRcPath = Join-Path $ObjectRoot "icon-resources.rc"
$IconResourceObject = Join-Path $ObjectRoot "icon-resources.o"
$IconRcLines = @(
    "LANGUAGE 9, 1",
    "101 ICON `"$((Join-Path $AssetRoot 'kirkware-icon.ico').Replace('\','/'))`""
)
[IO.File]::WriteAllLines(
    $IconRcPath, $IconRcLines, [Text.UTF8Encoding]::new($false))
Invoke-Checked "Compile icon resources" $Windres @(
    "--input=$IconRcPath", "--output=$IconResourceObject", "--output-format=coff")
$RcLines = @(
    "401 RCDATA `"$((Join-Path $AssetRoot 'active-image-b1.bin').Replace('\','/'))`"",
    "402 RCDATA `"$($BootstrapB1Resource.Replace('\','/'))`"",
    "403 RCDATA `"$($BootstrapB2Resource.Replace('\','/'))`"",
    "404 RCDATA `"$((Join-Path $AssetRoot 'reconstructed-bss.bin').Replace('\','/'))`"",
    "405 RCDATA `"$((Join-Path $AssetRoot 'reconstructed-bss.mask.bin').Replace('\','/'))`"",
      "406 RCDATA `"$((Join-Path $AssetRoot 'mbcinfo-template.bin').Replace('\','/'))`"",
      "502 RCDATA `"$((Join-Path $AssetRoot 'garrys-mod.png').Replace('\','/'))`"",
    "1 24 `"$((Join-Path $SourceRoot 'app.manifest').Replace('\','/'))`""
)
[IO.File]::WriteAllLines(
    $RcPath, $RcLines, [Text.UTF8Encoding]::new($false))
Invoke-Checked "Compile resources" $Windres @(
    "--input=$RcPath", "--output=$ResourceObject", "--output-format=coff")

$Common = @(
    "-std=c++20", "-O2", "-static", "-Wall", "-Wextra",
    "-Wpedantic", "-Werror", "-Wno-unused-parameter",
    "-Wno-unused-function", "-Wno-unused-const-variable",
    "-DKIRKWARE_UNLINK_ACTIVE_IMAGE",
    "-DKIRKWARE_PARK_THREAD",
    "-DKIRKWARE_FORCE_UNLINKED_MANUAL",
    "-DKIRKWARE_EXTERNAL_STATIC_TLS_CARRIER",
    "-DKIRKWARE_CRT_THREAD_INIT",
    "-DKIRKWARE_TRIGGER_STACK_SIZE=0x180000", ("-I" + $CoreRoot)
) + $PathMapFlags
Compile-Entry $Compiler $Common "build_kirkware_offline_handoff.cpp" "main" "kirkware_handoff_entry" "handoff.o"
Compile-Entry $Compiler $Common "build_kirkware_entry_envelope.cpp" "main" "kirkware_envelope_entry" "envelope.o"
Compile-Entry $Compiler $Common "inject_library.cpp" "wmain" "kirkware_bootstrap_entry" "bootstrap.o"
Compile-Entry $Compiler $Common "manual_map_kirkware_active.cpp" "main" "kirkware_swap_entry" "swap.o"
Compile-Entry $Compiler $Common "install_kirkware_network_worker_idle.cpp" "main" "kirkware_worker_idle_entry" "worker-idle.o"
Compile-Entry $Compiler $Common "install_kirkware_steam_offline_guard.cpp" "main" "kirkware_steam_offline_guard_entry" "steam-offline-guard.o"
Compile-Entry $Compiler $Common "install_kirkware_context.cpp" "main" "kirkware_context_entry" "context.o"
Compile-Entry $Compiler $Common "resolve_kirkware_lua_exports.cpp" "main" "kirkware_lua_resolver_entry" "lua-resolver.o"
Compile-Entry $Compiler ($Common + @("-Wno-unused-variable")) "invoke_kirkware_veh.cpp" "main" "kirkware_veh_entry" "veh.o"
Compile-Entry $Compiler $Common "install_kirkware_locale.cpp" "main" "kirkware_locale_entry" "locale.o"
Compile-Entry $Compiler $Common "resolve_kirkware_interfaces.cpp" "main" "kirkware_interfaces_entry" "interfaces.o"
Compile-Entry $Compiler $Common "install_kirkware_game_hooks.cpp" "main" "kirkware_game_hooks_entry" "game-hooks.o"
$DeferredWorkerArguments = @(
    "-std=c++20", "-O2", "-ffreestanding", "-fno-exceptions", "-fno-rtti",
    "-fno-stack-protector", "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables", "-fno-vectorize",
    "-fno-slp-vectorize", "-Wall", "-Wextra", "-Wpedantic", "-Werror"
) + $PathMapFlags + @(
    ("-I" + $CoreRoot), "-c",
    (Join-Path $CoreRoot "kirkware_deferred_event_worker.cpp"),
    "-o", (Join-Path $ObjectRoot "deferred-event-worker-code.o"))
Invoke-Checked "Compile deferred event worker" $Compiler $DeferredWorkerArguments
Invoke-Checked "Compile deferred event worker end marker" $Compiler @(
    "-c", (Join-Path $CoreRoot "kirkware_deferred_event_worker.S"),
    "-o", (Join-Path $ObjectRoot "deferred-event-worker-end.o"))
$DeferredWorkerRelocations = (& $Objdump -r (
    Join-Path $ObjectRoot "deferred-event-worker-code.o") 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0)
{
    throw "Deferred event worker relocation inspection failed"
}
if ($DeferredWorkerRelocations -match "(?im)^\s*[0-9A-F]+\s+IMAGE_REL_")
{
    throw "Deferred event worker contains a relocation"
}
Compile-Entry $Compiler $Common "call_kirkware_carrier_thread.cpp" "main" "kirkware_carrier_entry" "carrier.o"

$Output = Join-Path $BuildRoot $OutputName
$UiSources = @(
    (Join-Path $UiRoot "main.cpp"),
    (Join-Path $UiRoot "d3d9_texture.cpp"),
    (Join-Path $UiRoot "native_chain_bridge.cpp"),
    (Join-Path $UiRoot "kirkware_ui.cpp"),
    (Join-Path $ImGuiRoot "imgui.cpp"),
    (Join-Path $ImGuiRoot "imgui_draw.cpp"),
    (Join-Path $ImGuiRoot "imgui_tables.cpp"),
    (Join-Path $ImGuiRoot "imgui_widgets.cpp"),
    (Join-Path $ImGuiRoot "misc\freetype\imgui_freetype.cpp"),
    (Join-Path $ImGuiRoot "backends\imgui_impl_dx9.cpp"),
    (Join-Path $ImGuiRoot "backends\imgui_impl_win32.cpp")
)
$Link = @(
    "-std=c++20", "-O2", "-static", "-Wall", "-Wextra",
    "-Wpedantic", "-Werror", "-Wno-unused-parameter",
    "-Wno-missing-field-initializers", "-Wno-nontrivial-memcall",
    "-Wno-uninitialized-const-pointer", "-municode", "-mwindows",
    "-Wl,--no-insert-timestamp", "-Wl,--strip-debug", "-DUNICODE", "-D_UNICODE",
    "-DIMGUI_ENABLE_FREETYPE"
) + $PathMapFlags + @(
    ("-I" + $CoreRoot), ("-I" + $UiRoot),
    ("-I" + $ImGuiRoot), ("-I" + (Join-Path $ImGuiRoot "backends")),
    ("-I" + $FreeTypeInclude),
    (Join-Path $CoreRoot "gmod_main_menu_gate.cpp"),
    (Join-Path $CoreRoot "kirkware_clean_traces.cpp"),
    (Join-Path $CoreRoot "kirkware_config_sanitizer.cpp"),
    (Join-Path $CoreRoot "kirkware_native_main.cpp"),
    (Join-Path $CoreRoot "kirkware_bone_access_compat.cpp")
) + $UiSources + @(
    (Join-Path $ObjectRoot "handoff.o"),
    (Join-Path $ObjectRoot "envelope.o"),
    (Join-Path $ObjectRoot "bootstrap.o"),
    (Join-Path $ObjectRoot "swap.o"),
    (Join-Path $ObjectRoot "worker-idle.o"),
    (Join-Path $ObjectRoot "steam-offline-guard.o"),
    (Join-Path $ObjectRoot "context.o"),
    (Join-Path $ObjectRoot "lua-resolver.o"),
    (Join-Path $ObjectRoot "veh.o"),
    (Join-Path $ObjectRoot "locale.o"),
    (Join-Path $ObjectRoot "interfaces.o"),
    (Join-Path $ObjectRoot "game-hooks.o"),
    (Join-Path $ObjectRoot "deferred-event-worker-code.o"),
    (Join-Path $ObjectRoot "deferred-event-worker-end.o"),
    (Join-Path $ObjectRoot "carrier.o"),
    $ResourceObject, $IconResourceObject, $FreeTypeLibrary,
    "-o", $Output, "-ladvapi32",
    "-lbcrypt", "-ld3d9",
    "-ldwmapi", "-lgdi32", "-lole32", "-luuid", "-luser32",
    "-lwindowscodecs"
)
Invoke-Checked "Link executable" $Compiler $Link
$ImportTable = (& $Objdump -p $Output 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0)
{
    throw "Import-table inspection failed with exit code $LASTEXITCODE"
}
$DeferredSymbols = (& $Nm -n $Output 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0)
{
    throw "Deferred event worker symbol inspection failed"
}
$DeferredBeginMatch = [Regex]::Match(
    $DeferredSymbols,
    "(?im)^([0-9A-F]+)\s+T\s+KirkwareDeferredEventWorker\s*$")
$DeferredEndMatch = [Regex]::Match(
    $DeferredSymbols,
    "(?im)^([0-9A-F]+)\s+T\s+KirkwareDeferredEventWorkerEnd\s*$")
if (-not $DeferredBeginMatch.Success -or -not $DeferredEndMatch.Success)
{
    throw "Deferred event worker symbols are missing"
}
$DeferredBegin = [Convert]::ToUInt64(
    $DeferredBeginMatch.Groups[1].Value, 16)
$DeferredEnd = [Convert]::ToUInt64(
    $DeferredEndMatch.Groups[1].Value, 16)
$SectionTable = (& $Objdump -h $Output 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0)
{
    throw "Deferred event worker section inspection failed"
}
$DeferredSectionMatch = [Regex]::Match(
    $SectionTable,
    "(?im)^\s*\d+\s+\.kw_evt\s+([0-9A-F]+)\s+([0-9A-F]+)\s+TEXT\s*$")
if (-not $DeferredSectionMatch.Success)
{
    throw "Deferred event worker section is missing"
}
$DeferredSectionSize = [Convert]::ToUInt64(
    $DeferredSectionMatch.Groups[1].Value, 16)
$DeferredSectionAddress = [Convert]::ToUInt64(
    $DeferredSectionMatch.Groups[2].Value, 16)
if ($DeferredBegin -ne $DeferredSectionAddress -or
    $DeferredEnd -le $DeferredBegin -or
    $DeferredEnd - $DeferredBegin -ne $DeferredSectionSize -or
    $DeferredSectionSize -gt 0x10000)
{
    throw "Deferred event worker section is not exact and contiguous"
}
$ImageBaseMatch = [Regex]::Match(
    $ImportTable, "(?im)^ImageBase\s+([0-9A-F]+)\s*$")
if (-not $ImageBaseMatch.Success)
{
    throw "Executable image base was not reported"
}
$ExecutableImageBase = [Convert]::ToUInt64(
    $ImageBaseMatch.Groups[1].Value, 16)
$DeferredBeginRva = $DeferredBegin - $ExecutableImageBase
$DeferredEndRva = $DeferredEnd - $ExecutableImageBase
$BaseRelocations = (& $ReadObj --coff-basereloc $Output 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0)
{
    throw "Executable base-relocation inspection failed"
}
foreach ($RelocationMatch in [Regex]::Matches(
             $BaseRelocations, "(?im)^\s*Address:\s*0x([0-9A-F]+)\s*$"))
{
    $RelocationRva = [Convert]::ToUInt64(
        $RelocationMatch.Groups[1].Value, 16)
    if ($RelocationRva -ge $DeferredBeginRva -and
        $RelocationRva -lt $DeferredEndRva)
    {
        throw "Deferred event worker contains a base relocation"
    }
}
$ForbiddenDlls = @(
    "winhttp.dll", "wininet.dll", "ws2_32.dll", "dnsapi.dll",
    "urlmon.dll", "shell32.dll"
)
foreach ($ForbiddenDll in $ForbiddenDlls)
{
    if ($ImportTable -match ("(?im)^\s*DLL Name:\s*" +
                             [Regex]::Escape($ForbiddenDll) + "\s*$"))
    {
        throw "Forbidden outer import DLL: $ForbiddenDll"
    }
}
$ForbiddenApis = @(
    "CreateProcessA", "CreateProcessW", "CreateProcessAsUserA",
    "CreateProcessAsUserW", "CreateProcessWithLogonW",
    "CreateProcessWithTokenW", "WinExec", "ShellExecuteA",
    "ShellExecuteW", "ShellExecuteExA", "ShellExecuteExW",
    "OpenSCManagerA", "OpenSCManagerW", "CreateServiceA",
    "CreateServiceW", "OpenServiceA", "OpenServiceW", "StartServiceA",
    "StartServiceW", "ControlService", "DeleteService",
    "InternetOpenA", "InternetOpenW", "WinHttpOpen", "WSAStartup",
    "socket", "connect", "send", "recv", "getaddrinfo", "DnsQuery_A",
    "DnsQuery_W", "URLDownloadToFileA", "URLDownloadToFileW"
)
foreach ($ForbiddenApi in $ForbiddenApis)
{
    if ($ImportTable -match ("(?im)^\s*[0-9A-F]+\s+" +
                             [Regex]::Escape($ForbiddenApi) + "\s*$"))
    {
        throw "Forbidden outer import API: $ForbiddenApi"
    }
}
Invoke-Checked "Strip production symbols" $Strip @("--strip-all", $Output)
$StrippedHeaders = (& $ReadObj --file-headers $Output 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or
    $StrippedHeaders -notmatch "(?im)^\s*PointerToSymbolTable:\s+0x0\s*$" -or
    $StrippedHeaders -notmatch "(?im)^\s*SymbolCount:\s+0\s*$")
{
    throw "Production executable still contains a COFF symbol table"
}
$Hash = (Get-FileHash -LiteralPath $Output -Algorithm SHA256).Hash
Remove-Item -LiteralPath $ObjectRoot -Recurse -Force
Write-Host "$Hash  $OutputName"
