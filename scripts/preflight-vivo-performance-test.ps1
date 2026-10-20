# Mandatory clean-room gate for reproducible Vivo performance measurements.
[CmdletBinding()]
param(
    [string]$DeviceId = '10AG263F1B001HM',

    [string]$AdbTarget,

    # Use this only when LSX4 is intentionally already running (live capture)
    # or before APK installation. It still removes and rejects profilers, but
    # does not mistake the running game/install activity for background load.
    [switch]$CleanupOnly,

    [ValidateRange(10, 100)]
    [double]$MinimumCpuIdlePercent = 60.0,

    [ValidateRange(250, 32768)]
    [int]$MinimumAvailableMemoryMb = 1024,

    [ValidateRange(0, 6)]
    [int]$MaximumThermalStatus = 2,

    [ValidateRange(25, 60)]
    [double]$MaximumSurfaceTemperatureC = 48.0,

    [ValidateRange(50, 100)]
    [double]$MaximumProcessorTemperatureC = 80.0,

    [ValidateRange(1, 100)]
    [int]$MinimumBatteryLevelPercent = 20,

    [ValidateRange(1, 10000000)]
    [int]$MinimumPolicy0MaxKHz = 2361600,

    [ValidateRange(1, 10000000)]
    [int]$MinimumPolicy6MaxKHz = 2438400,

    [ValidateRange(1, 10)]
    [int]$CpuSampleSeconds = 2
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$dedicatedVivoDeviceId = '10AG263F1B001HM'
$dedicatedSm8650DeviceId = '65d16af6'
$wirelessAdbSerialPattern =
    '^adb-10AG263F1B001HM-[A-Za-z0-9_-]+(?: \([0-9]+\))?\._adb-tls-connect\._tcp(?: \([0-9]+\))?$'
$lsx4Package = 'app.lsx4.android'
$diagnosticProcessNames = @(
    'process-tracker',
    'simpleperf',
    'perfetto',
    'heapprofd',
    'screenrecord'
)
$maximumSignalBatchSize = 100

if ([string]::IsNullOrWhiteSpace($AdbTarget)) {
    $AdbTarget = if ([string]::IsNullOrWhiteSpace($env:ADB_SERIAL)) {
        $DeviceId
    } else {
        $env:ADB_SERIAL
    }
}

$isVivoTransport =
    $AdbTarget -ceq $dedicatedVivoDeviceId -or
    $AdbTarget -cmatch $wirelessAdbSerialPattern
$isSm8650Transport = $AdbTarget -ceq $dedicatedSm8650DeviceId
if ($isSm8650Transport) {
    $expectedDeviceId = $dedicatedSm8650DeviceId
    $deviceLabel = 'SM8650'
    $agentTarget = "device:sm8650-$dedicatedSm8650DeviceId"
    $secondaryCpuPolicy = 7
} elseif ($isVivoTransport) {
    $expectedDeviceId = $dedicatedVivoDeviceId
    $deviceLabel = 'Vivo'
    $agentTarget = 'device:vivo'
    $secondaryCpuPolicy = 6
} else {
    throw (
        "ADB target is not an approved performance transport: $AdbTarget. " +
        "Allowed direct targets are $dedicatedVivoDeviceId and $dedicatedSm8650DeviceId.")
}
if (
    $PSBoundParameters.ContainsKey('DeviceId') -and
    $DeviceId -cne $expectedDeviceId
) {
    throw (
        "Explicit DeviceId $DeviceId does not match approved target " +
        "$expectedDeviceId selected by AdbTarget $AdbTarget")
}

$adb = (Get-Command adb -ErrorAction Stop).Source
$python = (Get-Command python -ErrorAction Stop).Source
$workspaceRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$agentchat = Join-Path $workspaceRoot 'agentchat\agentchat.py'
if (-not (Test-Path -LiteralPath $agentchat -PathType Leaf)) {
    throw "AgentChat helper not found: $agentchat"
}
$agentRoom = 'увеличить fps'
$agentAdbTarget = "adb:$expectedDeviceId"

function Enter-DevicePreflightLease {
    & $python $agentchat event --agent codex --room $agentRoom `
        --action adb-performance-preflight --target $agentTarget `
        --summary "mandatory clean-room gate before $deviceLabel performance testing" |
        Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'AgentChat device event failed before Vivo performance preflight'
    }
    & $python $agentchat own --agent codex --room $agentRoom `
        --area $agentTarget | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'AgentChat device ownership failed before Vivo performance preflight'
    }
    & $python $agentchat own --agent codex --room $agentRoom `
        --area $agentAdbTarget | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'AgentChat physical ADB ownership failed before Vivo performance preflight'
    }
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $attempts = if ($AllowFailure) { 1 } else { 3 }
    $output = @()
    $status = 1
    for ($attempt = 1; $attempt -le $attempts; $attempt++) {
        $output = @(& $adb -s $AdbTarget @Arguments 2>$null)
        $status = $LASTEXITCODE
        if ($status -eq 0) {
            break
        }
        if ($attempt -lt $attempts) {
            Start-Sleep -Milliseconds 250
        }
    }
    if ($status -ne 0 -and -not $AllowFailure) {
        throw "adb failed after ${attempts} attempts with exit code ${status}: $($Arguments -join ' ')"
    }
    return @($output | ForEach-Object { "$_" })
}

