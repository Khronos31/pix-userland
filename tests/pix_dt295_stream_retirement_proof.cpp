// SPDX-License-Identifier: MIT
/* Offline DT295 retirement proof. Real dt295_stream.c event thread; the libusb
 * boundary is synthetic so late cancel callbacks can be delivered after the
 * adapter is gone. */
#include "pix-dt295-receiver-adapter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

extern "C" {
int dt295_actual_fixture_open(const char *);
void dt295_actual_fixture_close(void);
int dt295_actual_fixture_control(uint8_t, uint8_t, uint16_t, uint16_t, uint8_t *, uint16_t);
int dt295_actual_fixture_bulk(uint8_t, unsigned char *, int, int *);
int dt295_actual_fixture_interrupt(uint8_t, unsigned char *, int, int *);
int __real_dt295_stream_start(libusb_context *, libusb_device_handle *, dt295_stream **);
int __real_dt295_stream_stop(dt295_stream *, unsigned);
int __real_dt295_stream_destroy(dt295_stream *);
void __real_libusb_free_transfer(libusb_transfer *);
void __real_dt295_destroy(dt295 *);
void __real_dt295_routes_destroy(dt295_routes *);
void __real_dt295_assets_free(dt295_assets *);
}

namespace {

using dtv::userland::detail::ReceiverSystem;

std::mutex g_transfer_mutex;
std::vector<libusb_transfer *> g_transfers;
std::atomic<bool> g_release_callbacks{false};
std::atomic<unsigned> g_cancelled{0U};
std::atomic<unsigned> g_freed{0U};
std::atomic<unsigned> g_stops{0U};
std::atomic<unsigned> g_destroyed{0U};
std::atomic<unsigned> g_closes{0U};
std::atomic<unsigned> g_early_close{0U};
std::atomic<unsigned> g_device_freed{0U};
std::atomic<unsigned> g_routes_freed{0U};
std::atomic<unsigned> g_assets_freed{0U};
std::atomic<dt295_stream *> g_saved{nullptr};

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

bool wait_stream_done()
{
    dt295_stream *saved = g_saved.load();
    if (saved == nullptr) return false;
    for (unsigned attempt = 0U; attempt < 5000U; ++attempt) {
        dt295_stream_stats stats{};
        dt295_stream_snapshot(saved, &stats);
        if (stats.thread_done && stats.active_transfers == 0U) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool wait_cleanup(unsigned timeout_ms)
{
    for (unsigned attempt = 0U; attempt < timeout_ms; ++attempt) {
        if (g_destroyed.load() == 1U && g_closes.load() == 1U && g_freed.load() == 8U &&
            g_device_freed.load() == 1U && g_routes_freed.load() == 1U &&
            g_assets_freed.load() == 1U)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void counts(const char *stage)
{
    std::fprintf(stderr,
                 "%s: stop=%u destroy=%u close=%u early_close=%u cancel_callback=%u "
                 "transfer_free=%u device_free=%u routes_free=%u assets_free=%u\n",
                 stage, g_stops.load(), g_destroyed.load(), g_closes.load(),
                 g_early_close.load(), g_cancelled.load(), g_freed.load(),
                 g_device_freed.load(), g_routes_freed.load(), g_assets_freed.load());
}

dtv::userland::detail::Attachment attachment_for(std::uint64_t id)
{
    dtv::userland::detail::Attachment attachment{};
    attachment.owner_client_id = 7U;
    attachment.lease_id = id;
    attachment.attachment_id = id;
    attachment.receiver = 0U;
    attachment.system = ReceiverSystem::terrestrial;
    attachment.token[0] = static_cast<std::uint8_t>(id);
    return attachment;
}

}  // namespace

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "REQUIRE failed %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                            \
            return 1;                                                            \
        }                                                                        \
    } while (0)

extern "C" int __wrap_usb_open_model(usb_backend **output, const usb_selector *, int, int,
                                      int)
{
    if (output == nullptr) return -1;
    *output = reinterpret_cast<usb_backend *>(static_cast<std::uintptr_t>(0x295));
    return 0;
}

extern "C" void __wrap_usb_close(usb_backend *)
{
    dt295_stream *saved = g_saved.load();
    if (saved != nullptr) {
        dt295_stream_stats stats{};
        dt295_stream_snapshot(saved, &stats);
        if (!stats.thread_done || stats.active_transfers != 0U) ++g_early_close;
    }
    ++g_closes;
}

extern "C" void *__wrap_usb_native_context(usb_backend *)
{
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x29501));
}

extern "C" void *__wrap_usb_native_handle(usb_backend *)
{
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x29502));
}

