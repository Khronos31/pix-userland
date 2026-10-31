/* SPDX-License-Identifier: MIT */
/* Offline PIX Family DT295 public-entry proof. */
#include "pix_runtime.h"

#include <libusb.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

extern "C" {
#include "dt295_stream.h"
int dt295_actual_fixture_open(const char *);
int dt295_actual_fixture_open_runtime(const char *);
void dt295_actual_fixture_close(void);
int dt295_actual_fixture_control(uint8_t, uint8_t, uint16_t, uint16_t, uint8_t *, uint16_t);
int dt295_actual_fixture_bulk(uint8_t, unsigned char *, int, int *);
int dt295_actual_fixture_interrupt(uint8_t, unsigned char *, int, int *);
void dt295_actual_fixture_bootstrap(uint8_t packets[3][188]);
void dt295_actual_fixture_encrypt(uint8_t packet[188]);
}

#ifndef PIX_FIRMWARE_DT295_PATH
#error "PIX_FIRMWARE_DT295_PATH must name the DT295 bundle"
#endif

namespace {

std::mutex g_mutex;
bool g_bootstrap_sent = false;
unsigned g_si_cc[3] = {0U, 0U, 0U};

#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, \
                         #condition);                                             \
            return 1;                                                             \
        }                                                                         \
    } while (0)

void make_si(std::uint8_t packet[188], unsigned pid, unsigned cc)
{
    std::memset(packet, 0x5a, 188U);
    packet[0] = 0x47U;
    packet[1] = static_cast<std::uint8_t>(0x40U | ((pid >> 8) & 0x1fU));
    packet[2] = static_cast<std::uint8_t>(pid & 0xffU);
    packet[3] = static_cast<std::uint8_t>(0x10U | (cc & 15U));
    packet[4] = static_cast<std::uint8_t>(0x80U + pid);
}

class Clock final : public dtv::userland::detail::ReceiverClock {
public:
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

}  // namespace

extern "C" int __wrap_usb_open_model(usb_backend **output, const usb_selector *, int, int, int)
{
    if (output == nullptr) return -1;
    *output = reinterpret_cast<usb_backend *>(static_cast<std::uintptr_t>(0x295));
    return 0;
}
extern "C" void __wrap_usb_close(usb_backend *) {}
extern "C" void *__wrap_usb_native_context(usb_backend *)
{
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x29501));
}
extern "C" void *__wrap_usb_native_handle(usb_backend *)
{
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x29502));
}
extern "C" int __wrap_libusb_control_transfer(libusb_device_handle *, std::uint8_t type,
                                               std::uint8_t request, std::uint16_t value,
                                               std::uint16_t index, unsigned char *data,
                                               std::uint16_t length, unsigned)
{
    return dt295_actual_fixture_control(type, request, value, index, data, length);
}
extern "C" int __wrap_libusb_bulk_transfer(libusb_device_handle *, unsigned char endpoint,
                                            unsigned char *data, int length, int *actual,
                                            unsigned)
{
    return dt295_actual_fixture_bulk(endpoint, data, length, actual);
}
extern "C" int __wrap_libusb_interrupt_transfer(libusb_device_handle *, unsigned char endpoint,
                                                 unsigned char *data, int length, int *actual,
                                                 unsigned)
{
    return dt295_actual_fixture_interrupt(endpoint, data, length, actual);
}
extern "C" int __wrap_random_bytes(void *output, std::size_t length)
{
    if (output != nullptr) std::memset(output, 0, length);
    return 0;
}
extern "C" int __wrap_dt295_stream_start(libusb_context *, libusb_device_handle *,
                                          dt295_stream **stream)
{
    if (stream == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bootstrap_sent = false;
    g_si_cc[0] = g_si_cc[1] = g_si_cc[2] = 0U;
    *stream = reinterpret_cast<dt295_stream *>(static_cast<std::uintptr_t>(0x296));
    return 0;
}
extern "C" int __wrap_dt295_stream_read(dt295_stream *, void *buffer, std::size_t capacity,
                                         std::uint64_t *sequence, std::uint64_t *completed_ns,
                                         unsigned)
{
    if (buffer == nullptr || capacity < 188U) return 0;
    auto *output = static_cast<std::uint8_t *>(buffer);
    std::size_t length = 0U;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_bootstrap_sent) {
        std::uint8_t packets[3][188];
        dt295_actual_fixture_bootstrap(packets);
        for (unsigned i = 0U; i < 3U && length + 188U <= capacity; ++i) {
            std::memcpy(output + length, packets[i], 188U);
            length += 188U;
        }
        g_bootstrap_sent = true;
    }
    for (unsigned i = 0U; i < 9U && length + 188U <= capacity; ++i) {
        const unsigned which = i % 3U;
        std::uint8_t packet[188];
        make_si(packet, 16U + which, g_si_cc[which]++);
        std::memcpy(output + length, packet, 188U);
        length += 188U;
    }
    if (length == 0U) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 0;
    }
    if (sequence != nullptr) *sequence += length / 188U;
    if (completed_ns != nullptr) *completed_ns = 1U;
    return static_cast<int>(length);
}
extern "C" int __wrap_dt295_stream_stop(dt295_stream *, unsigned) { return 0; }
extern "C" int __wrap_dt295_stream_destroy(dt295_stream *) { return 0; }

