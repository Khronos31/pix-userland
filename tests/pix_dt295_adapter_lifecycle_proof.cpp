/* Offline DT295 incremental adapter lifecycle proof. */
#include "pix-dt295-receiver-adapter.h"

#include <libusb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

extern "C" {
int dt295_actual_fixture_open(const char *);
void dt295_actual_fixture_close(void);
int dt295_actual_fixture_control(uint8_t, uint8_t, uint16_t, uint16_t, uint8_t *, uint16_t);
int dt295_actual_fixture_bulk(uint8_t, unsigned char *, int, int *);
int dt295_actual_fixture_interrupt(uint8_t, unsigned char *, int, int *);
void dt295_actual_fixture_bootstrap(uint8_t packets[3][188]);
void dt295_actual_fixture_recording_packet(uint8_t packet[188], unsigned continuity);
void dt295_actual_fixture_encrypt(uint8_t packet[188]);
}

namespace {

using dtv::userland::Error;
using dtv::userland::detail::Attachment;
using dtv::userland::detail::ReceiverSystem;

struct FakeStream final {
    bool bootstrap_sent = false;
    unsigned continuity = 0U;
    unsigned generations = 0U;
};

FakeStream g_stream;
constexpr unsigned kContinuityPerRead = 16U;

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

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                             \
            return 1;                                                             \
        }                                                                         \
    } while (0)

#define CHECK_RESULT(result, what)                                             \
    do {                                                                       \
        if (!(result)) {                                                       \
            std::fprintf(stderr, "FAILED %s error=%d\n", what,                 \
                         static_cast<int>((result).error()));                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)

void drain_attachment(pix::capture::dt295::ReceiverAdapter& adapter,
                      const Attachment& attachment)
{
    std::uint8_t buffer[188U * 64U];
    for (unsigned attempt = 0U; attempt < 200U; ++attempt) {
        const auto result = adapter.read(attachment,
            dtv::userland::MutableByteView{buffer, sizeof(buffer)},
            std::chrono::milliseconds(5));
        if (!result || result.value().eof || result.value().timed_out) return;
    }
}

bool read_ts(pix::capture::dt295::ReceiverAdapter& adapter, const Attachment& attachment,
             std::size_t minimum)
{
    std::uint8_t buffer[188U * 64U];
    std::size_t total = 0U;
    for (unsigned attempt = 0U; attempt < 40U && total < minimum; ++attempt) {
        const auto result = adapter.read(attachment,
            dtv::userland::MutableByteView{buffer, sizeof(buffer)},
            std::chrono::milliseconds(100));
        if (!result) return false;
        if (result.value().bytes == 0U) continue;
        total += result.value().bytes;
        for (std::size_t offset = 0U; offset + 188U <= result.value().bytes; offset += 188U)
            if (buffer[offset] != 0x47U) return false;
    }
    return total >= minimum;
}

Attachment attachment_for(std::uint64_t id)
{
    Attachment attachment{};
    attachment.owner_client_id = 7U;
    attachment.lease_id = id;
    attachment.attachment_id = id;
    attachment.receiver = 0U;
    attachment.system = ReceiverSystem::terrestrial;
    attachment.token[0] = static_cast<std::uint8_t>(id);
    return attachment;
}

}  // namespace

extern "C" int __wrap_usb_open_model(usb_backend** output, const usb_selector*, int, int,
                                      int model)
{
    if (output == nullptr || model != USB_MODEL_DT295) return -1;
    *output = reinterpret_cast<usb_backend*>(static_cast<std::uintptr_t>(0x295));
    return 0;
}

extern "C" void __wrap_usb_close(usb_backend*) {}
extern "C" void* __wrap_usb_native_context(usb_backend*)
{
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x29501));
}
extern "C" void* __wrap_usb_native_handle(usb_backend*)
{
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x29502));
}

extern "C" int __wrap_libusb_control_transfer(libusb_device_handle*, std::uint8_t request_type,
                                               std::uint8_t request, std::uint16_t value,
                                               std::uint16_t index, unsigned char* data,
                                               std::uint16_t length, unsigned int)
{
    return dt295_actual_fixture_control(request_type, request, value, index, data, length);
}

extern "C" int __wrap_libusb_bulk_transfer(libusb_device_handle*, unsigned char endpoint,
                                            unsigned char* data, int length, int* actual,
                                            unsigned int)
{
    return dt295_actual_fixture_bulk(endpoint, data, length, actual);
}

extern "C" int __wrap_libusb_interrupt_transfer(libusb_device_handle*, unsigned char endpoint,
                                                 unsigned char* data, int length, int* actual,
                                                 unsigned int)
{
    return dt295_actual_fixture_interrupt(endpoint, data, length, actual);
}

