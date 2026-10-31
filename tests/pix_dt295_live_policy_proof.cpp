/* SPDX-License-Identifier: MIT */
/* Offline DT295 ready-time live policy and same-read publication proof. */
#include "pix-dt295-receiver-adapter.h"

#include <libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
void dt295_actual_fixture_bootstrap(uint8_t packets[3][188]);
void dt295_actual_fixture_update_packet(uint8_t packet[188], unsigned version,
                                        unsigned cc, unsigned mode);
void dt295_actual_fixture_encrypt(uint8_t packet[188]);
int __real_dt295_route_write(dt295 *, const dt295_route_op *, char *, size_t);
}

namespace {

using dtv::userland::detail::ReceiverSystem;
using Adapter = pix::capture::dt295::ReceiverAdapter;
using Attachment = dtv::userland::detail::Attachment;

std::mutex g_mutex;
std::condition_variable g_cond;
bool g_bootstrap_sent = false;
bool g_chunk_ready = false;
std::vector<std::uint8_t> g_chunk;
std::atomic<unsigned> g_route_writes{0U};

struct Clock final : dtv::userland::detail::ReceiverClock {
    std::uint64_t real_ms() const noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    std::uint64_t monotonic_ms() noexcept override
    {
        if (frozen.load(std::memory_order_acquire))
            return frozen_ms.load(std::memory_order_acquire);
        return real_ms();
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
    void freeze() noexcept
    {
        frozen_ms.store(real_ms(), std::memory_order_release);
        frozen.store(true, std::memory_order_release);
    }
    std::atomic<bool> frozen{false};
    std::atomic<std::uint64_t> frozen_ms{0U};
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

Attachment attachment_for(std::uint64_t id)
{
    Attachment attachment{};
    attachment.owner_client_id = 11U;
    attachment.lease_id = attachment.attachment_id = id;
    attachment.receiver = 0U;
    attachment.system = ReceiverSystem::terrestrial;
    attachment.token[0] = static_cast<std::uint8_t>(id);
    return attachment;
}

void queue_chunk(const std::vector<std::uint8_t> &chunk)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_chunk = chunk;
    g_chunk_ready = true;
    g_cond.notify_all();
}

void append_packet(std::vector<std::uint8_t> &chunk, std::uint8_t packet[188])
{
    dt295_actual_fixture_encrypt(packet);
    chunk.insert(chunk.end(), packet, packet + 188U);
}

void append_update(std::vector<std::uint8_t> &chunk, unsigned version, unsigned mode)
{
    std::uint8_t packet[188];
    dt295_actual_fixture_update_packet(packet, version, 1U, mode);
    append_packet(chunk, packet);
}

void append_ts(std::vector<std::uint8_t> &chunk, unsigned pid, unsigned cc, unsigned tag)
{
    std::uint8_t packet[188];
    std::memset(packet, 0x5a, sizeof(packet));
    packet[0] = 0x47U;
    packet[1] = static_cast<std::uint8_t>(0x01U | ((pid >> 8) & 0x1fU));
    packet[2] = static_cast<std::uint8_t>(pid & 0xffU);
    packet[3] = static_cast<std::uint8_t>(0x10U | (cc & 15U));
    packet[4] = static_cast<std::uint8_t>(tag);
    append_packet(chunk, packet);
}

int wait_ready(Adapter &adapter)
{
    for (unsigned n = 0U; n < 3000U; ++n) {
        if (adapter.acquisition().phase == Adapter::AcquisitionPhase::ready) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 1;
}

int wait_terminal(Adapter &adapter)
{
    for (unsigned n = 0U; n < 3000U; ++n) {
        if (adapter.acquisition().phase == Adapter::AcquisitionPhase::terminal) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 1;
}

void drain(Adapter &adapter, const Attachment &attachment)
{
    std::uint8_t buffer[188U * 16U];
    for (unsigned n = 0U; n < 20U; ++n) {
        const auto result = adapter.read(attachment,
            dtv::userland::MutableByteView{buffer, sizeof(buffer)},
            std::chrono::milliseconds(20));
        if (!result || result.value().bytes == 0U || result.value().eof) return;
    }
}

struct Rig final {
    explicit Rig(const std::vector<std::uint8_t> &assets) noexcept
        : adapter(assets.data(), assets.size(), selector, clock, 0U, 2048U) {}
    Clock clock;
    usb_selector selector{};
    Adapter adapter;
};

int prepare(Rig &rig, Attachment &attachment)
{
    if (!rig.adapter.ready()) return 1;
    if (!rig.adapter.open(0U)) return 1;
    if (!rig.adapter.program_hz(0U, ReceiverSystem::terrestrial, 473142857ULL, 1000U))
        return 1;
    if (!rig.adapter.start_capture(0U, ReceiverSystem::terrestrial)) return 1;
    attachment = attachment_for(200U);
    if (!rig.adapter.attach(attachment)) return 1;
    if (wait_ready(rig.adapter) != 0) return 1;
    rig.clock.freeze();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_chunk_ready = false;
        g_chunk.clear();
    }
    drain(rig.adapter, attachment);
    return 0;
}

int case_same_metadata(const std::vector<std::uint8_t> &assets)
{
    Rig rig(assets);
    Attachment attachment{};
    CHECK(prepare(rig, attachment) == 0);
    std::vector<std::uint8_t> chunk;
    append_update(chunk, 1U, 0U);
    append_ts(chunk, 0x101U, 1U, 0x31U);
    const unsigned writes = g_route_writes.load();
    queue_chunk(chunk);
    std::uint8_t buffer[188U * 4U];
    const auto read = rig.adapter.read(attachment,
        dtv::userland::MutableByteView{buffer, sizeof(buffer)},
        std::chrono::milliseconds(200));
    CHECK(read && read.value().bytes != 0U && !read.value().eof);
    CHECK(rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready);
    CHECK(g_route_writes.load() == writes);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "same stop");
    CHECK_RESULT(rig.adapter.detach(attachment), "same detach");
    CHECK_RESULT(rig.adapter.close(0U), "same close");
    return 0;
}

int case_safe_addition(const std::vector<std::uint8_t> &assets)
{
    Rig rig(assets);
    Attachment attachment{};
    CHECK(prepare(rig, attachment) == 0);
    std::vector<std::uint8_t> chunk;
    append_update(chunk, 1U, 1U);
    append_ts(chunk, 0x111U, 0U, 0x41U);
    const unsigned writes = g_route_writes.load();
    queue_chunk(chunk);
    std::uint8_t buffer[188U * 4U];
    bool saw_new = false;
    for (unsigned n = 0U; n < 10U; ++n) {
        const auto read = rig.adapter.read(attachment,
            dtv::userland::MutableByteView{buffer, sizeof(buffer)},
            std::chrono::milliseconds(50));
        if (!read || read.value().bytes == 0U) break;
        for (std::size_t at = 0U; at + 188U <= read.value().bytes; at += 188U) {
            const unsigned pid =
                (static_cast<unsigned>(buffer[at + 1] & 0x1fU) << 8) | buffer[at + 2];
            if (pid == 0x111U) saw_new = true;
        }
    }
    CHECK(saw_new);
    CHECK(rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready);
    CHECK(g_route_writes.load() == writes + 1U);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "add stop");
    CHECK_RESULT(rig.adapter.detach(attachment), "add detach");
    CHECK_RESULT(rig.adapter.close(0U), "add close");
    return 0;
}

int case_rejected_update(const std::vector<std::uint8_t> &assets, unsigned mode,
                         const char *what)
{
    Rig rig(assets);
    Attachment attachment{};
    CHECK(prepare(rig, attachment) == 0);
    std::vector<std::uint8_t> chunk;
    append_update(chunk, 1U, mode);
    append_ts(chunk, 0x101U, 1U, 0x51U);
    const unsigned writes = g_route_writes.load();
    queue_chunk(chunk);
    CHECK(wait_terminal(rig.adapter) == 0);
    std::uint8_t buffer[188U * 4U];
    const auto read = rig.adapter.read(attachment,
        dtv::userland::MutableByteView{buffer, sizeof(buffer)},
        std::chrono::milliseconds(100));
    CHECK(read && read.value().bytes == 0U && read.value().eof);
    CHECK(g_route_writes.load() == writes);
    std::fprintf(stderr, "live-reject %s: phase=%u failure=%u writes=%u\n", what,
                 static_cast<unsigned>(rig.adapter.acquisition().phase),
                 static_cast<unsigned>(rig.adapter.acquisition().failure),
                 g_route_writes.load() - writes);
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.close(0U), "reject close");
    return 0;
}

