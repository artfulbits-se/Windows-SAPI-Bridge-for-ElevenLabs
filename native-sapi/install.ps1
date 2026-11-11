[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$EngineDll,
    [switch]$Piper,
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) {
    throw 'Run the installer from 64-bit PowerShell.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($Piper -and -not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run PowerShell as Administrator to register the Piper SAPI voice.'
}

$sourceDll = (Get-Item -LiteralPath $EngineDll).FullName
$install = Join-Path $env:LOCALAPPDATA 'ElevenLabsSAPI'
$native = Join-Path $install 'native'
$null = New-Item -ItemType Directory -Path $native -Force

if ($Piper) {
    $piperDirectory = Join-Path $install 'piper'
    $null = New-Item -ItemType Directory -Path $piperDirectory -Force
    $venv = Join-Path $piperDirectory '.venv'
    $piperPython = Join-Path $venv 'Scripts\python.exe'
    if (-not (Test-Path -LiteralPath $piperPython -PathType Leaf)) {
        & $Python -m venv $venv
        if ($LASTEXITCODE -ne 0) { throw 'Could not create the Piper Python environment.' }
    }
    & $piperPython -m pip install --disable-pip-version-check 'piper-tts==1.8.0'
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the Piper runtime.' }
    $model = 'en_US-lessac-medium'
    if (-not (Test-Path -LiteralPath (Join-Path $piperDirectory "$model.onnx") -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $piperDirectory "$model.onnx.json") -PathType Leaf)) {
        & $piperPython -m piper.download_voices --data-dir $piperDirectory $model
        if ($LASTEXITCODE -ne 0) { throw 'Could not download the English Piper model.' }
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'piper_worker.py') -Destination $piperDirectory -Force
}

$config = Join-Path $install 'config.json'
if (-not (Test-Path -LiteralPath $config)) {
    Copy-Item -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'config.example.json') -Destination $config
    $acl = Get-Acl -LiteralPath $config
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sid in @($identity.User, [Security.Principal.SecurityIdentifier]::new('S-1-5-18'),
            [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'Allow'))
    }
    Set-Acl -LiteralPath $config -AclObject $acl
}

$hash = (Get-FileHash -LiteralPath $sourceDll).Hash
$dll = Join-Path $native ('ElevenLabsSapi-' + $hash.Substring(0, 12).ToLowerInvariant() + '.dll')
if (-not (Test-Path -LiteralPath $dll -PathType Leaf) -or
    (Get-FileHash -LiteralPath $dll).Hash -ne $hash) {
    Copy-Item -LiteralPath $sourceDll -Destination $dll -Force
}

$clsid = '{6C0A3A4E-8F2B-4E5D-A3C7-1B9F2E8D4A6C}'
$classPath = "SOFTWARE\Classes\CLSID\$clsid"
$user = [Microsoft.Win32.RegistryKey]::OpenBaseKey('CurrentUser', 'Registry64')
$engine = $user.CreateSubKey($classPath)
try {
    $engine.SetValue('', 'ElevenLabs and Piper SAPI Engine')
    $server = $engine.CreateSubKey('InprocServer32')
    try {
        $server.SetValue('ThreadingModel', 'Both')
        $server.SetValue('', $dll)
    } finally { $server.Dispose() }
} finally { $engine.Dispose(); $user.Dispose() }

if ($Piper) {
    $display = 'Piper - Lessac Medium (US English)'
    $tokenPath = 'SOFTWARE\Microsoft\Speech\Voices\Tokens\Piper_en_US_lessac_medium'
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    $token = $machine.CreateSubKey($tokenPath)
    try {
        $registered = $token.GetValue('CLSID')
        if ($registered -and $registered -ne $clsid) { throw 'Another engine owns the Lessac token.' }
        $token.SetValue('', $display)
        $token.SetValue('PiperVoiceId', $model)
        $attributes = $token.CreateSubKey('Attributes')
        try {
            $attributes.SetValue('Name', $display)
            $attributes.SetValue('Vendor', 'Piper')
            $attributes.SetValue('Language', '409')
            $attributes.SetValue('Gender', 'Female')
            $attributes.SetValue('Age', 'Adult')
        } finally { $attributes.Dispose() }
        $token.SetValue('CLSID', $clsid)
    } finally { $token.Dispose(); $machine.Dispose() }
    Write-Output "Registered $display."
}
Write-Output "Registered native SAPI engine at $dll."
