# Start the bot in the background. Logs, PID and saved screens stay in this folder.
#   .\start-bot.ps1                         the main version (bot.py here)
#   .\start-bot.ps1 -Version v5.2-exp-flash  a version from versions\<name>\
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
$folder = $PSScriptRoot
$script = 'bot.py'
if ($Version) {
    $script = Join-Path (Join-Path 'versions' $Version) 'bot.py'
    if (-not (Test-Path -LiteralPath (Join-Path $folder $script))) { Write-Output "No version '$Version' in versions\."; exit 1 }
}
$pidFile = Join-Path $folder 'bot.pid'
if (Test-Path -LiteralPath $pidFile) {
    $oldId = [int](Get-Content -LiteralPath $pidFile -Raw)
    $old = Get-CimInstance Win32_Process -Filter "ProcessId = $oldId"
    if ($old -and $old.CommandLine -match 'bot\.py') { Write-Output "Bot is already running (PID $oldId)."; exit }
}
$log = Join-Path $folder 'bot.log'
if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log (Join-Path $folder 'bot.prev.log') -Force }  # keep the last run's log
$python = (Get-Command python.exe -ErrorAction Stop).Source
$proc = Start-Process -FilePath $python -ArgumentList @('-u', $script, '--play', '--max-moves', '2000000000') -WorkingDirectory $folder -WindowStyle Hidden -RedirectStandardOutput (Join-Path $folder 'bot.log') -RedirectStandardError (Join-Path $folder 'bot-error.log') -PassThru
Set-Content -LiteralPath $pidFile -Value $proc.Id
Write-Output "Bot started in background (PID $($proc.Id), $script)."
