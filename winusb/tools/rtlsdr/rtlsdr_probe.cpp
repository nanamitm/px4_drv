// SPDX-License-Identifier: GPL-2.0-or-later
// USB 制御は librtlsdr に委ね、診断と有限長 CU8 収録を行う。
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

struct rtlsdr_dev;
using Device = rtlsdr_dev;

std::string quote(const std::string &s)
{
    std::string out = "\"";
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}

std::uint64_t number(const wchar_t *s, std::uint64_t max)
{
    if (!s || !*s) throw std::runtime_error("Empty numeric argument");
    std::uint64_t n = 0;
    for (; *s; ++s) {
        if (*s < L'0' || *s > L'9' || n > max / 10 ||
            (n == max / 10 && static_cast<unsigned>(*s - L'0') > max % 10))
            throw std::runtime_error("Numeric argument is invalid or out of range");
        n = n * 10 + (*s - L'0');
    }
    return n;
}

std::string utf8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid serial string");
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    out.pop_back();
    return out;
}

struct Library {
    HMODULE module = nullptr;
    Device *dev = nullptr;
    template<class T> T symbol(const char *name) {
        auto ptr = GetProcAddress(module, name);
        if (!ptr) throw std::runtime_error(std::string("Missing librtlsdr export: ") + name);
        // 関数ポインターへの変換は Win32 の GetProcAddress 契約に従う。
#pragma warning(suppress: 4191)
        return reinterpret_cast<T>(ptr);
    }
    using Count = std::uint32_t (__cdecl *)();
    using Strings = int (__cdecl *)(std::uint32_t, char *, char *, char *);
    using Open = int (__cdecl *)(Device **, std::uint32_t);
    using Close = int (__cdecl *)(Device *);
    using GetStrings = int (__cdecl *)(Device *, char *, char *, char *);
    using GetInt = int (__cdecl *)(Device *);
    using GetU32 = std::uint32_t (__cdecl *)(Device *);
    using SetInt = int (__cdecl *)(Device *, int);
    using SetU32 = int (__cdecl *)(Device *, std::uint32_t);
    using Gains = int (__cdecl *)(Device *, int *);
    using Read = int (__cdecl *)(Device *, void *, int, int *);
    Count count = nullptr; Strings strings = nullptr; Open open = nullptr; Close close = nullptr;
    GetStrings device_strings = nullptr; GetInt tuner = nullptr; Gains gains = nullptr;
    SetU32 rate = nullptr, frequency = nullptr; GetU32 get_rate = nullptr, get_frequency = nullptr;
    SetInt gain_mode = nullptr, gain = nullptr; GetInt reset = nullptr; Read read = nullptr;
    Library() {
        wchar_t path[32768]{};
        DWORD n = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        if (!n || n >= std::size(path)) throw std::runtime_error("Cannot resolve executable path");
        std::wstring file(path);
        file.resize(file.find_last_of(L"\\/") + 1);
        file += L"rtlsdr.dll";
        // 作業ディレクトリや PATH 上の同名 DLL をロードしない。
        module = LoadLibraryExW(file.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) throw std::runtime_error("Cannot load local rtlsdr.dll; run build.ps1 first (Win32 error " + std::to_string(GetLastError()) + ")");
        try {
            count = symbol<Count>("rtlsdr_get_device_count");
            strings = symbol<Strings>("rtlsdr_get_device_usb_strings");
            open = symbol<Open>("rtlsdr_open"); close = symbol<Close>("rtlsdr_close");
            device_strings = symbol<GetStrings>("rtlsdr_get_usb_strings");
            tuner = symbol<GetInt>("rtlsdr_get_tuner_type"); gains = symbol<Gains>("rtlsdr_get_tuner_gains");
            rate = symbol<SetU32>("rtlsdr_set_sample_rate"); frequency = symbol<SetU32>("rtlsdr_set_center_freq");
            get_rate = symbol<GetU32>("rtlsdr_get_sample_rate"); get_frequency = symbol<GetU32>("rtlsdr_get_center_freq");
            gain_mode = symbol<SetInt>("rtlsdr_set_tuner_gain_mode"); gain = symbol<SetInt>("rtlsdr_set_tuner_gain");
            reset = symbol<GetInt>("rtlsdr_reset_buffer"); read = symbol<Read>("rtlsdr_read_sync");
        } catch (...) { FreeLibrary(module); module = nullptr; throw; }
    }
    ~Library() { if (dev) close(dev); if (module) FreeLibrary(module); }
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;
    void select(const std::string &serial) {
        int found = -1;
        for (std::uint32_t i = 0, n = count(); i < n; ++i) {
            char m[256]{}, p[256]{}, s[256]{};
            if (strings(i, m, p, s) || serial != s) continue;
            if (found >= 0) throw std::runtime_error("Duplicate serial; connect only the intended RTL-SDR");
            found = static_cast<int>(i);
        }
        if (found < 0) throw std::runtime_error("Serial not found or USB access denied; check WinUSB and other SDR applications");
        int result = open(&dev, static_cast<std::uint32_t>(found));
        if (result || !dev) throw std::runtime_error("rtlsdr_open failed: " + std::to_string(result));
        char m[256]{}, p[256]{}, s[256]{};
        // 列挙後の抜去や並び替えで、別個体を操作するのを防ぐ。
        if (device_strings(dev, m, p, s) || serial != s)
            throw std::runtime_error("Device identity changed while opening");
    }
    std::vector<int> supported_gains() {
        int n = gains(dev, nullptr);
        if (n <= 0 || n > 256) throw std::runtime_error("Cannot read tuner gains");
        std::vector<int> values(static_cast<std::size_t>(n));
        int got = gains(dev, values.data());
        if (got != n) throw std::runtime_error("Tuner gain count changed");
        return values;
    }
};

