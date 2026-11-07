# Builds bbhelper.dex (the tablet-side helper) from BBHelper.java. Needs a JDK and the
# Android SDK (platforms\android-*\android.jar and build-tools\*\d8.bat). The bot pushes
# the dex to the tablet itself.
$ErrorActionPreference = 'Stop'
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
$jar = Get-ChildItem (Join-Path $sdk 'platforms') -Filter android.jar -Recurse | Sort-Object FullName | Select-Object -Last 1
$d8 = Get-ChildItem (Join-Path $sdk 'build-tools') -Filter d8.bat -Recurse | Sort-Object FullName | Select-Object -Last 1
$here = $PSScriptRoot
$out = Join-Path $env:TEMP 'bbhelper-build'
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory $out | Out-Null
javac -source 11 -target 11 -nowarn -cp $jar.FullName -d $out (Join-Path $here 'BBHelper.java')
if ($LASTEXITCODE) { exit 1 }
& $d8.FullName --min-api 29 --output $out (Get-ChildItem $out -Filter *.class).FullName
if ($LASTEXITCODE) { exit 1 }
Copy-Item (Join-Path $out 'classes.dex') (Join-Path $here 'bbhelper.dex') -Force
Write-Output "Built $(Join-Path $here 'bbhelper.dex')"
