// Test double of futurama.exe for the launcher tests and the screenshot smoke test (built as
// <launcher build>\fakegame\futurama.exe; never packaged). It implements the machine-readable command
// line of the shared contract with canned data and a fake game run:
//
//   --print-config-schema=<file>   copies testdata\sample_schema.json (env FAKE_FUTURAMA_SCHEMA = other file)
//   --query-gpu=<file>             copies testdata\sample_gpu.json    (env FAKE_FUTURAMA_GPU = other file)
//   --check-iso=<path> --result=<file>
//                                  XDVDFS magic "MICROSOFT*XBOX*MEDIA" at 0x10000 (xiso) or
//                                  0x18300000 + 0x10000 (redump); no hashing: a file name containing
//                                  "proto" counts as the NTSC prototype (unsupported), anything else with
//                                  the magic as retail EU. Exit 0 if "supported".
//   --echo-args=<file>             writes the received argv (CommandLineToArgvW) as a JSON array
//   --config=<ini>                 fake game run: writes <userdata>\logs\futurama.log (userdata from
//                                  [paths] userdata, default like the contract), prints
//                                  FAKE_FUTURAMA_STDERR to stderr, sleeps FAKE_FUTURAMA_SLEEP_MS and exits
//                                  with FAKE_FUTURAMA_EXIT (default 0). A missing INI = exit 1 + stderr line.
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n =
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string JsonString(const std::wstring& w) {
    std::string out = "\"";
    for (unsigned char c : Utf8(w)) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

bool WriteBytes(const std::wstring& path, const std::string& data) {
    const HANDLE h =
        CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    return ok != FALSE && written == data.size();
}

bool ReadBytes(const std::wstring& path, std::string& out) {
    const HANDLE h =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.assign(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

void Stderr(const std::string& text) {
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    return n > 0 && n < 4096 ? std::wstring(buf, n) : std::wstring();
}

std::wstring DirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

bool StartsWith(const std::wstring& s, const wchar_t* prefix, std::wstring* rest) {
    const size_t n = std::wcslen(prefix);
    if (s.compare(0, n, prefix) == 0) {
        *rest = s.substr(n);
        return true;
    }
    return false;
}

int CopyTestdata(const std::wstring& target, const wchar_t* envName, const wchar_t* fileName) {
    std::wstring source = Env(envName);
    if (source.empty()) {
        source = std::wstring(FAKE_TESTDATA_DIR) + L"/" + fileName;
    }
    std::string data;
    if (!ReadBytes(source, data)) {
        Stderr("fake futurama: cannot read " + Utf8(source) + "\n");
        return 2;
    }
    return WriteBytes(target, data) ? 0 : 2;
}

bool ReadAt(HANDLE h, std::int64_t offset, char* buffer, DWORD size) {
    LARGE_INTEGER pos{};
    pos.QuadPart = offset;
    DWORD read = 0;
    return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, buffer, size, &read, nullptr) &&
           read == size;
}

int CheckIso(const std::wstring& iso, const std::wstring& resultFile) {
    const HANDLE h =
        CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string json;
    if (h == INVALID_HANDLE_VALUE) {
        json = "{\"ok\":false,\"reason\":\"cannot open the file (Win32 error " +
               std::to_string(GetLastError()) +
               ")\",\"xdvdfs\":false,\"layout\":\"unknown\",\"xbe_sha1\":\"\",\"version\":\"unknown\","
               "\"supported\":false,\"title\":\"\",\"size\":0}";
        WriteBytes(resultFile, json);
        return 1;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    const char magic[] = "MICROSOFT*XBOX*MEDIA";
    char buf[20] = {};
    std::string layout = "unknown";
    if (ReadAt(h, 0x10000, buf, 20) && std::memcmp(buf, magic, 20) == 0) {
        layout = "xiso";
    } else if (ReadAt(h, 0x18300000LL + 0x10000, buf, 20) && std::memcmp(buf, magic, 20) == 0) {
        layout = "redump";
    }
    CloseHandle(h);
    const bool xdvdfs = layout != "unknown";
    std::wstring name = iso.substr(iso.find_last_of(L"\\/") + 1);
    for (wchar_t& c : name) {
        c = static_cast<wchar_t>(towlower(c));
    }
    const bool proto = name.find(L"proto") != std::wstring::npos;
    const bool supported = xdvdfs && !proto;
    json = std::string("{\"ok\":") + (xdvdfs ? "true" : "false") + ",\"reason\":" +
           (xdvdfs ? (supported ? "\"fake check: XDVDFS found, assumed retail EU\""
                                : "\"fake check: prototype by file name\"")
                   : "\"no XDVDFS volume descriptor (MICROSOFT*XBOX*MEDIA) found\"") +
           ",\"xdvdfs\":" + (xdvdfs ? "true" : "false") + ",\"layout\":\"" + layout + "\",\"xbe_sha1\":\"" +
           (xdvdfs ? "fake0000000000000000000000000000000000" : "") + "\",\"version\":\"" +
           (xdvdfs ? (proto ? "proto_ntsc" : "retail_eu") : "unknown") +
           "\",\"supported\":" + (supported ? "true" : "false") + ",\"title\":\"" +
           (xdvdfs ? "Futurama" : "") + "\",\"size\":" + std::to_string(size.QuadPart) + "}";
    WriteBytes(resultFile, json);
    return supported ? 0 : 1;
}

// Minimal INI lookup: value of [section] key (host rules are not needed for the fake).
std::wstring IniValue(const std::string& text, const std::string& section, const std::string& key) {
    std::string current;
    size_t pos = 0;
    std::string result;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == ';' || line[b] == '#') {
            continue;
        }
        line = line.substr(b);
        if (line[0] == '[') {
            current = line.substr(1, line.find(']') - 1);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || current != section) {
            continue;
        }
        std::string k = line.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) {
            k.pop_back();
        }
        if (k != key) {
            continue;
        }
        std::string v = line.substr(eq + 1);
        const size_t vb = v.find_first_not_of(" \t");
        v = vb == std::string::npos ? std::string() : v.substr(vb);
        for (size_t i = 1; i < v.size(); ++i) {
            if ((v[i] == ';' || v[i] == '#') && (v[i - 1] == ' ' || v[i - 1] == '\t')) {
                v = v.substr(0, i);
                break;
            }
        }
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
            v.pop_back();
        }
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            v = v.substr(1, v.size() - 2);
        }
        result = v;
    }
    return Wide(result);
}

