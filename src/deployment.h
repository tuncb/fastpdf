#pragma once
#include <windows.h>
#include <atomic>
#include <compare>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace deployment {
namespace fs = std::filesystem;
#ifdef FASTPDF_DEPLOYMENT_TEST
// Compiled only into the test executable; production paths cannot be overridden.
void set_test_paths(const fs::path& root, const fs::path& current_executable);
#endif
struct Version {
    unsigned major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};
struct Release {
    Version version;
    std::string tag, executable_url, checksum_url;
};
struct Download {
    fs::path directory;
    std::string checksum, version;
};
struct Session {
    std::wstring document;
    int page = 0, rotation = 0, fit = 0;
    double zoom = 1;
};
Version parse_version(std::string_view text);
std::string version_string(Version version);
Release parse_release(std::string_view json);
std::string parse_checksum(std::string_view text);
std::wstring quote_argument(std::wstring_view argument);
std::vector<std::wstring> session_arguments(const Session& session);
Session parse_session(const std::vector<std::wstring>& arguments);
fs::path executable_path();
fs::path install_directory();
bool installed_instance();
bool installation_present();
void install(bool shortcut);
bool default_pdf_reader();
void open_pdf_defaults(HWND owner);
void open_installed(const Session& session);
void set_shortcut(bool enabled);
bool shortcut_present();
// Return a shared lifetime lock. Maintenance takes an exclusive lock before replacement.
HANDLE acquire_instance_lock();
void require_no_other_instances();
Release check_release(const std::atomic<bool>& cancel);
Download download_release(const Release& release, const std::atomic<bool>& cancel);
void verify_executable(const fs::path& executable, std::string_view checksum, std::string_view version);
void replace_executable(const fs::path& executable, const fs::path& replacement);
void start_update(const Download& download, const Session& session, bool restart);
void start_uninstall();
// Used by maintenance after the reader exits; refuses to remove a busy installation.
void remove_installation();
// Handle internal maintenance commands before opening the reader. -1 means normal startup.
int maintenance_command(const std::vector<std::wstring>& arguments);
void startup_succeeded(const std::vector<std::wstring>& arguments);
void cleanup_download(const Download& download);
void cleanup_stale_helpers();
} // namespace deployment
