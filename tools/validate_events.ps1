# validate_events.ps1 - check event files with PEMF's own loader, outside the game.
#
#   .\tools\validate_events.ps1                 # content\PEMF\events
#   .\tools\validate_events.ps1 C:\my\events    # any folder of *.json events
#
# Builds tools\validate_events.cpp (x86, like the mod) into build\ and runs it.
# Every line saying REJECTED names the event and the field at fault.
param([string]$Folder = '')

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'
if (-not $Folder) { $Folder = Join-Path $root 'content\PEMF\events' }
New-Item -ItemType Directory -Force $build | Out-Null

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No VS install with the x86/x64 C++ toolset was found." }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'

$cmd = @"
call "$vcvars" x86 >nul 2>&1
if errorlevel 1 exit /b 1
cd /d "$build"
cl /nologo /O1 /MT /GS- /W3 /EHsc /std:c++17 /DWIN32 /D_CRT_SECURE_NO_WARNINGS "$root\tools\validate_events.cpp" /I"$root\src\core" /I"$build" /Fe:validate_events.exe /link kernel32.lib user32.lib winmm.lib ole32.lib >validate_build.log 2>&1
"@
$bat = Join-Path $build 'validate_events.cmd'
Set-Content -Path $bat -Value $cmd -Encoding ascii
cmd /c $bat
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $build 'validate_build.log'); throw "validator did not build" }

& (Join-Path $build 'validate_events.exe') $Folder
