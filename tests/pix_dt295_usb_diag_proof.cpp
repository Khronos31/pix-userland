/* SPDX-License-Identifier: MIT */
/* Offline proof for the opt-in first USB failure trace. */
#include "pix-dt295-receiver-adapter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

extern "C" {
int dt295_actual_fixture_open(const char *);
void dt295_actual_fixture_close(void);
}

namespace {

struct FakeControl final {
    unsigned calls = 0U;
    std::uint8_t request_type = 0U;
    std::uint8_t request = 0U;
    std::uint16_t value = 0U;
    std::uint16_t index = 0U;
    std::uint16_t length = 0U;
    unsigned timeout_ms = 0U;
};

FakeControl g_fake_control;

int duplicate_fd(int fd)
{
#ifdef _WIN32
    return _dup(fd);
#else
    return dup(fd);
#endif
}

int duplicate_to_fd(int source, int target)
{
#ifdef _WIN32
    return _dup2(source, target);
#else
    return dup2(source, target);
#endif
}

int close_fd(int fd)
{
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

int set_diag_env(bool enabled)
{
#ifdef _WIN32
    return _putenv_s("PIX_USB_DIAG", enabled ? "1" : "");
#else
    return enabled ? setenv("PIX_USB_DIAG", "1", 1) : unsetenv("PIX_USB_DIAG");
#endif
}

std::filesystem::path fixture_directory()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("pix-dt295-usb-diag-" + std::to_string(ticks));
    std::filesystem::create_directories(path);
    return path;
}

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
    std::FILE* file = nullptr;

    Capture()
    {
        file = std::tmpfile();
        if (file == nullptr) return;
        saved = duplicate_fd(2);
        if (saved < 0) {
            std::fclose(file);
            file = nullptr;
            return;
        }
        if (duplicate_to_fd(fileno(file), 2) < 0) {
            (void)close_fd(saved);
            saved = -1;
            std::fclose(file);
            file = nullptr;
        }
    }

    std::string finish()
    {
        std::fflush(stderr);
        if (saved >= 0) {
            (void)duplicate_to_fd(saved, 2);
            (void)close_fd(saved);
            saved = -1;
        }
        std::string content;
        if (file != nullptr) {
            if (std::fseek(file, 0, SEEK_END) == 0) {
                const long length = std::ftell(file);
                if (length > 0 && std::fseek(file, 0, SEEK_SET) == 0) {
                    content.resize(static_cast<std::size_t>(length));
                    (void)std::fread(content.data(), 1U, content.size(), file);
                }
            }
            std::fclose(file);
            file = nullptr;
        }
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

extern "C" int LIBUSB_CALL libusb_control_transfer(
    libusb_device_handle*, std::uint8_t request_type, std::uint8_t request,
    std::uint16_t value, std::uint16_t index, unsigned char*, std::uint16_t length,
    unsigned int timeout_ms)
{
    ++g_fake_control.calls;
    g_fake_control.request_type = request_type;
    g_fake_control.request = request;
    g_fake_control.value = value;
    g_fake_control.index = index;
    g_fake_control.length = length;
    g_fake_control.timeout_ms = timeout_ms;
    return LIBUSB_ERROR_TIMEOUT;
}

int main()
{
    usb_selector selector{};
    const auto directory = fixture_directory();
    const auto assets_path = directory / "dt295.assets";
    CHECK(dt295_actual_fixture_open(assets_path.string().c_str()) == 0);

    (void)set_diag_env(false);
    {
        Clock clock;
        Capture capture;
        pix::capture::dt295::ReceiverAdapter adapter("/nonexistent/pix-usb-diag", selector,
                                                      clock, 0U, 1U);
        g_fake_control = {};
        CHECK(adapter.test_boot(assets_path.string().c_str()) != 0);
        const std::string output = capture.finish();
        CHECK(g_fake_control.calls == 1U);
        CHECK(output.empty());
    }

    (void)set_diag_env(true);
    {
        Clock clock;
        Capture capture;
        pix::capture::dt295::ReceiverAdapter adapter("/nonexistent/pix-usb-diag", selector,
                                                      clock, 0U, 1U);
        g_fake_control = {};
        CHECK(adapter.test_boot(assets_path.string().c_str()) != 0);
        adapter.test_usb_failure("bulk", 0x81U, -7);
        const std::string output = capture.finish();
        CHECK(g_fake_control.calls == 1U);
        CHECK(g_fake_control.request_type == 0xc0U && g_fake_control.request == 0xbcU &&
              g_fake_control.value == 0x0800U && g_fake_control.index == 0x2008U &&
              g_fake_control.length == 2U && g_fake_control.timeout_ms == 1000U);
        CHECK(lines(output) == 1U);
        CHECK(contains(output, "stage=open.boot"));
        CHECK(contains(output, "receiver=0"));
        CHECK(contains(output, "op=control"));
        CHECK(contains(output, "endpoint=0x00"));
        CHECK(contains(output, "libusb=-7"));
        CHECK(contains(output, "error=7"));
        CHECK(contains(output, "boot_error=\"cannot read DT295 firmware state\""));
        CHECK(contains(output, "request_type=0xc0 request=0xbc value=0x0800 index=0x2008"));
        CHECK(contains(output, "length=2 timeout_ms=1000"));
        CHECK(!contains(output, "payload"));
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

    (void)set_diag_env(false);
    dt295_actual_fixture_close();
    std::filesystem::remove_all(directory);
    std::fprintf(stderr, "DT295 usb diag proof: off, dedup, and stage attribution OK\n");
    return 0;
}
