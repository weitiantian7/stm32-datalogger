[CmdletBinding()]
param(
    [string]$Port,
    [string]$StartSequence,
    [string]$Count,
    [int]$SlaveAddress = 1,
    [int]$BaudRate = 115200,
    [int]$TimeoutMs = 1000,
    [int]$Retries = 2,
    [int]$IntervalMs = 50,
    [int]$ReferenceMv = 3300,
    [string]$OutputPath,
    [switch]$ContinueOnReadError,
    [string]$DecodeHex
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$script:HistoryColumns = @(
    'sequence', 'uptime_ms', 'temperature_c', 'humidity_rh_pct',
    'adc1_raw', 'adc2_raw', 'reference_mv', 'adc1_mv', 'adc2_mv',
    'status', 'error_code', 'error_message'
)

function Get-ModbusCrc16 {
    param([byte[]]$Bytes)
    [uint32]$crc = 0xFFFF
    foreach ($value in $Bytes) {
        $crc = $crc -bxor $value
        for ($bit = 0; $bit -lt 8; $bit++) {
            if (($crc -band 1) -ne 0) { $crc = ($crc -shr 1) -bxor 0xA001 }
            else { $crc = $crc -shr 1 }
        }
    }
    return [uint16]$crc
}

function Add-ModbusCrc {
    param([byte[]]$Payload)
    $crc = Get-ModbusCrc16 $Payload
    [byte[]]$frame = @($Payload) + @([byte]($crc -band 255), [byte]($crc -shr 8))
    return ,$frame
}

function Get-UInt16BE {
    param([byte[]]$Bytes, [int]$Offset)
    return [int]($Bytes[$Offset] * 256 + $Bytes[$Offset + 1])
}

function Get-UInt32BE {
    param([byte[]]$Bytes, [int]$Offset)
    [uint64]$value = 0
    for ($i = 0; $i -lt 4; $i++) { $value = $value * 256 + $Bytes[$Offset + $i] }
    return [uint32]$value
}

function New-HistoryRequest {
    param([int]$Address, [uint32]$Sequence)
    if ($Address -lt 1 -or $Address -gt 247) { throw '从机地址必须在1到247之间。' }
    if ($Sequence -eq 0 -or $Sequence -eq [uint32]::MaxValue) { throw '序号0和0xFFFFFFFF为保留值。' }
    [byte[]]$payload = @(
        [byte]$Address, [byte]0x41,
        [byte](($Sequence -shr 24) -band 255),
        [byte](($Sequence -shr 16) -band 255),
        [byte](($Sequence -shr 8) -band 255),
        [byte]($Sequence -band 255)
    )
    return ,(Add-ModbusCrc $payload)
}

function New-HistoryErrorRow {
    param([uint32]$Sequence, [string]$Status, [string]$Code, [string]$Message)
    return [pscustomobject][ordered]@{
        sequence = $Sequence; uptime_ms = ''; temperature_c = ''; humidity_rh_pct = ''
        adc1_raw = ''; adc2_raw = ''; reference_mv = ''; adc1_mv = ''; adc2_mv = ''
        status = $Status; error_code = $Code; error_message = $Message
    }
}

function ConvertFrom-HistoryReply {
    param([byte[]]$Frame, [int]$Address, [uint32]$Sequence, [int]$VrefMv = 3300)
    if ($Frame.Length -lt 5) { throw [FormatException]::new('回复不完整。') }
    $bodyEnd = $Frame.Length - 3
    $actualCrc = Get-ModbusCrc16 ([byte[]]$Frame[0..$bodyEnd])
    $receivedCrc = [int]$Frame[$Frame.Length - 2] + 256 * [int]$Frame[$Frame.Length - 1]
    if ($actualCrc -ne $receivedCrc) { throw [FormatException]::new('回复CRC不正确。') }
    if ($Frame[0] -ne $Address) { throw [FormatException]::new('回复从机地址与请求不一致。') }

    if ($Frame[1] -eq 0xC1) {
        if ($Frame.Length -ne 5) { throw [FormatException]::new('异常回复长度应为5字节。') }
        $code = [int]$Frame[2]
        $message = switch ($code) {
            1 { '从机未识别0x41，请检查固件是否已下载。' }
            3 { '从机拒绝了请求格式或序号。' }
            4 { '记录读取失败：可能不存在、已覆盖、校验失败或Flash读取故障；当前固件未细分原因。' }
            default { '从机返回了异常回复。' }
        }
        return New-HistoryErrorRow $Sequence 'device_error' ('0x{0:X2}' -f $code) $message
    }

    if ($Frame[1] -ne 0x41 -or $Frame.Length -ne 21 -or $Frame[2] -ne 16) {
        throw [FormatException]::new('正常回复必须为0x41功能码、16字节数据、总长21字节。')
    }
    $returnedSequence = Get-UInt32BE $Frame 3
    if ($returnedSequence -ne $Sequence) { throw [FormatException]::new('回复记录序号与请求不一致。') }
    $temperature = Get-UInt16BE $Frame 11
    if ($temperature -ge 32768) { $temperature -= 65536 }
    $humidity = Get-UInt16BE $Frame 13
    $adc1 = Get-UInt16BE $Frame 15
    $adc2 = Get-UInt16BE $Frame 17
    if ($adc1 -gt 4095 -or $adc2 -gt 4095) {
        throw [FormatException]::new('ADC字段超出12位范围，请核对日志版本和字段定义。')
    }
    $culture = [Globalization.CultureInfo]::InvariantCulture
    return [pscustomobject][ordered]@{
        sequence = $returnedSequence
        uptime_ms = Get-UInt32BE $Frame 7
        temperature_c = ($temperature / 100.0).ToString('F2', $culture)
        humidity_rh_pct = ($humidity / 100.0).ToString('F2', $culture)
        adc1_raw = $adc1
        adc2_raw = $adc2
        reference_mv = $VrefMv
        adc1_mv = [int][Math]::Floor(($adc1 * [double]$VrefMv + 2047) / 4095)
        adc2_mv = [int][Math]::Floor(($adc2 * [double]$VrefMv + 2047) / 4095)
        status = 'ok'; error_code = ''; error_message = ''
    }
}

function Read-SerialBytes {
    param([object]$Connection, [int]$Length, [Diagnostics.Stopwatch]$Timer, [int]$LimitMs)
    [byte[]]$buffer = New-Object byte[] $Length
    $offset = 0
    while ($offset -lt $Length) {
        $remaining = $LimitMs - [int]$Timer.ElapsedMilliseconds
        if ($remaining -le 0) { throw [TimeoutException]::new('等待回复超时。') }
        $Connection.ReadTimeout = [Math]::Max(1, $remaining)
        $received = $Connection.Read($buffer, $offset, $Length - $offset)
        if ($received -le 0) { throw [TimeoutException]::new('串口未返回数据。') }
        $offset += $received
    }
    return ,$buffer
}

function Read-HistoryReply {
    param([object]$Connection, [int]$Address, [uint32]$Sequence,
          [int]$LimitMs = 1000, [int]$RetryCount = 2, [int]$VrefMv = 3300,
          [int]$RetryDelayMs = 100)
    [byte[]]$request = New-HistoryRequest $Address $Sequence
    $lastError = ''
    for ($attempt = 0; $attempt -le $RetryCount; $attempt++) {
        if ($attempt -gt 0) { Start-Sleep -Milliseconds $RetryDelayMs }
        try {
            $Connection.DiscardInBuffer()
            $Connection.Write($request, 0, $request.Length)
            $timer = [Diagnostics.Stopwatch]::StartNew()
            [byte[]]$header = Read-SerialBytes $Connection 3 $timer $LimitMs
            if ($header[0] -ne $Address) { throw [FormatException]::new('收到其他地址的数据。') }
            if ($header[1] -eq 0xC1) { $tailLength = 2 }
            elseif ($header[1] -eq 0x41 -and $header[2] -eq 16) { $tailLength = 18 }
            else { throw [FormatException]::new('回复功能码或数据长度不正确。') }
            [byte[]]$tail = Read-SerialBytes $Connection $tailLength $timer $LimitMs
            [byte[]]$frame = @($header) + @($tail)
            return ConvertFrom-HistoryReply $frame $Address $Sequence $VrefMv
        }
        catch {
            $lastError = $_.Exception.GetBaseException().Message
        }
    }
    return New-HistoryErrorRow $Sequence 'transport_error' '' (
        '请求{0}次后失败：{1}' -f ($RetryCount + 1), $lastError)
}

function ConvertTo-CsvCell {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return '' }
    if ($Value -is [IFormattable]) {
        $text = $Value.ToString($null, [Globalization.CultureInfo]::InvariantCulture)
    }
    else { $text = [string]$Value }
    if ($text -match '[,"\r\n]') { return '"' + $text.Replace('"', '""') + '"' }
    return $text
}