function Get-ProcessInventory {
    $lines = @(Invoke-Adb -Arguments @('shell', 'ps', '-An', '-o', 'PID,UID,NAME,ARGS'))
    if ($lines.Count -eq 0 -or $lines[0] -notmatch '\bPID\b.*\bUID\b') {
        throw 'Vivo preflight received an invalid process inventory header'
    }

    $inventory = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $lines | Select-Object -Skip 1) {
        if ($line -notmatch '^\s*(\d+)\s+(\d+)\s+(\S+)\s*(.*)$') {
            continue
        }
        $inventory.Add([pscustomobject]@{
            ProcessId = [int]$matches[1]
            Uid = [int]$matches[2]
            Name = $matches[3]
            CommandLine = $matches[4].Trim()
        })
    }
    if ($inventory.Count -eq 0) {
        throw 'Vivo preflight received an empty process inventory'
    }
    return @($inventory)
}

function Get-Lsx4Uid {
    $packageOutput = (Invoke-Adb `
        -Arguments @('shell', 'cmd', 'package', 'list', 'packages', '-U', $lsx4Package) `
        -AllowFailure) -join "`n"
    if ($packageOutput -match '(?:^|\s)uid:(\d+)(?:\s|$)') {
        return [int]$matches[1]
    }
    return $null
}

function Test-IsLsx4Process {
    param([Parameter(Mandatory = $true)]$Process)

    return ($Process.Name -ceq $lsx4Package -or
        $Process.Name.StartsWith("${lsx4Package}:", [System.StringComparison]::Ordinal))
}

function Test-DiagnosticCommandLine {
    param([Parameter(Mandatory = $true)]$Process)

    if ([string]::IsNullOrWhiteSpace($Process.CommandLine)) {
        # Only the Android Studio tracker has been observed with an empty ARGS
        # column on this device. Exact NAME + shell UID remain mandatory.
        return ($Process.Name -ceq 'process-tracker' -and $Process.Uid -eq 2000)
    }
    $firstToken = ($Process.CommandLine -split '\s+', 2)[0] -replace '\\', '/'
    $baseName = ($firstToken -split '/')[-1]
    return ($baseName -ceq $Process.Name)
}

function Test-IsEligibleDiagnosticProcess {
    param(
        [Parameter(Mandatory = $true)]$Process,
        [AllowNull()]$Lsx4Uid
    )

    if ($Process.Name -cnotin $diagnosticProcessNames -or
        (Test-IsLsx4Process $Process) -or
        -not (Test-DiagnosticCommandLine $Process)) {
        return $false
    }
    if ($Process.Name -ceq 'process-tracker') {
        return ($Process.Uid -eq 2000)
    }
    return ($Process.Uid -eq 2000 -or
        ($null -ne $Lsx4Uid -and $Process.Uid -eq $Lsx4Uid))
}

function Get-DiagnosticInventory {
    param([Parameter(Mandatory = $true)][object[]]$Inventory)

    return @($Inventory | Where-Object { $_.Name -cin $diagnosticProcessNames })
}

