#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "json.hpp"

namespace {
using Json = nlohmann::json;
using InternetHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

constexpr GUID engineClsid{0x6c0a3a4e, 0x8f2b, 0x4e5d,
                           {0xa3, 0xc7, 0x1b, 0x9f, 0x2e, 0x8d, 0x4a, 0x6c}};
constexpr GUID engineIid{0xa74d7c8e, 0x4cc5, 0x4f2f,
                         {0xa6, 0xeb, 0x80, 0x4d, 0xee, 0x18, 0x50, 0x0e}};
constexpr GUID waveFormatId{0xc31adbae, 0x527f, 0x4ff5,
                            {0xa2, 0x30, 0xf6, 0x2b, 0xb6, 0x1f, 0xf7, 0x0c}};
constexpr DWORD sampleRate = 22050;
constexpr WORD bytesPerSample = 2;
HMODULE module;
std::atomic<long> moduleReferences{0};

std::string utf8(std::wstring_view text) {
    const int length = static_cast<int>(text.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         length, nullptr, 0, nullptr, nullptr);
    if (!size) return {};
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
                       result.data(), size, nullptr, nullptr);
    return result;
}

Json loadConfig() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), path.size());
    if (!length || length == path.size()) return Json::object();
    path.resize(length);
    std::ifstream file(std::filesystem::path(path).parent_path().parent_path() / L"config.json");
    return Json::parse(file);
}

HRESULT writeAudio(ISpTTSEngineSite& site, const void* data, ULONG length) {
    auto next = static_cast<const unsigned char*>(data);
    while (length) {
        if (site.GetActions() & SPVES_ABORT) return S_FALSE;
        // System.Speech consumes the buffer without updating this out parameter.
        ULONG written = length;
        const HRESULT result = site.Write(next, length, &written);
        if (result != S_OK) return FAILED(result) ? result : S_FALSE;
        if (!written || written > length || written % bytesPerSample) return E_FAIL;
        next += written;
        length -= written;
    }
    return S_OK;
}

HRESULT writePcm(ISpTTSEngineSite& site, std::int16_t* samples, DWORD length,
                 ULONG fragmentVolume, unsigned char& trailingByte, DWORD& remainder) {
    const DWORD count = length / bytesPerSample;
    const DWORD complete = count * bytesPerSample;
    remainder = length - complete;
    const auto bytes = reinterpret_cast<unsigned char*>(samples);
    if (remainder) trailingByte = bytes[complete];

    USHORT volume = 100;
    site.GetVolume(&volume);
    const ULONG gain = std::min<ULONG>(volume, 100) * std::min<ULONG>(fragmentVolume, 100);
    if (gain != 10000) {
        for (DWORD index = 0; index < count; ++index) {
            samples[index] = static_cast<std::int16_t>(samples[index] * static_cast<int>(gain) / 10000);
        }
    }
    return writeAudio(site, bytes, complete);
}

bool disableRedirects(HINTERNET request) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    return WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
}

HRESULT checkStatus(HINTERNET request, DWORD expected) {
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    if (status == expected) return S_OK;
    OutputDebugStringW((L"ElevenLabs SAPI HTTP status " + std::to_wstring(status)).c_str());
    return (status == 401 || status == 403) ? E_ACCESSDENIED : E_FAIL;
}

HRESULT sendDialogueMessage(HINTERNET socket, const Json& message) {
    std::string payload = message.dump();
    const DWORD result = WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                              payload.data(), static_cast<DWORD>(payload.size()));
    return HRESULT_FROM_WIN32(result);
}

