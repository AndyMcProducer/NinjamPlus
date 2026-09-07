param(
    [Parameter(Mandatory)][ValidatePattern('^\d{1,3}(\.\d{1,3}){3}$')][string]$TurnAddress,
    [Parameter(Mandatory)][ValidateRange(1024,65535)][int]$NinjamPort,
    [ValidateSet('tcp','udp')][string]$Protocol = 'tcp',
    [ValidateRange(1,65535)][int]$TurnPort = 443,
    [ValidateSet('basic','severe','playout')][string]$Profile = 'basic',
    [switch]$SkipOutage,
    [string]$ClumsyPath = 'C:\Users\steve\Documents\clumsy-0.3-win64-a\clumsy.exe'
)
$ErrorActionPreference = 'Stop'
$log = Join-Path $PSScriptRoot '../test-results/clumsy-events.jsonl'
$stopFile = Join-Path $PSScriptRoot '../test-results/clumsy-stop'
function Write-Event($name, $state, $details) {
    @{at=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); name=$name; state=$state; details=$details} |
        ConvertTo-Json -Compress -Depth 5 | Add-Content -LiteralPath $log
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this bounded packet impairment test as administrator; WinDivert requires elevation.'
}
if (Get-Process clumsy -ErrorAction SilentlyContinue) { throw 'Close existing Clumsy before starting this test.' }
if (Test-Path -LiteralPath $stopFile) { throw 'Remove test-results/clumsy-stop before starting a new test.' }
$videoFilter = "$Protocol and (ip.SrcAddr == $TurnAddress or ip.DstAddr == $TurnAddress) and ($Protocol.SrcPort == $TurnPort or $Protocol.DstPort == $TurnPort)"
$audioFilter = "outbound and loopback and tcp and (tcp.SrcPort == $NinjamPort or tcp.DstPort == $NinjamPort)"
$cases = @(
    @{name='turn-lag300'; filter=$videoFilter; seconds=20; args='--lag on --lag-inbound on --lag-outbound on --lag-time 300'},
    @{name='turn-drop5'; filter=$videoFilter; seconds=20; args='--drop on --drop-inbound on --drop-outbound on --drop-chance 5'},
    @{name='turn-outage12'; filter=$videoFilter; seconds=12; args='--drop on --drop-inbound on --drop-outbound on --drop-chance 100'},
    @{name='audio-outage12'; filter=$audioFilter; seconds=12; args='--drop on --drop-inbound on --drop-outbound on --drop-chance 100'}
)
if ($Profile -eq 'severe' -or $Profile -eq 'playout') {
    # Throttle releases randomly selected batches after a bounded hold. Combined
    # with lag this creates varying delivery delay, not a fixed delay-only test.
    function New-Fault($name, $seconds, $recovery, $lag, $loss, $jitter, $filter) {
        $faultArgs = "--drop on --drop-inbound on --drop-outbound on --drop-chance $loss"
        if ($lag -gt 0) { $faultArgs += " --lag on --lag-inbound on --lag-outbound on --lag-time $lag" }
        if ($jitter -gt 0) { $faultArgs += " --throttle on --throttle-inbound on --throttle-outbound on --throttle-chance 35 --throttle-frame $jitter" }
        return @{name=$name; seconds=$seconds; recovery=$recovery; filter=$filter; args=$faultArgs}
    }
    $combined = "($videoFilter) or ($audioFilter)"
    $cases = @(
        (New-Fault 'loss20' 25 35 0 20 0 $videoFilter),
        (New-Fault 'loss40-jitter400-lag250' 25 45 250 40 400 $videoFilter),
        (New-Fault 'lag1000-jitter800' 20 45 1000 5 800 $videoFilter),
        (New-Fault 'dynamic1' 6 0 50 3 100 $combined),
        (New-Fault 'dynamic2' 6 0 400 15 400 $combined),
        (New-Fault 'dynamic3' 6 0 900 40 800 $combined),
        (New-Fault 'dynamic4' 6 0 1500 80 1000 $combined),
        (New-Fault 'dynamic5' 6 0 100 5 200 $combined),
        (New-Fault 'dynamic6' 6 60 500 25 600 $combined),
        (New-Fault 'combined-lag300-loss15-jitter500' 25 60 300 15 500 $combined),
        (New-Fault 'combined-outage20' 20 75 0 100 0 $combined)
    )
    if ($Profile -eq 'playout') {
        $cases = @(
            (New-Fault 'playout-loss20-lag300-jitter500' 25 45 300 20 500 $videoFilter),
            (New-Fault 'playout-loss40-lag800-jitter800' 20 45 800 40 800 $videoFilter),
            (New-Fault 'playout-combined-outage12' 12 45 0 100 0 $combined)
        )
    }
}
if ($SkipOutage) { $cases = @($cases | Where-Object { $_.name -notmatch 'outage' }) }
Write-Event 'suite' 'start' @{turn=$TurnAddress; turnPort=$TurnPort; protocol=$Protocol; profile=$Profile; ninjamPort=$NinjamPort; skipOutage=[bool]$SkipOutage}
foreach ($case in $cases) {
    if (Test-Path -LiteralPath $stopFile) { Write-Event 'suite' 'cancelled' $null; return }
    $child = $null
    try {
        $arguments = '--filter "' + $case.filter + '" --timeout ' + $case.seconds + ' ' + $case.args
        Write-Event $case.name 'launch' $arguments
        $child = Start-Process -FilePath $ClumsyPath -ArgumentList $arguments -WindowStyle Hidden -PassThru
        $deadline = [DateTimeOffset]::UtcNow.AddSeconds($case.seconds + 5)
        while (!$child.WaitForExit(200)) {
            if (Test-Path -LiteralPath $stopFile) { Write-Event 'suite' 'cancelled' $null; return }
            if ([DateTimeOffset]::UtcNow -ge $deadline) {
                Stop-Process -Id $child.Id -Force
                throw 'Clumsy exceeded its configured timeout.'
            }
        }
        Write-Event $case.name 'exited' @{exitCode=$child.ExitCode}
    } finally {
        if ($child -and !$child.HasExited) { Stop-Process -Id $child.Id -Force }
    }
    $recovery = if ($case.ContainsKey('recovery')) { $case.recovery } else { 30 }
    Write-Event $case.name 'recovery-start' @{seconds=$recovery}
    # This runner is a background process; media/diagnostic collection continues.
    $recoverUntil = [DateTimeOffset]::UtcNow.AddSeconds($recovery)
    while ([DateTimeOffset]::UtcNow -lt $recoverUntil) {
        if (Test-Path -LiteralPath $stopFile) { Write-Event 'suite' 'cancelled' $null; return }
        Start-Sleep -Milliseconds 200
    }
    Write-Event $case.name 'recovery-end' $null
}
Write-Event 'suite' 'complete' $null