function Format-ProcessIdentity {
    param([Parameter(Mandatory = $true)]$Process)

    $commandLine = if ([string]::IsNullOrWhiteSpace($Process.CommandLine)) {
        '<empty>'
    } else {
        $Process.CommandLine
    }
    return "$($Process.Name):pid=$($Process.ProcessId):uid=$($Process.Uid):cmd=$commandLine"
}

function Test-SameProcessIdentity {
    param(
        [Parameter(Mandatory = $true)]$Expected,
        [Parameter(Mandatory = $true)]$Actual
    )

    return ($Expected.ProcessId -eq $Actual.ProcessId -and
        $Expected.Uid -eq $Actual.Uid -and
        $Expected.Name -ceq $Actual.Name -and
        $Expected.CommandLine -ceq $Actual.CommandLine)
}

function Invoke-VerifiedSignalBatches {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Processes,
        [Parameter(Mandatory = $true)][ValidateSet('TERM', 'KILL')][string]$Signal,
        [AllowNull()]$Lsx4Uid
    )

    if ($Processes.Count -eq 0) {
        return
    }
    $signalArgument = if ($Signal -ceq 'TERM') { '-15' } else { '-9' }
    $signaledCount = 0

    for ($offset = 0; $offset -lt $Processes.Count; $offset += $maximumSignalBatchSize) {
        $lastIndex = [math]::Min(
            $offset + $maximumSignalBatchSize - 1,
            $Processes.Count - 1)
        $batch = @($Processes[$offset..$lastIndex])

        # One fresh bulk inventory per <=100 PID batch closes the PID-reuse
        # window without the former O(process-count) series of adb round trips.
        $fresh = @(Get-ProcessInventory)
        $freshByPid = @{}
        foreach ($current in $fresh) {
            $freshByPid[[int]$current.ProcessId] = $current
        }

        $verified = [System.Collections.Generic.List[object]]::new()
        foreach ($expected in $batch) {
            $processId = [int]$expected.ProcessId
            if (-not $freshByPid.ContainsKey($processId)) {
                continue
            }
            $current = $freshByPid[$processId]
            if (-not (Test-SameProcessIdentity -Expected $expected -Actual $current) -or
                -not (Test-IsEligibleDiagnosticProcess -Process $current -Lsx4Uid $Lsx4Uid)) {
                throw "Refusing to signal changed or untrusted pid $processId"
            }
            $verified.Add($current)
        }

        foreach ($uidGroup in @($verified | Group-Object Uid)) {
            $processIds = @($uidGroup.Group | ForEach-Object { "$($_.ProcessId)" })
            if ($processIds.Count -eq 0) {
                continue
            }
            $uid = [int]$uidGroup.Name
            if ($uid -eq 2000) {
                Invoke-Adb -Arguments (@('shell', 'kill', $signalArgument) + $processIds) `
                    -AllowFailure | Out-Null
            } elseif ($null -ne $Lsx4Uid -and $uid -eq $Lsx4Uid) {
                Invoke-Adb -Arguments (
                    @('shell', 'run-as', $lsx4Package, 'kill', $signalArgument) +
                    $processIds) -AllowFailure | Out-Null
            } else {
                throw "Refusing to signal diagnostic batch owned by uid $uid"
            }
            $signaledCount += $processIds.Count
        }
    }

    Write-Output "Vivo preflight: SIG$Signal sent to $signaledCount verified diagnostic process(es)."
}

function Get-CpuSnapshot {
    $cpuLine = @(
        Invoke-Adb -Arguments @('shell', 'cat', '/proc/stat') |
            Where-Object { $_ -match '^cpu\s+' }
    ) | Select-Object -First 1
    if (-not $cpuLine) {
        throw 'Vivo preflight could not read the aggregate /proc/stat CPU line'
    }

    $fields = $cpuLine.Trim() -split '\s+'
    if ($fields.Count -lt 9) {
        throw "Vivo preflight received an incomplete /proc/stat CPU line: $cpuLine"
    }
    $values = @()
    for ($index = 1; $index -le 8; $index++) {
        $parsed = 0L
        if (-not [long]::TryParse($fields[$index], [ref]$parsed)) {
            throw "Vivo preflight could not parse /proc/stat field: $($fields[$index])"
        }
        $values += $parsed
    }
    return [pscustomobject]@{
        Total = [long](($values | Measure-Object -Sum).Sum)
        Idle = [long]($values[3] + $values[4])
    }
}

function Get-AvailableMemoryMb {
    $memInfo = (Invoke-Adb -Arguments @('shell', 'cat', '/proc/meminfo')) -join "`n"
    if ($memInfo -notmatch '(?m)^MemAvailable:\s+(\d+)\s+kB\s*$') {
        throw 'Vivo preflight could not read MemAvailable from /proc/meminfo'
    }
    return [math]::Round(([double]$matches[1] / 1024.0), 1)
}

function Get-BatteryState {
    $battery = (Invoke-Adb -Arguments @('shell', 'dumpsys', 'battery')) -join "`n"
    if ($battery -notmatch '(?m)^\s*level:\s*(\d+)\s*$') {
        throw 'Vivo preflight could not read the battery level'
    }
    $levelPercent = [int]$matches[1]
    $powered = $false
    foreach ($label in @('AC', 'USB', 'Wireless', 'Dock')) {
        $escapedLabel = [regex]::Escape($label)
        $match = [regex]::Match(
            $battery, "(?im)^\s*$escapedLabel powered:\s*(true|false)\s*$")
        if (-not $match.Success) {
            throw "Vivo preflight could not read $label power state"
        }
        if ($match.Groups[1].Value -ieq 'true') {
            $powered = $true
        }
    }
    return [pscustomobject]@{
        LevelPercent = $levelPercent
        ExternalPower = $powered
    }
}

function Get-CpuPolicyState {
    param([Parameter(Mandatory = $true)][int]$Policy)

    $path = "/sys/devices/system/cpu/cpufreq/policy$Policy"
    $values = @{}
    foreach ($name in @('scaling_cur_freq', 'scaling_min_freq', 'scaling_max_freq')) {
        # Keep each path a literal adb argv item.  Windows adb re-serializes
        # `shell sh -c <script>` and can strip the remote command envelope,
        # making otherwise valid $p/$f expansion fail with a misleading 31.
        $output = @(Invoke-Adb -Arguments @('shell', 'cat', "$path/$name"))
        if ($output.Count -ne 1 -or $output[0].Trim() -notmatch '^\d+$') {
            throw "Vivo preflight could not read policy$Policy $name"
        }
        $values[$name] = [int]$output[0].Trim()
    }
    return [pscustomobject]@{
        Policy = $Policy
        CurrentKHz = $values['scaling_cur_freq']
        MinimumKHz = $values['scaling_min_freq']
        MaximumKHz = $values['scaling_max_freq']
    }
}

function Get-ThermalStatus {
    $thermalDump = (Invoke-Adb `
        -Arguments @('shell', 'dumpsys', 'thermalservice') -AllowFailure) -join "`n"
    if ($thermalDump -match '(?im)Thermal Status:\s*(\d+)') {
        return [int]$matches[1]
    }
    $statusOutput = ((Invoke-Adb `
        -Arguments @('shell', 'cmd', 'thermalservice', 'get-status') `
        -AllowFailure) -join "`n").Trim()
    if ($statusOutput -match '(\d+)') {
        return [int]$matches[1]
    }
    $statusNames = @{
        'none' = 0; 'light' = 1; 'moderate' = 2; 'severe' = 3
        'critical' = 4; 'emergency' = 5; 'shutdown' = 6
    }
    foreach ($name in $statusNames.Keys) {
        if ($statusOutput.ToLowerInvariant() -match "\b$name\b") {
            return [int]$statusNames[$name]
        }
    }
    return $null
}

function Get-SurfaceTemperatures {
    $surfaceSensorCommand = @'
for z in /sys/class/thermal/thermal_zone*; do n=$(cat "$z/type" 2>/dev/null) || continue; case "$n" in *skin*|*Skin*|*SKIN*|*shell*|*Shell*|*frame*|*Frame*|*battery*|*Battery*|*quiet*|*Quiet*) v=$(cat "$z/temp" 2>/dev/null) || continue; echo "$n|$v";; esac; done
'@
    $temperatures = [System.Collections.Generic.List[object]]::new()
    foreach ($line in @(Invoke-Adb `
        -Arguments @('shell', $surfaceSensorCommand) -AllowFailure)) {
        if ($line -notmatch '^([^|]+)\|(-?\d+)\s*$') {
            continue
        }
        $raw = [double]$matches[2]
        $celsius = if ([math]::Abs($raw) -ge 10000.0) {
            $raw / 1000.0
        } elseif ([math]::Abs($raw) -ge 200.0) {
            $raw / 10.0
        } else {
            $raw
        }
        if ($celsius -ge -20.0 -and $celsius -le 120.0) {
            $temperatures.Add([pscustomobject]@{
                Name = $matches[1]
                Celsius = [math]::Round($celsius, 1)
            })
        }
    }
    return @($temperatures)
}

function Get-ProcessorTemperatures {
    # Battery/skin can remain cool while Snapdragon CPU zones exceed 90 C.
    # Cross-check the framework's fresh HardwareProperties sample with actual
    # cluster/package sysfs sensors. dumpsys thermalservice also contains a
    # history and can put an older hot sample before the current one.
    $processorSensorCommand = @'
for z in /sys/class/thermal/thermal_zone*; do n=$(cat "$z/type" 2>/dev/null) || continue; case "$n" in *cpu*|*CPU*|*cpuss*|*CPUSS*) v=$(cat "$z/temp" 2>/dev/null) || continue; echo "$n|$v";; esac; done
'@
    $temperatures = [System.Collections.Generic.List[object]]::new()
    $hardwareProperties = (Invoke-Adb `
        -Arguments @('shell', 'dumpsys', 'hardware_properties') `
        -AllowFailure) -join "`n"
    $hardwareCpu = [regex]::Match(
        $hardwareProperties,
        '(?m)^CPU temperatures:\s*\[([^\]]+)\]\s*$')
    if ($hardwareCpu.Success) {
        $hardwareIndex = 0
        foreach ($value in @($hardwareCpu.Groups[1].Value -split ',')) {
            $celsius = 0.0
            $parsed = [double]::TryParse(
                $value.Trim(),
                [Globalization.NumberStyles]::Float,
                [Globalization.CultureInfo]::InvariantCulture,
                [ref]$celsius)
            if ($parsed -and $celsius -ge -20.0 -and $celsius -le 120.0) {
                $temperatures.Add([pscustomobject]@{
                    Name = "hardwarePropertiesCpu$hardwareIndex"
                    Celsius = [math]::Round($celsius, 1)
                })
            }
            ++$hardwareIndex
        }
    }
    foreach ($line in @(Invoke-Adb `
        -Arguments @('shell', $processorSensorCommand) -AllowFailure)) {
        if ($line -notmatch '^([^|]+)\|(-?\d+)\s*$') {
            continue
        }
        $sensorName = $matches[1]
        # Qualcomm exposes fixed thermal trip thresholds through zones whose
        # type still contains "cpu" (for example cpu-hw-trip-0=105000).
        # They are limits, not live package temperatures, and must never fail
        # the preflight temperature gate.
        if ($sensorName -match '(?i)(?:^|[-_])(?:hw[-_])?trip(?:[-_]|$)') {
            continue
        }
        $raw = [double]$matches[2]
        $celsius = if ([math]::Abs($raw) -ge 10000.0) {
            $raw / 1000.0
        } elseif ([math]::Abs($raw) -ge 200.0) {
            $raw / 10.0
        } else {
            $raw
        }
        if ($celsius -ge -20.0 -and $celsius -le 120.0) {
            $temperatures.Add([pscustomobject]@{
                Name = $sensorName
                Celsius = [math]::Round($celsius, 1)
            })
        }
    }
    return @($temperatures)
}

Enter-DevicePreflightLease

$reportedSerial = ((Invoke-Adb `
    -Arguments @('shell', 'getprop', 'ro.serialno')) -join "`n").Trim()
if ($reportedSerial -cne $expectedDeviceId) {
    throw (
        "$deviceLabel identity mismatch: expected ro.serialno=$expectedDeviceId, " +
        "actual=$reportedSerial")
}
Write-Output (
    "$deviceLabel preflight identity: adbTarget=$AdbTarget " +
    "ro.serialno=$reportedSerial.")

$lsx4Uid = Get-Lsx4Uid
$initialInventory = @(Get-ProcessInventory)
$initialDiagnostics = @(Get-DiagnosticInventory -Inventory $initialInventory)
$refusedInventory = @($initialDiagnostics | Where-Object {
    -not (Test-IsEligibleDiagnosticProcess -Process $_ -Lsx4Uid $lsx4Uid)
})
$eligibleInventory = @($initialDiagnostics | Where-Object {
    Test-IsEligibleDiagnosticProcess -Process $_ -Lsx4Uid $lsx4Uid
})

Invoke-VerifiedSignalBatches `
    -Processes $eligibleInventory -Signal TERM -Lsx4Uid $lsx4Uid
if ($eligibleInventory.Count -gt 0) {
    Start-Sleep -Milliseconds 500
}

$postTermInventory = @(Get-ProcessInventory)
$termSurvivors = @(Get-DiagnosticInventory -Inventory $postTermInventory |
    Where-Object {
        Test-IsEligibleDiagnosticProcess -Process $_ -Lsx4Uid $lsx4Uid
    })
Invoke-VerifiedSignalBatches `
    -Processes $termSurvivors -Signal KILL -Lsx4Uid $lsx4Uid
if ($termSurvivors.Count -gt 0) {
    Start-Sleep -Milliseconds 500
}

$finalInventory = @(Get-ProcessInventory)
$finalDiagnostics = @(Get-DiagnosticInventory -Inventory $finalInventory)
$violations = [System.Collections.Generic.List[string]]::new()
foreach ($process in $refusedInventory) {
    $violations.Add(
        'refused to kill untrusted diagnostic-like process ' +
        (Format-ProcessIdentity $process))
}
foreach ($process in $finalDiagnostics) {
    $violations.Add(
        'diagnostic process active at final gate ' +
        (Format-ProcessIdentity $process))
}

if ($CleanupOnly) {
    if ($violations.Count -gt 0) {
        throw (
            "Vivo performance cleanup FAILED. Do not start/continue a measurement:`n - " +
            ($violations -join "`n - "))
    }
    Write-Output (
        'Vivo performance preflight PASSED (cleanup-only): ' +
        'diagnostic processes=0; no LSX4 process was stopped.')
    return
}

$availableMemoryMb = Get-AvailableMemoryMb
$batteryState = Get-BatteryState
$cpuPolicy0 = Get-CpuPolicyState -Policy 0
$cpuPolicySecondary = Get-CpuPolicyState -Policy $secondaryCpuPolicy
$thermalStatus = Get-ThermalStatus
$surfaceTemperatures = @(Get-SurfaceTemperatures)
$processorTemperatures = @(Get-ProcessorTemperatures)
$firstCpu = Get-CpuSnapshot
Start-Sleep -Seconds $CpuSampleSeconds
$secondCpu = Get-CpuSnapshot
$cpuTotalDelta = $secondCpu.Total - $firstCpu.Total
$cpuIdleDelta = $secondCpu.Idle - $firstCpu.Idle
if ($cpuTotalDelta -le 0 -or $cpuIdleDelta -lt 0) {
    throw 'Vivo preflight received a non-monotonic /proc/stat sample'
}
$cpuIdlePercent = [math]::Round((100.0 * $cpuIdleDelta / $cpuTotalDelta), 1)

# A profiler can reconnect during health sampling. This inventory is the
# authoritative zero-process gate immediately before the test starts.
$lateDiagnostics = @(Get-DiagnosticInventory -Inventory @(Get-ProcessInventory))
foreach ($process in $lateDiagnostics) {
    $formatted = Format-ProcessIdentity $process
    if (-not ($violations | Where-Object { $_ -like "*$formatted*" })) {
        $violations.Add("diagnostic process active at final gate $formatted")
    }
}

$surfaceSummary = if ($surfaceTemperatures.Count -gt 0) {
    ($surfaceTemperatures |
        ForEach-Object { "$($_.Name)=$($_.Celsius)C" }) -join ', '
} else {
    'unavailable'
}
$thermalStatusSummary = if ($null -eq $thermalStatus) {
    'unavailable'
} else {
    $thermalStatus
}
$processorSummary = if ($processorTemperatures.Count -gt 0) {
    ($processorTemperatures |
        ForEach-Object { "$($_.Name)=$($_.Celsius)C" }) -join ', '
} else {
    'unavailable'
}
Write-Output (
    "$deviceLabel preflight sample: cpuIdle=$cpuIdlePercent% " +
    "memAvailable=$availableMemoryMb MiB thermalStatus=$thermalStatusSummary " +
    "surface=[$surfaceSummary] processor=[$processorSummary] " +
    "batteryLevel=$($batteryState.LevelPercent)% " +
    "externalPower=$($batteryState.ExternalPower.ToString().ToLowerInvariant()) " +
    "cpuPolicies=[policy0=$($cpuPolicy0.CurrentKHz)/$($cpuPolicy0.MinimumKHz)/" +
    "$($cpuPolicy0.MaximumKHz)KHz,policy$secondaryCpuPolicy=" +
    "$($cpuPolicySecondary.CurrentKHz)/$($cpuPolicySecondary.MinimumKHz)/" +
    "$($cpuPolicySecondary.MaximumKHz)KHz].")

if ($availableMemoryMb -lt $MinimumAvailableMemoryMb) {
    $violations.Add(
        "MemAvailable $availableMemoryMb MiB is below $MinimumAvailableMemoryMb MiB")
}
if ($batteryState.LevelPercent -lt $MinimumBatteryLevelPercent) {
    $violations.Add(
        "battery level $($batteryState.LevelPercent)% is below $MinimumBatteryLevelPercent%")
}
if ($cpuPolicy0.MaximumKHz -lt $MinimumPolicy0MaxKHz) {
    $violations.Add(
        "policy0 scaling_max_freq $($cpuPolicy0.MaximumKHz) KHz is below $MinimumPolicy0MaxKHz KHz")
}
if ($cpuPolicySecondary.MaximumKHz -lt $MinimumPolicy6MaxKHz) {
    $violations.Add(
        "policy$secondaryCpuPolicy scaling_max_freq " +
        "$($cpuPolicySecondary.MaximumKHz) KHz is below $MinimumPolicy6MaxKHz KHz")
}
if ($null -eq $thermalStatus -and $surfaceTemperatures.Count -eq 0 -and
    $processorTemperatures.Count -eq 0) {
    $violations.Add('thermal telemetry is unavailable')
}
if ($null -ne $thermalStatus -and $thermalStatus -gt $MaximumThermalStatus) {
    $violations.Add(
        "thermal status $thermalStatus exceeds allowed $MaximumThermalStatus")
}
foreach ($temperature in $surfaceTemperatures) {
    if ($temperature.Celsius -gt $MaximumSurfaceTemperatureC) {
        $violations.Add(
            "$($temperature.Name) temperature $($temperature.Celsius)C exceeds " +
            "$MaximumSurfaceTemperatureC C")
    }
}
foreach ($temperature in $processorTemperatures) {
    if ($temperature.Celsius -gt $MaximumProcessorTemperatureC) {
        $violations.Add(
            "$($temperature.Name) processor temperature $($temperature.Celsius)C exceeds " +
            "$MaximumProcessorTemperatureC C")
    }
}
if ($cpuIdlePercent -lt $MinimumCpuIdlePercent) {
    $violations.Add(
        "aggregate CPU idle $cpuIdlePercent% is below $MinimumCpuIdlePercent%")
}

if ($violations.Count -gt 0) {
    if ($cpuIdlePercent -lt $MinimumCpuIdlePercent) {
        $cpuReport = @(Invoke-Adb `
            -Arguments @('shell', 'dumpsys', 'cpuinfo') -AllowFailure |
            Select-Object -First 20)
        if ($cpuReport.Count -gt 0) {
            Write-Warning ("Vivo preflight CPU report:`n" + ($cpuReport -join "`n"))
        }
    }
    throw (
        "Vivo performance preflight FAILED. Do not start an FPS measurement:`n - " +
        ($violations -join "`n - "))
}

Write-Output (
    'Vivo performance preflight PASSED: diagnostic processes=0; ' +
    'background CPU, memory, and thermal gates are valid.')