extern "C" int __wrap_libusb_control_transfer(libusb_device_handle *, std::uint8_t request_type,
                                               std::uint8_t request, std::uint16_t value,
                                               std::uint16_t index, unsigned char *data,
                                               std::uint16_t length, unsigned int)
{
    return dt295_actual_fixture_control(request_type, request, value, index, data, length);
}

extern "C" int __wrap_libusb_bulk_transfer(libusb_device_handle *, unsigned char endpoint,
                                            unsigned char *data, int length, int *actual,
                                            unsigned int)
{
    return dt295_actual_fixture_bulk(endpoint, data, length, actual);
}

extern "C" int __wrap_libusb_interrupt_transfer(libusb_device_handle *, unsigned char endpoint,
                                                 unsigned char *data, int length, int *actual,
                                                 unsigned int)
{
    return dt295_actual_fixture_interrupt(endpoint, data, length, actual);
}

extern "C" int __wrap_random_bytes(void *output, std::size_t length)
{
    if (output != nullptr) std::memset(output, 0, length);
    return 0;
}

extern "C" int __wrap_libusb_submit_transfer(libusb_transfer *) { return 0; }

extern "C" int __wrap_libusb_cancel_transfer(libusb_transfer *transfer)
{
    std::lock_guard<std::mutex> lock(g_transfer_mutex);
    g_transfers.push_back(transfer);
    return 0;
}

