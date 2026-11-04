$pidFile = Join-Path $PSScriptRoot 'bot.pid'
if (-not (Test-Path -LiteralPath $pidFile)) { Write-Output 'No bot PID file found.'; exit }
$botId = [int](Get-Content -LiteralPath $pidFile -Raw)
$botProc = Get-CimInstance Win32_Process -Filter "ProcessId = $botId"
if ($botProc -and $botProc.CommandLine -match 'bot\.py') { Stop-Process -Id $botId; Write-Output "Stopped bot (PID $botId)." } else { Write-Output 'Bot is not running.' }
Remove-Item -LiteralPath $pidFile -Force
