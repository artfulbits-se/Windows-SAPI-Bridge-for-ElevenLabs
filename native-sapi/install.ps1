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
    $voices = @(
        @{ Id = 'en_US-lessac-medium'; Name = 'Lessac Medium (US English)'; Language = '409'; Gender = 'Male' }
        @{ Id = 'en_US-amy-medium'; Name = 'Amy Medium (US English)'; Language = '409'; Gender = 'Female' }
        @{ Id = 'en_GB-alan-medium'; Name = 'Alan Medium (UK English)'; Language = '809'; Gender = 'Male' }
        @{ Id = 'en_GB-alba-medium'; Name = 'Alba Medium (UK English)'; Language = '809'; Gender = 'Female' }
    )
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
    $missingModels = @(foreach ($voice in $voices) {
        $model = Join-Path $piperDirectory ($voice.Id + '.onnx')
        if (-not (Test-Path -LiteralPath $model -PathType Leaf) -or
            -not (Test-Path -LiteralPath "$model.json" -PathType Leaf)) {
            $voice.Id
        }
    })
    if ($missingModels.Count) {
        & $piperPython -m piper.download_voices --data-dir $piperDirectory @missingModels
        if ($LASTEXITCODE -ne 0) { throw 'Could not download the English Piper models.' }
    }
    foreach ($voice in $voices) {
        $metadata = Join-Path $piperDirectory ($voice.Id + '.onnx.json')
        $rate = & $piperPython -I -c "import json, sys; print(json.load(open(sys.argv[1], encoding='utf-8'))['audio']['sample_rate'])" $metadata
        if ($LASTEXITCODE -ne 0 -or $rate -ne '22050') {
            throw "The Piper model $($voice.Id) must produce 22.05 kHz audio."
        }
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
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64')
    try {
        foreach ($voice in $voices) {
            $display = 'Piper - ' + $voice.Name
            $tokenPath = 'SOFTWARE\Microsoft\Speech\Voices\Tokens\Piper_' + $voice.Id.Replace('-', '_')
            $token = $machine.CreateSubKey($tokenPath)
            try {
                $registered = $token.GetValue('CLSID')
                if ($registered -and $registered -ne $clsid) { throw "Another engine owns the token for $display." }
                $token.SetValue('', $display)
                $token.SetValue('PiperVoiceId', $voice.Id)
                $attributes = $token.CreateSubKey('Attributes')
                try {
                    $attributes.SetValue('Name', $display)
                    $attributes.SetValue('Vendor', 'Piper')
                    $attributes.SetValue('Language', $voice.Language)
                    $attributes.SetValue('Gender', $voice.Gender)
                    $attributes.SetValue('Age', 'Adult')
                } finally { $attributes.Dispose() }
                $token.SetValue('CLSID', $clsid)
            } finally { $token.Dispose() }
            Write-Output "Registered $display."
        }
    } finally { $machine.Dispose() }
}
Write-Output "Registered native SAPI engine at $dll."
