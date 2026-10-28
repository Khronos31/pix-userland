/* SPDX-License-Identifier: MIT */
#include <cstdint>
#include "pix-runtime-vectors.h"
#include "pix_runtime.h"
#include <dtv/userland/host_listener_context/enclosure_context.h>
#include "px4/posix_tuner_nonce.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

extern "C" {
void pix_test_fail_thread_create(void);
unsigned pix_test_release_count(void);
unsigned pix_test_close_count(void);
unsigned pix_test_shutdown_count(void);
}
#include <unistd.h>

namespace {

#ifndef PIXCTL_PATH
#error "PIXCTL_PATH must name the built pixctl client"
#endif
#ifndef PIX_RUNTIME_VERSION
#error "PIX_RUNTIME_VERSION must be provided by CMake"
#endif

bool write_file(const std::string& path, const std::uint8_t* bytes, std::size_t length)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return false;
    std::size_t offset = 0U;
    while (offset < length) {
        const ssize_t count = ::write(fd, bytes + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { (void)::close(fd); return false; }
        offset += static_cast<std::size_t>(count);
    }
    return ::close(fd) == 0;
}

bool write_zero_file(const std::string& path, std::size_t length)
{
    std::array<std::uint8_t, 4096U> zeros{};
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return false;
    std::size_t left = length;
    while (left != 0U) {
        const std::size_t amount = left < zeros.size() ? left : zeros.size();
        const ssize_t count = ::write(fd, zeros.data(), amount);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { (void)::close(fd); return false; }
        left -= static_cast<std::size_t>(count);
    }
    return ::close(fd) == 0;
}

pid_t launch(const char* executable, const std::vector<std::string>& arguments,
             const std::string& log_path)
{
    const pid_t child = ::fork();
    if (child != 0) return child;
    const int log = ::open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log >= 0) {
        (void)::dup2(log, STDOUT_FILENO);
        (void)::dup2(log, STDERR_FILENO);
        (void)::close(log);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>(executable));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execv(executable, argv.data());
    _exit(127);
}

