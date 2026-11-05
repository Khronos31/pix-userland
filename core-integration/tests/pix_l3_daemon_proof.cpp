// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include "pix-runtime-vectors.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PIXD_L3_PATH
#error "PIXD_L3_PATH must name the L3 pixd fixture"
#endif
#ifndef PIXTS_PATH
#error "PIXTS_PATH must name pix-ts"
#endif
#ifndef PIXCTL_PATH
#error "PIXCTL_PATH must name pixctl"
#endif
#ifndef PIX_FIRMWARE_DT181_PATH
#error "PIX_FIRMWARE_DT181_PATH must name the DT181 firmware bundle"
#endif

namespace {

pid_t launch(const char *executable, const std::vector<std::string> &arguments,
             const std::string &log_path)
{
    const pid_t child = ::fork();
    if (child != 0) return child;
    const int log = ::open(log_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (log >= 0) {
        (void)::dup2(log, STDOUT_FILENO);
        (void)::dup2(log, STDERR_FILENO);
        (void)::close(log);
    }
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 2U);
    argv.push_back(const_cast<char *>(executable));
    for (const auto &argument : arguments) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable, argv.data());
    _exit(127);
}

bool wait_ready(const std::string &log_path, pid_t child)
{
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        std::ifstream log(log_path);
        const std::string text((std::istreambuf_iterator<char>(log)), {});
        if (text.find("pixd ready:") != std::string::npos) return true;
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::vector<std::uint8_t> read_file(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}

int run_client(const char *executable, const std::vector<std::string> &arguments,
               const std::string &log_path)
{
    const pid_t child = launch(executable, arguments, log_path);
    int status = 0;
    if (child <= 0 || ::waitpid(child, &status, 0) != child ||
        !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

std::string dump(const std::string &path)
{
    const auto bytes = read_file(path);
    return std::string(bytes.begin(), bytes.end());
}

}  // namespace

int main()
{
    const char *parent = ::geteuid() == 0 ? "/run" : std::getenv("HOME");
    if (parent == nullptr || parent[0] != '/') return 1;
    std::string root_template = std::string(parent) + "/pix-l3-XXXXXX";
    std::vector<char> storage(root_template.begin(), root_template.end());
    storage.push_back('\0');
    char *created = ::mkdtemp(storage.data());
    if (created == nullptr) return 1;
    const std::string root(created);
    if (::chmod(root.c_str(), 0750) != 0) return 2;

    const std::string daemon_log = root + "/pixd.log";
    const pid_t daemon = launch(PIXD_L3_PATH,
        {"--device", "serial:PIXDT18101", "--firmware", PIX_FIRMWARE_DT181_PATH,
         "--runtime-dir", root, "--group"}, daemon_log);
    if (daemon <= 0 || !wait_ready(daemon_log, daemon)) {
        std::fprintf(stderr, "PROOF L3 daemon startup failed\n%s\n",
                     dump(daemon_log).c_str());
        if (daemon > 0) {
            (void)::kill(daemon, SIGTERM);
            (void)::waitpid(daemon, nullptr, 0);
        }
        return 3;
    }

    std::string status_output;
    const std::string status_log = root + "/pixctl.log";
    const int status_rc = run_client(PIXCTL_PATH,
        {"--runtime-dir", root, "--device", "serial:PIXDT18101", "--group",
         "status"}, status_log);
    status_output = dump(status_log);
    if (status_rc != 0) {
        std::fprintf(stderr, "PROOF pixctl status rc=%d\n%s\n", status_rc,
                     status_output.c_str());
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 4;
    }

    const std::string output_path = root + "/client.ts";
    const std::string client_log = root + "/pix-ts.log";
    const int client_rc = run_client(PIXTS_PATH,
        {"--device", "serial:PIXDT18101", "--receiver", "0", "--channel", "T13",
         "--time", "1", "--output", output_path, "--runtime-dir", root,
         "--group"}, client_log);
    if (client_rc != 0) {
        std::fprintf(stderr, "PROOF pix-ts rc=%d\n%s\n", client_rc,
                     dump(client_log).c_str());
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 5;
    }
    const auto output = read_file(output_path);
    if (output.size() != sizeof(plain) ||
        std::memcmp(output.data(), plain, sizeof(plain)) != 0) {
        std::fprintf(stderr, "PROOF pix-ts TS mismatch size=%zu expected=%zu\n%s\n",
                     output.size(), sizeof(plain), dump(client_log).c_str());
        (void)::kill(daemon, SIGTERM);
        (void)::waitpid(daemon, nullptr, 0);
        return 6;
    }

    (void)::kill(daemon, SIGTERM);
    int daemon_status = 0;
    if (::waitpid(daemon, &daemon_status, 0) != daemon ||
        !WIFEXITED(daemon_status) || WEXITSTATUS(daemon_status) != 0) {
        std::fprintf(stderr, "PROOF L3 daemon close failed\n%s\n",
                     dump(daemon_log).c_str());
        return 7;
    }
    std::printf("PROOF L3 DT181 standard daemon: status=0 client=0 bytes=%zu "
                "daemon_close=0 root=%s\n", output.size(), root.c_str());
    return 0;
}