HRESULT streamTurbo(HINTERNET connection, const std::wstring& keyHeader, const std::wstring& voiceId,
                    const std::string& text, const Json& config, ULONG fragmentVolume,
                    ISpTTSEngineSite& site) {
    constexpr auto path =
        L"/v1/text-to-dialogue/stream-input?model_id=eleven_v4_turbo&output_format=pcm_22050";
    InternetHandle request(WinHttpOpenRequest(connection, L"GET", path, nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE), WinHttpCloseHandle);
    if (!request || !disableRedirects(request.get()) ||
        !WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request.get(), keyHeader.c_str(), -1L, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT result = checkStatus(request.get(), 101);
    if (FAILED(result)) return result;

    InternetHandle socket(WinHttpWebSocketCompleteUpgrade(request.get(), 0), WinHttpCloseHandle);
    if (!socket) return HRESULT_FROM_WIN32(GetLastError());
    request.reset();

    if (site.GetActions() & SPVES_ABORT) return S_FALSE;
    const auto voice = utf8(voiceId);
    const Json settings{{"stability", config.value("stability", 0.5)},
                        {"similarity_boost", config.value("similarity_boost", 0.75)}};
    result = sendDialogueMessage(socket.get(), {{"voices", Json::array({voice})},
                                                 {"voice_settings", settings}});
    if (FAILED(result)) return result;
    const Json line{{"text", text}, {"voice_id", voice}};
    result = sendDialogueMessage(socket.get(), {{"inputs", Json::array({line})}});
    if (FAILED(result)) return result;
    // Finish the request explicitly so a short subtitle is rendered immediately.
    result = sendDialogueMessage(socket.get(), {{"close_socket", true}});
    if (FAILED(result)) return result;

    std::array<char, 8192> buffer{};
    std::string frame;
    std::vector<std::int16_t> samples;
    unsigned char trailingByte = 0;
    DWORD remainder = 0;
    bool receivedAudio = false;
    for (;;) {
        if (site.GetActions() & SPVES_ABORT) return S_FALSE;
        DWORD received = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        const DWORD error = WinHttpWebSocketReceive(socket.get(), buffer.data(),
                                                    buffer.size(), &received, &type);
        if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
        if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) return E_FAIL;
        if (frame.size() + received > 16 * 1024 * 1024) return E_FAIL;
        frame.append(buffer.data(), received);
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;

        const Json message = Json::parse(frame);
        frame.clear();
        if (message.contains("error") && !message["error"].is_null()) return E_FAIL;
        const auto audio = message.find("audio");
        if (audio != message.end() && audio->is_string()) {
            const auto& encoded = audio->get_ref<const std::string&>();
            if (!encoded.empty()) {
                samples.resize((encoded.size() + remainder + 1) / bytesPerSample);
                auto bytes = reinterpret_cast<unsigned char*>(samples.data());
                if (remainder) bytes[0] = trailingByte;
                DWORD decoded = static_cast<DWORD>(samples.size() * bytesPerSample - remainder);
                if (!CryptStringToBinaryA(encoded.data(), static_cast<DWORD>(encoded.size()),
                                          CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, bytes + remainder,
                                          &decoded, nullptr, nullptr)) {
                    return HRESULT_FROM_WIN32(GetLastError());
                }
                receivedAudio = receivedAudio || decoded != 0;
                result = writePcm(site, samples.data(), decoded + remainder,
                                  fragmentVolume, trailingByte, remainder);
                if (result != S_OK) return result;
            }
        }
        if (message.value("is_final", false)) {
            return receivedAudio && !remainder ? S_OK : E_FAIL;
        }
    }
}

