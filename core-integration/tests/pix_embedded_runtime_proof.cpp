/* SPDX-License-Identifier: MIT */
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PIXD_PATH
#error "PIXD_PATH must name the embedded pixd fixture"
#endif

namespace {

pid_t launch(const std::vector<std::string>& arguments, const std::string& cwd,
             const std::string& log_path)
{
    const pid_t child = ::fork();
    if (child != 0) return child;
    if (::chdir(cwd.c_str()) != 0) _exit(126);
    (void)::clearenv();
    const int log = ::open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log >= 0) {
        (void)::dup2(log, STDOUT_FILENO);
        (void)::dup2(log, STDERR_FILENO);
        (void)::close(log);
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char*>(PIXD_PATH));
    for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);
    ::execv(PIXD_PATH, argv.data());
    _exit(127);
}

std::string read_log(const std::string& path)
{
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

bool wait_exit(pid_t child, int& status)
{
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) return true;
        if (result < 0 && errno != EINTR) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    (void)::kill(child, SIGKILL);
    (void)::waitpid(child, &status, 0);
    return false;
}

bool prove_model(const std::string& root, const char* serial, const char* model,
                 unsigned index, const char* firmware_option = nullptr,
                 const char* firmware_path = nullptr)
{
    const std::string empty = root + "/empty-" + std::to_string(index);
    const std::string runtime = root;
    const std::string log = root + "/pixd-" + std::to_string(index) + ".log";
    if (::mkdir(empty.c_str(), 0700) != 0) return false;
    std::vector<std::string> arguments;
    if (firmware_option != nullptr && firmware_path != nullptr) {
        arguments.emplace_back(firmware_option);
        arguments.emplace_back(firmware_path);
    }
    arguments.insert(arguments.end(), {"--device", std::string("serial:") + serial,
                                       "--runtime-dir", runtime, "--group"});
    const pid_t daemon = launch(arguments, empty, log);
    if (daemon <= 0) return false;
    bool ready = false;
    for (unsigned attempt = 0U; attempt < 500U; ++attempt) {
        const std::string text = read_log(log);
        if (text.find(std::string("pixd ready: model=") + model) != std::string::npos) {
            ready = true;
            break;
        }
        int status = 0;
        const pid_t result = ::waitpid(daemon, &status, WNOHANG);
        if (result == daemon) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!ready || ::kill(daemon, SIGTERM) != 0) {
        (void)::kill(daemon, SIGKILL);
        (void)::waitpid(daemon, nullptr, 0);
        return false;
    }
    int status = 0;
    if (!wait_exit(daemon, status) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "embedded pixd proof failed to stop cleanly: %s\n", model);
        return false;
    }
    const std::string text = read_log(log);
    const bool clean = text.find("PIX_CONTEXT_CLOSE result=") == std::string::npos &&
           std::filesystem::is_empty(empty) &&
           (std::string(model) != "PIX-DT181-PU0" ||
            (text.find("PIX_CLEANUP shutdown=1") != std::string::npos &&
             text.find("PIX_CLEANUP release-interface=1") != std::string::npos &&
             text.find("PIX_CLEANUP close-handle=1") != std::string::npos));
    if (!clean) std::fprintf(stderr, "embedded pixd proof lacked clean shutdown: %s\n", model);
    return clean;
}

struct RemoveTree {
    std::string path;

    bool remove() const
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        return !error;
    }

    ~RemoveTree()
    {
        (void)remove();
    }
};

}  // namespace

int main()
{
    const char* temp_parent = ::geteuid() == 0 ? "/run" : std::getenv("HOME");
    if (temp_parent == nullptr || temp_parent[0] != '/') return 1;
    std::string parent(temp_parent);
    while (parent.size() > 1U && parent.back() == '/') parent.pop_back();
    const std::string root_template = parent == "/"
        ? "/p-XXXXXX" : parent + "/p-XXXXXX";
    std::vector<char> root_storage(root_template.begin(), root_template.end());
    root_storage.push_back('\0');
    char* root_value = ::mkdtemp(root_storage.data());
    if (root_value == nullptr) return 1;
    const std::string root(root_value);
    const RemoveTree cleanup{root};
    const int result = [&]() {
        if (::chmod(root.c_str(), 0750) != 0) return 1;
        if (!prove_model(root, "PIXDT18101", "PIX-DT181-PU0", 0U,
                         "-F", PIX_FIRMWARE_DT181_PATH)) return 2;
        if (!prove_model(root, "PIXDT29501", "PIX-DT295", 1U)) return 2;

        const std::string empty = root + "/empty-cli";
        const std::string help_log = root + "/help.log";
        if (::mkdir(empty.c_str(), 0700) != 0) return 3;
        const pid_t help = launch({"--help"}, empty, help_log);
        int status = 0;
        if (help <= 0 || !wait_exit(help, status) || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0 ||
            read_log(help_log).find("--firmware FILE|-F FILE") == std::string::npos ||
            read_log(help_log).find("--assets") != std::string::npos)
            return 4;
        const pid_t override = launch({"--assets", "/unused", "--device", "usb:1-1"},
                                     empty, root + "/override.log");
        if (override <= 0 || !wait_exit(override, status) || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 2) return 5;
        return 0;
    }();
    if (!cleanup.remove()) {
        std::fprintf(stderr, "embedded pixd proof failed to clean its temporary root\n");
        return 6;
    }
    if (result != 0) return result;
    std::puts("embedded pixd: --firmware override and omitted embedded firmware both reached ready");
    return 0;
}