int case_split_pmt(const std::vector<std::uint8_t> &assets)
{
    /* The same dangerous PMT section is split at a byte boundary across two
     * TS packets in two reads. The first packed fragment can coexist with a
     * TS; the final fragment completes the update and a following TS must not
     * be published. */
    Rig rig(assets);
    Attachment attachment{};
    CHECK(prepare(rig, attachment) == 0);
    std::uint8_t full[188];
    dt295_actual_fixture_update_packet(full, 1U, 0U, 3U);
    const size_t section_size =
        3U + (size_t)(((full[6] & 0x0fU) << 8) | full[7]);
    const size_t split = section_size > 8U ? 8U : section_size / 2U;
    std::uint8_t first[188];
    std::memset(first, 0xff, sizeof(first));
    first[0] = 0x47U;
    first[1] = 0x41U;
    first[2] = 0x00U;
    first[3] = 0x31U;
    const size_t first_payload = 1U + split;
    first[4] = static_cast<std::uint8_t>(183U - first_payload);
    const size_t first_at = 5U + first[4];
    first[5] = 0U;
    first[first_at] = 0U;
    std::memcpy(first + first_at + 1U, full + 5, split);
    std::vector<std::uint8_t> partial(first, first + 188U);
    append_ts(partial, 0x101U, 1U, 0x61U);
    queue_chunk(partial);
    std::uint8_t buffer[188U * 4U];
    const auto before = rig.adapter.read(attachment,
        dtv::userland::MutableByteView{buffer, sizeof(buffer)},
        std::chrono::milliseconds(200));
    CHECK(before && before.value().bytes != 0U && !before.value().eof);
    std::uint8_t second[188];
    std::memset(second, 0xff, sizeof(second));
    second[0] = 0x47U;
    second[1] = 0x01U;
    second[2] = 0x00U;
    second[3] = 0x32U;
    const size_t second_payload = section_size - split;
    second[4] = static_cast<std::uint8_t>(183U - second_payload);
    const size_t second_at = 5U + second[4];
    second[5] = 0U;
    std::memcpy(second + second_at, full + 5 + split, second_payload);
    std::vector<std::uint8_t> completion(second, second + 188U);
    append_ts(completion, 0x101U, 2U, 0x62U);
    queue_chunk(completion);
    std::uint8_t after_buffer[188U * 4U];
    const auto after = rig.adapter.read(attachment,
        dtv::userland::MutableByteView{after_buffer, sizeof(after_buffer)},
        std::chrono::milliseconds(100));
    CHECK(after && after.value().bytes == 0U && after.value().eof);
    std::fprintf(stderr, "live-split-pmt: before_bytes=%zu after_bytes=%zu terminal=%u\n",
                 before.value().bytes, after.value().bytes,
                 static_cast<unsigned>(rig.adapter.acquisition().phase));
    (void)rig.adapter.detach(attachment);
    CHECK_RESULT(rig.adapter.close(0U), "split close");
    return 0;
}

