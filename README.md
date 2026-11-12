# Windows SAPI Bridge for ElevenLabs and Piper

This fork exposes ElevenLabs cloud voices and offline Piper voices through 64-bit Windows SAPI5. An application such as ALLPlayer can select any of these voices from its existing speech synthesizer settings.

| Voice | Synthesis |
| --- | --- |
| `ElevenLabs - Adam - Dominant, Firm` and other ElevenLabs voices | ElevenLabs API. `eleven_v4_turbo` uses the Text to Dialogue WebSocket. `eleven_v4` and other supported HTTP models use the Text to Speech streaming endpoint. |
| `Piper - Lessac Medium (US English)`, `Piper - Amy Medium (US English)` | Local US English voices. |
| `Piper - Alan Medium (UK English)`, `Piper - Alba Medium (UK English)` | Local UK English voices. |

The native COM DLL runs inside the 64-bit SAPI application. Piper runs in a child process that loads the selected model once and streams audio to the DLL over private pipes. The worker exits when the SAPI voice is released. All Piper voices work offline after setup and use zero ElevenLabs credits. ALLPlayer's reading speed and volume controls apply to Piper.

## Build and install

Use Windows 10 or 11 on x64, 64-bit PowerShell, Git, and 64-bit Python 3.9 or later on `PATH`. You need internet access during setup to download the compiler, headers, and any Piper models you install. Run ALLPlayer or another 64-bit SAPI5 application under the Windows account that installs the bridge.

### 1. Clone the fork

Open PowerShell in the directory where you want the source checkout.

```powershell
git clone https://github.com/artfulbits-se/Windows-SAPI-Bridge-for-ElevenLabs.git
Set-Location Windows-SAPI-Bridge-for-ElevenLabs
```

### 2. Build the native engine

