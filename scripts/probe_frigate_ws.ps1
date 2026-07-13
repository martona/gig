#Requires -Version 7.1
<#
Frigate /ws discovery probe (motion-events project).

Frigate's own web UI receives real-time state over a WebSocket at /ws on the
same port and behind the same auth as the REST API -- no MQTT broker needed.
This probe logs everything that arrives on that socket so we can learn the
exact topic names, payload shapes, and on-connect replay behavior before
building the in-app client.

Output goes to its own log file (NOT the gig app log) because the periodic
stats/state updates are noisy. Each line is:  [yyyy-MM-dd HH:mm:ss.fff] <raw message>
Lines starting with "--- " are probe lifecycle events (connect/close/error).

Capture (Ctrl+C to stop, or -DurationSeconds):
  pwsh scripts/probe_frigate_ws.ps1 -BaseUrl https://frigate.lan:8971/ -User admin
  pwsh scripts/probe_frigate_ws.ps1 -BaseUrl https://frigate.lan:8971/ -User admin -DurationSeconds 14400

Summarize a finished capture (topic histogram + lifecycle events):
  pwsh scripts/probe_frigate_ws.ps1 -Summarize -OutFile frigate-ws-probe.log

TLS note: certificate verification is DISABLED (this is a discovery tool; the
app itself pins). Password is prompted (masked) when -User is given without
-Password.
#>
[CmdletBinding(DefaultParameterSetName = 'Capture')]
param(
    [Parameter(ParameterSetName = 'Capture', Mandatory)]
    [string]$BaseUrl,

    [Parameter(ParameterSetName = 'Capture')]
    [string]$User = '',

    [Parameter(ParameterSetName = 'Capture')]
    [string]$Password = '',

    # 0 = run until Ctrl+C.
    [Parameter(ParameterSetName = 'Capture')]
    [int]$DurationSeconds = 0,

    [string]$OutFile = 'frigate-ws-probe.log',

    [Parameter(ParameterSetName = 'Summarize', Mandatory)]
    [switch]$Summarize
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --------------------------------------------------------------------------
# Summarize mode: reduce a capture to what we actually want to know.
# --------------------------------------------------------------------------
if ($Summarize) {
    $path = [IO.Path]::GetFullPath($OutFile, (Get-Location).Path)
    if (-not (Test-Path $path)) { throw "no capture at $path" }

    $lineRe  = [regex]'^\[([^\]]+)\]\s(.*)$'
    $topicRe = [regex]'"topic"\s*:\s*"([^"]+)"'
    $topics = [ordered]@{}
    $meta = [System.Collections.Generic.List[string]]::new()

    foreach ($line in [IO.File]::ReadLines($path)) {
        $m = $lineRe.Match($line)
        if (-not $m.Success) { continue }
        $ts = $m.Groups[1].Value
        $msg = $m.Groups[2].Value
        if ($msg.StartsWith('--- ')) {
            $meta.Add("$ts  $($msg.Substring(4))")
            continue
        }
        $tm = $topicRe.Match($msg)
        $key = if ($tm.Success) { $tm.Groups[1].Value } else { '(no topic field)' }
        if (-not $topics.Contains($key)) {
            $sample = $msg -replace '\s+', ' '
            if ($sample.Length -gt 120) { $sample = $sample.Substring(0, 120) + '...' }
            $topics[$key] = [pscustomobject]@{ Topic = $key; Count = 0; First = $ts; Last = $ts; Sample = $sample }
        }
        $t = $topics[$key]
        $t.Count++
        $t.Last = $ts
    }

    Write-Host "`n== Topics ($($topics.Count)) =="
    $topics.Values | Sort-Object Count -Descending | Format-Table Topic, Count, First, Last -AutoSize
    Write-Host '== First sample per topic =='
    foreach ($t in $topics.Values | Sort-Object Count -Descending) {
        Write-Host "  $($t.Topic)"
        Write-Host "    $($t.Sample)"
    }
    Write-Host "`n== Probe lifecycle ($($meta.Count) events) =="
    $show = if ($meta.Count -gt 60) { $meta[0..29] + '  ...' + $meta[($meta.Count - 30)..($meta.Count - 1)] } else { $meta }
    $show | ForEach-Object { Write-Host "  $_" }
    return
}

# --------------------------------------------------------------------------
# Capture mode.
# --------------------------------------------------------------------------

# The TLS-accept-anything callback must be a compiled delegate: .NET invokes it
# on a worker thread, where a PowerShell scriptblock cannot run.
if (-not ('GigProbeTls' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Net.Security;
public static class GigProbeTls {
    public static readonly RemoteCertificateValidationCallback AcceptAll =
        (sender, certificate, chain, sslPolicyErrors) => true;
}
'@
}

$base = $BaseUrl.TrimEnd('/')
$baseUri = [Uri]$base
$wsScheme = switch ($baseUri.Scheme) {
    'https' { 'wss' }
    'http'  { 'ws' }
    default { throw "BaseUrl must be http(s)://, got '$($baseUri.Scheme)'" }
}
$wsUri = [Uri]"${wsScheme}://$($baseUri.Authority)$($baseUri.AbsolutePath.TrimEnd('/'))/ws"

if ($User -and -not $Password) {
    $Password = Read-Host -MaskInput "password for $User"
}

function Get-RootMessage([System.Exception]$ex) {
    while ($ex.InnerException) { $ex = $ex.InnerException }
    return $ex.Message
}

# Same call gig makes (src/net/frigate_auth.cpp): POST /api/login, token
# arrives as the frigate_token cookie.
function Get-FrigateToken {
    $session = [Microsoft.PowerShell.Commands.WebRequestSession]::new()
    $body = @{ user = $User; password = $Password } | ConvertTo-Json -Compress
    $resp = Invoke-WebRequest -Uri "$base/api/login" -Method Post -ContentType 'application/json' `
        -Body $body -WebSession $session -SkipCertificateCheck -SkipHttpErrorCheck -TimeoutSec 30
    if ($resp.StatusCode -lt 200 -or $resp.StatusCode -ge 300) {
        $snippet = ([string]$resp.Content) -replace '\s+', ' '
        if ($snippet.Length -gt 200) { $snippet = $snippet.Substring(0, 200) + '...' }
        throw "login failed: HTTP $($resp.StatusCode) ($snippet)"
    }
    $cookie = $session.Cookies.GetCookies($baseUri) | Where-Object Name -EQ 'frigate_token' | Select-Object -First 1
    if (-not $cookie) {
        Write-Warning 'login OK but no frigate_token cookie -- connecting unauthenticated'
        return $null
    }
    return $cookie.Value
}

# Blocks on $task in 200ms slices so Ctrl+C stays responsive; $false = deadline hit.
function Wait-ProbeTask([System.Threading.Tasks.Task]$task, [datetime]$deadline) {
    while (-not $task.Wait(200)) {
        if ([DateTime]::UtcNow -ge $deadline) { return $false }
    }
    return $true
}

# Complete the close handshake so the server side ends cleanly instead of
# logging an aborted connection.
function Close-Quietly([System.Net.WebSockets.ClientWebSocket]$ws) {
    try {
        $null = $ws.CloseOutputAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure,
            'probe done', [Threading.CancellationToken]::None).Wait(2000)
    } catch {}
}

$logPath = [IO.Path]::GetFullPath($OutFile, (Get-Location).Path)
$writer = [IO.StreamWriter]::new($logPath, $true, [Text.Encoding]::UTF8)
$writer.AutoFlush = $true

function Write-ProbeLog([string]$line) {
    $script:writer.WriteLine("[$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss.fff'))] $line")
}

$deadline = if ($DurationSeconds -gt 0) { [DateTime]::UtcNow.AddSeconds($DurationSeconds) } else { [DateTime]::MaxValue }
Write-Host "capturing $wsUri -> $logPath (Ctrl+C to stop)"
Write-ProbeLog "--- probe started: $wsUri"

try {
    while ([DateTime]::UtcNow -lt $deadline) {
        $ws = $null
        try {
            $token = $null
            if ($User) {
                $token = Get-FrigateToken
                if ($token) { Write-ProbeLog '--- login ok' }
            }

            $ws = [System.Net.WebSockets.ClientWebSocket]::new()
            $ws.Options.RemoteCertificateValidationCallback = [GigProbeTls]::AcceptAll
            if ($token) {
                $ws.Options.Cookies = [System.Net.CookieContainer]::new()
                $ws.Options.Cookies.Add([System.Net.Cookie]::new('frigate_token', $token, '/', $baseUri.Host))
            }

            Write-ProbeLog "--- connecting"
            # Bound each handshake attempt: ConnectAsync has no built-in timeout,
            # so a server that accepts TCP but never answers the upgrade (hung
            # container, wedged nginx worker) would otherwise pin the probe at
            # "connecting" forever with no error and no retry.
            $attemptDeadline = [DateTime]::UtcNow.AddSeconds(30)
            if ($deadline -lt $attemptDeadline) { $attemptDeadline = $deadline }
            if (-not (Wait-ProbeTask $ws.ConnectAsync($wsUri, [Threading.CancellationToken]::None) $attemptDeadline)) {
                $ws.Abort()
                if ([DateTime]::UtcNow -ge $deadline) { break }
                throw 'connect timed out after 30s (server accepted TCP but never completed the WebSocket handshake)'
            }
            Write-ProbeLog '--- connected'
            Write-Host "$((Get-Date).ToString('HH:mm:ss')) connected"

            $buffer = [byte[]]::new(65536)
            $segment = [ArraySegment[byte]]::new($buffer)
            $msg = [IO.MemoryStream]::new()
            $expired = $false

            while ($ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
                $recv = $ws.ReceiveAsync($segment, [Threading.CancellationToken]::None)
                if (-not (Wait-ProbeTask $recv $deadline)) { $ws.Abort(); $expired = $true; break }
                $r = $recv.Result
                if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
                    Write-ProbeLog "--- closed by server: $($r.CloseStatus) $($r.CloseStatusDescription)"
                    Close-Quietly $ws
                    break
                }
                $msg.Write($buffer, 0, $r.Count)
                if (-not $r.EndOfMessage) { continue }

                $text = if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Text) {
                    [Text.Encoding]::UTF8.GetString($msg.ToArray())
                } else {
                    "[binary message, $($msg.Length) bytes]"
                }
                $msg.SetLength(0)
                Write-ProbeLog $text

                $preview = $text -replace '\s+', ' '
                if ($preview.Length -gt 160) { $preview = $preview.Substring(0, 160) + '...' }
                Write-Host "$((Get-Date).ToString('HH:mm:ss')) $preview"
            }
            if ($expired) { break }
        }
        catch {
            $reason = Get-RootMessage $_.Exception
            Write-ProbeLog "--- error: $reason"
            Write-Host "$((Get-Date).ToString('HH:mm:ss')) error: $reason (retrying in 5s)"
        }
        finally {
            if ($ws) { $ws.Dispose() }
        }

        if ([DateTime]::UtcNow -ge $deadline) { break }
        Write-ProbeLog '--- reconnecting in 5s'
        Start-Sleep -Seconds 5
    }
    Write-ProbeLog '--- probe stopped (duration reached)'
}
finally {
    $writer.Dispose()
    Write-Host "capture saved to $logPath"
    Write-Host "summarize with: pwsh scripts/probe_frigate_ws.ps1 -Summarize -OutFile `"$logPath`""
}
