# End-to-end run of agent_wire_ipc.exe: one server process, one --client exchange, one --bench run.
# Usage: pwsh scripts/wire_scenario.ps1 [-Exe .\agent_wire_ipc.exe] [-Count 1000] [-Out bench.txt]
# Exit code 0 only when the client receipt was ACCEPTED, every bench receipt was accepted and the
# server exited 0 after its two sessions. Output is printed verbatim and, with -Out, saved to a file.
param(
    [string]$Exe = ".\agent_wire_ipc.exe",
    [int]$Count = 1000,
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$Exe = (Resolve-Path $Exe).Path
$serverLog = Join-Path ([IO.Path]::GetTempPath()) ("agent_wire_server_" + [guid]::NewGuid().ToString("N") + ".txt")

$server = Start-Process -FilePath $Exe -ArgumentList "--server", "2" -PassThru -NoNewWindow -RedirectStandardOutput $serverLog
try {
    $clientText = & $Exe --client "hello agent" | Out-String
    $clientExit = $LASTEXITCODE
    $benchText = & $Exe --bench $Count | Out-String
    $benchExit = $LASTEXITCODE
    if (-not $server.WaitForExit(30000)) { $server.Kill(); throw "server did not exit after two sessions" }
}
finally {
    if (-not $server.HasExited) { $server.Kill() }
}
$serverText = Get-Content $serverLog -Raw
Remove-Item $serverLog -ErrorAction SilentlyContinue

$report = @"
## server (exit $($server.ExitCode))
$serverText
## client (exit $clientExit)
$clientText
## bench $Count (exit $benchExit)
$benchText
"@
Write-Host $report
if ($Out) { $report | Out-File $Out -Encoding utf8 }

$ok = $clientExit -eq 0 -and $benchExit -eq 0 -and $server.ExitCode -eq 0 -and
      $clientText.Contains("result            ACCEPTED") -and
      $benchText.Contains("result            $Count/$Count receipts accepted")
if (-not $ok) { Write-Error "wire scenario failed"; exit 1 }
exit 0