void check(int result, const char *operation)
{
    if (result) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

std::string tuner_name(int type)
{
    static const char *names[] = {"Unknown", "E4000", "FC0012", "FC0013", "FC2580", "R820T/R820T2", "R828D"};
    return type >= 0 && type < static_cast<int>(std::size(names)) ? names[type] : "Unknown";
}

int wmain(int argc, wchar_t **argv)
{
    try {
        if (argc < 2) throw std::runtime_error("Usage: rtlsdr_probe list | probe SERIAL | capture SERIAL HZ RATE SAMPLES auto|GAIN_TENTHS_DB FILE");
        const std::wstring action = argv[1];
        const bool capture = action == L"capture";
        if (!((action == L"list" && argc == 2) || (action == L"probe" && argc == 3) || (capture && argc == 8)))
            throw std::runtime_error("Invalid arguments");
        std::uint32_t hz = 0, rate = 0;
        std::uint64_t samples = 0;
        int requested_gain = 0;
        bool automatic_gain = false;
        if (capture) {
            hz = static_cast<std::uint32_t>(number(argv[3], 2000000000));
            rate = static_cast<std::uint32_t>(number(argv[4], 2400000));
            samples = number(argv[5], 1440000000);
            if (hz < 24000000 || rate < 225001 || (rate > 300000 && rate <= 900000) || !samples)
                throw std::runtime_error("Unsupported frequency, sample rate, or sample count");
            automatic_gain = std::wstring(argv[6]) == L"auto";
            if (!automatic_gain) {
                bool negative = argv[6][0] == L'-';
                requested_gain = static_cast<int>(number(argv[6] + (negative ? 1 : 0), 600)) * (negative ? -1 : 1);
            }
            if (GetFileAttributesW(argv[7]) != INVALID_FILE_ATTRIBUTES)
                throw std::runtime_error("Output already exists");
        }
        Library api;
        if (action == L"list") {
            std::cout << "[";
            bool first = true;
            for (std::uint32_t i = 0, n = api.count(); i < n; ++i) {
                char m[256]{}, p[256]{}, s[256]{};
                int result = api.strings(i, m, p, s);
                if (!first) std::cout << ",";
                first = false;
                std::cout << "{\"index\":" << i << ",\"manufacturer\":" << quote(m)
                    << ",\"product\":" << quote(p) << ",\"serial\":" << quote(s)
                    << ",\"usb_strings_result\":" << result << "}";
            }
            std::cout << "]\n";
            return 0;
        }
        const std::string serial = utf8(argv[2]);
        if (serial.empty()) throw std::runtime_error("Serial must not be empty");
        api.select(serial);
        const auto gains = api.supported_gains();
        const auto tuner = tuner_name(api.tuner(api.dev));
        if (!capture) {
            std::cout << "{\"serial\":" << quote(serial) << ",\"tuner\":" << quote(tuner) << ",\"gains_tenths_db\":[";
            for (std::size_t i = 0; i < gains.size(); ++i) std::cout << (i ? "," : "") << gains[i];
            std::cout << "]}\n";
            return 0;
        }
        // 先に周波数を確定し、レート変更時の再選局が未設定の 0 Hz を使わないようにする。
        // RTL-SDR Blog 版では 0 Hz が HF 直接サンプリングへの自動切替を誘発する。
        check(api.frequency(api.dev, hz), "Set frequency");
        check(api.rate(api.dev, rate), "Set sample rate");
        check(api.gain_mode(api.dev, automatic_gain ? 0 : 1), "Set gain mode");
        int actual_gain = 0;
        if (!automatic_gain) {
            actual_gain = *std::min_element(gains.begin(), gains.end(), [requested_gain](int a, int b) {
                return std::abs(a - requested_gain) < std::abs(b - requested_gain);
            });
            check(api.gain(api.dev, actual_gain), "Set gain");
        }
        check(api.reset(api.dev), "Reset USB buffer");
        // CREATE_NEW で、事前確認後に別処理が作ったファイルも上書きしない。
        HANDLE file = CreateFileW(argv[7], GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create output file: " + std::to_string(GetLastError()));
        std::array<unsigned char, 16384> buffer{};
        std::uint64_t remaining = samples * 2, written = 0, clipped = 0;
        const auto start = std::chrono::steady_clock::now();
        try {
            while (remaining) {
                int received = 0;
                // 同期転送自体が停止した場合は親 PowerShell が期限を監視してこの子だけを回収する。
                check(api.read(api.dev, buffer.data(), static_cast<int>(buffer.size()), &received), "Read I/Q");
                if (received <= 0 || received > static_cast<int>(buffer.size()) || received % 2)
                    throw std::runtime_error("Invalid or empty I/Q read");
                DWORD size = static_cast<DWORD>(std::min<std::uint64_t>(remaining, static_cast<unsigned>(received)));
                DWORD done = 0;
                if (!WriteFile(file, buffer.data(), size, &done, nullptr) || done != size)
                    throw std::runtime_error("I/Q write failed or disk full");
                for (DWORD i = 0; i < size; ++i) if (buffer[i] == 0 || buffer[i] == 255) ++clipped;
                written += size;
                remaining -= size;
            }
            if (!FlushFileBuffers(file)) throw std::runtime_error("I/Q flush failed");
            CloseHandle(file); file = INVALID_HANDLE_VALUE;
        } catch (...) { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); throw; }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "{\"serial\":" << quote(serial) << ",\"tuner\":" << quote(tuner)
            << ",\"format\":\"CU8\",\"sample_rate_requested\":" << rate
            << ",\"sample_rate_actual\":" << api.get_rate(api.dev)
            << ",\"frequency_hz\":" << api.get_frequency(api.dev)
            << ",\"automatic_gain\":" << (automatic_gain ? "true" : "false")
            << ",\"gain_tenths_db\":" << (automatic_gain ? "null" : std::to_string(actual_gain))
            << ",\"samples\":" << samples << ",\"bytes\":" << written
            << ",\"clipped_components\":" << clipped << ",\"elapsed_seconds\":" << seconds << "}\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