function Write-HistoryCsvRow {
    param([IO.StreamWriter]$Writer, [object]$Record)
    [string[]]$cells = @(foreach ($name in $script:HistoryColumns) {
        ConvertTo-CsvCell $Record.$name
    })
    $Writer.WriteLine(($cells -join ','))
}

function Export-HistoryRange {
    param([object]$Connection, [uint32]$Start, [int]$Total, [int]$Address,
          [string]$CsvPath, [int]$LimitMs = 1000, [int]$RetryCount = 2,
          [int]$GapMs = 50, [int]$VrefMv = 3300,
          [switch]$KeepGoing, [switch]$Quiet)
    $stream = $null
    $writer = $null
    $saved = 0
    $failed = 0
    $processed = 0
    $stopReason = ''
    try {
        $stream = [IO.File]::Open($CsvPath, [IO.FileMode]::CreateNew,
                                 [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($true))
        $writer.AutoFlush = $true
        $writer.WriteLine(($script:HistoryColumns -join ','))
        for ($index = 0; $index -lt $Total; $index++) {
            if ($index -gt 0 -and $GapMs -gt 0) { Start-Sleep -Milliseconds $GapMs }
            [uint32]$sequence = [uint64]$Start + [uint64]$index
            $record = Read-HistoryReply $Connection $Address $sequence $LimitMs $RetryCount $VrefMv
            Write-HistoryCsvRow $writer $record
            $processed++
            if ($record.status -eq 'ok') { $saved++ }
            else { $failed++ }
            if (-not $Quiet -and ($Total -le 20 -or $processed % 25 -eq 0 -or
                                  $processed -eq $Total -or $record.status -ne 'ok')) {
                Write-Host ('[{0}/{1}] 序号{2}，{3}，成功{4}条，错误{5}条' -f
                            $processed, $Total, $sequence, $record.status, $saved, $failed)
            }
            if ($record.status -ne 'ok') {
                $canContinue = $KeepGoing -and $record.status -eq 'device_error' -and
                               $record.error_code -eq '0x04'
                if (-not $canContinue) { $stopReason = $record.error_message; break }
            }
        }
    }
    finally {
        if ($null -ne $writer) { $writer.Dispose() }
        elseif ($null -ne $stream) { $stream.Dispose() }
    }
    return [pscustomobject]@{
        Saved = $saved; Failed = $failed; Processed = $processed; Requested = $Total
        Completed = ($processed -eq $Total); StopReason = $stopReason; Path = $CsvPath
    }
}

function ConvertTo-HistoryInteger {
    param([string]$Text, [string]$Label)
    $value = $Text.Trim()
    if ($value -match '^0[xX][0-9a-fA-F]+$') {
        return [Convert]::ToUInt64($value.Substring(2), 16)
    }
    if ($value -match '^[0-9]+$') {
        return [uint64]::Parse($value, [Globalization.CultureInfo]::InvariantCulture)
    }
    throw ('{0}请填写十进制数字或带0x前缀的十六进制数字。' -f $Label)
}

# Dot-sourcing loads the functions for offline tests, without opening a serial port.
if ($MyInvocation.InvocationName -eq '.') { return }

$connection = $null
$exitCode = 1
try {
    if ($SlaveAddress -lt 1 -or $SlaveAddress -gt 247) { throw '从机地址必须在1到247之间。' }
    if ($ReferenceMv -lt 1 -or $ReferenceMv -gt 65535) { throw '参考电压必须在1到65535mV之间。' }
    if ($TimeoutMs -lt 100 -or $TimeoutMs -gt 60000) { throw '超时时间必须在100到60000ms之间。' }
    if ($Retries -lt 0 -or $Retries -gt 10) { throw '重试次数必须在0到10之间。' }
    if ($IntervalMs -lt 5 -or $IntervalMs -gt 60000) { throw '请求间隔必须在5到60000ms之间。' }
    if ($BaudRate -lt 1) { throw '波特率必须为正数。' }

    if (-not [string]::IsNullOrWhiteSpace($DecodeHex)) {
        [string[]]$tokens = @($DecodeHex.Trim() -split '[\s,]+' | Where-Object { $_ -ne '' })
        [byte[]]$frame = @(foreach ($token in $tokens) {
            if ($token -notmatch '^(0[xX])?[0-9a-fA-F]{2}$') { throw '请按两位十六进制字节输入一帧回复。' }
            [Convert]::ToByte(($token -replace '^0[xX]', ''), 16)
        })
        if ($frame.Length -ne 21) { throw '离线解码模式需要一条完整的21字节正常回复。' }
        $sequenceValue = Get-UInt32BE $frame 3
        $decoded = ConvertFrom-HistoryReply $frame $SlaveAddress $sequenceValue $ReferenceMv
        $decoded | Format-List
        $exitCode = 0
    }
    else {
        Write-Host 'STM32 历史记录导出工具（0x41，115200 / 8N1）'
        Write-Host '请让STM32保持运行，并先在串口助手中点击“关闭串口”。'
        if ([string]::IsNullOrWhiteSpace($Port)) {
            $entered = Read-Host '串口号 [默认COM3]'
            if ([string]::IsNullOrWhiteSpace($entered)) { $Port = 'COM3' } else { $Port = $entered }
        }
        $Port = $Port.Trim().ToUpperInvariant()
        if ($Port -notmatch '^COM[1-9][0-9]*$') { throw '串口号格式应为COM3、COM4等。' }
        if ([string]::IsNullOrWhiteSpace($StartSequence)) {
            $entered = Read-Host '起始序号 [默认17613，也可填0x44CD]'
            if ([string]::IsNullOrWhiteSpace($entered)) { $StartSequence = '17613' }
            else { $StartSequence = $entered }
        }
        if ([string]::IsNullOrWhiteSpace($Count)) {
            $entered = Read-Host '读取条数 [默认10]'
            if ([string]::IsNullOrWhiteSpace($entered)) { $Count = '10' } else { $Count = $entered }
        }
        [uint64]$startValue = ConvertTo-HistoryInteger $StartSequence '起始序号'
        [uint64]$countValue = ConvertTo-HistoryInteger $Count '读取条数'
        if ($startValue -lt 1 -or $startValue -gt 4294967294) { throw '起始序号必须在1到0xFFFFFFFE之间。' }
        if ($countValue -lt 1 -or $countValue -gt 1000000) { throw '一次读取条数必须在1到1000000之间。' }
        [uint64]$endValue = $startValue + $countValue - 1
        if ($endValue -gt 4294967294) { throw '结束序号超出0xFFFFFFFE。' }

        if ([string]::IsNullOrWhiteSpace($OutputPath)) {
            $folder = Join-Path $PSScriptRoot 'exports'
            [void][IO.Directory]::CreateDirectory($folder)
            $name = 'history_{0}_to_{1}_{2}.csv' -f $startValue, $endValue, (Get-Date -Format 'yyyyMMdd_HHmmss_fff')
            $OutputPath = Join-Path $folder $name
        }
        $OutputPath = [IO.Path]::GetFullPath($OutputPath)
        if (Test-Path -LiteralPath $OutputPath) { throw '输出文件已存在，请选择新文件名。' }
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputPath))

        $connection = [IO.Ports.SerialPort]::new($Port, $BaudRate,
            [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
        $connection.Handshake = [IO.Ports.Handshake]::None
        $connection.DtrEnable = $false
        $connection.RtsEnable = $false
        $connection.WriteTimeout = $TimeoutMs
        try { $connection.Open() }
        catch {
            throw ('无法打开{0}：{1}。请检查连接，并关闭占用该串口的其他程序。' -f
                   $Port, $_.Exception.GetBaseException().Message)
        }
        Write-Host ('开始读取序号{0}到{1}，共{2}条。' -f $startValue, $endValue, $countValue)
        $summary = Export-HistoryRange -Connection $connection -Start ([uint32]$startValue) `
            -Total ([int]$countValue) -Address $SlaveAddress -CsvPath $OutputPath `
            -LimitMs $TimeoutMs -RetryCount $Retries -GapMs $IntervalMs `
            -VrefMv $ReferenceMv -KeepGoing:$ContinueOnReadError
        Write-Host ('已保存：{0}' -f $summary.Path)
        Write-Host ('正常记录{0}条，错误记录{1}条，已处理{2}/{3}条。' -f
                    $summary.Saved, $summary.Failed, $summary.Processed, $summary.Requested)
        if ($summary.Failed -eq 0 -and $summary.Completed) {
            Write-Host '导出完成，可以用Excel打开CSV文件。' -ForegroundColor Green
            $exitCode = 0
        }
        else {
            if ($summary.StopReason) { Write-Host ('停止原因：{0}' -f $summary.StopReason) -ForegroundColor Yellow }
            Write-Host 'CSV已保留成功记录和明确的错误行，请检查status列。' -ForegroundColor Yellow
            $exitCode = 2
        }
    }
}
catch {
    Write-Host ('操作未完成：{0}' -f $_.Exception.GetBaseException().Message) -ForegroundColor Red
    if ($OutputPath -and (Test-Path -LiteralPath $OutputPath)) {
        Write-Host ('已经写出的数据保留在：{0}' -f $OutputPath)
    }
    $exitCode = 1
}
finally {
    if ($null -ne $connection) { $connection.Dispose() }
}
exit $exitCode
