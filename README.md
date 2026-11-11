# Windows SAPI Bridge for ElevenLabs and Piper

This fork exposes ElevenLabs cloud voices and offline Piper voices through 64-bit Windows SAPI5. An application such as ALLPlayer can select any of these voices from its existing speech synthesizer settings.

| Voice | Synthesis |
| --- | --- |
| `ElevenLabs - Adam - Dominant, Firm` and other ElevenLabs voices | ElevenLabs API. `eleven_v4_turbo` uses the Text to Dialogue WebSocket. `eleven_v4` and other supported HTTP models use the Text to Speech streaming endpoint. |
| `Piper - Lessac Medium (US English)`, `Piper - Amy Medium (US English)` | Local US English voices. |
| `Piper - Alan Medium (UK English)`, `Piper - Alba Medium (UK English)` | Local UK English voices. |

The native COM DLL runs inside the 64-bit SAPI application. Piper runs in a child process that loads the selected model once and streams audio to the DLL over private pipes. The worker exits when the SAPI voice is released. All Piper voices work offline after setup and use zero ElevenLabs credits. ALLPlayer's reading speed and volume controls apply to Piper.

## Build and install

Use Windows 10 or 11 on x64, 64-bit PowerShell, and x64 Python 3.9 or later. The build below uses [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases), the [Windows SDK SAPI DDK header](https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/um/sapiddk.h), and the [nlohmann JSON single header](https://github.com/nlohmann/json/releases/tag/v3.12.0). Put `sapiddk.h` and `json.hpp` in the same include directory. Set `$compiler` to the full path to the x64 llvm-mingw `clang++.exe` and `$includes` to the include directory.

```powershell
$dll = Join-Path $env:TEMP 'ElevenLabsSapi.dll'
& $compiler -std=c++17 -O2 -shared -static -I $includes -o $dll `
  native-sapi/ElevenLabsSapi.cpp native-sapi/ElevenLabsSapi.def `
  -lole32 -luuid -lwinhttp -lcrypt32
```

Run the installer below from 64-bit PowerShell as Administrator under the same Windows account that will run the speech application. It installs Piper 1.8.0 and its English models into a dedicated environment, registers the native DLL for that account, and adds one classic SAPI token per voice. Run the same command with a rebuilt DLL to update an existing Piper installation. For an ElevenLabs-only install, omit `-Piper`.

```powershell
.\native-sapi\install.ps1 -EngineDll $dll -Piper
```

Fully quit and reopen ALLPlayer, including any tray process. In **Settings → Speech Synthesizer**, enable **Read subtitles, using voice**, select a **Piper** voice, and click **Test the reader**.

### Add or refresh ElevenLabs voices

Piper setup works without an ElevenLabs account. To add cloud voices, edit `%LOCALAPPDATA%\ElevenLabsSAPI\config.json` with your ElevenLabs API key and model. The installer creates this file from `config.example.json` on first setup. Keep the real key in the local configuration.

```json
{
  "api_key": "YOUR_API_KEY_HERE",
  "model_id": "eleven_v4_turbo"
}
```

Run these commands from an administrator PowerShell window under the same Windows account. Run the voice installer again whenever your ElevenLabs voice list changes.

```powershell
python -m pip install -r requirements.txt
python install_voices.py --native
```

Select an `ElevenLabs - …` entry in ALLPlayer. The bridge rereads the configuration on each cloud utterance, so a model change takes effect on the next subtitle.

## Verification and troubleshooting

- The native engine has generated Adam audio through Windows SAPI using both Eleven v4 and v4 Turbo. All four listed Piper voices have also produced non-silent 22.05 kHz audio through Windows SAPI. The first request for a Piper voice loads its model and can take several seconds.
- If a new voice is missing, restart the speech application after registration and confirm it is a 64-bit SAPI5 client.
- For Piper startup errors, check for `piper_worker.py`, `.venv\Scripts\python.exe`, and the selected voice's `.onnx` and `.onnx.json` files in `%LOCALAPPDATA%\ElevenLabsSAPI\piper`. Rerun the native installer with `-Piper` to repair that setup.
- For cloud errors, check the model ID, API permissions, and available credits. Cloud speech sends the utterance to ElevenLabs. The Piper worker reads only the local model and the text passed over its process pipes.
- The original Python COM bridge and its cleanup tools remain available. See [SAPI_STATUS.md](SAPI_STATUS.md) for the original marshaling issue and [CLEANUP_GUIDE.md](CLEANUP_GUIDE.md) for legacy registrations.

## Credits and licensing

The bridge is [MIT licensed](LICENSE). The original Windows SAPI bridge was created by [xTaraRose](https://github.com/xTaraRose/Windows-SAPI-Bridge-for-ElevenLabs). The local implementation takes design cues from [Piper SAPI5](https://github.com/mflow888/piper-chromium-sapi5) and runs [OHF Piper](https://github.com/OHF-Voice/piper1-gpl) as a separate GPL-3.0 runtime. Piper and the voices are downloaded at setup time. Dataset licenses are listed in the [Lessac](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_US/lessac/medium/MODEL_CARD), [Amy](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_US/amy/medium/MODEL_CARD), [Alan](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_GB/alan/medium/MODEL_CARD), and [Alba](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_GB/alba/medium/MODEL_CARD) model cards.

[Support the original author](https://www.buymeacoffee.com/xTaraRose).
