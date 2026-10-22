#include "deployment.h"
#include "pdf.h"
#include "resource.h"
#include "dialog_style.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <fstream>
#include <chrono>
#include <thread>

namespace deployment {
#ifdef FASTPDF_DEPLOYMENT_TEST
static fs::path test_root, test_executable;
void set_test_paths(const fs::path& root, const fs::path& current_executable) {
    test_root = root;
    test_executable = current_executable;
}
#endif
static constexpr wchar_t uninstall_key[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FastPDF";
static constexpr size_t max_download = 64 * 1024 * 1024;
static void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void win_check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(std::string(message) + " (Windows error " + std::to_string(GetLastError()) +
                                 ")");
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
struct Internet {
    HINTERNET value = nullptr;
    ~Internet() {
        if (value)
            WinHttpCloseHandle(value);
    }
};
struct Registry {
    HKEY value = nullptr;
    ~Registry() {
        if (value)
            RegCloseKey(value);
    }
};
static fs::path known_folder(REFKNOWNFOLDERID id) {
#ifdef FASTPDF_DEPLOYMENT_TEST
    if (!test_root.empty()) {
        const auto directory = test_root / (id == FOLDERID_Programs ? "Start Menu" : "LocalAppData");
        fs::create_directories(directory);
        return directory;
    }
#endif
    PWSTR text = nullptr;
    check(SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &text)), "Cannot locate the Windows user folder");
    fs::path result(text);
    CoTaskMemFree(text);
    return result;
}
fs::path executable_path() {
#ifdef FASTPDF_DEPLOYMENT_TEST
    if (!test_executable.empty())
        return test_executable;
#endif
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    win_check(length > 0 && length < path.size(), "Cannot locate FastPDF");
    path.resize(length);
    return path;
}
fs::path install_directory() {
    return known_folder(FOLDERID_LocalAppData) / "Programs" / "FastPDF";
}
static fs::path installed_executable() {
    return install_directory() / "fastpdf.exe";
}
static fs::path cache_directory() {
    return known_folder(FOLDERID_LocalAppData) / "FastPDF" / "Maintenance";
}
static fs::path shortcut_path() {
    return known_folder(FOLDERID_Programs) / "FastPDF.lnk";
}
static bool same_file(const fs::path& left, const fs::path& right) {
    std::error_code error;
    return fs::equivalent(left, right, error) && !error;
}
bool installed_instance() {
    return same_file(executable_path(), installed_executable());
}
bool installation_present() {
    return fs::is_regular_file(installed_executable());
}
static void reject_reparse(const fs::path& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    check(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
          "Installation paths must not be symbolic links or junctions");
}
static void ensure_directory(const fs::path& path) {
    reject_reparse(path);
    fs::create_directories(path);
    reject_reparse(path);
}
static std::string sha256(std::span<const uint8_t> bytes) {
    auto hash = pdf::digest(BCRYPT_SHA256_ALGORITHM, bytes);
    std::string text;
    constexpr char hex[] = "0123456789abcdef";
    for (auto byte : hash) {
        text += hex[byte >> 4];
        text += hex[byte & 15];
    }
    return text;
}
static pdf::Bytes read_file(const fs::path& path, size_t maximum = max_download) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(bool(input), "Cannot read deployment file");
    const auto length = input.tellg();
    check(length >= 0 && uint64_t(length) <= maximum, "Deployment file exceeds size limit");
    pdf::Bytes bytes(static_cast<size_t>(length));
    input.seekg(0);
    if (!bytes.empty())
        check(bool(input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))),
              "Cannot read deployment file");
    return bytes;
}
static void write_file(const fs::path& path, std::span<const uint8_t> bytes) {
    reject_reparse(path);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    check(bool(output), "Cannot create deployment file");
    output.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    output.close();
    check(bool(output), "Cannot finish writing deployment file");
}
static HANDLE maintenance_lock() {
    const auto path = pdf::utf8(install_directory().wstring());
    auto name = L"Local\\FastPDF.Maintenance." + pdf::utf8(sha256(pdf::view(path)));
    HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
    win_check(handle != nullptr, "Cannot create maintenance lock");
    const DWORD result = WaitForSingleObject(handle, 0);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
        CloseHandle(handle);
        throw std::runtime_error("Another FastPDF installation or update is already in progress.");
    }
    return handle;
}
struct MaintenanceLock {
    HANDLE value = maintenance_lock();
    ~MaintenanceLock() {
        ReleaseMutex(value);
        CloseHandle(value);
    }
};
HANDLE acquire_instance_lock() {
    if (!installed_instance())
        return nullptr;
    const auto file = install_directory() / "running.lock";
    reject_reparse(file);
    HANDLE handle =
        CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    win_check(handle != INVALID_HANDLE_VALUE, "FastPDF is being updated. Please try again in a moment");
    return handle;
}
static HANDLE exclusive_instance_lock(DWORD timeout) {
    const auto end = GetTickCount64() + timeout;
    const auto file = install_directory() / "running.lock";
    reject_reparse(file);
    for (;;) {
        HANDLE handle =
            CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (handle != INVALID_HANDLE_VALUE)
            return handle;
        DWORD error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION || GetTickCount64() >= end)
            throw std::runtime_error(
                "Close all FastPDF windows and try again. The installed reader is still in use.");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void require_no_other_instances() {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    win_check(snapshot.value != INVALID_HANDLE_VALUE, "Cannot inspect running FastPDF instances");
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL valid = Process32FirstW(snapshot.value, &entry); valid;
         valid = Process32NextW(snapshot.value, &entry)) {
        if (entry.th32ProcessID == GetCurrentProcessId())
            continue;
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
        if (!process.value)
            continue;
        std::wstring name(32768, L'\0');
        DWORD length = DWORD(name.size());
        if (QueryFullProcessImageNameW(process.value, 0, name.data(), &length)) {
            name.resize(length);
            check(!same_file(fs::path(name), installed_executable()),
                  "Close the other FastPDF windows first, then try again.");
        }
    }
}
static void registry_string(HKEY key, const wchar_t* name, const std::wstring& value) {
    check(RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                         DWORD((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS,
          "Cannot write the FastPDF registration");
}
static constexpr wchar_t pdf_progid[] = L"FastPDF.PDF";
static constexpr wchar_t capabilities_key[] = L"Software\\FastPDF\\Capabilities";
static constexpr wchar_t registered_apps_key[] = L"Software\\RegisteredApplications";
static constexpr wchar_t pdf_open_with_key[] = L"Software\\Classes\\.pdf\\OpenWithProgids";
static void registry_entry(const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    Registry key;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.value,
                          nullptr) == ERROR_SUCCESS,
          "Cannot register FastPDF as a PDF reader");
    registry_string(key.value, name, value);
}
static void associations_changed() {
#ifndef FASTPDF_DEPLOYMENT_TEST
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}
static void register_pdf_reader() {
    const auto executable = quote_argument(installed_executable().wstring());
    const auto command = executable + L" -- \"%1\"";
    registry_entry(L"Software\\Classes\\FastPDF.PDF", nullptr, L"PDF document");
    registry_entry(L"Software\\Classes\\FastPDF.PDF\\Application", L"ApplicationName", L"FastPDF");
    registry_entry(L"Software\\Classes\\FastPDF.PDF\\shell\\open\\command", nullptr, command);
    registry_entry(L"Software\\Classes\\Applications\\fastpdf.exe", L"FriendlyAppName", L"FastPDF");
    registry_entry(L"Software\\Classes\\Applications\\fastpdf.exe\\SupportedTypes", L".pdf", L"");
    registry_entry(L"Software\\Classes\\Applications\\fastpdf.exe\\shell\\open\\command", nullptr, command);
    registry_entry(capabilities_key, L"ApplicationName", L"FastPDF");
    registry_entry(capabilities_key, L"ApplicationDescription", L"FastPDF PDF reader");
    registry_entry(L"Software\\FastPDF\\Capabilities\\FileAssociations", L".pdf", pdf_progid);
    registry_entry(registered_apps_key, L"FastPDF", capabilities_key);
    Registry key;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, pdf_open_with_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                          &key.value, nullptr) == ERROR_SUCCESS &&
              RegSetValueExW(key.value, pdf_progid, 0, REG_NONE, nullptr, 0) == ERROR_SUCCESS,
          "Cannot add FastPDF to Open with for PDF files");
    associations_changed();
}
static bool registry_missing(LSTATUS status) {
    return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}