bool wait_ready(const std::string& log_path, pid_t child)
{
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        std::ifstream log(log_path);
        const std::string text((std::istreambuf_iterator<char>(log)), {});
        if (text.find("pixd ready:") != std::string::npos) return true;
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::vector<std::uint8_t> read_file(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}

int run_status(const std::string& runtime_dir, std::string& output,
               const char* selector = "usb:1-2", bool group = false,
               bool log_argv = true)
{
    if (log_argv) {
        std::fprintf(stderr, "PROOF status_argv=%s --runtime-dir %s --device %s status\n",
                     PIXCTL_PATH, runtime_dir.c_str(), selector);
    }
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        (void)::close(output_pipe[0]);
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        (void)::close(output_pipe[1]);
        const char* argv[] = {PIXCTL_PATH, "--runtime-dir", runtime_dir.c_str(),
                              "--device", selector, "status", nullptr};
        const char* grouped[] = {PIXCTL_PATH, "--runtime-dir", runtime_dir.c_str(),
                                 "--device", selector, "--group", "status", nullptr};
        if (group)
            ::execv(PIXCTL_PATH, const_cast<char* const*>(grouped));
        ::execv(PIXCTL_PATH, const_cast<char* const*>(argv));
        _exit(127);
    }
    (void)::close(output_pipe[1]);
    std::array<char, 4096U> buffer{};
    ssize_t count = 0;
    while ((count = ::read(output_pipe[0], buffer.data(), buffer.size())) > 0)
        output.append(buffer.data(), static_cast<std::size_t>(count));
    (void)::close(output_pipe[0]);
    int status = 0;
    if (pid < 0 || ::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

int run_card_status(const std::string& runtime_dir, std::string& output)
{
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid == 0) {
        (void)::close(output_pipe[0]);
        if (::dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        if (::dup2(output_pipe[1], STDERR_FILENO) < 0) _exit(127);
        (void)::close(output_pipe[1]);
        const char* argv[] = {PIXCTL_PATH, "--runtime-dir", runtime_dir.c_str(),
                              "--device", "serial:PIXDT29501", "--group",
                              "card-status", nullptr};
        ::execv(PIXCTL_PATH, const_cast<char* const*>(argv));
        _exit(127);
    }
    (void)::close(output_pipe[1]);
    std::array<char, 4096U> buffer{};
    ssize_t count = 0;
    while ((count = ::read(output_pipe[0], buffer.data(), buffer.size())) > 0)
        output.append(buffer.data(), static_cast<std::size_t>(count));
    (void)::close(output_pipe[0]);
    int status = 0;
    if (pid < 0 || ::waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

bool check_status(std::string_view json, const char* receiver_state, bool leased,
                  const char* tune_revision)
{
    const std::size_t context_key = json.find("\"context\":{");
    const std::size_t snapshot_key = json.find("\"snapshot\":");
    if (context_key == std::string_view::npos || snapshot_key == std::string_view::npos ||
        snapshot_key <= context_key ||
        json.substr(context_key, snapshot_key - context_key).find(
            "\"state\":\"ready\"") == std::string_view::npos) return false;
    const std::size_t epoch_key = json.find("\"epoch\":\"");
    if (epoch_key == std::string_view::npos || epoch_key + 42U > json.size()) return false;
    const std::size_t epoch_start = epoch_key + 9U;
    if (json[epoch_start + 32U] != '"') return false;
    for (std::size_t i = 0U; i < 32U; ++i) {
        const char c = json[epoch_start + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    const std::size_t instance_key = json.find("\"instance\":\"");
    if (instance_key == std::string_view::npos || instance_key + 12U >= json.size() ||
        json[instance_key + 12U] == '"') return false;
    return json.size() > 2U && json.front() == '{' && json.back() == '\n' &&
           json.find("\"name\":\"userland.status\"") != std::string_view::npos &&
           json.find("\"product\":\"pix-userland\"") != std::string_view::npos &&
           json.find(std::string("\"version\":\"") + PIX_RUNTIME_VERSION + "\"") !=
               std::string_view::npos &&
           json.find("\"instance\":\"") != std::string_view::npos &&
           json.find("\"descriptor\":{\"receiver\":0") != std::string_view::npos &&
           json.find(std::string("\"state\":\"") + receiver_state + "\"") !=
               std::string_view::npos &&
           json.find(std::string("\"leased\":") + (leased ? "true" : "false")) !=
               std::string_view::npos &&
           json.find(std::string("\"tune_revision\":\"") + tune_revision + "\"") !=
               std::string_view::npos &&
           json.find("\"measurements\":{}") != std::string_view::npos &&
           json.find("\"readers\":[]") != std::string_view::npos;
}

std::string json_string(std::string_view json, const char* name)
{
    const std::string key = std::string("\"") + name + "\":\"";
    const std::size_t start = json.find(key);
    if (start == std::string_view::npos) return {};
    const std::size_t value_start = start + key.size();
    const std::size_t end = json.find('"', value_start);
    if (end == std::string_view::npos) return {};
    return std::string(json.substr(value_start, end - value_start));
}

bool wait_streaming_status(const std::string& runtime_dir, const std::string& epoch,
                           const std::string& instance, std::string& status,
                           unsigned& attempts)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        ++attempts;
        status.clear();
        if (run_status(runtime_dir, status, "serial:PIXDT18101", true, false) == 0 &&
            check_status(status, "streaming", true, "1") &&
            json_string(status, "epoch") == epoch &&
            json_string(status, "instance") == instance) return true;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (true);
    return false;
}

bool write_text(const std::string& path, std::string_view value)
{
    const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd < 0) return false;
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const ssize_t count = ::write(fd, value.data() + offset, value.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { (void)::close(fd); return false; }
        offset += static_cast<std::size_t>(count);
    }
    return ::close(fd) == 0;
}

std::size_t line_count(const std::string& path)
{
    std::ifstream file(path);
    std::size_t count = 0U;
    std::string line;
    while (std::getline(file, line)) ++count;
    return count;
}

std::size_t occurrence_count(std::string_view text, std::string_view needle)
{
    std::size_t count = 0U;
    std::size_t position = 0U;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

}  // namespace

int main()
{
    const char* temp_parent = ::geteuid() == 0 ? "/run" : std::getenv("HOME");
    if (temp_parent == nullptr || temp_parent[0] != '/') return 1;
    const std::string root_template = std::string(temp_parent) +
        "/pix-public-runtime-proof-XXXXXX";
    std::vector<char> root_storage(root_template.begin(), root_template.end());
    root_storage.push_back('\0');
    char* root_value = ::mkdtemp(root_storage.data());
    if (root_value == nullptr) return 1;
    const std::string root(root_value);
    const std::string assets = root + "/assets";
    if (::mkdir(assets.c_str(), 0700) != 0 ||
        !write_zero_file(assets + "/fpga.bin", 699673U) ||
        !write_zero_file(assets + "/bootstrap.bin", 16U) ||
        !write_zero_file(assets + "/validation.bin", 64U) ||
        ::chmod(root.c_str(), 0770) != 0) return 2;
    const auto asset_failure = [&](const std::string& asset_root,
                                   const std::string& log_path,
                                   const char* detail) {
        const pid_t child = launch(PIXD_PATH,
            {"--device", "serial:PIXDT18101", "--assets", asset_root,
             "--runtime-dir", root}, log_path);
        int status = 0;
        if (child <= 0 || ::waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 2) return false;
        const auto bytes = read_file(log_path);
        const std::string log(bytes.begin(), bytes.end());
        return log.find(detail) != std::string::npos &&
               log.find("--assets entry fpga.bin") != std::string::npos &&
               log.find(root) == std::string::npos;
    };
    if (!asset_failure(root + "/missing-assets", root + "/pixd-missing-asset.log",
                       "not found")) return 42;
    const std::string short_assets = root + "/short-assets";
    const std::uint8_t short_asset[] = {1U, 2U, 3U};
    if (::mkdir(short_assets.c_str(), 0700) != 0 ||
        !write_file(short_assets + "/fpga.bin", short_asset, sizeof(short_asset)) ||
        !asset_failure(short_assets, root + "/pixd-short-asset.log",
                       "expected 699673 bytes, found 3")) return 43;
    const auto pixd_failure = [&](const std::vector<std::string>& arguments,
                                  const std::string& log_path,
                                  const std::vector<std::string>& details) {
        const pid_t child = launch(PIXD_PATH, arguments, log_path);
        int status = 0;
        if (child <= 0 || ::waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 2) return false;
        const auto bytes = read_file(log_path);
        const std::string log(bytes.begin(), bytes.end());
        for (const std::string& detail : details)
            if (log.find(detail) == std::string::npos) return false;
        return log.find(root) == std::string::npos;
    };
    if (!pixd_failure({"--device", "serial:PIXDT18101", "--assets", assets,
                       "--runtime-dir", root, "--instance", "bad/"},
                      root + "/pixd-invalid-instance.log",
                      {"invalid --instance", "1..64 ASCII bytes",
                       "Windows reserved device names"})) return 45;
    const std::string long_runtime = root + "/" + std::string(180U, 'x');
    if (!pixd_failure({"--device", "serial:PIXDT18101", "--assets", assets,
                       "--runtime-dir", long_runtime},
                      root + "/pixd-long-endpoint.log",
                      {"endpoint path is too long", "path bytes", "including NUL",
                       "native capacity", "Shorten --runtime-dir or --instance"})) return 46;
    if (!pixd_failure({"--device", "serial:PIXDT18101", "--assets", assets,
                       "--runtime-dir", "relative"},
                      root + "/pixd-relative-runtime.log",
                      {"--runtime-dir must be a non-empty absolute local path"})) return 47;
    const std::string mode_log = root + "/pixd-runtime-mode.log";
    const pid_t mode_process = launch(PIXD_PATH,
        {"--device", "serial:PIXDT18101", "--assets", assets,
         "--runtime-dir", root}, mode_log);
    int mode_status = 0;
    if (mode_process <= 0 || ::waitpid(mode_process, &mode_status, 0) != mode_process ||
        !WIFEXITED(mode_status) || WEXITSTATUS(mode_status) != 70) return 51;
    const auto mode_bytes = read_file(mode_log);
    const std::string mode_text(mode_bytes.begin(), mode_bytes.end());
    if (mode_text.find("runtime directory permissions are too broad") == std::string::npos ||
        mode_text.find("Restrict --runtime-dir permissions") == std::string::npos ||
        mode_text.find(root) != std::string::npos) return 51;
    const std::string nested_runtime = root + "/nested-runtime";
    const std::string product_directory = nested_runtime + "/pix-userland";
    if (::mkdir(nested_runtime.c_str(), 0700) != 0 ||
        ::chmod(nested_runtime.c_str(), 0700) != 0 ||
        ::mkdir(product_directory.c_str(), 0700) != 0 ||
        ::chmod(product_directory.c_str(), 0770) != 0) return 53;
    const std::string nested_mode_log = root + "/pixd-product-mode.log";
    const pid_t nested_mode_process = launch(PIXD_PATH,
        {"--device", "serial:PIXDT18101", "--assets", assets,
         "--runtime-dir", nested_runtime, "--instance", "pix"}, nested_mode_log);
    int nested_mode_status = 0;
    if (nested_mode_process <= 0 ||
        ::waitpid(nested_mode_process, &nested_mode_status, 0) != nested_mode_process ||
        !WIFEXITED(nested_mode_status) || WEXITSTATUS(nested_mode_status) != 70) return 53;
    const auto nested_mode_bytes = read_file(nested_mode_log);
    const std::string nested_mode_text(nested_mode_bytes.begin(), nested_mode_bytes.end());
    if (nested_mode_text.find("runtime directory permissions are too broad") ==
            std::string::npos ||
        nested_mode_text.find("directory layer: product") == std::string::npos ||
        nested_mode_text.find(nested_runtime) != std::string::npos) return 53;
    const auto client_failure = [&](const char* executable,
                                    const std::vector<std::string>& arguments,
                                    const std::string& log_path,
                                    const char* detail) {
        const pid_t child = launch(executable, arguments, log_path);
        int status = 0;
        if (child <= 0 || ::waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 2) return false;
        const auto bytes = read_file(log_path);
        const std::string log(bytes.begin(), bytes.end());
        return log.find(detail) != std::string::npos &&
               log.find(root) == std::string::npos;
    };
    if (!client_failure(PIXCTL_PATH,
            {"--runtime-dir", "relative", "--instance", "pix-proof",
             "--device", "usb:1-2", "status"},
            root + "/pixctl-invalid-runtime.log",
            "--runtime-dir must be an absolute local runtime directory")) return 48;
    if (!client_failure(PIXCTL_PATH,
            {"--runtime-dir", long_runtime, "--instance", "pix-proof",
             "--device", "usb:1-2", "status"},
            root + "/pixctl-long-endpoint.log",
            "endpoint path is too long")) return 49;
    if (!client_failure(PIXTS_PATH,
            {"--device", "usb:1-2", "--instance", "pix-proof", "--channel", "T13",
             "--runtime-dir", long_runtime, "--output", "-"},
            root + "/pix-ts-long-endpoint.log",
            "endpoint path is too long")) return 50;
    const std::string expected_path = root + "/expected.ts";
    const std::string output_path = root + "/client.ts";
    const std::string off_grid_path = root + "/off-grid.ts";
    const std::string satellite_path = root + "/satellite.ts";
    const std::string satellite_slot_path = root + "/satellite-slot.ts";
    const std::string tune_log = root + "/tunes.log";
    const std::string list_json_log = root + "/list-json.log";
    const std::string ts_version_log = root + "/pix-ts-version.log";
    const std::string pixd_version_log = root + "/pixd-version.log";
    const std::string unsupported_lnb_log = root + "/unsupported-lnb.log";
    const std::string missing_group_root_log = root + "/missing-group-root.log";
    (void)::setenv("PIX_TEST_TUNE_LOG", tune_log.c_str(), 1);
    if (!write_file(expected_path, plain, sizeof(plain))) return 3;

    const pid_t list_process = launch(PIXD_PATH, {"--list-json"}, list_json_log);
    int list_status = 0;
    if (list_process <= 0 || ::waitpid(list_process, &list_status, 0) != list_process ||
        !WIFEXITED(list_status) || WEXITSTATUS(list_status) != 0) return 22;
    const auto list_json = read_file(list_json_log);
    const std::string list_text(list_json.begin(), list_json.end());
    if (list_text.find("\"model\":\"PIX-DT295\"") == std::string::npos ||
        list_text.find("\"usb\":\"06b8:103b\"") == std::string::npos ||
        list_text.find("\"serial\":\"PIXDT29501\"") == std::string::npos ||
        list_text.find("\"systems\":[\"ISDB-T\",\"ISDB-S\"]") == std::string::npos) return 24;
    const pid_t ts_version = launch(PIXTS_PATH, {"--version"}, ts_version_log);
    int ts_version_status = 0;
    if (ts_version <= 0 || ::waitpid(ts_version, &ts_version_status, 0) != ts_version ||
        !WIFEXITED(ts_version_status) || WEXITSTATUS(ts_version_status) != 0) return 25;
    const auto ts_version_bytes = read_file(ts_version_log);
    const std::string ts_version_text(ts_version_bytes.begin(), ts_version_bytes.end());
    if (ts_version_text != std::string("pix-ts ") + PIX_RUNTIME_VERSION + "\n") return 26;
    const pid_t pixd_version = launch(PIXD_PATH, {"--version"}, pixd_version_log);
    int pixd_version_status = 0;
    if (pixd_version <= 0 ||
        ::waitpid(pixd_version, &pixd_version_status, 0) != pixd_version ||
        !WIFEXITED(pixd_version_status) || WEXITSTATUS(pixd_version_status) != 0) return 31;
    const auto pixd_version_bytes = read_file(pixd_version_log);
    const std::string pixd_version_text(pixd_version_bytes.begin(), pixd_version_bytes.end());
    if (pixd_version_text != std::string("pix-userland ") + PIX_RUNTIME_VERSION + "\n")
        return 32;
    const pid_t unsupported_lnb = launch(PIXTS_PATH,
        {"--channel", "T13", "--lnb-voltage", "15"}, unsupported_lnb_log);
    int unsupported_lnb_status = 0;
    if (unsupported_lnb <= 0 ||
        ::waitpid(unsupported_lnb, &unsupported_lnb_status, 0) != unsupported_lnb ||
        !WIFEXITED(unsupported_lnb_status) || WEXITSTATUS(unsupported_lnb_status) != 2)
        return 33;
    const pid_t missing_group_root = launch(PIXD_PATH,
        {"--device", "usb:1-2", "--group"}, missing_group_root_log);
    int missing_group_root_status = 0;
    if (missing_group_root <= 0 ||
        ::waitpid(missing_group_root, &missing_group_root_status, 0) != missing_group_root ||
        !WIFEXITED(missing_group_root_status) ||
        WEXITSTATUS(missing_group_root_status) != 2) return 34;
    if (::chmod(root.c_str(), 0750) != 0) return 34;

    const pid_t daemon = launch(PIXD_PATH,
        {"--device", "serial:PIXDT18101", "--assets", assets,
         "--runtime-dir", root, "--group"},
        root + "/pixd.log");
    if (daemon <= 0 || !wait_ready(root + "/pixd.log", daemon)) {
        std::fprintf(stderr, "PROOF pixd startup failed root=%s\n", root.c_str());
        if (daemon > 0) { (void)::kill(daemon, SIGTERM); (void)::waitpid(daemon, nullptr, 0); }
        return 4;
    }
    const std::string duplicate_log = root + "/pixd-duplicate.log";
    const pid_t duplicate = launch(PIXD_PATH,
        {"--device", "serial:PIXDT18101", "--assets", assets,
         "--runtime-dir", root, "--group"}, duplicate_log);
    int duplicate_status = 0;
    const bool duplicate_failed = duplicate > 0 &&
        ::waitpid(duplicate, &duplicate_status, 0) == duplicate &&
        WIFEXITED(duplicate_status) && WEXITSTATUS(duplicate_status) == 70;
    const auto duplicate_bytes = read_file(duplicate_log);
    const std::string duplicate_text(duplicate_bytes.begin(), duplicate_bytes.end());
    if (!duplicate_failed ||
        duplicate_text.find("PIX_STAGE reserve error=4") == std::string::npos ||
        duplicate_text.find("endpoint namespace is already in use") == std::string::npos ||
        duplicate_text.find("ownership") != std::string::npos ||
        duplicate_text.find("permissions") != std::string::npos) {
        if (duplicate > 0) { (void)::kill(duplicate, SIGTERM); (void)::waitpid(duplicate, nullptr, 0); }
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 52;
    }

    std::string idle_status;
    const std::size_t idle_tunes = line_count(tune_log);
    if (run_status(root, idle_status, "serial:PIXDT18101", true) != 0 ||
        !check_status(idle_status, "free", false, "0") ||
        !write_text(root + "/status-idle.json", idle_status) ||
        line_count(tune_log) != idle_tunes) {
        std::fprintf(stderr, "PROOF idle status failed: %s", idle_status.c_str());
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 23;
    }
    const std::string idle_epoch = json_string(idle_status, "epoch");
    const std::string idle_instance = json_string(idle_status, "instance");

    const pid_t client = launch(PIXTS_PATH,
        {"--device", "serial:PIXDT18101", "--freq", "473142857", "--time", "3",
         "--output", output_path, "--runtime-dir", root, "--group"}, root + "/pix-ts.log");
    std::string streaming_ready_status;
    unsigned readiness_attempts = 0U;
    const bool capture_ready = client > 0 &&
        wait_streaming_status(root, idle_epoch, idle_instance, streaming_ready_status,
                              readiness_attempts);
    std::fprintf(stderr, "PROOF streaming_readiness=actual_status attempts=%u ready=%u\n",
                 readiness_attempts, capture_ready ? 1U : 0U);
    std::string streaming_status;
    const std::size_t stream_tunes = line_count(tune_log);
    if (!capture_ready || !write_text(root + "/status-streaming-ready.json",
                                      streaming_ready_status) ||
        stream_tunes == 0U ||
        run_status(root, streaming_status, "serial:PIXDT18101", true) != 0 ||
        !check_status(streaming_status, "streaming", true, "1") ||
        json_string(streaming_status, "epoch") != idle_epoch ||
        json_string(streaming_status, "instance") != idle_instance ||
        !write_text(root + "/status-streaming.json", streaming_status) ||
        line_count(tune_log) != stream_tunes) {
        std::fprintf(stderr, "PROOF streaming status failed: %s",
                     capture_ready ? streaming_status.c_str() : streaming_ready_status.c_str());
        (void)::kill(client, SIGTERM);
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(client, nullptr, 0);
        (void)::waitpid(daemon, nullptr, 0);
        return 24;
    }
    std::fprintf(stderr, "PROOF observation_invariants=tune_count:%zu->%zu lease:true tune_revision:1\n",
                 stream_tunes, line_count(tune_log));
    int client_status = 0;
    if (client <= 0 || ::waitpid(client, &client_status, 0) != client ||
        !WIFEXITED(client_status) || WEXITSTATUS(client_status) != 0) {
        std::fprintf(stderr, "PROOF pix-ts failed root=%s\n", root.c_str());
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 5;
    }
    const pid_t off_grid = launch(PIXTS_PATH,
        {"--device", "serial:PIXDT18101", "--freq", "473142000", "--time", "1",
         "--output", off_grid_path, "--runtime-dir", root, "--group"},
        root + "/pix-ts-off-grid.log");
    int off_grid_status = 0;
    if (off_grid <= 0 || ::waitpid(off_grid, &off_grid_status, 0) != off_grid ||
        !WIFEXITED(off_grid_status) || WEXITSTATUS(off_grid_status) != 0) {
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 22;
    }
    const pid_t satellite = launch(PIXTS_PATH,
        {"--device", "serial:PIXDT18101", "--channel", "BS01", "--tsid", "4660",
         "--time", "1", "--output", satellite_path,
         "--runtime-dir", root, "--group"}, root + "/pix-ts-satellite.log");
    int satellite_status = 0;
    if (satellite <= 0 || ::waitpid(satellite, &satellite_status, 0) != satellite ||
        !WIFEXITED(satellite_status) || WEXITSTATUS(satellite_status) != 0) {
        std::fprintf(stderr, "PROOF satellite pix-ts failed root=%s\n", root.c_str());
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 11;
    }
    const pid_t satellite_slot = launch(PIXTS_PATH,
        {"--device", "serial:PIXDT18101", "--channel", "BS01", "--slot", "3",
         "--time", "1", "--output", satellite_slot_path, "--runtime-dir", root,
         "--group"}, root + "/pix-ts-satellite-slot.log");
    int satellite_slot_status = 0;
    if (satellite_slot <= 0 ||
        ::waitpid(satellite_slot, &satellite_slot_status, 0) != satellite_slot ||
        !WIFEXITED(satellite_slot_status) || WEXITSTATUS(satellite_slot_status) != 0) {
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 27;
    }
    const std::string stream_endpoint = root + "/pix-userland/v1/" + idle_instance +
        "/stream.sock";
    if (::unlink(stream_endpoint.c_str()) != 0) {
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 54;
    }
    const std::string missing_stream_log = root + "/pix-ts-missing-stream.log";
    const pid_t missing_stream_client = launch(PIXTS_PATH,
        {"--device", "serial:PIXDT18101", "--channel", "T13", "--time", "1",
         "--output", root + "/missing-stream.ts", "--runtime-dir", root, "--group"},
        missing_stream_log);
    int missing_stream_status = 0;
    if (missing_stream_client <= 0 ||
        ::waitpid(missing_stream_client, &missing_stream_status, 0) != missing_stream_client ||
        !WIFEXITED(missing_stream_status) || WEXITSTATUS(missing_stream_status) != 3) {
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 55;
    }
    const auto missing_stream_bytes = read_file(missing_stream_log);
    const std::string missing_stream_text(missing_stream_bytes.begin(), missing_stream_bytes.end());
    if (missing_stream_text.find("stream endpoint is missing") == std::string::npos ||
        missing_stream_text.find("control endpoint is missing") != std::string::npos ||
        missing_stream_text.find(root) != std::string::npos) {
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 56;
    }
    (void)::kill(daemon, SIGTERM);
    int daemon_status = 0;
    if (::waitpid(daemon, &daemon_status, 0) != daemon || !WIFEXITED(daemon_status) ||
        WEXITSTATUS(daemon_status) != 0) {
        std::fprintf(stderr, "PROOF pixd close failed root=%s\n", root.c_str());
        return 6;
    }
    std::ifstream daemon_log_file(root + "/pixd.log");
    const std::string daemon_log((std::istreambuf_iterator<char>(daemon_log_file)), {});
    if (daemon_log.find("PIX_CLEANUP shutdown=1") == std::string::npos ||
        daemon_log.find("PIX_CLEANUP release-interface=1") == std::string::npos ||
        daemon_log.find("PIX_CLEANUP close-handle=1") == std::string::npos) return 12;

    (void)::setenv("PIX_TEST_DT295_FAKE", "1", 1);
    const std::array<std::pair<const char*, const char*>, 3U> card_cases{{
        {"3", "present=yes"}, {"0", "present=no"}, {"1", "present=unknown"}}};
    for (std::size_t index = 0U; index < card_cases.size(); ++index) {
        (void)::setenv("PIX_TEST_DT295_RAW", card_cases[index].first, 1);
        if (index == 1U) (void)::setenv("PIX_TEST_DT295_NOTIFICATION", "1", 1);
        else (void)::unsetenv("PIX_TEST_DT295_NOTIFICATION");
        const std::string log = root + "/dt295-card-" + std::to_string(index) + ".log";
        const pid_t card_daemon = launch(PIXD_PATH,
            {"--device", "serial:PIXDT29501", "--assets", assets,
             "--runtime-dir", root, "--group"}, log);
        if (card_daemon <= 0 || !wait_ready(log, card_daemon)) return 36;
        std::string card_output;
        if (run_card_status(root, card_output) != 0 ||
            card_output.find(card_cases[index].second) == std::string::npos) {
            std::fprintf(stderr, "PROOF DT295 card-status expected=%s got=%s",
                         card_cases[index].second, card_output.c_str());
            (void)::kill(card_daemon, SIGTERM);
            (void)::waitpid(card_daemon, nullptr, 0);
            return 37;
        }
        (void)::kill(card_daemon, SIGTERM);
        int card_daemon_status = 0;
        if (::waitpid(card_daemon, &card_daemon_status, 0) != card_daemon ||
            !WIFEXITED(card_daemon_status) || WEXITSTATUS(card_daemon_status) != 0) return 38;
        std::fprintf(stderr, "PROOF DT295 card-status %s via existing pixctl path\n",
                     card_cases[index].second);
    }
    (void)::setenv("PIX_TEST_DT295_RAW", "3", 1);
    (void)::unsetenv("PIX_TEST_DT295_NOTIFICATION");
    (void)::setenv("PIX_TEST_DT295_REINSERT_RAW", "1", 1);
    const std::string reinsert_log = root + "/dt295-card-reinsert.log";
    const pid_t reinsert_daemon = launch(PIXD_PATH,
        {"--device", "serial:PIXDT29501", "--assets", assets,
         "--runtime-dir", root, "--group"}, reinsert_log);
    if (reinsert_daemon <= 0 || !wait_ready(reinsert_log, reinsert_daemon)) return 39;
    std::string reinsert_output;
    if (run_card_status(root, reinsert_output) != 0 ||
        reinsert_output.find("present=yes") == std::string::npos) {
        std::fprintf(stderr, "PROOF DT295 reinsert card-status expected=present=yes got=%s",
                     reinsert_output.c_str());
        (void)::kill(reinsert_daemon, SIGTERM);
        (void)::waitpid(reinsert_daemon, nullptr, 0);
        return 40;
    }
    (void)::kill(reinsert_daemon, SIGTERM);
    int reinsert_daemon_status = 0;
    if (::waitpid(reinsert_daemon, &reinsert_daemon_status, 0) != reinsert_daemon ||
        !WIFEXITED(reinsert_daemon_status) || WEXITSTATUS(reinsert_daemon_status) != 0) return 41;
    std::fprintf(stderr, "PROOF DT295 reinsert card-status present=yes via one pixctl request\n");
    (void)::unsetenv("PIX_TEST_DT295_FAKE");
    (void)::unsetenv("PIX_TEST_DT295_RAW");
    (void)::unsetenv("PIX_TEST_DT295_NOTIFICATION");
    (void)::unsetenv("PIX_TEST_DT295_REINSERT_RAW");

    const auto expected = read_file(expected_path);
    const auto actual = read_file(output_path);
    const auto off_grid_actual = read_file(off_grid_path);
    const auto satellite_actual = read_file(satellite_path);
    const auto satellite_slot_actual = read_file(satellite_slot_path);
    std::ifstream tune_file(tune_log);
    const std::string tunes((std::istreambuf_iterator<char>(tune_file)), {});
    if (actual != expected || off_grid_actual != expected || satellite_actual != expected ||
        satellite_slot_actual != expected ||
        tunes.find("system=0 hz=473142857 slot=-1 tsid=-1") == std::string::npos ||
        tunes.find("system=0 hz=473142000 slot=-1 tsid=-1") == std::string::npos ||
        tunes.find("system=1 hz=1049480000 slot=3 tsid=4660") == std::string::npos) {
        std::fprintf(stderr, "PROOF T/S byte or selection mismatch expected=%zu T=%zu off-grid=%zu S=%zu root=%s\n",
                     expected.size(), actual.size(), off_grid_actual.size(), satellite_actual.size(), root.c_str());
        return 7;
    }
    std::printf("PROOF real pixd/pix-ts separate processes T/S byte_match=%zu slot=3 TSID=0x1234 root=%s\n",
                actual.size(), root.c_str());

    const std::string no_ts_path = root + "/no-ts.ts";
    const std::string no_ts_runtime = root + "/no-ts-runtime";
    if (::mkdir(no_ts_runtime.c_str(), 0700) != 0) return 28;
    (void)::setenv("PIX_NO_TS", "1", 1);
    const pid_t no_ts_daemon = launch(PIXD_PATH,
        {"--device", "usb:1-2", "--assets", assets, "--runtime-dir", no_ts_runtime,
         "--instance", "pix-no-ts"}, root + "/pixd-no-ts.log");
    if (no_ts_daemon <= 0 || !wait_ready(root + "/pixd-no-ts.log", no_ts_daemon)) return 13;
    const pid_t no_ts_client = launch(PIXTS_PATH,
        {"--instance", "pix-no-ts", "--channel", "T13", "--time", "1",
         "--output", no_ts_path, "--runtime-dir", no_ts_runtime}, root + "/pix-ts-no-ts.log");
    int no_ts_status = 0;
    if (no_ts_client <= 0 || ::waitpid(no_ts_client, &no_ts_status, 0) != no_ts_client ||
        !WIFEXITED(no_ts_status) || WEXITSTATUS(no_ts_status) != 0 ||
        !read_file(no_ts_path).empty()) {
        (void)::kill(no_ts_daemon, SIGTERM);
        (void)::waitpid(no_ts_daemon, nullptr, 0);
        return 14;
    }
    (void)::kill(no_ts_daemon, SIGTERM);
    int no_ts_daemon_status = 0;
    if (::waitpid(no_ts_daemon, &no_ts_daemon_status, 0) != no_ts_daemon ||
        !WIFEXITED(no_ts_daemon_status) || WEXITSTATUS(no_ts_daemon_status) != 0) return 15;
    (void)::unsetenv("PIX_NO_TS");
    std::printf("PROOF no-TS short recording and daemon close succeeded root=%s\n", root.c_str());

    const std::string gate_path = root + "/callback-held";
    const std::string release_path = root + "/callback-release";
    const std::string held_runtime = root + "/held-runtime";
    if (::mkdir(held_runtime.c_str(), 0700) != 0) return 29;
    (void)::setenv("PIX_HOLD_CALLBACK_FILE", gate_path.c_str(), 1);
    (void)::setenv("PIX_RELEASE_CALLBACK_FILE", release_path.c_str(), 1);
    const pid_t held_daemon = launch(PIXD_PATH,
        {"--device", "usb:1-2", "--assets", assets, "--runtime-dir", held_runtime,
         "--instance", "pix-held"}, root + "/pixd-held.log");
    if (held_daemon <= 0 || !wait_ready(root + "/pixd-held.log", held_daemon)) return 16;
    std::ifstream held_start_file(root + "/pixd-held.log");
    const std::string held_start_log((std::istreambuf_iterator<char>(held_start_file)), {});
    const std::size_t held_start_closes = occurrence_count(
        held_start_log, "PIX_CLEANUP close-handle=");
    const pid_t held_client = launch(PIXTS_PATH,
        {"--instance", "pix-held", "--channel", "T13", "--time", "3",
         "--output", root + "/held.ts", "--runtime-dir", held_runtime}, root + "/pix-ts-held.log");
    bool callback_held = false;
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        if (::access(gate_path.c_str(), F_OK) == 0) { callback_held = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!callback_held) {
        (void)::kill(held_daemon, SIGTERM);
        (void)::kill(held_client, SIGTERM);
        (void)::waitpid(held_daemon, nullptr, 0);
        (void)::waitpid(held_client, nullptr, 0);
        return 17;
    }
    (void)::kill(held_daemon, SIGTERM);
    bool deadline_seen = false;
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        std::ifstream held_log_file(root + "/pixd-held.log");
        const std::string held_log((std::istreambuf_iterator<char>(held_log_file)), {});
        if (held_log.find("PIX_CLOSE deadline_expired=1") != std::string::npos) {
            if (occurrence_count(held_log, "PIX_CLEANUP close-handle=") !=
                held_start_closes) return 18;
            deadline_seen = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!deadline_seen || !write_file(release_path,
            reinterpret_cast<const std::uint8_t*>("release"), 7U)) return 19;
    int held_daemon_status = 0;
    if (::waitpid(held_daemon, &held_daemon_status, 0) != held_daemon ||
        !WIFEXITED(held_daemon_status) || WEXITSTATUS(held_daemon_status) != 70) return 20;
    std::ifstream released_log_file(root + "/pixd-held.log");
    const std::string released_log((std::istreambuf_iterator<char>(released_log_file)), {});
    if (released_log.find("PIX_CLEANUP shutdown=1") == std::string::npos ||
        released_log.find("PIX_CLEANUP release-interface=1") == std::string::npos ||
        occurrence_count(released_log, "PIX_CLEANUP close-handle=") !=
            held_start_closes + 1U) return 21;
    int held_client_status = 0;
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        const pid_t result = ::waitpid(held_client, &held_client_status, WNOHANG);
        if (result == held_client) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (::waitpid(held_client, &held_client_status, WNOHANG) == 0) {
        (void)::kill(held_client, SIGTERM);
        (void)::waitpid(held_client, &held_client_status, 0);
    }
    (void)::unsetenv("PIX_HOLD_CALLBACK_FILE");
    (void)::unsetenv("PIX_RELEASE_CALLBACK_FILE");
    std::printf("PROOF held callback close deadline retained owner; release then quiesced once root=%s\n",
                root.c_str());

    const std::string fd_runtime = root + "/fd-runtime";
    const std::string fd295_runtime = root + "/fd295-runtime";
    const std::string fd_log = root + "/fd.log";
    if (::mkdir(fd_runtime.c_str(), 0700) != 0 ||
        ::mkdir(fd295_runtime.c_str(), 0700) != 0) return 54;
    const int usb_fd = ::open("/dev/null", O_RDWR);
    if (usb_fd < 3 || ::fcntl(usb_fd, F_SETFD, 0) < 0) return 55;
    if (::setenv("PIX_TEST_USB_FD_LOG", fd_log.c_str(), 1) != 0 ||
        ::setenv("PIX_TEST_USB_FD_MODEL", "dt181", 1) != 0) return 56;
    const auto fd_exit = [&](const std::vector<std::string>& arguments,
                             const std::string& log_path, int expected) {
        const pid_t child = launch(PIXD_PATH, arguments, log_path);
        int status = 0;
        return child > 0 && ::waitpid(child, &status, 0) == child &&
            WIFEXITED(status) && WEXITSTATUS(status) == expected;
    };
    const std::string fd_value = std::to_string(usb_fd);
    const pid_t fd_daemon = launch(PIXD_PATH,
        {"--fd", fd_value, "--assets", assets, "--runtime-dir", fd_runtime,
         "--instance", "pixfd"}, root + "/pixd-fd.log");
    if (fd_daemon <= 0 || !wait_ready(root + "/pixd-fd.log", fd_daemon)) {
        if (fd_daemon > 0) {
            (void)::kill(fd_daemon, SIGTERM);
            (void)::waitpid(fd_daemon, nullptr, 0);
        }
        return 57;
    }
    (void)::kill(fd_daemon, SIGTERM);
    int fd_daemon_status = 0;
    if (::waitpid(fd_daemon, &fd_daemon_status, 0) != fd_daemon ||
        !WIFEXITED(fd_daemon_status) || WEXITSTATUS(fd_daemon_status) != 0 ||
        ::fcntl(usb_fd, F_GETFD) < 0) return 58;
    std::ifstream fd_log_file(fd_log);
    const std::string fd_log_text((std::istreambuf_iterator<char>(fd_log_file)), {});
    if (fd_log_text.find("wrap fd=") == std::string::npos ||
        fd_log_text.find("cloexec=1") == std::string::npos) return 59;
    if (!fd_exit({"--fd", "0"}, root + "/pixd-fd-zero.log", 2) ||
        !fd_exit({"--fd", "0x3"}, root + "/pixd-fd-hex.log", 2) ||
        !fd_exit({"--fd", fd_value, "--fd", fd_value},
                 root + "/pixd-fd-repeat.log", 2) ||
        !fd_exit({"--fd", "99999999"}, root + "/pixd-fd-closed.log", 2) ||
        !fd_exit({}, root + "/pixd-fd-missing.log", 2)) return 60;
    if (!fd_exit({"--fd", fd_value, "--device", "serial:PIXDT29501"},
                 root + "/pixd-fd-selector-mismatch.log", 3)) return 61;
    if (!fd_exit({"--fd", fd_value, "--device", "usb:1-3"},
                 root + "/pixd-fd-topology-mismatch.log", 3)) return 61;
    if (::setenv("PIX_TEST_USB_FD_MODEL", "wrong-vid", 1) != 0 ||
        !fd_exit({"--fd", fd_value}, root + "/pixd-fd-wrong-vid.log", 3)) return 62;
    if (::setenv("PIX_TEST_USB_FD_MODEL", "invalid", 1) != 0 ||
        !fd_exit({"--fd", fd_value}, root + "/pixd-fd-non-usb.log", 2)) return 63;
    if (::setenv("PIX_TEST_USB_FD_MODEL", "dt295", 1) != 0 ||
        ::setenv("PIX_TEST_DT295_FAKE", "1", 1) != 0) return 64;
    const pid_t fd295_daemon = launch(PIXD_PATH,
        {"--fd", fd_value, "--assets", assets, "--runtime-dir", fd295_runtime,
         "--instance", "pixfd295"}, root + "/pixd-fd295.log");
    if (fd295_daemon <= 0 || !wait_ready(root + "/pixd-fd295.log", fd295_daemon)) {
        if (fd295_daemon > 0) {
            (void)::kill(fd295_daemon, SIGTERM);
            (void)::waitpid(fd295_daemon, nullptr, 0);
        }
        return 65;
    }
    (void)::kill(fd295_daemon, SIGTERM);
    int fd295_status = 0;
    if (::waitpid(fd295_daemon, &fd295_status, 0) != fd295_daemon ||
        !WIFEXITED(fd295_status) || WEXITSTATUS(fd295_status) != 0 ||
        ::fcntl(usb_fd, F_GETFD) < 0) return 66;
    std::ifstream fd295_log_file(fd_log);
    const std::string fd295_log_text((std::istreambuf_iterator<char>(fd295_log_file)), {});
    if (fd295_log_text.find("usb_open_model model=295 fd=") == std::string::npos) return 67;
    (void)::close(usb_fd);
    (void)::unsetenv("PIX_TEST_USB_FD_MODEL");
    (void)::unsetenv("PIX_TEST_DT295_FAKE");
    (void)::unsetenv("PIX_TEST_USB_FD_LOG");
    std::printf("PROOF Android USB fd model detection, selector assertion and borrowed-fd cleanup passed\n");

    const auto located = pix::runtime::locate_dt181("usb:1-2");
    if (!located) return 8;
    auto family_result = pix::runtime::Family::create(located.value(), assets.c_str());
    if (!family_result) return 9;
    std::unique_ptr<dtv::userland::host_runtime::EnclosureFamily> family(
        std::move(family_result.value()));
    pix_test_fail_thread_create();
    const std::string unwind_runtime = root + "/unwind-runtime";
    if (::mkdir(unwind_runtime.c_str(), 0700) != 0) return 30;
    std::array<std::uint8_t, 16U> epoch{};
    dtv::userland::wire::ListResponse list{};
    if (!pix::runtime::build_list(located.value(), "0.0.0-dev", epoch, list)) return 10;
    dtv::userland::host_listener_context::ContextConfig config{};
    config.runtime_directory = unwind_runtime;
    config.product = "pix-userland";
    config.instance = "pix-proof";
    config.server.epoch = epoch;
    config.server.producer = list.producer;
    config.canonical_list = list;
    config.context.epoch = epoch;
    config.context.instance.present = true;
    std::memcpy(config.context.instance.value.bytes.data(), "pix-proof", 9U);
    config.context.instance.value.length = 9U;
    config.stream_engine_epoch = 1U;
    config.stream_arm_budget_ms = 5000U;
    auto assembled = dtv::userland::host_listener_context::EnclosureContext::assemble(
        std::move(family), std::unique_ptr<px4::userland::TunerNonceSource>(
            new (std::nothrow) px4::userland::ipc::posix::PosixTunerNonceSource()),
        config);
    if (assembled || pix_test_shutdown_count() != 1U ||
        pix_test_release_count() != 1U || pix_test_close_count() != 3U) {
        std::fprintf(stderr, "PROOF early unwind cleanup counts shutdown=%u release=%u close=%u\n",
            pix_test_shutdown_count(), pix_test_release_count(), pix_test_close_count());
        return 10;
    }
    std::printf("PROOF early unwind releases shutdown=%u interface=%u handle=%u\n",
        pix_test_shutdown_count(), pix_test_release_count(), pix_test_close_count());
    return 0;
}
