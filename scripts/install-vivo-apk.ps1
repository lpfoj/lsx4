[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$ApkPath,

    [Parameter(Mandatory = $true)]
    [string]$DeviceId,

    [ValidateRange(15, 600)]
    [int]$TimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'

$expectedDeviceId = '10AG263F1B001HM'
$expectedPackage = 'app.lsx4.android'
$installerPackage = 'com.android.packageinstaller'
if ($DeviceId -cne $expectedDeviceId) {
    throw "This installer helper is restricted to the dedicated Vivo $expectedDeviceId"
}

$wirelessAdbSerialPattern =
    '^adb-10AG263F1B001HM-[A-Za-z0-9]+(?: \([0-9]+\))?\._adb-tls-connect\._tcp$'
$adbTarget = if ([string]::IsNullOrWhiteSpace($env:ADB_SERIAL)) {
    $DeviceId
} else {
    $env:ADB_SERIAL
}
if ($adbTarget -cne $expectedDeviceId -and
    $adbTarget -cnotmatch $wirelessAdbSerialPattern) {
    throw "ADB_SERIAL is not an approved transport for the dedicated Vivo $expectedDeviceId"
}

$adb = (Get-Command adb -ErrorAction Stop).Source
$python = (Get-Command python -ErrorAction Stop).Source
$workspaceRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$agentchat = Join-Path $workspaceRoot 'agentchat\agentchat.py'
if (-not (Test-Path -LiteralPath $agentchat -PathType Leaf)) {
    throw "AgentChat helper not found: $agentchat"
}
$agentRoom = 'увеличить fps'
$agentTarget = 'device:vivo'
$agentAdbTarget = "adb:$expectedDeviceId"
$deviceActionSequence = 0
$resolvedApk = (Resolve-Path -LiteralPath $ApkPath -ErrorAction Stop).Path
$tempBase = Join-Path ([IO.Path]::GetTempPath()) ("lsx4-adb-install-" + [guid]::NewGuid())
$stdoutPath = "$tempBase.out"
$stderrPath = "$tempBase.err"

function Assert-AgentChatDeviceLease {
    param([Parameter(Mandatory = $true)][string]$Action)

    $script:deviceActionSequence++
    & $python $agentchat event --agent codex --room $agentRoom `
        --action $Action --target $agentTarget `
        --summary "install-vivo-apk device action $script:deviceActionSequence" |
        Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "AgentChat device event failed before $Action"
    }
    & $python $agentchat own --agent codex --room $agentRoom `
        --area $agentTarget | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "AgentChat device ownership failed before $Action"
    }
    & $python $agentchat own --agent codex --room $agentRoom `
        --area $agentAdbTarget | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "AgentChat physical ADB ownership failed before $Action"
    }
}

