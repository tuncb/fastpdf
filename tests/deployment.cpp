#include "deployment.h"
#include "pdf.h"
#include "installation_dialog.h"
#include "resource.h"
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <functional>
#include <system_error>
#include <thread>

using namespace deployment;
static int assertions = 0;
static void test(bool value, const char* message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
}
static void test_windows(bool value, const char* message) {
    const DWORD error = value ? ERROR_SUCCESS : GetLastError();
    ++assertions;
    if (!value)
        throw std::runtime_error(std::string(message) + " (Windows error " + std::to_string(error) +
                                 ": " + std::system_category().message(int(error)) + ")");
}
static void rejects(const std::function<void()>& action, const char* message) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    test(rejected, message);
}
static void put(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    if (!output)
        throw std::runtime_error("Cannot write test fixture");
}
static std::string get(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read test fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
static std::string hash_file(const fs::path& path) {
    const auto bytes = get(path);
    std::string result;
    const auto hash = pdf::digest(BCRYPT_SHA256_ALGORITHM, pdf::view(bytes));
    for (auto byte : hash) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
struct RegistrySandbox {
    HKEY key = nullptr;
    std::wstring path = L"Software\\FastPDF.DeploymentTest." + std::to_wstring(GetCurrentProcessId());
    RegistrySandbox() {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key,
                            nullptr) != ERROR_SUCCESS ||
            RegOverridePredefKey(HKEY_CURRENT_USER, key) != ERROR_SUCCESS)
            throw std::runtime_error("Cannot create private test registry");
    }
    ~RegistrySandbox() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (key)
            RegCloseKey(key);
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};
static void registry_fixture(const wchar_t* path, const wchar_t* name, const wchar_t* value) {
    HKEY key = nullptr;
    test(RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
             ERROR_SUCCESS,
         "Create isolated registry fixture");
    const auto status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                                       DWORD((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    test(status == ERROR_SUCCESS, "Write isolated registry fixture");
}
static std::wstring registry_value(const wchar_t* path, const wchar_t* name = nullptr) {
    wchar_t value[32768]{};
    DWORD size = sizeof(value);
    test(RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS,
         "Read isolated registry value");
    return value;
}
static bool registry_key_present(const wchar_t* path) {
    HKEY key = nullptr;
    const auto status = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &key);
    if (key)
        RegCloseKey(key);
    test(status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND,
         "Inspect isolated registry key");
    return status == ERROR_SUCCESS;
}
static std::function<void(HWND)> dialog_action;
static std::exception_ptr dialog_error;
static LRESULT CALLBACK installation_hook(int code, WPARAM wp, LPARAM lp) {
    const auto window = HWND(wp);
    if (code == HCBT_ACTIVATE && dialog_action && GetDlgItem(window, IDC_INSTALL_FOLDER)) {
        auto action = std::move(dialog_action);
        dialog_action = {};
        try {
            action(window);
        } catch (...) {
            dialog_error = std::current_exception();
            SendMessageW(window, WM_COMMAND, IDCANCEL, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static ui::InstallationAction installation_fixture(ui::InstallationOptions& options,
                                                   std::function<void(HWND)> action) {
    dialog_action = std::move(action);
    dialog_error = nullptr;
    const auto hook = SetWindowsHookExW(WH_CBT, installation_hook, nullptr, GetCurrentThreadId());
    test(hook != nullptr, "Automate the installation dialog without desktop input");
    const auto result = ui::show_installation_dialog(GetModuleHandleW(nullptr), nullptr, options);
    UnhookWindowsHookEx(hook);
    test(!dialog_action, "Installation dialog was exercised");
    if (dialog_error)
        std::rethrow_exception(dialog_error);
    return result;
}
static void installation_dialog_tests() {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    test(InitCommonControlsEx(&controls) != FALSE, "Initialize installation controls");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ui::InstallationOptions options{L"C:\\isolated user 文書\\LocalAppData\\Programs\\FastPDF"};
    const auto install = installation_fixture(options, [&](HWND window) {
        test(IsDlgButtonChecked(window, IDC_INSTALL_SHORTCUT) == BST_CHECKED,
             "New installations default to a Start menu shortcut");
        test(IsDlgButtonChecked(window, IDC_INSTALL_PDF) == BST_UNCHECKED, "PDF default choice is opt-in");
        test(!(GetWindowLongPtrW(GetDlgItem(window, IDC_INSTALL_UNINSTALL), GWL_STYLE) & WS_VISIBLE),
             "Fresh installation hides uninstall");
        wchar_t path[256]{};
        GetDlgItemTextW(window, IDC_INSTALL_FOLDER, path, int(std::size(path)));
        test(path == options.folder, "Installation folder preserves Unicode and spaces");
        SendDlgItemMessageW(window, IDC_INSTALL_SHORTCUT, BM_CLICK, 0, 0);
        SendDlgItemMessageW(window, IDC_INSTALL_PDF, BM_CLICK, 0, 0);
        SendMessageW(window, WM_COMMAND, IDOK, 0);
    });
    test(install == ui::InstallationAction::apply && !options.shortcut && options.default_pdf,
         "Install submits both independent checkbox choices");
    const auto cancel = installation_fixture(options, [](HWND window) {
        SendDlgItemMessageW(window, IDC_INSTALL_PDF, BM_CLICK, 0, 0);
        SendMessageW(window, WM_CLOSE, 0, 0);
    });
    test(cancel == ui::InstallationAction::cancel && options.default_pdf,
         "Cancel discards changed installation choices");
    options.installed = options.present = true;
    const auto apply = installation_fixture(options, [](HWND window) {
        wchar_t caption[64]{};
        GetDlgItemTextW(window, IDOK, caption, int(std::size(caption)));
        test(std::wstring_view(caption) == L"Apply", "Installed reader offers Apply");
        test(GetWindowLongPtrW(GetDlgItem(window, IDC_INSTALL_UNINSTALL), GWL_STYLE) & WS_VISIBLE,
             "Installed reader offers Uninstall");
        SendMessageW(window, WM_COMMAND, IDOK, 0);
    });
    test(apply == ui::InstallationAction::apply, "Installed options can be applied");
    options.installed = false;
    const auto open = installation_fixture(options, [](HWND window) {
        for (const int id : {IDC_INSTALL_SHORTCUT, IDC_INSTALL_PDF})
            test(!(GetWindowLongPtrW(GetDlgItem(window, id), GWL_STYLE) & WS_VISIBLE),
                 "Portable copy cannot edit an existing installation");
        SendMessageW(window, WM_COMMAND, IDOK, 0);
    });
    test(open == ui::InstallationAction::open, "Portable copy opens the installed reader");
    const auto uninstall = installation_fixture(
        options, [](HWND window) { SendMessageW(window, WM_COMMAND, IDC_INSTALL_UNINSTALL, 0); });
    test(uninstall == ui::InstallationAction::uninstall, "Existing installation can request uninstall");
}
static std::wstring environment(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    DWORD size = GetEnvironmentVariableW(name, value.data(), DWORD(value.size()));
    if (size == 0 || size >= value.size())
        return {};
    value.resize(size);
    return value;
}
static void future_version(const fs::path& candidate, Version version) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(candidate.c_str(), &ignored);
    test_windows(size != 0, "Read future version fixture size");
    std::vector<uint8_t> info(size);
    test_windows(GetFileVersionInfoW(candidate.c_str(), 0, size, info.data()) != 0,
                 "Read future version fixture");
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    test_windows(VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) != 0,
                 "Read fixed version fixture");
    fixed->dwFileVersionMS = MAKELONG(WORD(version.minor), WORD(version.major));
    fixed->dwFileVersionLS = MAKELONG(0, WORD(version.patch));
    fixed->dwProductVersionMS = fixed->dwFileVersionMS;
    fixed->dwProductVersionLS = fixed->dwFileVersionLS;
    const auto metadata = std::string("{\"version\":\"") + version_string(version) +
                          "\",\"platform\":\"windows-x64\",\"commit\":\"" + std::string(40, 'a') + "\"}";
    // A newly inspected executable can be briefly locked. Retry the entire resource
    // transaction with a fresh handle and the same data, for at most five seconds.
    const auto deadline = GetTickCount64() + 5000;
    for (;;) {
        const char* operation = "Open resource fixture for editing";
        HANDLE resources = BeginUpdateResourceW(candidate.c_str(), FALSE);
        DWORD error = ERROR_SUCCESS;
        if (!resources) {
            error = GetLastError();
        } else {
            operation = "Write future native version";
            if (!UpdateResourceW(resources, RT_VERSION, MAKEINTRESOURCEW(1), 1033, info.data(),
                                 DWORD(info.size()))) {
                error = GetLastError();
            } else {
                operation = "Write future build metadata";
                if (!UpdateResourceW(resources, RT_RCDATA, MAKEINTRESOURCEW(201), 1033,
                                     const_cast<char*>(metadata.data()), DWORD(metadata.size())))
                    error = GetLastError();
            }
            if (error != ERROR_SUCCESS) {
                EndUpdateResourceW(resources, TRUE);
            } else {
                operation = "Finish future executable fixture";
                if (EndUpdateResourceW(resources, FALSE))
                    return;
                error = GetLastError();
            }
        }
        // The resource writer can report an existing file lock as ERROR_OPEN_FAILED.
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_OPEN_FAILED &&
             error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) ||
            GetTickCount64() >= deadline) {
            SetLastError(error);
            test_windows(false, operation);
        }
        Sleep(100);
    }
}
static void future_version_tests(const fs::path& reader, const fs::path& scratch) {
    const auto candidate = scratch / "future-version-retry.exe";
    fs::copy_file(reader, candidate);
    Version future = parse_version(FASTPDF_VERSION);
    ++future.minor;
    HANDLE locked = CreateFileW(candidate.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    test_windows(locked != INVALID_HANDLE_VALUE, "Lock future version fixture");
    {
        std::jthread release_lock([locked] {
            Sleep(300);
            CloseHandle(locked);
        });
        future_version(candidate, future);
    }
    verify_executable(candidate, hash_file(candidate), version_string(future));
    fs::remove(candidate);

    const auto blocked = scratch / "future-version-blocked.exe";
    fs::copy_file(reader, blocked);
    locked = CreateFileW(blocked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    test_windows(locked != INVALID_HANDLE_VALUE, "Keep future version fixture locked");
    std::string failure;
    const auto started = GetTickCount64();
    try {
        future_version(blocked, future);
    } catch (const std::exception& error) {
        failure = error.what();
    }
    CloseHandle(locked);
    test(GetTickCount64() - started >= 5000 && GetTickCount64() - started < 10000,
         "Persistent resource lock exhausts the bounded retry");
    test(failure.find("Windows error ") != std::string::npos,
         "Resource failure reports the Windows error");
    verify_executable(blocked, hash_file(blocked), FASTPDF_VERSION);
    fs::remove(blocked);
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        // A restarted test reader uses a private folder/hive inherited only from this test process.
        // This code is in the test executable, never in the released reader.
        if (argc >= 7 && std::wstring_view(argv[1]) == L"--restore-view") {
            const auto root = environment(L"FASTPDF_TEST_ROOT");
            const auto hive = environment(L"FASTPDF_TEST_HIVE");
            test(!root.empty() && !hive.empty(), "Restart fixture requires isolation");
            if (environment(L"FASTPDF_TEST_FAIL_STARTUP") == L"1") {
                CoUninitialize();
                return 42;
            }
            const auto current = executable_path();
            set_test_paths(root, current);
            HKEY key = nullptr;
            test(RegOpenKeyExW(HKEY_CURRENT_USER, hive.c_str(), 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS,
                 "Open inherited test hive");
            test(RegOverridePredefKey(HKEY_CURRENT_USER, key) == ERROR_SUCCESS, "Isolate restarted reader");
            std::vector<std::wstring> arguments(argv + 1, argv + argc);
            const auto restored = parse_session({arguments.begin(), arguments.begin() + 6});
            HANDLE instance = acquire_instance_lock();
            put(fs::path(root) / "restored.txt", pdf::utf8(restored.document) + "\n" +
                                                     std::to_string(restored.page) + "\n" +
                                                     std::to_string(restored.rotation));
            startup_succeeded(arguments);
            if (instance)
                CloseHandle(instance);
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            RegCloseKey(key);
            CoUninitialize();
            return 0;
        }
        test(argc == 3, "Expected reader and scratch directory");
        const fs::path reader = fs::absolute(argv[1]);
        const fs::path scratch = fs::absolute(argv[2]);
        fs::create_directories(scratch);
        installation_dialog_tests();
        future_version_tests(reader, scratch);
        test(parse_version("v1.20.3") > parse_version("1.9.99"), "Numeric version ordering");
        test(parse_version("0.2.0") == Version{0, 2, 0}, "Parse current version");
        for (auto text : {"", "1.2", "1.2.3.4", "01.2.3", "1.2.3-beta", "-1.2.3", "65536.0.0", "v.2.3"})
            rejects([&] { parse_version(text); }, "Reject malformed version");
        const std::string json =
            R"({"body":"Notes: \uD83D\uDE00, escaped \"tag_name\": \"v99.0.0\"", "tag_name":"v1.2.3", "draft":false, "prerelease":false, "assets":[{"name":"fastpdf.exe","browser_download_url":"https://github.com/tuncb/fastpdf/releases/download/v1.2.3/fastpdf.exe"},{"name":"SHA256SUMS.txt","browser_download_url":"https://github.com/tuncb/fastpdf/releases/download/v1.2.3/SHA256SUMS.txt"}]})";
        test(parse_release(json).version == Version{1, 2, 3},
             "Parse real JSON including escaped release notes");
        auto replace = [&](std::string_view from, std::string_view to) {
            auto changed = json;
            const auto position = changed.find(from);
            test(position != std::string::npos, "Fixture replacement exists");
            changed.replace(position, from.size(), to);
            return changed;
        };
        for (auto text :
             {replace("\"draft\":false", "\"draft\":true"),
              replace("\"prerelease\":false", "\"prerelease\":true"),
              replace("\"draft\":false", "\"draft\":false,\"draft\":false"),
              replace("https://github.com", "http://github.com"), replace("tuncb/fastpdf", "other/fastpdf"),
              replace("SHA256SUMS.txt", "other.txt"), replace("\\uD83D\\uDE00", "\\uD83Dbroken"),
              replace("\"tag_name\":\"v1.2.3\"", "\"tag_name\":\"1.2.3\""), json + "garbage",
              json.substr(0, json.size() - 1)})
            rejects([&] { parse_release(text); }, "Reject malformed or untrusted release data");
        const std::string digest(64, 'a');
        std::atomic<bool> canceled = true;
        rejects([&] { check_release(canceled); }, "Canceled check never makes a network request");
        test(parse_checksum(digest + "  fastpdf.exe\r\n") == digest, "Parse Windows checksum file");
        for (auto text : {digest + "  other.exe", digest + "  fastpdf.exe\n" + digest + "  fastpdf.exe",
                          std::string(64, 'g') + "  fastpdf.exe", digest.substr(1) + "  fastpdf.exe"})
            rejects([&] { parse_checksum(text); }, "Checksum must name exactly the expected asset");
        for (std::wstring argument : {L"", L"C:\\folder with spaces\\café.pdf", L"C:\\ends in slash\\",
                                      L"a\\\"quoted\\\"argument", L"--leading-dashes"}) {
            const auto command = L"fastpdf.exe " + quote_argument(argument);
            int count = 0;
            auto values = CommandLineToArgvW(command.c_str(), &count);
            test(values && count == 2 && values[1] == argument, "Windows command-line roundtrip");
            LocalFree(values);
        }
        const Session session{L"C:\\documents with spaces\\文書.pdf", 17, 270, 2, 1.25};
        const auto restored = parse_session(session_arguments(session));
        test(restored.document == session.document && restored.page == 17 && restored.rotation == 270 &&
                 restored.fit == 2 && restored.zoom == 1.25,
             "Restore document and view without passwords");
        for (auto bad : {L"nan", L"inf", L"-1", L"100000000000", L"1.5", L"12junk"}) {
            auto args = session_arguments(session);
            args[1] = bad;
            rejects([&] { parse_session(args); }, "Reject invalid restored page");
        }
        verify_executable(reader, hash_file(reader), FASTPDF_VERSION);
        rejects([&] { verify_executable(reader, std::string(64, '0'), FASTPDF_VERSION); },
                "Reject mismatched checksum");
        rejects([&] { verify_executable(reader, hash_file(reader), "9.9.9"); },
                "Reject mismatched embedded version");
        auto corrupt = get(reader);
        uint32_t offset = 0;
        std::memcpy(&offset, corrupt.data() + 60, 4);
        corrupt[offset + 4] = '\x4c';
        corrupt[offset + 5] = '\x01';
        const auto invalid = scratch / "invalid.exe";
        put(invalid, corrupt);
        rejects([&] { verify_executable(invalid, hash_file(invalid), FASTPDF_VERSION); },
                "Reject x86 executable with valid checksum");
        fs::remove(invalid);
        const auto target = scratch / "replacement.exe", source = scratch / "candidate.exe";
        const auto backup = fs::path(target.wstring() + L".previous");
        put(target, "original");
        put(source, "new version");
        HANDLE locked =
            CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        test(locked != INVALID_HANDLE_VALUE, "Create locked target fixture");
        rejects([&] { replace_executable(target, source); }, "Locked executable must not be replaced");
        CloseHandle(locked);
        test(get(target) == "original", "Failed replacement preserves the installed version");
        replace_executable(target, source);
        test(get(target) == "new version" && get(backup) == "original",
             "Successful replacement retains recovery backup");
        rejects([&] { replace_executable(target, source); }, "Unresolved backup cannot be overwritten");
        fs::remove(target);
        fs::rename(backup, target);
        rejects([&] { replace_executable(target, scratch / "missing.exe"); },
                "Missing download cannot damage installation");
        test(get(target) == "original", "Source failure preserves the original");
        fs::remove(target);
        fs::remove(source);
        {
            RegistrySandbox registry;
            const auto sandbox = scratch / L"user with spaces 文書";
            set_test_paths(sandbox, reader);
            const auto stage = sandbox / "LocalAppData" / "FastPDF" / "Maintenance" /
                               "{00000000-0000-0000-0000-000000000000}";
            fs::create_directories(stage);
            put(stage / "maintenance.exe", "temporary helper");
            put(stage / "owner.pid", "1234");
            put(stage / "fastpdf.exe", "temporary download");
            HANDLE helper = CreateFileW((stage / "maintenance.exe").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            test(helper != INVALID_HANDLE_VALUE, "Lock a temporary helper fixture");
            cleanup_download({stage, {}, {}});
            test(fs::exists(stage / "owner.pid") && !fs::exists(stage / "fastpdf.exe"),
                 "Busy helper keeps its cleanup marker");
            CloseHandle(helper);
            cleanup_download({stage, {}, {}});
            test(!fs::exists(stage), "Exited helper and marker are removed together");
            rejects([&] { cleanup_download({scratch, {}, {}}); }, "Cleanup refuses unrelated directories");
            fs::remove(stage.parent_path());
            fs::remove(stage.parent_path().parent_path());
            test(!installation_present(), "Start with no installation");
            registry_fixture(L"Software\\Classes\\.pdf", nullptr, L"OtherReader.PDF");
            registry_fixture(L"Software\\Classes\\.pdf\\OpenWithProgids", L"OtherReader.PDF", L"");
            registry_fixture(L"Software\\RegisteredApplications", L"OtherReader", L"Software\\OtherReader");
            constexpr wchar_t choice[] =
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice";
            registry_fixture(choice, L"ProgId", L"OtherReader.PDF");
            registry_fixture(choice, L"Hash", L"untouched-user-choice");
            install(true);
            test(installation_present() && shortcut_present(), "Install reader and Start menu shortcut");
            test(registry_value(L"Software\\FastPDF\\Capabilities\\FileAssociations", L".pdf") ==
                         L"FastPDF.PDF" &&
                     registry_value(L"Software\\RegisteredApplications", L"FastPDF") ==
                         L"Software\\FastPDF\\Capabilities",
                 "Register per-user PDF capabilities for Windows Default apps");
            const auto pdf_command = registry_value(L"Software\\Classes\\FastPDF.PDF\\shell\\open\\command");
            test(pdf_command ==
                     quote_argument((install_directory() / "fastpdf.exe").wstring()) + L" -- \"%1\"",
                 "PDF opening command quotes the installed path and document placeholder");
            test(registry_value(L"Software\\Classes\\Applications\\fastpdf.exe\\shell\\open\\command") ==
                     pdf_command,
                 "Open with launches the installed reader");
            DWORD type = 0, bytes = 0;
            test(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf\\OpenWithProgids", L"FastPDF.PDF",
                              RRF_RT_REG_NONE, &type, nullptr, &bytes) == ERROR_SUCCESS &&
                     type == REG_NONE && bytes == 0,
                 "List FastPDF as an alternate PDF handler");
            test(registry_value(L"Software\\Classes\\.pdf") == L"OtherReader.PDF" &&
                     registry_value(choice, L"ProgId") == L"OtherReader.PDF" &&
                     registry_value(choice, L"Hash") == L"untouched-user-choice",
                 "Installation preserves the existing PDF default and protected user choice");
            HKEY uninstall = nullptr;
            test(RegOpenKeyExW(HKEY_CURRENT_USER,
                               L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FastPDF", 0,
                               KEY_QUERY_VALUE, &uninstall) == ERROR_SUCCESS,
                 "Register uninstall in private user hive");
            wchar_t command[32768]{};
            DWORD size = sizeof(command);
            test(RegQueryValueExW(uninstall, L"UninstallString", nullptr, nullptr,
                                  reinterpret_cast<BYTE*>(command), &size) == ERROR_SUCCESS,
                 "Uninstall command registered");
            test(std::wstring(command) ==
                     quote_argument((install_directory() / "fastpdf.exe").wstring()) + L" --uninstall",
                 "Uninstall path correctly quoted");
            RegCloseKey(uninstall);
            rejects([&] { install(true); }, "Portable installer cannot overwrite an existing installation");
            set_test_paths(sandbox, install_directory() / "fastpdf.exe");
            test(installed_instance(), "Identify installed reader");
            HANDLE first = acquire_instance_lock(), second = acquire_instance_lock();
            test(first && second, "Multiple reader instances can share the installation");
            rejects([&] { remove_installation(); }, "Uninstall refuses a busy reader");
            test(installation_present() && shortcut_present(),
                 "Refused uninstall leaves the installation intact");
            const auto lock_path = install_directory() / "running.lock";
            HANDLE exclusive = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                           OPEN_EXISTING, 0, nullptr);
            test(exclusive == INVALID_HANDLE_VALUE, "Readers block replacement");
            CloseHandle(first);
            CloseHandle(second);
            exclusive = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
            test(exclusive != INVALID_HANDLE_VALUE, "Maintenance can lock an idle installation");
            rejects([&] { acquire_instance_lock(); }, "Reader cannot start during replacement");
            CloseHandle(exclusive);
            install(false);
            test(!shortcut_present() && installation_present(),
                 "Apply shortcut option without replacing executable");
            set_shortcut(true);
            test(shortcut_present(), "Recreate shortcut");
            put(install_directory() / "user-document.pdf", "user data");
            remove_installation();
            test(!installation_present() && !shortcut_present(),
                 "Uninstall removes application and shortcut");
            test(get(install_directory() / "user-document.pdf") == "user data",
                 "Uninstall preserves user files");
            test(!registry_key_present(L"Software\\Classes\\FastPDF.PDF") &&
                     !registry_key_present(L"Software\\Classes\\Applications\\fastpdf.exe") &&
                     !registry_key_present(L"Software\\FastPDF\\Capabilities"),
                 "Uninstall removes FastPDF's PDF handler and capabilities");
            test(RegGetValueW(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"FastPDF", RRF_RT_ANY,
                              nullptr, nullptr, &bytes) == ERROR_FILE_NOT_FOUND &&
                     RegGetValueW(HKEY_CURRENT_USER, L"Software\\Classes\\.pdf\\OpenWithProgids",
                                  L"FastPDF.PDF", RRF_RT_ANY, nullptr, nullptr,
                                  &bytes) == ERROR_FILE_NOT_FOUND,
                 "Uninstall removes only FastPDF's values from shared registration keys");
            test(registry_value(L"Software\\Classes\\.pdf") == L"OtherReader.PDF" &&
                     registry_value(choice, L"ProgId") == L"OtherReader.PDF" &&
                     registry_value(choice, L"Hash") == L"untouched-user-choice" &&
                     registry_value(L"Software\\RegisteredApplications", L"OtherReader") ==
                         L"Software\\OtherReader" &&
                     registry_value(L"Software\\Classes\\.pdf\\OpenWithProgids", L"OtherReader.PDF").empty(),
                 "Uninstall preserves other PDF readers and the user's default choice");
            test(RegOpenKeyExW(HKEY_CURRENT_USER,
                               L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FastPDF", 0,
                               KEY_QUERY_VALUE, &uninstall) == ERROR_FILE_NOT_FOUND,
                 "Uninstall removes its registration");
            // All filesystem mutations use this generated scratch subtree; no user installation is touched.
            fs::remove(install_directory() / "user-document.pdf");
            fs::remove(install_directory());
            fs::remove(sandbox / "LocalAppData" / "Programs");
            fs::remove(sandbox / "LocalAppData");
            fs::remove(sandbox / "Start Menu");
            fs::remove(sandbox);
            set_test_paths({}, {});
        }
        {
            RegistrySandbox registry;
            const auto sandbox = scratch / "helper-roundtrip";
            const auto test_reader = executable_path();
            set_test_paths(sandbox, test_reader);
            install(false);
            const auto installed = install_directory() / "fastpdf.exe";
            const auto stage = sandbox / "LocalAppData" / "FastPDF" / "Maintenance" /
                               "{00000000-0000-0000-0000-000000000001}";
            fs::create_directories(stage);
            fs::copy_file(test_reader, stage / "maintenance.exe");
            fs::copy_file(test_reader, stage / "fastpdf.exe");
            Version future = parse_version(FASTPDF_VERSION);
            ++future.minor;
            future_version(stage / "fastpdf.exe", future);
            const auto checksum = hash_file(stage / "fastpdf.exe");
            set_test_paths(sandbox, stage / "maintenance.exe");
            test(SetEnvironmentVariableW(L"FASTPDF_TEST_ROOT", sandbox.c_str()) != 0 &&
                     SetEnvironmentVariableW(L"FASTPDF_TEST_HIVE", registry.path.c_str()) != 0,
                 "Set restart fixture isolation");
            std::vector<std::wstring> arguments{L"--finish-update", L"4294967295", pdf::utf8(checksum),
                                                pdf::utf8(version_string(future)), L"restart"};
            auto resume = session_arguments(session);
            arguments.insert(arguments.end(), resume.begin(), resume.end());
            test(SetEnvironmentVariableW(L"FASTPDF_TEST_FAIL_STARTUP", L"1") != 0,
                 "Simulate a failed new reader startup");
            rejects([&] { maintenance_command(arguments); }, "Failed new reader startup is reported");
            test(hash_file(installed) == hash_file(test_reader),
                 "Failed restart restores the original executable");
            test(!fs::exists(install_directory() / "fastpdf.exe.previous"), "Rollback consumes the backup");
            test(SetEnvironmentVariableW(L"FASTPDF_TEST_FAIL_STARTUP", nullptr) != 0,
                 "Clear simulated startup failure");
            test(maintenance_command(arguments) == 0,
                 "Helper verifies, replaces, registers, and restarts the reader");
            test(get(sandbox / "restored.txt") == pdf::utf8(session.document) + "\n17\n270",
                 "Helper restart preserves the document and page");
            test(!fs::exists(install_directory() / "fastpdf.exe.previous"),
                 "Confirmed startup releases the recovery backup");
            verify_executable(installed, checksum, version_string(future));
            // The restarted fixture has signaled readiness; allow it to finish releasing its instance lock.
            bool removed = false;
            for (int attempt = 0; attempt < 20 && !removed; ++attempt) {
                try {
                    remove_installation();
                    removed = true;
                } catch (...) {
                    Sleep(50);
                }
            }
            test(removed, "Uninstall the isolated updated reader");
            test(SetEnvironmentVariableW(L"FASTPDF_TEST_ROOT", nullptr) != 0 &&
                     SetEnvironmentVariableW(L"FASTPDF_TEST_HIVE", nullptr) != 0,
                 "Clear restart isolation");
            fs::remove(sandbox / "restored.txt");
            fs::remove(stage.parent_path());
            fs::remove(stage.parent_path().parent_path());
            fs::remove(sandbox / "LocalAppData" / "Programs");
            fs::remove(sandbox / "LocalAppData");
            fs::remove(sandbox / "Start Menu");
            fs::remove(sandbox);
            set_test_paths({}, {});
        }
        std::cout << assertions << " deployment assertions passed\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