static void unregister_pdf_reader() {
    for (const auto path : {L"Software\\Classes\\FastPDF.PDF",
                            L"Software\\Classes\\Applications\\fastpdf.exe", capabilities_key}) {
        const auto status = RegDeleteTreeW(HKEY_CURRENT_USER, path);
        check(status == ERROR_SUCCESS || registry_missing(status),
              "Cannot remove the FastPDF PDF registration");
    }
    for (const auto& entry :
         {std::pair{registered_apps_key, L"FastPDF"}, std::pair{pdf_open_with_key, pdf_progid}}) {
        const auto status = RegDeleteKeyValueW(HKEY_CURRENT_USER, entry.first, entry.second);
        check(status == ERROR_SUCCESS || registry_missing(status),
              "Cannot remove the FastPDF PDF registration");
    }
    associations_changed();
}
bool default_pdf_reader() {
    wchar_t executable[32768]{};
    DWORD size = DWORD(std::size(executable));
    return AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".pdf", L"open", executable, &size) == S_OK &&
           same_file(executable, installed_executable());
}
void open_pdf_defaults(HWND owner) {
    // Windows owns the user's default choice. Register capabilities, then let Settings confirm it.
    check(installation_present(), "Install FastPDF before choosing it as the default PDF reader");
    auto result = ShellExecuteW(owner, L"open", L"ms-settings:defaultapps?registeredAppUser=FastPDF", nullptr,
                                nullptr, SW_SHOWNORMAL);
    if (INT_PTR(result) <= 32)
        result = ShellExecuteW(owner, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
    check(INT_PTR(result) > 32,
          "FastPDF is installed, but Windows Default apps could not be opened. Open Settings > Apps > "
          "Default apps and choose FastPDF for .pdf files.");
}
static void register_installation(std::string_view version) {
    Registry key;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, uninstall_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key.value,
                          nullptr) == ERROR_SUCCESS,
          "Cannot create the uninstall registration");
    const auto executable = installed_executable();
    registry_string(key.value, L"DisplayName", L"FastPDF");
    registry_string(key.value, L"Publisher", L"tuncb");
    registry_string(key.value, L"DisplayVersion", pdf::utf8(version));
    registry_string(key.value, L"DisplayIcon", executable.wstring());
    registry_string(key.value, L"InstallLocation", install_directory().wstring());
    registry_string(key.value, L"UninstallString", quote_argument(executable.wstring()) + L" --uninstall");
    DWORD value = 1;
    check(RegSetValueExW(key.value, L"NoModify", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                         sizeof(value)) == ERROR_SUCCESS &&
              RegSetValueExW(key.value, L"NoRepair", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                             sizeof(value)) == ERROR_SUCCESS,
          "Cannot finish the uninstall registration");
    register_pdf_reader();
}
bool shortcut_present() {
    return fs::is_regular_file(shortcut_path());
}
void set_shortcut(bool enabled) {
    const auto path = shortcut_path();
    if (!enabled) {
        std::error_code error;
        fs::remove(path, error);
        check(!error, "Cannot remove the FastPDF Start menu shortcut");
        return;
    }
    reject_reparse(path);
    IShellLinkW* link = nullptr;
    check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))),
          "Cannot create the Start menu shortcut");
    IPersistFile* file = nullptr;
    HRESULT result = link->SetPath(installed_executable().c_str());
    if (SUCCEEDED(result))
        result = link->SetWorkingDirectory(install_directory().c_str());
    if (SUCCEEDED(result))
        result = link->SetDescription(L"FastPDF PDF reader");
    if (SUCCEEDED(result))
        result = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(result))
        result = file->Save(path.c_str(), TRUE);
    if (file)
        file->Release();
    link->Release();
    check(SUCCEEDED(result), "Cannot save the Start menu shortcut");
}
void install(bool shortcut) {
    MaintenanceLock maintenance;
    ensure_directory(install_directory());
    reject_reparse(installed_executable());
    if (!installed_instance()) {
        Handle idle{exclusive_instance_lock(0)};
        // Existing installations are updated from the installed reader, never overwritten here.
        check(!installation_present(), "FastPDF is already installed. Open the installed copy to update it.");
        fs::copy_file(executable_path(), installed_executable());
        try {
            register_installation(FASTPDF_VERSION);
            set_shortcut(shortcut);
        } catch (...) {
            std::error_code error;
            fs::remove(installed_executable(), error);
            RegDeleteTreeW(HKEY_CURRENT_USER, uninstall_key);
            try {
                unregister_pdf_reader();
            } catch (...) {
                // Preserve the installation failure if registry cleanup also fails.
            }
            throw;
        }
    } else {
        register_installation(FASTPDF_VERSION);
        set_shortcut(shortcut);
    }
}
static pdf::Bytes request(std::wstring_view url, size_t maximum, const std::atomic<bool>& cancel) {
    check(!cancel, "Update canceled");
    URL_COMPONENTS parts{sizeof(parts)};
    parts.dwSchemeLength = parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        DWORD(-1);
    win_check(WinHttpCrackUrl(url.data(), DWORD(url.size()), 0, &parts) != 0, "Invalid update URL");
    check(parts.nScheme == INTERNET_SCHEME_HTTPS, "Updates require HTTPS");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet session{WinHttpOpen(L"FastPDF/" FASTPDF_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    win_check(session.value != nullptr, "Cannot initialize update networking");
    win_check(WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000) != 0, "Cannot set network timeouts");
    Internet connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    win_check(connection.value != nullptr, "Cannot connect to GitHub");
    Internet http{WinHttpOpenRequest(connection.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    win_check(http.value != nullptr, "Cannot create the update request");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    win_check(WinHttpSetOption(http.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) !=
                  0,
              "Cannot secure update redirects");
    const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    win_check(WinHttpSendRequest(http.value, headers, DWORD(-1), nullptr, 0, 0, 0) != 0 &&
                  WinHttpReceiveResponse(http.value, nullptr) != 0,
              "Cannot contact GitHub. Check your connection and try again");
    DWORD status = 0, size = sizeof(status);
    win_check(WinHttpQueryHeaders(http.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                  WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != 0,
              "Cannot read the GitHub response");
    check(status == 200, "GitHub could not serve the update (the service may be unavailable or rate "
                         "limited). Try again later.");
    pdf::Bytes bytes;
    std::array<uint8_t, 65536> buffer{};
    const ULONGLONG deadline = GetTickCount64() + 120000;
    for (;;) {
        check(!cancel, "Update canceled");
        check(GetTickCount64() < deadline, "Update download timed out");
        DWORD count = 0;
        win_check(WinHttpReadData(http.value, buffer.data(), DWORD(buffer.size()), &count) != 0,
                  "The update download was interrupted");
        if (!count)
            break;
        check(bytes.size() + count <= maximum, "Update response exceeds size limit");
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
    }
    check(!cancel, "Update canceled");
    return bytes;
}
Release check_release(const std::atomic<bool>& cancel) {
    return parse_release(pdf::string(
        request(L"https://api.github.com/repos/tuncb/fastpdf/releases/latest", 2 * 1024 * 1024, cancel)));
}
void verify_executable(const fs::path& executable, std::string_view checksum, std::string_view version) {
    const auto bytes = read_file(executable);
    check(sha256(bytes) == checksum, "The downloaded update does not match its SHA-256 checksum");
    check(bytes.size() >= 64 && bytes[0] == 'M' && bytes[1] == 'Z', "Update is not a Windows executable");
    uint32_t offset = 0;
    std::memcpy(&offset, bytes.data() + 60, 4);
    check(offset <= bytes.size() - 6 && std::memcmp(bytes.data() + offset, "PE\0\0\x64\x86", 6) == 0,
          "Update is not a Windows x64 executable");
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
    check(size > 0 && size <= 1024 * 1024, "Update has no valid version information");
    pdf::Bytes info(size);
    win_check(GetFileVersionInfoW(executable.c_str(), 0, size, info.data()) != 0,
              "Cannot read update version");
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    check(VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) != 0 &&
              length >= sizeof(VS_FIXEDFILEINFO) && fixed->dwSignature == 0xfeef04bd,
          "Invalid update version information");
    const auto expected = parse_version(version);
    check(HIWORD(fixed->dwFileVersionMS) == expected.major &&
              LOWORD(fixed->dwFileVersionMS) == expected.minor &&
              HIWORD(fixed->dwFileVersionLS) == expected.patch && LOWORD(fixed->dwFileVersionLS) == 0,
          "Downloaded executable version does not match the release");
    HMODULE module = LoadLibraryExW(executable.c_str(), nullptr,
                                    LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    win_check(module != nullptr, "Cannot inspect update resources");
    bool notices = FindResourceW(module, MAKEINTRESOURCEW(IDR_BUILD_METADATA), RT_RCDATA) &&
                   FindResourceW(module, MAKEINTRESOURCEW(IDR_FONT_LICENSE), RT_RCDATA) &&
                   FindResourceW(module, MAKEINTRESOURCEW(IDR_UNICODE_LICENSE), RT_RCDATA);
    FreeLibrary(module);
    check(notices, "Update is missing FastPDF resources");
}
static fs::path make_stage() {
    ensure_directory(cache_directory());
    GUID guid{};
    check(SUCCEEDED(CoCreateGuid(&guid)), "Cannot create an update identifier");
    wchar_t text[40]{};
    check(StringFromGUID2(guid, text, 40) > 0, "Cannot format the update identifier");
    fs::path directory = cache_directory() / text;
    check(fs::create_directory(directory), "Cannot create the update folder");
    return directory;
}
static void validate_stage(const fs::path& stage) {
    check(stage.is_absolute() && stage.lexically_normal().parent_path() == cache_directory() &&
              stage.filename().wstring().size() == 38 && stage.filename().wstring().front() == L'{',
          "Invalid maintenance folder");
    reject_reparse(cache_directory());
    reject_reparse(stage);
    for (const auto* name : {L"maintenance.exe", L"fastpdf.exe", L"owner.pid"})
        reject_reparse(stage / name);
}
void cleanup_download(const Download& download) {
    if (download.directory.empty())
        return;
    validate_stage(download.directory);
    std::error_code error;
    fs::remove(download.directory / "fastpdf.exe", error);
    if (error)
        return;
    fs::remove(download.directory / "maintenance.exe", error);
    // A helper cannot delete its own mapped image. Keep its PID marker so a later
    // reader launch can clean this exact directory after the helper has exited.
    if (error)
        return;
    fs::remove(download.directory / "owner.pid", error);
    fs::remove(download.directory, error);
}
static void mark_stage(const fs::path& stage, DWORD pid) {
    const auto text = std::to_string(pid);
    write_file(stage / "owner.pid", pdf::view(text));
}
Download download_release(const Release& release, const std::atomic<bool>& cancel) {
    check(release.version > parse_version(FASTPDF_VERSION),
          "The update must be newer than this FastPDF version");
    Download download{make_stage(), {}, version_string(release.version)};
    try {
        mark_stage(download.directory, GetCurrentProcessId());
        download.checksum =
            parse_checksum(pdf::string(request(pdf::utf8(release.checksum_url), 4096, cancel)));
        const auto bytes = request(pdf::utf8(release.executable_url), max_download, cancel);
        write_file(download.directory / "fastpdf.exe", bytes);
        verify_executable(download.directory / "fastpdf.exe", download.checksum, download.version);
        return download;
    } catch (...) {
        cleanup_download(download);
        throw;
    }
}
void replace_executable(const fs::path& executable, const fs::path& replacement) {
    const auto staging = fs::path(executable.wstring() + L".new");
    const auto backup = fs::path(executable.wstring() + L".previous");
    reject_reparse(executable);
    reject_reparse(staging);
    reject_reparse(backup);
    check(!fs::exists(backup),
          "A previous update needs recovery. Restart the installed reader before trying again.");
    fs::copy_file(replacement, staging, fs::copy_options::overwrite_existing);
    if (!ReplaceFileW(executable.c_str(), staging.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
        const DWORD error = GetLastError();
        std::error_code ignored;
        // ReplaceFile may have moved the original before failing; recover it if necessary.
        if (!fs::exists(executable) && fs::exists(backup))
            fs::rename(backup, executable, ignored);
        fs::remove(staging, ignored);
        throw std::runtime_error(
            "Cannot replace FastPDF. The previous version has been retained (Windows error " +
            std::to_string(error) + ").");
    }
}
static HANDLE launch(const fs::path& executable, const std::vector<std::wstring>& args, DWORD* pid = nullptr,
                     bool hidden = false) {
#ifdef FASTPDF_DEPLOYMENT_TEST
    hidden = true;
#endif
    std::wstring command = quote_argument(executable.wstring());
    for (const auto& arg : args)
        command += L" " + quote_argument(arg);
    check(command.size() < 32767, "The restart command is too long");
    STARTUPINFOW startup{sizeof(startup)};
    if (hidden) {
        startup.dwFlags = STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION process{};
    win_check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             executable.parent_path().c_str(), &startup, &process) != 0,
              "Cannot start FastPDF");
    CloseHandle(process.hThread);
    if (pid)
        *pid = process.dwProcessId;
    return process.hProcess;
}
void open_installed(const Session& session) {
    Handle process{launch(installed_executable(), session_arguments(session))};
}
static void start_helper(const fs::path& stage, std::vector<std::wstring> args) {
    validate_stage(stage);
    const auto helper = stage / "maintenance.exe";
    mark_stage(stage, GetCurrentProcessId());
    fs::copy_file(executable_path(), helper);
    DWORD pid = 0;
    Handle process{launch(helper, args, &pid, true)};
    mark_stage(stage, pid);
}
void start_update(const Download& download, const Session& session, bool restart) {
    check(installed_instance(), "Updates must be started from the installed FastPDF reader");
    require_no_other_instances();
    validate_stage(download.directory);
    verify_executable(download.directory / "fastpdf.exe", download.checksum, download.version);
    auto args = std::vector<std::wstring>{L"--finish-update", std::to_wstring(GetCurrentProcessId()),
                                          pdf::utf8(download.checksum), pdf::utf8(download.version),
                                          restart ? L"restart" : L"exit"};
    auto view = session_arguments(session);
    args.insert(args.end(), view.begin(), view.end());
    start_helper(download.directory, std::move(args));
}
void start_uninstall() {
    require_no_other_instances();
    const auto stage = make_stage();
    try {
        start_helper(stage, {L"--finish-uninstall", std::to_wstring(GetCurrentProcessId())});
    } catch (...) {
        cleanup_download({stage, {}, {}});
        throw;
    }
}
static DWORD parse_pid(std::wstring_view text) {
    check(!text.empty() && text.size() <= 10, "Invalid maintenance process ID");
    uint64_t result = 0;
    for (wchar_t c : text) {
        check(c >= L'0' && c <= L'9', "Invalid maintenance process ID");
        result = result * 10 + unsigned(c - L'0');
    }
    check(result > 0 && result <= MAXDWORD, "Invalid maintenance process ID");
    return DWORD(result);
}
static void wait_for_parent(DWORD pid) {
    Handle process{OpenProcess(SYNCHRONIZE, FALSE, pid)};
    if (!process.value && GetLastError() == ERROR_INVALID_PARAMETER)
        return;
    win_check(process.value != nullptr, "Cannot wait for the reader to close");
    check(WaitForSingleObject(process.value, 60000) == WAIT_OBJECT_0,
          "FastPDF did not close in time. The installation has not been changed.");
}
static void restore_backup() {
    const auto executable = installed_executable();
    const auto backup = fs::path(executable.wstring() + L".previous");
    win_check(MoveFileExW(backup.c_str(), executable.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0,
              "Cannot restore the previous FastPDF executable");
    register_installation(FASTPDF_VERSION);
}
void remove_installation() {
    MaintenanceLock maintenance;
    reject_reparse(install_directory());
    check(installation_present(), "FastPDF is not installed");
    {
        Handle idle{exclusive_instance_lock(0)};
        reject_reparse(installed_executable());
        set_shortcut(false);
        unregister_pdf_reader();
        fs::remove(installed_executable());
        const LSTATUS status = RegDeleteTreeW(HKEY_CURRENT_USER, uninstall_key);
        check(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND,
              "Cannot remove uninstall registration");
        std::error_code ignored;
        fs::remove(install_directory() / "fastpdf.exe.previous", ignored);
        fs::remove(install_directory() / "fastpdf.exe.new", ignored);
    }
    std::error_code ignored;
    fs::remove(install_directory() / "running.lock", ignored);
    fs::remove(install_directory(), ignored); // Remove only an empty app folder, never user files.
}
int maintenance_command(const std::vector<std::wstring>& arguments) {
    if (arguments.empty() || (arguments[0] != L"--finish-update" && arguments[0] != L"--finish-uninstall"))
        return -1;
    const bool update = arguments[0] == L"--finish-update";
    try {
        check(arguments.size() == (update ? 11u : 2u), "Invalid maintenance arguments");
        const auto stage = executable_path().parent_path();
        validate_stage(stage);
        check(executable_path().filename() == L"maintenance.exe", "Invalid maintenance executable");
        const DWORD parent = parse_pid(arguments[1]);
        check(parent != GetCurrentProcessId(), "Invalid maintenance parent");
        MaintenanceLock maintenance;
        reject_reparse(install_directory());
        check(installation_present(), "FastPDF is not installed");
        if (update) {
            check(arguments[4] == L"restart" || arguments[4] == L"exit", "Invalid restart mode");
            check(parse_version(pdf::utf8(arguments[3])) > parse_version(FASTPDF_VERSION),
                  "Update must be newer");
            verify_executable(stage / "fastpdf.exe", pdf::utf8(arguments[2]), pdf::utf8(arguments[3]));
            parse_session({arguments.begin() + 5, arguments.end()});
        }
        wait_for_parent(parent);
        if (update) {
            Handle idle{exclusive_instance_lock(60000)};
            replace_executable(installed_executable(), stage / "fastpdf.exe");
            try {
                register_installation(pdf::utf8(arguments[3]));
            } catch (...) {
                restore_backup();
                throw;
            }
        } else
            remove_installation();
        if (update && arguments[4] == L"restart") {
            const std::wstring event_name = L"Local\\FastPDF.Ready." + stage.filename().wstring();
            Handle ready{CreateEventW(nullptr, TRUE, FALSE, event_name.c_str())};
            win_check(ready.value != nullptr, "Cannot create restart confirmation");
            std::vector<std::wstring> args(arguments.begin() + 5, arguments.end());
            args.push_back(L"--update-ready");
            args.push_back(event_name);
            try {
                Handle process{launch(installed_executable(), args)};
                HANDLE waits[] = {ready.value, process.value};
                const DWORD status = WaitForMultipleObjects(2, waits, FALSE, 20000);
                if (status != WAIT_OBJECT_0) {
                    if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0) {
                        Handle idle{exclusive_instance_lock(0)};
                        restore_backup();
                    }
                    throw std::runtime_error("The updated reader did not confirm startup. The previous "
                                             "version has been retained.");
                }
            } catch (...) {
                const auto backup = install_directory() / "fastpdf.exe.previous";
                if (fs::exists(backup)) {
                    try {
                        Handle idle{exclusive_instance_lock(0)};
                        restore_backup();
                    } catch (...) {
                    }
                }
                throw;
            }
        }
        cleanup_download({stage, {}, {}});
        return 0;
    } catch (const std::exception& error) {
#ifdef FASTPDF_DEPLOYMENT_TEST
        throw std::runtime_error(error.what());
#else
        ui::message_box(nullptr, pdf::utf8(error.what()).c_str(), L"FastPDF Maintenance", MB_OK | MB_ICONERROR);
        return 1;
#endif
    }
}
void startup_succeeded(const std::vector<std::wstring>& arguments) {
    if (!installed_instance())
        return;
    std::error_code ignored;
    fs::remove(install_directory() / "fastpdf.exe.previous", ignored);
    for (size_t i = 0; i + 1 < arguments.size(); ++i) {
        if (arguments[i] == L"--update-ready" && arguments[i + 1].starts_with(L"Local\\FastPDF.Ready.")) {
            Handle ready{OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[i + 1].c_str())};
            if (ready.value)
                SetEvent(ready.value);
        }
    }
}
void cleanup_stale_helpers() {
    try {
        const auto cache = cache_directory();
        reject_reparse(cache);
        if (!fs::is_directory(cache))
            return;
        for (const auto& entry : fs::directory_iterator(cache)) {
            if (!entry.is_directory())
                continue;
            try {
                validate_stage(entry.path());
                auto bytes = read_file(entry.path() / "owner.pid", 32);
                const auto pid = parse_pid(pdf::utf8(pdf::string(bytes)));
                Handle owner{OpenProcess(SYNCHRONIZE, FALSE, pid)};
                if (owner.value && WaitForSingleObject(owner.value, 0) != WAIT_OBJECT_0)
                    continue;
                if (!owner.value && GetLastError() != ERROR_INVALID_PARAMETER)
                    continue;
                cleanup_download({entry.path(), {}, {}});
            } catch (...) { /* Leave unrecognized or busy files alone. */
            }
        }
    } catch (...) { /* Cache cleanup must not prevent reading a document. */
    }
}
} // namespace deployment
