# A throwaway listener for CS2's Game State Integration, to find out what actually arrives.
#
# Deliberately NOT in tools/server: that is another session's code, and the question here is
# only "does the data exist and what is in it". If the answer is yes, the receiving half goes
# there properly.
#
# A raw TcpListener rather than HttpListener on purpose: HttpListener wants a urlacl
# reservation for a prefix unless it is running elevated, and nothing here is worth asking for
# administrator for. CS2 sends an ordinary POST with Content-Length, so reading the headers and
# then that many bytes is the whole protocol we need.

param(
    [int]$Port = 57448,
    [int]$Seconds = 600,
    [string]$OutFile = (Join-Path $env:TEMP 'cs2vr-gsi.jsonl')
)

$ErrorActionPreference = 'Stop'

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
"listening on 127.0.0.1:$Port, writing to $OutFile"
"stop with ctrl+c, or it gives up after $Seconds s"

if (Test-Path $OutFile) { Remove-Item $OutFile }
$deadline = (Get-Date).AddSeconds($Seconds)
$count = 0

while ((Get-Date) -lt $deadline) {
    if (-not $listener.Pending()) { Start-Sleep -Milliseconds 50; continue }

    $client = $listener.AcceptTcpClient()
    try {
        $stream = $client.GetStream()
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)

        # Headers, to find Content-Length. CS2 does not use chunked encoding for this.
        $len = 0
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line -or $line -eq '') { break }
            if ($line -match '^(?i)Content-Length:\s*(\d+)') { $len = [int]$Matches[1] }
        }

        $body = ''
        if ($len -gt 0) {
            $buf = New-Object char[] $len
            $read = 0
            while ($read -lt $len) {
                $n = $reader.Read($buf, $read, $len - $read)
                if ($n -le 0) { break }
                $read += $n
            }
            $body = -join $buf[0..($read - 1)]
        }

        # Answer before doing anything slow: CS2 throttles and will consider us timed out.
        $resp = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Length: 0`r`nConnection: close`r`n`r`n")
        $stream.Write($resp, 0, $resp.Length)
        $stream.Flush()

        if ($body) {
            Add-Content -Path $OutFile -Value $body -Encoding utf8
            $count++
            if ($count -eq 1) { "first payload received, $($body.Length) bytes" }
            if ($count % 50 -eq 0) { "$count payloads" }
        }
    } catch {
        "a connection failed: $($_.Exception.Message)"
    } finally {
        $client.Close()
    }
}

$listener.Stop()
"done: $count payloads -> $OutFile"
