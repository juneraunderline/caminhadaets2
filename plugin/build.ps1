# Build Take a Walk with the Visual Studio x64 C++ toolchain.
# Usage:
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Install
#   powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Install -GameDir "D:\SteamLibrary\steamapps\common\Euro Truck Simulator 2"

[CmdletBinding()]
param(
    [switch]$Install,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'
$obj = Join-Path $out 'obj'

function Test-GameDirectory([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { return $false }
    return Test-Path -LiteralPath (Join-Path $Path 'bin\win_x64\eurotrucks2.exe')
}

function Find-GameDirectory {
    $steamRoots = @()
    $registryKeys = @(
        'HKCU:\Software\Valve\Steam',
        'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam',
        'HKLM:\SOFTWARE\Valve\Steam'
    )
    foreach ($key in $registryKeys) {
        $entry = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($entry) {
            if ($entry.SteamPath) { $steamRoots += $entry.SteamPath }
            if ($entry.InstallPath) { $steamRoots += $entry.InstallPath }
        }
    }
    if ($env:ProgramFiles) { $steamRoots += (Join-Path $env:ProgramFiles 'Steam') }
    if ($env:ProgramFiles -and ${env:ProgramFiles(x86)}) {
        $steamRoots += (Join-Path ${env:ProgramFiles(x86)} 'Steam')
    }

    foreach ($steam in ($steamRoots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)) {
        $libraries = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($line in Get-Content -LiteralPath $vdf) {
                if ($line -match '"path"\s+"(.+)"') {
                    $libraries += $Matches[1].Replace('\\', '\')
                }
            }
        }
        foreach ($library in ($libraries | Select-Object -Unique)) {
            $candidate = Join-Path $library 'steamapps\common\Euro Truck Simulator 2'
            if (Test-GameDirectory $candidate) { return $candidate }
        }
    }
    return $null
}

if (-not (Test-Path -LiteralPath $root)) { throw "Plugin source directory not found: $root" }
$sources = @(Get-ChildItem -LiteralPath (Join-Path $root 'src') -File |
    Where-Object { $_.Extension -in '.cpp', '.c' } |
    Sort-Object Name)
if ($sources.Count -eq 0) { throw 'No C/C++ source files were found in plugin\src.' }
if (-not (Test-Path -LiteralPath (Join-Path $root 'src\takeawalk.def'))) {
    throw 'The DLL export definition file plugin\src\takeawalk.def is missing.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'vswhere.exe was not found. Install Visual Studio 2022 with the Desktop development with C++ workload.'
}
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) {
    throw 'Visual Studio C++ x64 build tools were not found. Install the Desktop development with C++ workload.'
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "Visual Studio x64 environment script not found: $vcvars" }

New-Item -ItemType Directory -Force -Path $out, $obj | Out-Null
Get-ChildItem -LiteralPath $obj -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force

# A response file avoids cmd.exe wildcard expansion and fragile nested quoting.
$arguments = @(
    '/LD'
    '/O2'
    '/MT'
    '/W4'
    '/EHsc'
    '/std:c++17'
    '/D_CRT_SECURE_NO_WARNINGS'
    '/Ithird_party\scs_sdk\include'
    '/Ithird_party'
    '/Fobuild\obj\'
    '/Febuild\takeawalk.dll'
)
foreach ($source in $sources) {
    $arguments += ('src\' + $source.Name)
}
$arguments += @(
)
$responseFile = Join-Path $out 'compile.rsp'
Set-Content -LiteralPath $responseFile -Value $arguments -Encoding ASCII

$cmdFile = Join-Path $out 'build.cmd'
$cmdLines = @(
    '@echo off'
    ('call "' + $vcvars + '" >nul')
    'if errorlevel 1 exit /b %errorlevel%'
    ('cl.exe /nologo @"' + $responseFile + '" /link user32.lib gdi32.lib winmm.lib /DEF:src\takeawalk.def /EXPORT:scs_telemetry_init /EXPORT:scs_telemetry_shutdown /IMPLIB:build\takeawalk.lib')
    'if errorlevel 1 exit /b %errorlevel%'
    'dumpbin.exe /nologo /exports build\takeawalk.dll > build\exports.txt'
    'if errorlevel 1 exit /b %errorlevel%'
    'exit /b 0'
)
Set-Content -LiteralPath $cmdFile -Value $cmdLines -Encoding ASCII

Push-Location $root
try {
    & $env:ComSpec /d /c "`"$cmdFile`""
    if ($LASTEXITCODE -ne 0) {
        throw "MSVC build failed with exit code $LASTEXITCODE. See the compiler output above."
    }
}
finally {
    Pop-Location
}

$dll = Join-Path $out 'takeawalk.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw 'MSVC returned success but takeawalk.dll was not created.' }
$exports = Get-Content -LiteralPath (Join-Path $out 'exports.txt') -Raw
if ($exports -notmatch 'scs_telemetry_init' -or $exports -notmatch 'scs_telemetry_shutdown') {
    throw 'The DLL was linked without the required SCS telemetry exports. Build stopped before packaging.'
}
Write-Host "Build successful and SCS exports verified: $dll"

if ($Install) {
    if (-not (Test-GameDirectory $GameDir)) {
        if ($GameDir) { Write-Warning "No ETS2 installation found at '$GameDir'." }
        $GameDir = Find-GameDirectory
    }
    if (-not (Test-GameDirectory $GameDir)) {
        throw 'ETS2 was not found in Steam libraries. Re-run with -GameDir "full game folder path". The DLL was built but not installed.'
    }
    if (Get-Process -Name 'eurotrucks2' -ErrorAction SilentlyContinue) {
        throw 'Close Euro Truck Simulator 2 before installing the plugin.'
    }

    $plugins = Join-Path $GameDir 'bin\win_x64\plugins'
    New-Item -ItemType Directory -Force -Path $plugins | Out-Null
    Copy-Item -LiteralPath $dll -Destination $plugins -Force

    $soundDestination = Join-Path $plugins 'takeawalk'
    New-Item -ItemType Directory -Force -Path $soundDestination | Out-Null
    Get-ChildItem -LiteralPath $soundDestination -Filter 'footstep*.ogg' -File |
        Remove-Item -Force
    Copy-Item -Path (Join-Path $root 'sounds\*.ogg') -Destination $soundDestination -Force
    Copy-Item -Path (Join-Path $root 'sounds\*.mp3') -Destination $soundDestination -Force

    $settings = Join-Path $plugins 'takeawalk.ini'
    if (-not (Test-Path -LiteralPath $settings)) {
        Copy-Item -LiteralPath (Join-Path $root 'takeawalk.ini') -Destination $settings
    }
    Write-Host "Installed Take a Walk into: $plugins"
}