HRESULT streamSpeech(const std::wstring& voiceId, const std::string& text,
                     const Json& config, long fragmentRate, ULONG fragmentVolume,
                     ISpTTSEngineSite& site) {
    const std::string key = config.value("api_key", "");
    if (key.empty() || key == "YOUR_API_KEY_HERE" ||
        key.find_first_of("\r\n") != std::string::npos) {
        return E_ACCESSDENIED;
    }

    InternetHandle session(WinHttpOpen(L"ElevenLabsSapi/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0),
                           WinHttpCloseHandle);
    if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 15000)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    InternetHandle connection(WinHttpConnect(session.get(), L"api.elevenlabs.io",
                                              INTERNET_DEFAULT_HTTPS_PORT, 0),
                              WinHttpCloseHandle);
    if (!connection) return HRESULT_FROM_WIN32(GetLastError());

    const std::wstring keyHeader = L"xi-api-key: " + std::wstring(key.begin(), key.end()) + L"\r\n";
    const std::string model = config.value("model_id", "eleven_multilingual_v2");
    if (model == "eleven_v4_turbo") {
        return streamTurbo(connection.get(), keyHeader, voiceId, text, config, fragmentVolume, site);
    }

    long rate = 0;
    site.GetRate(&rate);
    const double rateScale = config.value("sapi_rate_scaling", true)
        ? 1.0 + 0.05 * std::clamp(rate + fragmentRate, -10L, 10L)
        : 1.0;
    const double speed = std::clamp(config.value("speed", 1.0) * rateScale, 0.7, 1.2);
    Json voiceSettings{
        {"stability", config.value("stability", 0.5)},
        {"similarity_boost", config.value("similarity_boost", 0.75)},
        {"style", config.value("style", 0.0)},
        {"use_speaker_boost", config.value("use_speaker_boost", true)}
    };
    if (model != "eleven_v3") voiceSettings["speed"] = speed;
    std::string body = Json{{"text", text}, {"model_id", model},
                            {"voice_settings", voiceSettings}}.dump();

    const std::wstring path = L"/v1/text-to-speech/" + voiceId + L"/stream?output_format=pcm_22050";
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"POST", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE), WinHttpCloseHandle);
    if (!request) return HRESULT_FROM_WIN32(GetLastError());

    const std::wstring headers = L"Content-Type: application/json\r\n" + keyHeader;
    const DWORD size = static_cast<DWORD>(body.size());
    if (!disableRedirects(request.get()) ||
        !WinHttpSendRequest(request.get(), headers.c_str(), -1L, body.data(), size, size, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    const HRESULT status = checkStatus(request.get(), 200);
    if (FAILED(status)) return status;

    std::array<std::int16_t, 2048> samples{};
    auto bytes = reinterpret_cast<unsigned char*>(samples.data());
    unsigned char trailingByte = 0;
    DWORD remainder = 0;
    for (;;) {
        if (site.GetActions() & SPVES_ABORT) return S_FALSE;
        if (remainder) bytes[0] = trailingByte;
        DWORD received = 0;
        if (!WinHttpReadData(request.get(), bytes + remainder, sizeof(samples) - remainder, &received)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        if (!received) return remainder ? E_FAIL : S_OK;

        const HRESULT result = writePcm(site, samples.data(), received + remainder,
                                        fragmentVolume, trailingByte, remainder);
        if (result != S_OK) return result;
    }
}

class Engine final : public ISpTTSEngine, public ISpObjectWithToken {
public:
    Engine() { ++moduleReferences; }
    ~Engine() {
        if (token_) token_->Release();
        --moduleReferences;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == engineIid) {
            *object = static_cast<ISpTTSEngine*>(this);
        } else if (iid == __uuidof(ISpObjectWithToken)) {
            *object = static_cast<ISpObjectWithToken*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE SetObjectToken(ISpObjectToken* token) override {
        if (!token) return E_POINTER;
        if (token_) return E_UNEXPECTED;
        wchar_t* id = nullptr;
        const HRESULT result = token->GetStringValue(L"ElevenLabsVoiceId", &id);
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> value(id, CoTaskMemFree);
        if (FAILED(result)) return result;
        if (!id || !*id || std::wstring_view(id).find_first_not_of(
                L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-") !=
                std::wstring_view::npos) {
            return E_INVALIDARG;
        }
        try {
            voiceId_ = id;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        token->AddRef();
        token_ = token;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetObjectToken(ISpObjectToken** token) override {
        if (!token) return E_POINTER;
        *token = token_;
        if (!token_) return E_UNEXPECTED;
        token_->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetOutputFormat(const GUID*, const WAVEFORMATEX*,
                                               GUID* formatId, WAVEFORMATEX** format) override {
        if (!formatId || !format) return E_POINTER;
        *format = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!*format) return E_OUTOFMEMORY;
        **format = {WAVE_FORMAT_PCM, 1, sampleRate, sampleRate * bytesPerSample, bytesPerSample, 16, 0};
        *formatId = waveFormatId;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Speak(DWORD, REFGUID, const WAVEFORMATEX*,
                                    const SPVTEXTFRAG* fragments, ISpTTSEngineSite* site) override {
        if (!site) return E_POINTER;
        if (!token_) return E_UNEXPECTED;
        try {
            std::wstring text;
            long rate = 0;
            ULONG volume = 100;
            for (auto fragment = fragments; fragment; fragment = fragment->pNext) {
                if (!fragment->pTextStart || !fragment->ulTextLen) continue;
                const auto action = fragment->State.eAction;
                if (action != SPVA_Speak && action != SPVA_Pronounce && action != SPVA_SpellOut) continue;
                if (text.empty()) {
                    rate = fragment->State.RateAdj;
                    volume = fragment->State.Volume;
                }
                text.append(fragment->pTextStart, fragment->ulTextLen);
            }
            if (text.find_first_not_of(L" \r\n\t") == std::wstring::npos ||
                (site->GetActions() & SPVES_ABORT)) {
                return S_OK;
            }
            const auto encoded = utf8(text);
            if (encoded.empty()) return E_INVALIDARG;
            const auto result = streamSpeech(voiceId_, encoded, loadConfig(), rate, volume, *site);
            return result == S_FALSE ? S_OK : result;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    std::atomic<ULONG> references_{1};
    ISpObjectToken* token_ = nullptr;
    std::wstring voiceId_;
};

class EngineFactory final : public IClassFactory {
public:
    EngineFactory() { ++moduleReferences; }
    ~EngineFactory() { --moduleReferences; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *object = static_cast<IClassFactory*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto engine = new (std::nothrow) Engine();
        if (!engine) return E_OUTOFMEMORY;
        const HRESULT result = engine->QueryInterface(iid, object);
        engine->Release();
        return result;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++moduleReferences;
        else --moduleReferences;
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
};
}

BOOL WINAPI DllMain(HMODULE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module = instance;
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != engineClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = new (std::nothrow) EngineFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT result = factory->QueryInterface(iid, object);
    factory->Release();
    return result;
}

STDAPI DllCanUnloadNow() {
    return moduleReferences == 0 ? S_OK : S_FALSE;
}
