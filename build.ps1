# Builds v5.6-hyperspeed with MinGW-w64 g++ (winget install BrechtSanders.WinLibs.POSIX.UCRT):
#   bbbot.exe       the bot (start-bot.ps1 -Version v5.6-hyperspeed runs it)
#   build\sim.exe   offline planner benchmark (sim.exe 96 1500 7000 [v51|v54|name=value...])
#   build\bbplan.dll  the planner for bench.py
# The tablet helper is built separately: helper\build-helper.ps1.
$ErrorActionPreference = 'Continue'  # compiler warnings go to stderr; failures are checked below
$here = $PSScriptRoot
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $gpp = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter g++.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gpp) { Write-Output 'g++ not found: winget install BrechtSanders.WinLibs.POSIX.UCRT'; exit 1 }
    $env:Path = "$($gpp.DirectoryName);$env:Path"
}
$src = Join-Path $here 'src'
$build = Join-Path $here 'build'
New-Item -ItemType Directory $build -Force | Out-Null
$flags = @('-O2', '-mpopcnt', '-std=c++20', '-static', '-Wall', '-Wno-unused-function')
$common = @("$src\planner.cpp", "$src\vision.cpp", "$src\util.cpp")
g++ @flags -o (Join-Path $here 'bbbot.exe') "$src\bot.cpp" "$src\device.cpp" @common -lws2_32
if ($LASTEXITCODE) { exit 1 }
g++ @flags -o (Join-Path $build 'sim.exe') "$src\sim.cpp" "$src\planner.cpp"
if ($LASTEXITCODE) { exit 1 }
g++ @flags -shared -o (Join-Path $build 'bbplan.dll') "$src\capi.cpp" "$src\planner.cpp"
if ($LASTEXITCODE) { exit 1 }
g++ @flags -o (Join-Path $build 'visiontest.exe') "$src\visiontest.cpp" @common
if ($LASTEXITCODE) { exit 1 }
Write-Output 'Built bbbot.exe, build\sim.exe, build\bbplan.dll, build\visiontest.exe'