extern "C" int __wrap_dt295_stream_start(libusb_context*, libusb_device_handle*,
                                          dt295_stream** stream)
{
    if (stream == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    g_stream.bootstrap_sent = false;
    g_stream.continuity = 0U;
    ++g_stream.generations;
    *stream = reinterpret_cast<dt295_stream*>(&g_stream);
    return 0;
}

extern "C" int __wrap_dt295_stream_read(dt295_stream*, void* buffer, std::size_t capacity,
                                         std::uint64_t* sequence, std::uint64_t* completed_ns,
                                         unsigned)
{
    if (buffer == nullptr || capacity < 188U) return 0;
    auto* output = static_cast<std::uint8_t*>(buffer);
    std::size_t length = 0U;
    if (!g_stream.bootstrap_sent) {
        std::uint8_t packets[3][188];
        dt295_actual_fixture_bootstrap(packets);
        for (unsigned index = 0U; index < 3U && length + 188U <= capacity; ++index) {
            std::memcpy(output + length, packets[index], 188U);
            dt295_actual_fixture_encrypt(output + length);
            length += 188U;
        }
        g_stream.bootstrap_sent = true;
    }
    for (unsigned index = 0U; index < kContinuityPerRead && length + 188U <= capacity;
         ++index) {
        std::uint8_t packet[188];
        dt295_actual_fixture_recording_packet(packet, g_stream.continuity++);
        dt295_actual_fixture_encrypt(packet);
        std::memcpy(output + length, packet, 188U);
        length += 188U;
    }
    if (length == 0U) return 0;
    if (sequence != nullptr) *sequence += length / 188U;
    if (completed_ns != nullptr) *completed_ns = 1U;
    return static_cast<int>(length);
}

extern "C" int __wrap_random_bytes(void* output, std::size_t length)
{
    if (output != nullptr) std::memset(output, 0, length);
    return 0;
}

extern "C" int __wrap_dt295_stream_stop(dt295_stream*, unsigned) { return 0; }
extern "C" int __wrap_dt295_stream_destroy(dt295_stream*) { return 0; }

int main()
{
    char directory[] = "/tmp/pix-dt295-adapter-lifecycle-XXXXXX";
    if (mkdtemp(directory) == nullptr) {
        std::fprintf(stderr, "FAILED mkdtemp\n");
        return 1;
    }
    const std::string assets_path = std::string(directory) + "/dt295.assets";
    CHECK(dt295_actual_fixture_open(assets_path.c_str()) == 0);
    {
        std::FILE* file = std::fopen(assets_path.c_str(), "rb");
        CHECK(file != nullptr);
        std::fclose(file);
    }

    Clock clock;
    usb_selector selector{};
    pix::capture::dt295::ReceiverAdapter adapter(directory, selector, clock, 0U, 2048U);
    if (!adapter.ready()) {
        std::fprintf(stderr, "FAILED adapter init_error=%d\n",
                     static_cast<int>(adapter.init_error()));
        return 1;
    }
    CHECK_RESULT(adapter.open(0U), "open");

    Attachment first = attachment_for(1U);
    CHECK_RESULT(adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U),
                 "tune T13");
    CHECK_RESULT(adapter.start_capture(0U, ReceiverSystem::terrestrial), "start_capture");
    CHECK_RESULT(adapter.attach(first), "attach");
    CHECK(read_ts(adapter, first, 188U * 8U));
    CHECK_RESULT(adapter.stop_capture(0U, ReceiverSystem::terrestrial), "stop_capture");
    CHECK_RESULT(adapter.detach(first), "detach");
    drain_attachment(adapter, first);

    Attachment second = attachment_for(2U);
    CHECK_RESULT(adapter.program_hz(0U, ReceiverSystem::terrestrial, 479142857ULL, 1000U),
                 "retune T14");
    CHECK_RESULT(adapter.start_capture(0U, ReceiverSystem::terrestrial), "restart_capture");
    CHECK_RESULT(adapter.attach(second), "reattach");
    CHECK(read_ts(adapter, second, 188U * 8U));
    CHECK_RESULT(adapter.detach(second), "redetach");
    drain_attachment(adapter, second);
    CHECK_RESULT(adapter.stop_capture(0U, ReceiverSystem::terrestrial), "restop_capture");
    CHECK_RESULT(adapter.close(0U), "close");
    CHECK_RESULT(adapter.shutdown(), "shutdown");

    CHECK(g_stream.generations >= 2U);
    dt295_actual_fixture_close();
    std::fprintf(stderr, "DT295 adapter lifecycle: open->tune->start->TS->stop->retune OK\n");
    return 0;
}