int case_split_safe_addition(const std::vector<std::uint8_t> &assets)
{
    Rig rig(assets);
    Attachment attachment{};
    CHECK(prepare(rig, attachment) == 0);
    std::uint8_t full[188];
    dt295_actual_fixture_update_packet(full, 1U, 0U, 1U);
    const size_t section_size = 3U + (size_t)(((full[6] & 0x0fU) << 8) | full[7]);
    const size_t split = section_size > 8U ? 8U : section_size / 2U;
    std::uint8_t first[188];
    std::memset(first, 0xff, sizeof(first));
    first[0] = 0x47U;
    first[1] = 0x41U;
    first[2] = 0x00U;
    first[3] = 0x31U;
    first[4] = static_cast<std::uint8_t>(183U - (1U + split));
    const size_t first_at = 5U + first[4];
    first[5] = 0U;
    first[first_at] = 0U;
    std::memcpy(first + first_at + 1U, full + 5, split);
    std::vector<std::uint8_t> partial(first, first + 188U);
    append_ts(partial, 0x101U, 1U, 0x71U);
    queue_chunk(partial);
    std::uint8_t buffer[188U * 4U];
    const auto before = rig.adapter.read(attachment,
        dtv::userland::MutableByteView{buffer, sizeof(buffer)},
        std::chrono::milliseconds(200));
    CHECK(before && before.value().bytes != 0U && !before.value().eof);
    std::uint8_t second[188];
    std::memset(second, 0xff, sizeof(second));
    second[0] = 0x47U;
    second[1] = 0x01U;
    second[2] = 0x00U;
    second[3] = 0x32U;
    const size_t second_payload = section_size - split;
    second[4] = static_cast<std::uint8_t>(183U - second_payload);
    const size_t second_at = 5U + second[4];
    second[5] = 0U;
    std::memcpy(second + second_at, full + 5 + split, second_payload);
    std::vector<std::uint8_t> completion(second, second + 188U);
    append_ts(completion, 0x111U, 0U, 0x72U);
    const unsigned writes = g_route_writes.load();
    queue_chunk(completion);
    bool saw_new = false;
    for (unsigned n = 0U; n < 10U; ++n) {
        const auto read = rig.adapter.read(attachment,
            dtv::userland::MutableByteView{buffer, sizeof(buffer)},
            std::chrono::milliseconds(50));
        if (!read || read.value().bytes == 0U) break;
        for (std::size_t at = 0U; at + 188U <= read.value().bytes; at += 188U) {
            const unsigned pid =
                (static_cast<unsigned>(buffer[at + 1] & 0x1fU) << 8) | buffer[at + 2];
            if (pid == 0x111U) saw_new = true;
        }
    }
    CHECK(saw_new);
    CHECK(rig.adapter.acquisition().phase == Adapter::AcquisitionPhase::ready);
    CHECK(g_route_writes.load() == writes + 1U);
    CHECK_RESULT(rig.adapter.stop_capture(0U, ReceiverSystem::terrestrial), "split add stop");
    CHECK_RESULT(rig.adapter.detach(attachment), "split add detach");
    CHECK_RESULT(rig.adapter.close(0U), "split add close");
    return 0;
}

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
extern "C" int __wrap_dt295_route_write(dt295 *device, const dt295_route_op *op,
                                         char *error, size_t error_size)
{
    g_route_writes.fetch_add(1U, std::memory_order_acq_rel);
    return __real_dt295_route_write(device, op, error, error_size);
}