int FakeGame(const std::wstring& ini, const std::wstring& exePath) {
    std::string text;
    if (!ReadBytes(ini, text)) {
        Stderr("config file not found: " + Utf8(ini) + "\n");
        return 1;
    }
    std::wstring userdata = IniValue(text, "paths", "userdata");
    if (userdata.empty()) {
        std::wstring portable = DirOf(exePath) + L"\\portable.txt";
        if (GetFileAttributesW(portable.c_str()) != INVALID_FILE_ATTRIBUTES) {
            userdata = DirOf(exePath);
        } else {
            userdata = Env(L"LOCALAPPDATA") + L"\\FuturamaPC";
        }
    } else if (userdata.size() < 2 || userdata[1] != L':') {
        userdata = DirOf(ini) + L"\\" + userdata;
    }
    const std::wstring logs = userdata + L"\\logs";
    CreateDirectoryW(userdata.c_str(), nullptr);
    CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring exitText = Env(L"FAKE_FUTURAMA_EXIT");
    const int exitCode = exitText.empty() ? 0 : _wtoi(exitText.c_str());
    std::string log;
    log += "[    0.000] main      CORE  info  fake futurama.exe (launcher test double)\n";
    log += "[    0.001] main      CORE  info  config: " + Utf8(ini) + "\n";
    log += "[    0.001] main      CORE  info  iso: " + Utf8(IniValue(text, "paths", "iso")) + "\n";
    log += "[    0.002] main      CORE  info  render: resolution " +
           Utf8(IniValue(text, "render", "resolution")) + ", msaa " + Utf8(IniValue(text, "render", "msaa")) +
           ", render_scale " + Utf8(IniValue(text, "render", "render_scale")) + "\n";
    for (int i = 0; i < 40; ++i) {
        log += "[    0." + std::to_string(100 + i) + "] main      GAME  info  fake log line " +
               std::to_string(i + 1) + "\n";
    }
    log += "[    1.000] main      CORE  info  process exit: code " + std::to_string(exitCode) + "\n";
    WriteBytes(logs + L"\\futurama.log", log);
    const std::wstring err = Env(L"FAKE_FUTURAMA_STDERR");
    if (!err.empty()) {
        Stderr(Utf8(err) + "\n");
    }
    const std::wstring sleepText = Env(L"FAKE_FUTURAMA_SLEEP_MS");
    if (!sleepText.empty()) {
        Sleep(static_cast<DWORD>(_wtoi(sleepText.c_str())));
    }
    return exitCode;
}

}  // namespace

int wmain() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 1;
    }
    std::vector<std::wstring> args(argv + 1, argv + argc);
    const std::wstring exePath = argv[0];
    LocalFree(argv);
    std::wstring value;
    std::wstring checkIso;
    std::wstring result;
    std::wstring config;
    for (const std::wstring& a : args) {
        if (StartsWith(a, L"--print-config-schema=", &value)) {
            return CopyTestdata(value, L"FAKE_FUTURAMA_SCHEMA", L"sample_schema.json");
        }
        if (StartsWith(a, L"--query-gpu=", &value)) {
            return CopyTestdata(value, L"FAKE_FUTURAMA_GPU", L"sample_gpu.json");
        }
        if (StartsWith(a, L"--echo-args=", &value)) {
            std::string json = "[";
            for (size_t i = 0; i < args.size(); ++i) {
                json += (i > 0 ? "," : "") + JsonString(args[i]);
            }
            return WriteBytes(value, json + "]") ? 0 : 2;
        }
        if (StartsWith(a, L"--check-iso=", &value)) {
            checkIso = value;
        } else if (StartsWith(a, L"--result=", &value)) {
            result = value;
        } else if (StartsWith(a, L"--config=", &value)) {
            config = value;
        } else {
            Stderr("unknown command line option '" + Utf8(a) + "'\n");
            return 1;
        }
    }
    if (!checkIso.empty()) {
        if (result.empty()) {
            Stderr("--check-iso needs --result=<file.json>\n");
            return 1;
        }
        return CheckIso(checkIso, result);
    }
    if (config.empty()) {
        Stderr("fake futurama: no --config given\n");
        return 1;
    }
    return FakeGame(config, exePath);
}