int main()
{
    if (dt295_actual_fixture_open_runtime(PIX_FIRMWARE_DT295_PATH) != 0) return 1;

    pix::runtime::LocatedDevice device{};
    device.model = pix::runtime::Model::dt295;
    device.fd = -1;
    auto created = pix::runtime::Family::create(device, PIX_FIRMWARE_DT295_PATH);
    if (!created) {
        std::fprintf(stderr, "FAILED Family::create error=%d\n",
                     static_cast<int>(created.error()));
        return 1;
    }
    std::unique_ptr<pix::runtime::Family> family(std::move(created.value()));
    CHECK(family->supports_dt295_scan());
    const auto idle = family->dt295_acquisition();
    CHECK(idle && idle.value().phase == pix::runtime::Dt295AcquisitionPhase::idle);

    auto& tuner = family->tuner_backend();
    CHECK(tuner.open_receiver(0U));
    CHECK(tuner.tune_terrestrial_hz(0U, 473142857ULL, 1000U));
    auto lease = family->begin_dt295_si_scan();
    CHECK(lease);

    std::uint8_t si[188U * 8U];
    bool got_si = false;
    for (unsigned attempt = 0U; attempt < 20U && !got_si; ++attempt) {
        auto read = family->read_dt295_si_scan(lease.value(),
            dtv::userland::MutableByteView{si, sizeof(si)},
            std::chrono::milliseconds(100));
        CHECK(read);
        if (read.value().bytes != 0U) {
            CHECK(read.value().bytes % 188U == 0U);
            const unsigned pid = (static_cast<unsigned>(si[1] & 0x1fU) << 8) | si[2];
            CHECK(pid == 16U || pid == 17U || pid == 18U);
            got_si = true;
        }
    }
    CHECK(got_si);
    const auto active = family->dt295_acquisition();
    CHECK(active);
    CHECK(active.value().phase == pix::runtime::Dt295AcquisitionPhase::preparing ||
          active.value().phase == pix::runtime::Dt295AcquisitionPhase::ready ||
          active.value().phase == pix::runtime::Dt295AcquisitionPhase::terminal);
    CHECK(family->end_dt295_si_scan(lease.value()));

    CHECK(tuner.start_capture(0U, px4::userland::ipc::System::ISDB_T));
    px4::userland::TunerAttachment attachment{};
    attachment.owner_client_id = 1U;
    attachment.lease_id = attachment.attachment_id = 1U;
    attachment.receiver = 0U;
    attachment.system = px4::userland::ipc::System::ISDB_T;
    auto* stream = family->stream_control();
    CHECK(stream != nullptr);
    CHECK(stream->attach(attachment));
    std::uint8_t ts[188U * 8U];
    bool got_ts = false;
    for (unsigned attempt = 0U; attempt < 100U && !got_ts; ++attempt) {
        auto read = stream->read(attachment,
            dtv::userland::MutableByteView{ts, sizeof(ts)},
            dtv::userland::Timeout{100U});
        CHECK(read);
        if (read.value().bytes != 0U) got_ts = true;
    }
    CHECK(got_ts);
    CHECK(tuner.stop_capture(0U, px4::userland::ipc::System::ISDB_T));
    CHECK(stream->detach(attachment));

    CHECK(family->shutdown());

    dt295_actual_fixture_close();
    std::fprintf(stderr, "DT295 family contract: state/scan/begin/read/end + normal OK\n");
    return 0;
}