extern "C" int __wrap_libusb_handle_events_timeout_completed(libusb_context *, timeval *, int *)
{
    std::vector<libusb_transfer *> ready;
    if (g_release_callbacks.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(g_transfer_mutex);
        ready.swap(g_transfers);
    }
    for (auto *transfer : ready) {
        transfer->status = LIBUSB_TRANSFER_CANCELLED;
        transfer->actual_length = 0;
        transfer->callback(transfer);
        ++g_cancelled;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 0;
}

extern "C" void __wrap_libusb_free_transfer(libusb_transfer *transfer)
{
    ++g_freed;
    __real_libusb_free_transfer(transfer);
}

extern "C" int __wrap_dt295_stream_start(libusb_context *context, libusb_device_handle *handle,
                                          dt295_stream **stream)
{
    const int rc = __real_dt295_stream_start(context, handle, stream);
    if (rc == 0 && stream != nullptr) g_saved.store(*stream);
    return rc;
}

extern "C" int __wrap_dt295_stream_stop(dt295_stream *stream, unsigned timeout_ms)
{
    ++g_stops;
    return __real_dt295_stream_stop(stream, timeout_ms);
}

extern "C" int __wrap_dt295_stream_destroy(dt295_stream *stream)
{
    const int rc = __real_dt295_stream_destroy(stream);
    if (rc == 0) {
        ++g_destroyed;
        g_saved.store(nullptr);
    }
    return rc;
}

extern "C" void __wrap_dt295_destroy(dt295 *device)
{
    ++g_device_freed;
    __real_dt295_destroy(device);
}

extern "C" void __wrap_dt295_routes_destroy(dt295_routes *routes)
{
    ++g_routes_freed;
    __real_dt295_routes_destroy(routes);
}

extern "C" void __wrap_dt295_assets_free(dt295_assets *assets)
{
    ++g_assets_freed;
    __real_dt295_assets_free(assets);
}

static int run_detach(pix::capture::dt295::ReceiverAdapter &adapter)
{
    const auto attachment = attachment_for(1U);
    REQUIRE(adapter.attach(attachment));
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
    std::atomic<bool> eof{false};
    std::thread reader([&] {
        std::uint8_t output[188U]{};
        entered.store(true, std::memory_order_release);
        const auto result = adapter.read(attachment,
            dtv::userland::MutableByteView{output, sizeof(output)},
            std::chrono::seconds(30));
        eof.store(result && result.value().eof && !result.value().timed_out,
                  std::memory_order_release);
        done.store(true, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(!done.load());
    REQUIRE(!adapter.detach(attachment));
    reader.join();
    REQUIRE(eof.load());
    const auto started = adapter.start_capture(0U, ReceiverSystem::terrestrial);
    const auto attached = adapter.attach(attachment);
    REQUIRE(!started && started.error() == dtv::userland::Error::BUSY);
    REQUIRE(!attached && attached.error() == dtv::userland::Error::BUSY);
    counts("after-failed-detach-reader-eof-and-reuse-busy");
    g_release_callbacks.store(true, std::memory_order_release);
    REQUIRE(wait_stream_done());
    REQUIRE(adapter.stop_capture(0U, ReceiverSystem::terrestrial));
    REQUIRE(adapter.stop_capture(0U, ReceiverSystem::terrestrial));
    REQUIRE(adapter.shutdown());
    counts("after-detach-late-completion-and-retry");
    REQUIRE(g_destroyed.load() == 1U && g_closes.load() == 1U && g_freed.load() == 8U);
    REQUIRE(g_device_freed.load() == 1U && g_routes_freed.load() == 1U &&
            g_assets_freed.load() == 1U);
    REQUIRE(g_early_close.load() == 0U);
    return 0;
}

static int run_retry(pix::capture::dt295::ReceiverAdapter *adapter)
{
    REQUIRE(!adapter->shutdown());
    counts("after-failed-shutdown");
    REQUIRE(g_saved.load() != nullptr);
    dt295_stream_stats stats{};
    dt295_stream_snapshot(g_saved.load(), &stats);
    REQUIRE(stats.active_transfers == 8U && !stats.thread_done);
    g_release_callbacks.store(true, std::memory_order_release);
    REQUIRE(wait_stream_done());
    REQUIRE(g_closes.load() == 0U && g_early_close.load() == 0U && g_destroyed.load() == 0U);
    REQUIRE(adapter->shutdown());
    REQUIRE(adapter->shutdown());
    counts("after-retry");
    REQUIRE(g_destroyed.load() == 1U && g_closes.load() == 1U && g_freed.load() == 8U);
    REQUIRE(g_device_freed.load() == 1U && g_routes_freed.load() == 1U &&
            g_assets_freed.load() == 1U);
    REQUIRE(g_early_close.load() == 0U);
    return 0;
}

static int run_destructor(pix::capture::dt295::ReceiverAdapter *adapter)
{
    delete adapter;
    counts("after-destructor");
    REQUIRE(g_saved.load() != nullptr);
    dt295_stream_stats stats{};
    dt295_stream_snapshot(g_saved.load(), &stats);
    REQUIRE(stats.active_transfers == 8U && !stats.thread_done);
    REQUIRE(g_stops.load() >= 2U);
    REQUIRE(g_destroyed.load() == 0U && g_closes.load() == 0U && g_early_close.load() == 0U);
    REQUIRE(g_freed.load() == 0U && g_device_freed.load() == 0U &&
            g_routes_freed.load() == 0U && g_assets_freed.load() == 0U);
    REQUIRE(!wait_cleanup(200U));
    g_release_callbacks.store(true, std::memory_order_release);
    REQUIRE(wait_cleanup(10000U));
    counts("after-late-completion-autonomous-cleanup");
    REQUIRE(g_cancelled.load() == 8U);
    REQUIRE(g_early_close.load() == 0U);
    return 0;
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    const std::string mode = argv[1];
    REQUIRE(mode == "retry" || mode == "detach" || mode == "destructor");
    char directory[] = "/tmp/pix-dt295-retirement-XXXXXX";
    REQUIRE(mkdtemp(directory) != nullptr);
    const std::string path = std::string(directory) + "/dt295.assets";
    REQUIRE(dt295_actual_fixture_open(path.c_str()) == 0);
    std::ifstream assets_file(path, std::ios::binary);
    REQUIRE(assets_file.good());
    const std::vector<std::uint8_t> assets{
        std::istreambuf_iterator<char>(assets_file), std::istreambuf_iterator<char>()};
    assets_file.close();
    REQUIRE(!assets.empty());

    Clock clock;
    usb_selector selector{};
    auto *adapter = new pix::capture::dt295::ReceiverAdapter(
        assets.data(), assets.size(), selector, clock, 0U, 2048U);
    REQUIRE(adapter != nullptr);
    REQUIRE(adapter->ready());
    REQUIRE(adapter->open(0U));
    REQUIRE(adapter->program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U));
    REQUIRE(adapter->start_capture(0U, ReceiverSystem::terrestrial));

    int rc = 0;
    if (mode == "detach")
        rc = run_detach(*adapter);
    else if (mode == "retry")
        rc = run_retry(adapter);
    else
        rc = run_destructor(adapter);
    if (mode != "destructor") delete adapter;
    if (rc == 0) {
        dt295_actual_fixture_close();
        (void)unlink(path.c_str());
        (void)rmdir(directory);
    }
    return rc;
}
