#include "deployment.h"
#include "pdf.h"
#include <bcrypt.h>
#include <shellapi.h>
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <functional>

using namespace deployment;
static int assertions = 0;
static void test(bool value, const char* message) {
    ++assertions;
    if (!value)
        throw std::runtime_error(message);
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
    std::vector<uint8_t> info(size);
    test(size && GetFileVersionInfoW(candidate.c_str(), 0, size, info.data()), "Read future version fixture");
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    test(VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) != 0,
         "Read fixed version fixture");
    fixed->dwFileVersionMS = MAKELONG(WORD(version.minor), WORD(version.major));
    fixed->dwFileVersionLS = MAKELONG(0, WORD(version.patch));
    fixed->dwProductVersionMS = fixed->dwFileVersionMS;
    fixed->dwProductVersionLS = fixed->dwFileVersionLS;
    HANDLE resources = BeginUpdateResourceW(candidate.c_str(), FALSE);
    test(resources != nullptr, "Open resource fixture for editing");
    test(UpdateResourceW(resources, RT_VERSION, MAKEINTRESOURCEW(1), 1033, info.data(), DWORD(info.size())) !=
             0,
         "Write future native version");
    const auto metadata = std::string("{\"version\":\"") + version_string(version) +
                          "\",\"platform\":\"windows-x64\",\"commit\":\"" + std::string(40, 'a') + "\"}";
    test(UpdateResourceW(resources, RT_RCDATA, MAKEINTRESOURCEW(201), 1033,
                         const_cast<char*>(metadata.data()), DWORD(metadata.size())) != 0,
         "Write future build metadata");
    test(EndUpdateResourceW(resources, FALSE) != 0, "Finish future executable fixture");
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
        test(parse_version("v1.20.3") > parse_version("1.9.99"), "Numeric version ordering");
        test(parse_version("0.3.0") == Version{0, 3, 0}, "Parse current version");
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
            install(true);
            test(installation_present() && shortcut_present(), "Install reader and Start menu shortcut");
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
