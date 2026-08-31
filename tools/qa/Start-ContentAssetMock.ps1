# A stand-in for the assets repository's release downloads.
#
# It exists to exercise the parts of the downloader that a healthy connection never reaches:
# resume, a server that ignores Range, a transfer that dies mid-body. The real GitHub host has
# been verified to answer 206 with a well-formed Content-Range, so the mock's job is not to
# prove the happy path - it is to produce the failures on demand.
#
# Built on a raw TcpListener rather than HttpListener on purpose. HttpListener routes through
# HTTP.SYS, which needs a URL reservation for anything but the default prefixes and answers a
# missing one with a connection reset; and half of what this script exists to do - drop a
# connection mid-body, ignore a Range header - is easier to express at the socket than through
# a framework that keeps trying to be correct.
#
# Serves <base>/<tag>/<asset>, which is the shape the downloader builds. Loopback only, which
# is also the only shape the downloader will accept from DAR_QA_CONTENT_BASE.
[CmdletBinding()]
param(
    # Directory holding the bundle files, flat.
    [Parameter(Mandatory)][string]$AssetRoot,
    [int]$Port = 8788,
    # Answer every request with 200 and the whole file, as a proxy that strips Range would.
    [switch]$NoRange,
    # Close the connection after this many bytes of body. 0 disables.
    [long]$DropAfter = 0,
    # Bytes per second, 0 for unlimited.
    [long]$Throttle = 0,
    [int]$TimeoutSeconds = 900
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $AssetRoot -PathType Container)) {
    throw "Asset root was not found: $AssetRoot"
}

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $Port)
# A previous run killed mid-connection leaves the port in TIME_WAIT, and without this the next
# case fails to bind and looks like a downloader fault.
$listener.Server.SetSocketOption([Net.Sockets.SocketOptionLevel]::Socket,
    [Net.Sockets.SocketOptionName]::ReuseAddress, $true)
$listener.Start()
Write-Host "content asset mock on http://127.0.0.1:$Port/ serving $AssetRoot"
if ($NoRange) { Write-Host "  Range requests will be ignored" }
if ($DropAfter -gt 0) { Write-Host "  connections drop after $DropAfter bytes" }
if ($Throttle -gt 0) { Write-Host "  throttled to $Throttle bytes/s" }

function Send-Text {
    param($Stream, [string]$Text)
    $bytes = [Text.Encoding]::ASCII.GetBytes($Text)
    $Stream.Write($bytes, 0, $bytes.Length)
}

$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
try {
    while ([DateTime]::UtcNow -lt $deadline) {
        if (-not $listener.Pending()) {
            Start-Sleep -Milliseconds 50
            continue
        }

        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        try {
            # Read the request head. Bodies are never sent to this server, so everything up to
            # the blank line is the whole request.
            $head = ""
            $byte = New-Object byte[] 1
            while (-not $head.EndsWith("`r`n`r`n")) {
                if ($stream.Read($byte, 0, 1) -le 0) { break }
                $head += [char]$byte[0]
                if ($head.Length -gt 8192) { break }
            }
            if (-not $head) { continue }

            $lines = $head -split "`r`n"
            $requestLine = $lines[0]
            $path = ($requestLine -split ' ')[1]
            $rangeStart = -1L
            foreach ($line in $lines) {
                if ($line -match '^(?i)Range:\s*bytes=(\d+)-') { $rangeStart = [long]$Matches[1] }
            }

            $name = ($path.Trim('/') -split '/')[-1]
            $file = Join-Path $AssetRoot $name
            Write-Host "  -> $name range=$rangeStart"

            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
                Send-Text $stream "HTTP/1.1 404 Not Found`r`nContent-Length: 0`r`nConnection: close`r`n`r`n"
                continue
            }

            $source = [IO.File]::OpenRead($file)
            $total = $source.Length
            $start = 0L
            $status = "200 OK"
            $extra = ""
            if (-not $NoRange -and $rangeStart -ge 0) {
                if ($rangeStart -ge $total) {
                    Send-Text $stream "HTTP/1.1 416 Range Not Satisfiable`r`nContent-Length: 0`r`nConnection: close`r`n`r`n"
                    $source.Dispose()
                    continue
                }
                $start = $rangeStart
                $source.Position = $start
                $status = "206 Partial Content"
                $extra = "Content-Range: bytes $start-$($total - 1)/$total`r`n"
            }

            $remaining = $total - $start
            Send-Text $stream ("HTTP/1.1 $status`r`nContent-Type: application/octet-stream`r`n" +
                "Content-Length: $remaining`r`n" + $extra + "Connection: close`r`n`r`n")

            $buffer = New-Object byte[] (256 * 1024)
            $sent = 0L
            while ($sent -lt $remaining) {
                $want = [Math]::Min($buffer.Length, $remaining - $sent)
                $read = $source.Read($buffer, 0, $want)
                if ($read -le 0) { break }
                $stream.Write($buffer, 0, $read)
                $sent += $read
                if ($DropAfter -gt 0 -and $sent -ge $DropAfter) {
                    Write-Host "  dropping $name after $sent bytes"
                    $client.Client.LingerState = [Net.Sockets.LingerOption]::new($true, 0)
                    break
                }
                if ($Throttle -gt 0) {
                    Start-Sleep -Milliseconds ([int](1000.0 * $read / $Throttle))
                }
            }
            $source.Dispose()
        }
        catch {
            Write-Host "  request failed: $($_.Exception.Message)"
        }
        finally {
            try { $stream.Dispose() } catch { }
            try { $client.Close() } catch { }
        }
    }
}
finally {
    $listener.Stop()
}