Run this block from the repository root. It downloads the tested versions of [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104), the [SAPI DDK header](https://github.com/microsoft/win32metadata/blob/9cf94833626504b330c2f616ce71a2d3497cdc76/generation/WinSDK/RecompiledIdlHeaders/um/sapiddk.h), and [nlohmann JSON](https://github.com/nlohmann/json/releases/tag/v3.12.0). The compiler download is about 182 MiB and is reused on subsequent builds.

```powershell
$ErrorActionPreference = 'Stop'
$build = Join-Path $env:TEMP 'ElevenLabsSapiBuild'
$includes = Join-Path $build 'include'
New-Item -ItemType Directory -Path $includes -Force | Out-Null

$toolchain = 'llvm-mingw-20261104-ucrt-x86_64'
$compiler = Join-Path $build "$toolchain\bin\clang++.exe"
if (-not (Test-Path -LiteralPath $compiler)) {
    $archive = Join-Path $build "$toolchain.zip"
    Invoke-WebRequest -UseBasicParsing `
        -Uri "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/$toolchain.zip" `
        -OutFile $archive
    if ((Get-FileHash -LiteralPath $archive).Hash -ne '552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f') {
        throw 'The LLVM-MinGW archive checksum did not match.'
    }
    tar.exe -xf $archive -C $build
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract LLVM-MinGW.' }
}

Invoke-WebRequest -UseBasicParsing `
    -Uri 'https://raw.githubusercontent.com/microsoft/win32metadata/9cf94833626504b330c2f616ce71a2d3497cdc76/generation/WinSDK/RecompiledIdlHeaders/um/sapiddk.h' `
    -OutFile (Join-Path $includes 'sapiddk.h')
Invoke-WebRequest -UseBasicParsing `
    -Uri 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' `
    -OutFile (Join-Path $includes 'json.hpp')

$dll = Join-Path $build 'ElevenLabsSapi.dll'
& $compiler -std=c++17 -O2 -shared -static -I $includes -o $dll `
  native-sapi/ElevenLabsSapi.cpp native-sapi/ElevenLabsSapi.def `
  -lole32 -luuid -lwinhttp -lcrypt32
if ($LASTEXITCODE -ne 0) { throw 'The native engine build failed.' }
Get-Item -LiteralPath $dll
```

### 3. Install the engine and Piper voices

Open **64-bit PowerShell as Administrator** under the same Windows account. Change to the repository root again and run the installer.

```powershell
$dll = Join-Path $env:TEMP 'ElevenLabsSapiBuild\ElevenLabsSapi.dll'
.\native-sapi\install.ps1 -EngineDll $dll -Piper
```

This registers the native DLL for your account and installs the four English Piper voices. The installer downloads Piper and the models into `%LOCALAPPDATA%\ElevenLabsSAPI\piper`. If Python is installed outside `PATH`, pass its executable path with `-Python`.

For **ElevenLabs only**, run the same command without `-Piper` and continue to step 4. For **Piper only**, skip to step 5. To use both, keep `-Piper` and continue to step 4.

### 4. Add ElevenLabs voices (optional)

The native installer creates `%LOCALAPPDATA%\ElevenLabsSAPI\config.json` on first setup. Open it and set `api_key` to a key with permission to list voices and synthesize speech. Set `model_id` to `eleven_v4_turbo` for v4 Turbo or `eleven_v4` for v4. The key belongs in this local file.

```powershell
notepad.exe (Join-Path $env:LOCALAPPDATA 'ElevenLabsSAPI\config.json')
```

Set these two fields in the existing JSON file.

```json
{
  "api_key": "YOUR_API_KEY_HERE",
  "model_id": "eleven_v4_turbo"
}
```

After saving the file, run the voice registration commands from the repository root in the same administrator PowerShell window.

```powershell
python -m venv .venv
& .\.venv\Scripts\python.exe -m pip install -r requirements.txt
& .\.venv\Scripts\python.exe install_voices.py --native
```

Run `install_voices.py --native` again when your ElevenLabs voice list changes. The bridge rereads `model_id` for each cloud utterance, so changing the model takes effect on the next subtitle.

### 5. Verify the voices

Fully quit and reopen ALLPlayer, including any tray process. In **Settings → Speech Synthesizer**, enable **Read subtitles, using voice**, select a **Piper** or **ElevenLabs** voice, and click **Test the reader**. The first Piper request loads its model and may take several seconds.

You can list the registered voices from a normal 64-bit PowerShell window under the same account.

```powershell
$speech = New-Object -ComObject SAPI.SpVoice
foreach ($vendor in 'Piper', 'ElevenLabs') {
    $voices = $speech.GetVoices("Vendor=$vendor", '')
    for ($index = 0; $index -lt $voices.Count; $index++) {
        $voices.Item($index).GetDescription()
    }
}
```

### Update an existing installation

Pull the latest changes from the fork, repeat the build in step 2, and rerun the installer in step 3 with the same voice options. Restart ALLPlayer after installation.

## Verification and troubleshooting

- The native engine has generated Adam audio through Windows SAPI using both Eleven v4 and v4 Turbo. All four listed Piper voices have also produced non-silent 22.05 kHz audio through Windows SAPI. The first request for a Piper voice loads its model and can take several seconds.
- If a new voice is missing, restart the speech application after registration and confirm it is a 64-bit SAPI5 client.
- For Piper startup errors, check for `piper_worker.py`, `.venv\Scripts\python.exe`, and the selected voice's `.onnx` and `.onnx.json` files in `%LOCALAPPDATA%\ElevenLabsSAPI\piper`. Rerun the native installer with `-Piper` to repair that setup.
- For cloud errors, check the model ID, API permissions, and available credits. Cloud speech sends the utterance to ElevenLabs. The Piper worker reads only the local model and the text passed over its process pipes.
- The original Python COM bridge and its cleanup tools remain available. See [SAPI_STATUS.md](SAPI_STATUS.md) for the original marshaling issue and [CLEANUP_GUIDE.md](CLEANUP_GUIDE.md) for legacy registrations.

## Credits and licensing

The bridge is [MIT licensed](LICENSE). The original Windows SAPI bridge was created by [xTaraRose](https://github.com/xTaraRose/Windows-SAPI-Bridge-for-ElevenLabs). The local implementation takes design cues from [Piper SAPI5](https://github.com/mflow888/piper-chromium-sapi5) and runs [OHF Piper](https://github.com/OHF-Voice/piper1-gpl) as a separate GPL-3.0 runtime. Piper and the voices are downloaded at setup time. Dataset licenses are listed in the [Lessac](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_US/lessac/medium/MODEL_CARD), [Amy](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_US/amy/medium/MODEL_CARD), [Alan](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_GB/alan/medium/MODEL_CARD), and [Alba](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_GB/alba/medium/MODEL_CARD) model cards.

[Support the original author](https://www.buymeacoffee.com/xTaraRose).
