/* SPDX-License-Identifier: MIT */
/* Offline proof for the opt-in first USB failure trace. */
#include "pix-dt295-receiver-adapter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

namespace {

struct Clock final : dtv::userland::detail::ReceiverClock {
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

struct Capture final {
    int saved = -1;
    std::string path;

    Capture()
    {
        char name[] = "/tmp/pix-dt295-usb-diag-XXXXXX";
        const int fd = mkstemp(name);
        if (fd < 0) return;
        path = name;
        saved = dup(STDERR_FILENO);
        if (saved < 0) {
            close(fd);
            return;
        }
        (void)dup2(fd, STDERR_FILENO);
        close(fd);
    }

    std::string finish()
    {
        std::fflush(stderr);
        if (saved >= 0) {
            (void)dup2(saved, STDERR_FILENO);
            close(saved);
            saved = -1;
        }
        std::string content;
        if (path.empty()) return content;
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (file != nullptr) {
            char buffer[4096];
            std::size_t count = 0U;
            while ((count = std::fread(buffer, 1U, sizeof(buffer), file)) != 0U)
                content.append(buffer, count);
            std::fclose(file);
        }
        (void)unlink(path.c_str());
        return content;
    }
};

unsigned lines(const std::string& text)
{
    unsigned count = 0U;
    for (const char character : text)
        if (character == '\n') ++count;
    return count;
}

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                             \
            return 1;                                                             \
        }                                                                         \
    } while (0)

}  // namespace

int main()
{
    usb_selector selector{};

    (void)unsetenv("PIX_USB_DIAG");
    {
        Clock clock;
        Capture capture;
        pix::capture::dt295::ReceiverAdapter adapter("/nonexistent/pix-usb-diag", selector,
                                                      clock, 0U, 1U);
        adapter.test_usb_stage("open.boot");
        adapter.test_usb_failure("control", 0U, -1);
        const std::string output = capture.finish();
        CHECK(output.empty());
    }

    (void)setenv("PIX_USB_DIAG", "1", 1);
    {
        Clock clock;
        Capture capture;
        pix::capture::dt295::ReceiverAdapter adapter("/nonexistent/pix-usb-diag", selector,
                                                      clock, 0U, 1U);
        adapter.test_usb_stage("open.boot");
        adapter.test_usb_failure("control", 0U, -1);
        adapter.test_usb_stage("tune.terrestrial");
        adapter.test_usb_failure("bulk", 0x81U, -7);
        const std::string output = capture.finish();
        CHECK(lines(output) == 1U);
        CHECK(contains(output, "stage=open.boot"));
        CHECK(contains(output, "receiver=0"));
        CHECK(contains(output, "op=control"));
        CHECK(contains(output, "endpoint=0x00"));
        CHECK(contains(output, "libusb=-1"));
        CHECK(contains(output, "error=7"));
        CHECK(!contains(output, "tune.terrestrial"));
    }

    {
        Clock clock;
        Capture capture;
        pix::capture::dt295::ReceiverAdapter adapter("/nonexistent/pix-usb-diag", selector,
                                                      clock, 0U, 1U);
        std::atomic<int> phase{0};
        std::thread stream_thread([&adapter, &phase] {
            while (phase.load(std::memory_order_acquire) < 1)
                std::this_thread::yield();
            adapter.test_usb_stage("stream.read");
            phase.store(2, std::memory_order_release);
        });
        adapter.test_usb_stage("card.observe");
        phase.store(1, std::memory_order_release);
        while (phase.load(std::memory_order_acquire) < 2)
            std::this_thread::yield();
        adapter.test_usb_failure("control", 0U, -1);
        stream_thread.join();
        const std::string output = capture.finish();
        CHECK(lines(output) == 1U);
        CHECK(contains(output, "stage=card.observe"));
        CHECK(!contains(output, "stage=stream.read"));
    }

    (void)unsetenv("PIX_USB_DIAG");
    std::fprintf(stderr, "DT295 usb diag proof: off, dedup, and stage attribution OK\n");
    return 0;
}
