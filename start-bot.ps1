$ErrorActionPreference = 'Stop'
$folder = $PSScriptRoot
$pidFile = Join-Path $folder 'bot.pid'
if (Test-Path -LiteralPath $pidFile) {
    $oldId = [int](Get-Content -LiteralPath $pidFile -Raw)
    $old = Get-CimInstance Win32_Process -Filter "ProcessId = $oldId"
    if ($old -and $old.CommandLine -match 'bot\.py') { Write-Output "Bot is already running (PID $oldId)."; exit }
}
$log = Join-Path $folder 'bot.log'
if (Test-Path -LiteralPath $log) { Move-Item -LiteralPath $log (Join-Path $folder 'bot.prev.log') -Force }  # keep the last run's log
$python = (Get-Command python.exe -ErrorAction Stop).Source
$proc = Start-Process -FilePath $python -ArgumentList @('-u', 'bot.py', '--play', '--max-moves', '2000000000') -WorkingDirectory $folder -WindowStyle Hidden -RedirectStandardOutput (Join-Path $folder 'bot.log') -RedirectStandardError (Join-Path $folder 'bot-error.log') -PassThru
Set-Content -LiteralPath $pidFile -Value $proc.Id
Write-Output "Bot started in background (PID $($proc.Id))."
