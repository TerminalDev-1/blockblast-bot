# Start the bot in the background. Logs, PID and saved screens stay in this folder.
#   .\start-bot.ps1                         the main version (bot.py here)
#   .\start-bot.ps1 -Version v5.2-exp-flash  a version from versions\<name>\ (bot.py, or bbbot.exe for C++ versions)
#   .\start-bot.ps1 -Extra '--gap-ms 30'     extra options for the bot
param([string]$Version = '', [string]$Extra = '')
$ErrorActionPreference = 'Stop'
$folder = $PSScriptRoot
$dir = if ($Version) { Join-Path 'versions' $Version } else { '.' }
$exe = Join-Path $dir 'bbbot.exe'
$script = Join-Path $dir 'bot.py'
if (Test-Path -LiteralPath (Join-Path $folder $exe)) { $program = Join-Path $folder $exe; $botArgs = @() ; $shown = $exe }
elseif (Test-Path -LiteralPath (Join-Path $folder $script)) { $program = (Get-Command python.exe -ErrorAction Stop).Source; $botArgs = @('-u', $script); $shown = $script }
else { Write-Output "No version '$Version' in versions\."; exit 1 }
$pidFile = Join-Path $folder 'bot.pid'
if (Test-Path -LiteralPath $pidFile) {
    $oldId = [int](Get-Content -LiteralPath $pidFile -Raw)
    $old = Get-CimInstance Win32_Process -Filter "ProcessId = $oldId"
    if ($old -and $old.CommandLine -match 'bot\.py|bbbot\.exe') { Write-Output "Bot is already running (PID $oldId)."; exit }
}
$log = Join-Path $folder 'bot.log'
if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log (Join-Path $folder 'bot.prev.log') -Force }  # keep the last run's log
$botArgs += @('--play', '--max-moves', '2000000000') + ($Extra -split ' ' | Where-Object { $_ })
$proc = Start-Process -FilePath $program -ArgumentList $botArgs -WorkingDirectory $folder -WindowStyle Hidden -RedirectStandardOutput (Join-Path $folder 'bot.log') -RedirectStandardError (Join-Path $folder 'bot-error.log') -PassThru
Set-Content -LiteralPath $pidFile -Value $proc.Id
Write-Output "Bot started in background (PID $($proc.Id), $shown)."
