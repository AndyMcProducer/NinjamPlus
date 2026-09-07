param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [ValidateSet('status','connect','disconnect','hotspot','openVideo','recordStart','recordStop')]
    [string]$Action = 'status',
    [string]$Server,
    [string]$User,
    [bool]$Enabled = $true
)
$ErrorActionPreference = 'Stop'
$controlDirectory = (Resolve-Path -LiteralPath $Directory).Path
$statusFile = Join-Path $controlDirectory 'status.json'
if ($Action -eq 'status') {
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            Get-Content -Raw -LiteralPath $statusFile | ConvertFrom-Json
            return
        } catch {
            if ($attempt -eq 19) { throw }
            Start-Sleep -Milliseconds 50
        }
    }
}
if ($Action -eq 'connect' -and (!$Server -or !$User)) {
    throw 'connect requires -Server and -User'
}
$commandFile = Join-Path $controlDirectory 'command.json'
if (Test-Path -LiteralPath $commandFile) { throw 'A command is already pending' }
$commandId = [Guid]::NewGuid().ToString('N')
$command = @{id=$commandId;action=$Action}
if ($Action -eq 'connect') { $command.server=$Server; $command.user=$User }
if ($Action -eq 'hotspot') { $command.enabled=$Enabled }
$temporaryCommand = Join-Path $controlDirectory ($commandId + '.tmp')
$command | ConvertTo-Json -Compress | Set-Content -LiteralPath $temporaryCommand -Encoding UTF8
Move-Item -LiteralPath $temporaryCommand -Destination $commandFile
$deadline = [DateTimeOffset]::UtcNow.AddSeconds(10)
while ([DateTimeOffset]::UtcNow -lt $deadline) {
    try {
        $snapshot = Get-Content -Raw -LiteralPath $statusFile | ConvertFrom-Json
        if ($snapshot.commandId -eq $commandId) {
            $snapshot
            if ($snapshot.commandResult -ne 'accepted') { throw $snapshot.commandResult }
            return
        }
    } catch {
        if ($snapshot -and $snapshot.commandId -eq $commandId) { throw }
    }
    Start-Sleep -Milliseconds 100
}
throw 'No acknowledgement within 10 seconds; the command may still execute. Check status before retrying.'
