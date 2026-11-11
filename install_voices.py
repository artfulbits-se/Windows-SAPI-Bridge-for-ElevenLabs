"""
Windows SAPI Bridge for ElevenLabs — Voice Installer
======================================================
Queries your ElevenLabs account and registers every voice as a native Windows
SAPI5 voice, making them available in any application that uses Windows TTS
(e.g. Narrator, Calibre, NVDA, Balabolka, custom apps, etc.).

MUST be run as Administrator.
Usage:  python install_voices.py
"""

import sys
import os
import json
import re
import ctypes
import requests
import io

try:
    import winreg
except ImportError:
    print('ERROR: This script must be run on Windows.')
    sys.exit(1)

# Force UTF-8 output on Windows
if sys.stdout.encoding.lower() != 'utf-8':
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

# ─── Paths & constants ────────────────────────────────────────────────────────

_script_dir  = os.path.dirname(os.path.abspath(__file__))
_config_path = os.path.join(_script_dir, 'config.json')
_state_path  = os.path.join(_script_dir, '.installed_voices.json')

ELEVENLABS_BASE = 'https://api.elevenlabs.io/v1'

# Must match CLSID_ElevenLabsEngine in elevenlabs_engine.py
ENGINE_CLSID  = '{6C0A3A4E-8F2B-4E5D-A3C7-1B9F2E8D4A6C}'

SPEECH_VOICES = r'SOFTWARE\Microsoft\Speech\Voices\Tokens'
CLASSES_CLSID = r'SOFTWARE\Classes\CLSID'

# KEY_WOW64_64KEY ensures we write to the 64-bit hive from any Python bitness.
_ACCESS = winreg.KEY_WRITE | winreg.KEY_WOW64_64KEY

# ─── Helpers ──────────────────────────────────────────────────────────────────

def banner(msg: str):
    print(f'\n{msg}')
    print('─' * len(msg))


def load_config() -> dict:
    config_path = _config_path
    local_app_data = os.environ.get('LOCALAPPDATA')
    if local_app_data:
        native_config = os.path.join(local_app_data, 'ElevenLabsSAPI', 'config.json')
        if os.path.isfile(native_config):
            config_path = native_config
    if not os.path.exists(config_path):
        print(f'ERROR: config.json not found at {config_path}')
        sys.exit(1)
    with open(config_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def sanitize(name: str) -> str:
    return re.sub(r'[^A-Za-z0-9_\-]', '_', name)


def find_python() -> str:
    """Prefer pythonw.exe (no console window) for the COM server process."""
    exe_dir = os.path.dirname(sys.executable)
    for candidate in ('pythonw.exe', 'python.exe'):
        full = os.path.join(exe_dir, candidate)
        if os.path.exists(full):
            return full
    return sys.executable

# ─── ElevenLabs API ───────────────────────────────────────────────────────────

def fetch_voices(api_key: str) -> list:
    resp = requests.get(
        f'{ELEVENLABS_BASE}/voices',
        headers={'xi-api-key': api_key},
        timeout=15,
    )
    resp.raise_for_status()
    out = []
    for v in resp.json().get('voices', []):
        labels = v.get('labels') or {}
        out.append({
            'voice_id': v.get('voice_id', ''),
            'name':     v.get('name', 'Unknown'),
            'gender':   labels.get('gender', 'neutral').lower(),
        })
    return out

# ─── Registry helpers ─────────────────────────────────────────────────────────

def reg_set(hive, path: str, values: dict):
    with winreg.CreateKeyEx(hive, path, 0, _ACCESS) as k:
        for name, value in values.items():
            winreg.SetValueEx(k, name, 0, winreg.REG_SZ, str(value))

# ─── Registration ─────────────────────────────────────────────────────────────

def register_engine_clsid(python_exe: str, engine_py: str):
    """Register the shared COM engine under its CLSID."""
    base       = f'{CLASSES_CLSID}\\{ENGINE_CLSID}'
    server_cmd = f'"{python_exe}" "{engine_py}"'
    reg_set(winreg.HKEY_LOCAL_MACHINE, base,
            {'': 'ElevenLabs TTS Engine'})
    reg_set(winreg.HKEY_LOCAL_MACHINE, f'{base}\\LocalServer32',
            {'': server_cmd})
    print(f'  Engine CLSID : {ENGINE_CLSID}')
    print(f'  LocalServer32: {server_cmd}')


def register_voice(voice_id: str, name: str, gender: str) -> str:
    """Register a single ElevenLabs voice as a SAPI5 voice token."""
    safe_name    = sanitize(name)
    token_key    = f'ElevenLabs_{safe_name}'
    token_path   = f'{SPEECH_VOICES}\\{token_key}'
    display_name = f'ElevenLabs - {name}'
    gender_str   = 'Female' if gender == 'female' else 'Male'

    reg_set(winreg.HKEY_LOCAL_MACHINE, token_path, {
        '':                  display_name,
        'CLSID':             ENGINE_CLSID,
        'ElevenLabsVoiceId': voice_id,   # read by engine at runtime
    })
    reg_set(winreg.HKEY_LOCAL_MACHINE, f'{token_path}\\Attributes', {
        'Name':     display_name,
        'Vendor':   'ElevenLabs',
        'Language': '409',          # 0x0409 = en-US; audio works for all languages
        'Gender':   gender_str,
        'Age':      'Adult',
    })
    print(f'  ✓  {display_name}  [{voice_id}]')
    return token_key

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    print('╔══════════════════════════════════════════════════════════╗')
    print('║   Windows SAPI Bridge for ElevenLabs — Voice Installer   ║')
    print('╚══════════════════════════════════════════════════════════╝')

    if not ctypes.windll.shell32.IsUserAnAdmin():
        print('\nERROR: Must be run as Administrator.')
        print('Right-click Command Prompt → "Run as administrator", then retry.')
        sys.exit(1)

    cfg     = load_config()
    api_key = cfg.get('api_key', '')
    if not api_key or api_key == 'YOUR_API_KEY_HERE':
        print('\nERROR: Open config.json and replace YOUR_API_KEY_HERE with your key.')
        sys.exit(1)

    python_exe  = find_python()
    engine_path = os.path.join(_script_dir, 'elevenlabs_engine.py')
    if not os.path.exists(engine_path):
        print(f'\nERROR: elevenlabs_engine.py not found at {engine_path}')
        sys.exit(1)

    banner('Step 1 — Fetching your ElevenLabs voices')
    try:
        voices = fetch_voices(api_key)
    except requests.HTTPError as exc:
        print(f'  ERROR: API request failed — {exc}')
        print('  Check your API key and internet connection.')
        sys.exit(1)
    print(f'  Found {len(voices)} voice(s).')

    banner('Step 2 — Registering engine CLSID')
    register_engine_clsid(python_exe, engine_path)

    banner('Step 3 — Registering voice tokens')
    registered = []
    for v in voices:
        try:
            key = register_voice(v['voice_id'], v['name'], v['gender'])
            registered.append(key)
        except Exception as exc:
            print(f'  ✗  {v["name"]} — FAILED: {exc}')

    with open(_state_path, 'w', encoding='utf-8') as f:
        json.dump({'voices': registered, 'engine_clsid': ENGINE_CLSID}, f, indent=2)

    print(f'\n✅  Done!  {len(registered)} voice(s) registered.')
    print('\nThe ElevenLabs voices will now appear in any SAPI5-compatible')
    print('application\'s voice selection — no per-app setup required.')
    print(f'\nLogs: %APPDATA%\\ElevenLabsSAPI\\engine.log')


if __name__ == '__main__':
    main()