function Invoke-AdbDeviceAction {
    param(
        [Parameter(Mandatory = $true)][string]$Action,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    Assert-AgentChatDeviceLease -Action $Action
    $output = @(& $adb -s $adbTarget @Arguments 2>$null)
    $status = $LASTEXITCODE
    if ($status -ne 0 -and -not $AllowFailure) {
        throw "adb action $Action failed with exit code $status"
    }
    return $output
}

$reportedSerial = ((Invoke-AdbDeviceAction `
    -Action 'adb-verify-ro-serialno' `
    -Arguments @('shell', 'getprop', 'ro.serialno')) -join "`n").Trim()
if ($reportedSerial -cne $expectedDeviceId) {
    throw "Dedicated Vivo identity mismatch: expected ro.serialno=$expectedDeviceId, actual=$reportedSerial"
}
Write-Output "Vivo device identity verified: adbTarget=$adbTarget ro.serialno=$reportedSerial"

$performancePreflight = Join-Path $PSScriptRoot 'preflight-vivo-performance-test.ps1'
if (-not (Test-Path -LiteralPath $performancePreflight -PathType Leaf)) {
    throw "Mandatory Vivo performance preflight not found: $performancePreflight"
}
Write-Output 'Running mandatory Vivo diagnostic cleanup before installation.'
& $performancePreflight -DeviceId $DeviceId -AdbTarget $adbTarget -CleanupOnly

function Get-NodeByResourceId {
    param(
        [string]$Xml,
        [string]$ResourceId
    )

    foreach ($match in [regex]::Matches($Xml, '<node\b[^>]*>')) {
        if ($match.Value -match ('resource-id="' + [regex]::Escape($ResourceId) + '"')) {
            return $match.Value
        }
    }
    return $null
}

function Get-UiNodes {
    param([string]$Xml)

    return @(
        foreach ($match in [regex]::Matches($Xml, '<node\b[^>]*>')) {
            $match.Value
        }
    )
}

function Get-NodeSemanticText {
    param([string]$Node)

    $parts = @()
    foreach ($attributeName in @('text', 'content-desc', 'resource-id')) {
        $attribute = [regex]::Match(
            $Node,
            '(?:^|\s)' + [regex]::Escape($attributeName) + '="([^"]*)"')
        if (-not $attribute.Success -or
            [string]::IsNullOrWhiteSpace($attribute.Groups[1].Value)) {
            continue
        }

        $value = [System.Net.WebUtility]::HtmlDecode($attribute.Groups[1].Value)
        if ($attributeName -eq 'resource-id' -and $value -match ':id/(.+)$') {
            $value = $matches[1]
        }
        $parts += ($value -replace '[_-]+', ' ')
    }
    return ($parts -join ' ')
}

function Get-InstallerConsentNode {
    param([string]$Xml)

    $knownIds = @(
        'com.android.packageinstaller:id/checkbox_frame',
        'com.android.packageinstaller:id/deleted_file_state_cb',
        'com.android.packageinstaller:id/install_risk_checkbox',
        'com.android.packageinstaller:id/risk_checkbox',
        'com.android.packageinstaller:id/check_box',
        'com.android.packageinstaller:id/checkbox',
        'com.vivo.packageinstaller:id/deleted_file_state_cb',
        'com.vivo.packageinstaller:id/install_risk_checkbox',
        'com.vivo.packageinstaller:id/checkbox_frame'
    )
    foreach ($resourceId in $knownIds) {
        $node = Get-NodeByResourceId $Xml $resourceId
        if ($node -and $node -match 'enabled="true"' -and
            ($node -match 'checked="false"' -or
             $node -notmatch 'checkable="true"')) {
            return $node
        }
    }

    $unchecked = @(
        Get-UiNodes $Xml | Where-Object {
            $_ -match 'checkable="true"' -and
            $_ -match 'checked="false"' -and
            $_ -match 'enabled="true"' -and
            $_ -match 'class="[^\"]*(?:CheckBox|Switch|CheckedTextView)"'
        }
    )
    $acknowledgementPattern = '(?i)(understand|agree|accept|risk|threat|security|unknown|install|\u043f\u043e\u043d\u0438\u043c\u0430\u044e|\u0441\u043e\u0433\u043b\u0430\u0441|\u043f\u0440\u0438\u043d\u044f\u0442\u044c|\u0440\u0438\u0441\u043a|\u0443\u0433\u0440\u043e\u0437|\u0431\u0435\u0437\u043e\u043f\u0430\u0441|\u043d\u0435\u0438\u0437\u0432\u0435\u0441\u0442|\u0443\u0441\u0442\u0430\u043d\u043e\u0432|\u7ee7\u7eed|\u5b89\u88c5|\u98ce\u9669|\u540c\u610f|\ud655\uc778|\ub3d9\uc758|\uc124\uce58|\uc704\ud5d8)'
    foreach ($node in $unchecked) {
        if ((Get-NodeSemanticText $node) -match $acknowledgementPattern) {
            return $node
        }
    }
    if ($unchecked.Count -eq 1) {
        return $unchecked[0]
    }
    return $null
}

function Get-InstallerPositiveButtonNode {
    param([string]$Xml)

    $knownIds = @(
        'android:id/button1',
        'android:id/button3',
        'com.android.packageinstaller:id/btn_continue',
        'com.android.packageinstaller:id/install_confirm_button',
        'com.android.packageinstaller:id/install_button',
        'com.android.packageinstaller:id/btn_install',
        'com.android.packageinstaller:id/button_install',
        'com.android.packageinstaller:id/continue_button',
        'com.vivo.packageinstaller:id/btn_continue',
        'com.vivo.packageinstaller:id/install_button'
    )
    $positivePattern = '(?i)(continue installation|continue|install|update|reinstall|proceed|\u043f\u0440\u043e\u0434\u043e\u043b\u0436\u0438\u0442\u044c \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u043a\u0443|\u043f\u0440\u043e\u0434\u043e\u043b\u0436\u0438\u0442\u044c|\u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c|\u043e\u0431\u043d\u043e\u0432\u0438\u0442\u044c|\u043f\u0435\u0440\u0435\u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c|\u7ee7\u7eed\u5b89\u88c5|\u5b89\u88c5|\u7ee7\u7eed|\uc5c5\ub370\uc774\ud2b8|\uacc4\uc18d|\uc124\uce58)'
    $negativePattern = '(?i)(open|launch|cancel|not now|do not install|don''t install|uninstall|\u043e\u0442\u043a\u0440\u044b\u0442\u044c|\u043e\u0442\u043c\u0435\u043d|\u043d\u0435 \u0443\u0441\u0442\u0430\u043d\u0430\u0432\u043b|\u0443\u0434\u0430\u043b\u0438\u0442\u044c|\u53d6\u6d88|\u6682\u4e0d\u5b89\u88c5|\ucde8\uc18c|\uc124\uce58 \uc548 \ud568)'
    foreach ($resourceId in $knownIds) {
        $node = Get-NodeByResourceId $Xml $resourceId
        $semanticText = if ($node) { Get-NodeSemanticText $node } else { '' }
        if ($node -and $node -match 'enabled="true"' -and
            $semanticText -match $positivePattern -and
            $semanticText -notmatch $negativePattern) {
            return $node
        }
    }

    foreach ($node in Get-UiNodes $Xml) {
        $semanticText = Get-NodeSemanticText $node
        if ($node -match 'class="[^\"]*Button"' -and
            $node -match 'clickable="true"' -and
            $node -match 'enabled="true"' -and
            $semanticText -match $positivePattern -and
            $semanticText -notmatch $negativePattern) {
            return $node
        }
    }
    return $null
}

function Test-IsPackageInstallerSurface {
    param([string]$Xml)

    if ($Xml -match 'resource-id="(?:com\.(?:android|google|vivo|bbk)\.)?[^\"]*packageinstaller[^\"]*:id/' -or
        $Xml -match 'package="[^\"]*(?:packageinstaller|permissioncontroller)[^\"]*"') {
        return $true
    }
    return $Xml -match 'resource-id="[^\"]*(?:deleted_file_state_cb|install_risk_checkbox|install_confirm_button|btn_continue)"'
}

function Get-CurrentStandardOutput {
    if (Test-Path -LiteralPath $stdoutPath) {
        return (Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue)
    }
    return ''
}

function Invoke-TapNode {
    param([string]$Node)

    if (-not $Node -or $Node -notmatch 'bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"') {
        return $false
    }
    $x = [int](([int]$matches[1] + [int]$matches[3]) / 2)
    $y = [int](([int]$matches[2] + [int]$matches[4]) / 2)
    Invoke-AdbDeviceAction -Action 'adb-installer-tap' `
        -Arguments @('shell', 'input', 'tap', "$x", "$y") | Out-Null
    return $true
}

$install = $null
try {
    Assert-AgentChatDeviceLease -Action 'adb-install-apk'
    $install = Start-Process -FilePath $adb `
        -ArgumentList @('-s', "`"$adbTarget`"", 'install', '-r', "`"$resolvedApk`"") `
        -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $adbReportedSuccess = $false
    $checkboxTapped = $false
    $positiveButtonTapped = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $install.Refresh()
        $stdoutSnapshot = Get-CurrentStandardOutput
        if ($stdoutSnapshot -match '(?m)^\s*Success\s*$') {
            $adbReportedSuccess = $true
            break
        }
        if ($install.HasExited) {
            break
        }

        Invoke-AdbDeviceAction -Action 'adb-installer-ui-dump' `
            -Arguments @('shell', 'uiautomator', 'dump', '/sdcard/lsx4-install.xml') `
            -AllowFailure | Out-Null
        $xml = ((Invoke-AdbDeviceAction -Action 'adb-installer-ui-read' `
            -Arguments @('shell', 'cat', '/sdcard/lsx4-install.xml') `
            -AllowFailure) -join '')
        if (Test-IsPackageInstallerSurface $xml) {
            $checkbox = Get-InstallerConsentNode $xml
            if ($checkbox) {
                if (Invoke-TapNode $checkbox) {
                    $checkboxTapped = $true
                    Start-Sleep -Milliseconds 350
                    continue
                }
            }

            $positiveButton = Get-InstallerPositiveButtonNode $xml
            if ($positiveButton) {
                $positiveButtonTapped = Invoke-TapNode $positiveButton
                if ($positiveButtonTapped) {
                    Start-Sleep -Milliseconds 350
                    continue
                }
            }
        }
        Start-Sleep -Milliseconds 500
    }

    $install.Refresh()
    if (-not $adbReportedSuccess -and -not $install.HasExited) {
        throw "APK installation timed out after $TimeoutSeconds seconds"
    }
    if ($adbReportedSuccess) {
        if (-not $install.HasExited -and -not $install.WaitForExit(2000)) {
            Stop-Process -Id $install.Id -Force -ErrorAction SilentlyContinue
            $install.WaitForExit()
        }
    } else {
        $install.WaitForExit()
    }
    $stdout = if (Test-Path -LiteralPath $stdoutPath) {
        Get-Content -LiteralPath $stdoutPath -Raw
    } else { '' }
    $stderr = if (Test-Path -LiteralPath $stderrPath) {
        Get-Content -LiteralPath $stderrPath -Raw
    } else { '' }
    if ($stdout -match '(?m)^\s*Failure\b' -or
        (-not $adbReportedSuccess -and $install.ExitCode -ne 0)) {
        throw "adb install failed ($($install.ExitCode))`n$stdout`n$stderr"
    }
    if ($stdout -notmatch '(?m)^\s*Success\s*$') {
        throw "adb install ended without reporting Success`n$stdout`n$stderr"
    }
    Write-Output $stdout.Trim()
    # A repeated install may finish without presenting the OEM surface after
    # the user-consent state has already been accepted.  In either case this
    # helper, rather than a user, owns the complete confirmation phase.
    $confirmationActions = @()
    if ($checkboxTapped) { $confirmationActions += 'checkbox' }
    if ($positiveButtonTapped) { $confirmationActions += 'continue/install' }
    if ($confirmationActions.Count -gt 0) {
        Write-Output ("Vivo package-installer actions: " +
            ($confirmationActions -join ', ') + '.')
    } else {
        Write-Output 'Vivo package-installer actions: OEM prompt was not required.'
    }
    Write-Output 'Vivo package-installer confirmation completed automatically.'

    # Vivo's USB-install consent path rejects an adb session whose installer
    # identity is overridden up front. Attribute the successful package update
    # afterwards through PackageManager's supported set-installer command.
    Invoke-AdbDeviceAction -Action 'adb-set-installer-attribution' `
        -Arguments @('shell', 'pm', 'set-installer', $expectedPackage,
            $installerPackage) | Out-Null
    $packageState = ((Invoke-AdbDeviceAction `
        -Action 'adb-verify-installer-attribution' `
        -Arguments @('shell', 'dumpsys', 'package', $expectedPackage)) -join "`n")
    if ($packageState -notmatch
        ('installerPackageName=' + [regex]::Escape($installerPackage))) {
        throw "Vivo installer attribution verification failed for $expectedPackage"
    }
    Write-Output "Vivo installer attribution verified: $installerPackage"
} finally {
    if ($install) {
        $install.Refresh()
        if (-not $install.HasExited) {
            Stop-Process -Id $install.Id -Force -ErrorAction SilentlyContinue
        }
    }
    Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
}