extern "C" int __wrap_dt295_stream_start(libusb_context *, libusb_device_handle *,
                                          dt295_stream **stream)
{
    if (stream == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bootstrap_sent = false;
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
    std::unique_lock<std::mutex> lock(g_mutex);
    if (!g_bootstrap_sent) {
        std::uint8_t packets[3][188];
        dt295_actual_fixture_bootstrap(packets);
        for (unsigned i = 0U; i < 3U && length + 188U <= capacity; ++i) {
            std::memcpy(output + length, packets[i], 188U);
            dt295_actual_fixture_encrypt(output + length);
            length += 188U;
        }
        g_bootstrap_sent = true;
    }
    if (length == 0U && !g_chunk_ready) {
        g_cond.wait_for(lock, std::chrono::milliseconds(5),
                        [&] { return g_chunk_ready; });
    }
    if (g_chunk_ready) {
        const std::size_t copy = std::min(capacity - length, g_chunk.size());
        std::memcpy(output + length, g_chunk.data(), copy);
        length += copy;
        g_chunk.clear();
        g_chunk_ready = false;
    }
    if (length == 0U) return 0;
    if (sequence != nullptr) *sequence += length / 188U;
    if (completed_ns != nullptr) *completed_ns = 1U;
    return static_cast<int>(length);
}

extern "C" int __wrap_dt295_stream_stop(dt295_stream *, unsigned) { return 0; }
extern "C" int __wrap_dt295_stream_destroy(dt295_stream *) { return 0; }

int main(int argc, char **argv)
{
    char directory[] = "/tmp/pix-dt295-live-XXXXXX";
    if (mkdtemp(directory) == nullptr) return 1;
    const std::string path = std::string(directory) + "/dt295.assets";
    if (dt295_actual_fixture_open(path.c_str()) != 0) return 1;
    std::ifstream file(path, std::ios::binary);
    if (!file.good()) return 1;
    const std::vector<std::uint8_t> assets{
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    file.close();
    if (assets.empty()) return 1;

    const std::string only = argc > 1 ? argv[1] : std::string();
    const auto run = [&](const char *name, const auto &body) {
        if (!only.empty() && only != name) return true;
        if (body() != 0) {
            std::fprintf(stderr, "CASE failed: %s\n", name);
            return false;
        }
        return true;
    };
    CHECK(run("same-metadata", [&] { return case_same_metadata(assets); }));
    CHECK(run("safe-addition", [&] { return case_safe_addition(assets); }));
    CHECK(run("new-ecm", [&] { return case_rejected_update(assets, 2U, "new-ecm"); }));
    CHECK(run("ecm-rebind", [&] { return case_rejected_update(assets, 3U, "ecm-rebind"); }));
    CHECK(run("metadata-only", [&] { return case_rejected_update(assets, 4U, "metadata-only"); }));
    CHECK(run("split-pmt", [&] { return case_split_pmt(assets); }));
    CHECK(run("split-safe", [&] { return case_split_safe_addition(assets); }));

    dt295_actual_fixture_close();
    (void)unlink(path.c_str());
    (void)rmdir(directory);
    std::fprintf(stderr, "DT295 live policy: safe/reject/split OK\n");
    return 0;
}
