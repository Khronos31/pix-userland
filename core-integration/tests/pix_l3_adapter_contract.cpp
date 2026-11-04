// SPDX-License-Identifier: GPL-2.0-or-later
#define _POSIX_C_SOURCE 200809L
#include "pix_l3_adapter.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

struct Pending final {
    uint64_t id = 0U;
    dtv_l3_provider_binding *binding = nullptr;
    dtv_l3_transfer_complete_fn complete = nullptr;
    void *callback_context = nullptr;
};

struct FakeBackend;

FakeBackend *fake(void *context);

struct FakeBackend final {
    uint64_t claim_calls = 0U;
    uint64_t release_calls = 0U;
    uint64_t transfer_calls = 0U;
    uint64_t tune_calls = 0U;
    uint64_t configure_calls = 0U;
    uint64_t bind_calls = 0U;
    uint64_t initialize_calls = 0U;
    uint64_t async_submit_calls = 0U;
    uint64_t cancel_calls = 0U;
    uint32_t seen_model = 0U;
    uint64_t seen_selector = 0U;
    dtv_l3_context *bound_owner = nullptr;
    dtv_l3_claim_result claim_result{DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    dtv_l3_cancel_scope cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE;
    dtv_l3_result cancel_result = DTV_L3_ACCEPTED;
    bool async = false;
    std::vector<dtv_l3_release_result> releases;
    std::vector<dtv_l3_result> tunes;
    std::vector<std::uint8_t> last_out;
    std::vector<std::uint8_t> seen_firmware;
    std::vector<Pending> pending;
    std::uint8_t fill = 0x47U;
};

dtv_l3_result async_submit(void *context, dtv_l3_provider_binding *binding,
                           uint64_t transfer_id,
                           const dtv_l3_transfer_request *request,
                           dtv_l3_transfer_complete_fn complete,
                           void *callback_context)
{
    FakeBackend *value = fake(context);
    ++value->async_submit_calls;
    if (request->out_size != 0U && request->out_data != nullptr) {
        value->last_out.assign(request->out_data,
                               request->out_data + request->out_size);
    }
    Pending pending{};
    pending.id = transfer_id;
    pending.binding = binding;
    pending.complete = complete;
    pending.callback_context = callback_context;
    value->pending.push_back(pending);
    return DTV_L3_ACCEPTED;
}

dtv_l3_result async_cancel(void *context, dtv_l3_provider_binding *binding,
                           uint64_t transfer_id)
{
    (void)binding;
    (void)transfer_id;
    FakeBackend *value = fake(context);
    ++value->cancel_calls;
    return value->cancel_result;
}

dtv_l3_result product_bind(void *context, dtv_l3_context *owner)
{
    FakeBackend *value = fake(context);
    ++value->bind_calls;
    value->bound_owner = owner;
    return DTV_L3_OK;
}

dtv_l3_result product_initialize(void *context, dtv_l3_context *owner)
{
    FakeBackend *value = fake(context);
    ++value->initialize_calls;
    value->bound_owner = owner;
    return DTV_L3_OK;
}

bool wait_pending(FakeBackend &backend, std::size_t count)
{
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (backend.pending.size() >= count) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool complete_pending(FakeBackend &backend, uint64_t id, dtv_l3_result result,
                      std::uint8_t fill)
{
    for (std::size_t i = 0U; i < backend.pending.size(); ++i) {
        if (backend.pending[i].id != id) {
            continue;
        }
        const Pending pending = backend.pending[i];
        backend.pending.erase(backend.pending.begin() +
                              static_cast<std::ptrdiff_t>(i));
        std::uint8_t data[188];
        for (std::size_t j = 0U; j < sizeof(data); ++j) {
            data[j] = fill;
        }
        dtv_l3_transfer_completion completion{};
        completion.result = result;
        if (result == DTV_L3_OK) {
            completion.data = data;
            completion.size = sizeof(data);
            completion.actual_length = sizeof(data);
            completion.actual_length_known = 1U;
        }
        pending.complete(pending.binding, pending.id, &completion,
                         pending.callback_context);
        return true;
    }
    return false;
}

FakeBackend *fake(void *context)
{
    return static_cast<FakeBackend *>(context);
}

dtv_l3_claim_result claim(void *context, std::uint32_t claim_index)
{
    (void)claim_index;
    FakeBackend *value = fake(context);
    ++value->claim_calls;
    return value->claim_result;
}

dtv_l3_release_result release(void *context, std::uint32_t claim_index)
{
    (void)claim_index;
    FakeBackend *value = fake(context);
    const std::size_t index = static_cast<std::size_t>(value->release_calls++);
    if (index < value->releases.size()) {
        return value->releases[index];
    }
    return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
            DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result transfer(void *context, std::uint64_t transfer_id,
                       const pix_l3_transfer *request,
                       std::uint8_t *in_buffer, std::size_t in_capacity,
                       pix_l3_transfer_result *completion)
{
    (void)transfer_id;
    FakeBackend *value = fake(context);
    ++value->transfer_calls;
    if (request->out_size != 0U && request->out_data != nullptr) {
        value->last_out.assign(request->out_data,
                               request->out_data + request->out_size);
    }
    if (request->in_capacity == 0U) {
        completion->result = DTV_L3_OK;
        completion->actual_length = request->out_size;
        completion->actual_length_known = 1U;
        return DTV_L3_OK;
    }
    if (in_buffer == nullptr || in_capacity < request->in_capacity) {
        return DTV_L3_INVALID;
    }
    for (std::size_t i = 0U; i < request->in_capacity; ++i) {
        in_buffer[i] = value->fill;
    }
    completion->result = DTV_L3_OK;
    completion->data = in_buffer;
    completion->size = request->in_capacity;
    completion->actual_length = request->in_capacity;
    completion->actual_length_known = 1U;
    return DTV_L3_OK;
}

dtv_l3_result cancel(void *context, std::uint64_t transfer_id)
{
    (void)context;
    (void)transfer_id;
    return DTV_L3_UNSUPPORTED;
}

dtv_l3_result configure(void *context, std::uint32_t model,
                        std::uint64_t selector, const std::uint8_t *firmware,
                        std::size_t firmware_size)
{
    FakeBackend *value = fake(context);
    ++value->configure_calls;
    value->seen_model = model;
    value->seen_selector = selector;
    if (firmware != nullptr && firmware_size != 0U) {
        value->seen_firmware.assign(firmware, firmware + firmware_size);
    }
    return DTV_L3_OK;
}

dtv_l3_result tune(void *context, dtv_l3_provider_binding *binding,
                   std::uint32_t receiver_id, std::uint64_t requested_hz,
                   std::uint64_t *programmed_hz)
{
    (void)binding;
    FakeBackend *value = fake(context);
    ++value->tune_calls;
    const std::size_t index = static_cast<std::size_t>(value->tune_calls - 1U);
    if (index < value->tunes.size() && value->tunes[index] != DTV_L3_OK) {
        return value->tunes[index];
    }
    if (receiver_id != 0U) {
        return DTV_L3_UNSUPPORTED;
    }
    *programmed_hz = requested_hz;
    return DTV_L3_OK;
}

pix_l3_usb_backend_ops usb_ops_for(FakeBackend *backend)
{
    pix_l3_usb_backend_ops ops{};
    ops.context = backend;
    ops.cancel_scope = backend->cancel_scope;
    ops.claim = claim;
    ops.release_claim = release;
    if (backend->async) {
        ops.submit = async_submit;
        ops.cancel_async = async_cancel;
        ops.cancel_supported = backend->cancel_result != DTV_L3_UNSUPPORTED;
    } else {
        ops.transfer = transfer;
        ops.cancel = cancel;
        ops.cancel_supported = 0U;
    }
    return ops;
}

pix_l3_product_ops product_ops_for(FakeBackend *backend)
{
    pix_l3_product_ops ops{};
    ops.context = backend;
    ops.configure = configure;
    ops.bind = product_bind;
    ops.initialize = product_initialize;
    ops.tune = tune;
    ops.scope_query = nullptr;
    return ops;
}

dtv_l3_result read_firmware(void *context, const std::uint8_t **data,
                            std::size_t *size)
{
    static const std::uint8_t bytes[4] = {0x11U, 0x22U, 0x33U, 0x44U};
    (void)context;
    *data = bytes;
    *size = sizeof(bytes);
    return DTV_L3_OK;
}

struct Session final {
    pix_l3_factory *factory = nullptr;
    dtv_l3_usb_provider usb{};
    dtv_l3_adapter_factory adapter_factory{};
    dtv_l3_context *owner = nullptr;
    dtv_l3_op *open_op = nullptr;

    bool start(FakeBackend &backend, pix_l3_model model,
               const dtv_l3_firmware &firmware)
    {
        pix_l3_factory_config config{};
        config.model = model;
        config.usb = usb_ops_for(&backend);
        config.product = product_ops_for(&backend);
        if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK ||
            pix_l3_factory_usb_provider(factory, &usb) != DTV_L3_OK ||
            pix_l3_factory_adapter_factory(factory, &adapter_factory) !=
                DTV_L3_OK) {
            return false;
        }
        dtv_l3_open_config open_config{};
        open_config.model = static_cast<std::uint32_t>(model);
        open_config.selector = 0x55U;
        open_config.firmware = &firmware;
        const dtv_l3_open_result opened =
            dtv_l3_open_factory(&usb, &adapter_factory, &open_config, 1U);
        owner = opened.owner;
        open_op = opened.op;
        if (opened.result != DTV_L3_ACCEPTED || owner == nullptr ||
            open_op == nullptr) {
            return false;
        }
        dtv_l3_op_result result{};
        if (dtv_l3_op_wait(open_op, 3000U) != DTV_L3_OK ||
            dtv_l3_op_poll(open_op, &result) != DTV_L3_OK ||
            result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK) {
            return false;
        }
        dtv_l3_open_status status{};
        return dtv_l3_open_status_get(owner, &status) == DTV_L3_OK &&
               status.state == DTV_L3_OPEN_READY;
    }

    bool finish()
    {
        if (open_op != nullptr) {
            dtv_l3_op_release(open_op);
            open_op = nullptr;
        }
        bool released = owner == nullptr;
        for (int attempt = 0; owner != nullptr && attempt < 500; ++attempt) {
            dtv_l3_close(owner, 100U);
            dtv_l3_close_status status{};
            if (dtv_l3_close_status_get(owner, &status) == DTV_L3_OK &&
                status.cleanup_complete != 0U &&
                status.physical_quiesced == 0U) {
                dtv_l3_cleanup_retry(owner);
            }
            const dtv_l3_result result = dtv_l3_context_release(owner);
            if (result == DTV_L3_OK || result == DTV_L3_INVALID) {
                owner = nullptr;
                released = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (factory != nullptr) {
            pix_l3_factory_destroy(factory);
            factory = nullptr;
        }
        return released;
    }
};

bool wait_op(dtv_l3_op *op)
{
    if (dtv_l3_op_wait(op, 3000U) != DTV_L3_OK) {
        return false;
    }
    dtv_l3_op_result result{};
    return dtv_l3_op_poll(op, &result) == DTV_L3_OK &&
           (result.state == DTV_L3_OP_DONE ||
            result.state == DTV_L3_OP_FAILED);
}

dtv_l3_firmware firmware_bytes(const std::uint8_t *bytes, std::size_t size)
{
    dtv_l3_firmware firmware{};
    firmware.source = DTV_L3_FIRMWARE_BYTES;
    firmware.bytes = bytes;
    firmware.size = size;
    return firmware;
}

int common_capabilities()
{
    FakeBackend backend;
    const std::uint8_t image[2] = {0x01U, 0x02U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT295, firmware)) {
        return 1;
    }
    dtv_l3_capabilities common{};
    if (dtv_l3_capabilities_get(session.owner, &common) != DTV_L3_OK ||
        common.receiver_count != 1U || common.reader_count != 1U ||
        common.lnb_power != 0U || common.serial != 0U ||
        common.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE) {
        return 2;
    }
    const dtv_l3_capability_catalog *catalog = nullptr;
    if (dtv_l3_capabilities_get_catalog(session.owner, &catalog) !=
            DTV_L3_OK ||
        catalog == nullptr || catalog->receiver_count != 1U ||
        catalog->readers == nullptr || catalog->reader_count != 1U ||
        catalog->receivers[0].systems !=
            (DTV_L3_SYSTEM_ISDB_T | DTV_L3_SYSTEM_ISDB_S) ||
        catalog->readers[0].external_apdu != DTV_L3_SUPPORT_UNSUPPORTED ||
        catalog->readers[0].support != DTV_L3_SUPPORT_SUPPORTED ||
        catalog->lnb_scope_count != 0U) {
        return 6;
    }
    if (backend.bind_calls != 1U || backend.initialize_calls != 1U ||
        backend.bound_owner != session.owner) {
        return 7;
    }
    dtv_l3_reader *reader = nullptr;
    if (dtv_l3_reader_acquire(session.owner, 0U, &reader) !=
        DTV_L3_UNSUPPORTED) {
        return 3;
    }
    if (dtv_l3_lnb_set(session.owner, 0U, 1U) != DTV_L3_UNSUPPORTED) {
        return 4;
    }
    if (!session.finish()) {
        return 5;
    }
    return 0;
}

int factory_capability_differences()
{
    FakeBackend backend;
    const std::uint8_t image[2] = {0x01U, 0x02U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    pix_l3_factory_config config{};
    config.model = PIX_L3_MODEL_DT181;
    config.usb = usb_ops_for(&backend);
    config.product = product_ops_for(&backend);
    pix_l3_factory *factory = nullptr;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK) {
        return 1;
    }
    dtv_l3_capabilities common{};
    pix_l3_product_capabilities details{};
    if (pix_l3_factory_capabilities(factory, &common) != DTV_L3_OK ||
        common.receiver_count != 1U || common.reader_count != 1U ||
        common.lnb_power != 0U) {
        return 2;
    }
    if (pix_l3_factory_details(factory, &details) != DTV_L3_OK ||
        details.model != PIX_L3_MODEL_DT181 ||
        details.selects_satellite_stream_before_tune != 1U ||
        details.custom_firmware_candidate != 0U ||
        details.firmware_required != 1U ||
        details.cancel_supported != 0U ||
        details.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE ||
        details.submit_nonblocking != 0U) {
        return 3;
    }
    pix_l3_factory_destroy(factory);

    config.model = PIX_L3_MODEL_DT295;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK ||
        pix_l3_factory_details(factory, &details) != DTV_L3_OK ||
        details.model != PIX_L3_MODEL_DT295 ||
        details.selects_satellite_stream_before_tune != 0U ||
        details.custom_firmware_candidate != 1U ||
        details.satellite_slot_count != 8U ||
        details.bridge_shared != 0U ||
        details.external_apdu != PIX_L3_UNSUPPORTED ||
        details.presence_observation != PIX_L3_SUPPORTED ||
        details.lnb_power != PIX_L3_UNSUPPORTED ||
        details.route_owned_by_product != 1U ||
        details.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE) {
        return 4;
    }
    (void)firmware;
    pix_l3_factory_destroy(factory);
    return 0;
}

int factory_rejects_invalid()
{
    pix_l3_factory_config config{};
    pix_l3_factory *factory = nullptr;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_INVALID ||
        factory != nullptr) {
        return 1;
    }
    FakeBackend backend;
    config.model = static_cast<pix_l3_model>(1);
    config.usb = usb_ops_for(&backend);
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_INVALID) {
        return 2;
    }
    config.model = PIX_L3_MODEL_DT295;
    config.usb = usb_ops_for(&backend);
    config.usb.transfer = nullptr;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_INVALID) {
        return 3;
    }
    return 0;
}

int firmware_bytes_and_callback()
{
    FakeBackend backend;
    const std::uint8_t image[3] = {0x31U, 0x32U, 0x33U};
    const dtv_l3_firmware bytes = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT295, bytes)) {
        return 1;
    }
    if (backend.configure_calls != 1U ||
        backend.seen_model != static_cast<std::uint32_t>(PIX_L3_MODEL_DT295) ||
        backend.seen_selector != 0x55U ||
        backend.seen_firmware.size() != sizeof(image) ||
        backend.seen_firmware[0] != image[0] ||
        backend.seen_firmware[2] != image[2]) {
        return 2;
    }
    if (!session.finish()) {
        return 3;
    }

    FakeBackend callback_backend;
    dtv_l3_firmware callback{};
    callback.source = DTV_L3_FIRMWARE_READ_CALLBACK;
    callback.read = read_firmware;
    callback.read_context = nullptr;
    Session second;
    if (!second.start(callback_backend, PIX_L3_MODEL_DT181, callback)) {
        return 4;
    }
    if (callback_backend.seen_firmware.size() != 4U ||
        callback_backend.seen_firmware[0] != 0x11U ||
        callback_backend.seen_firmware[3] != 0x44U) {
        return 5;
    }
    return second.finish() ? 0 : 6;
}

#ifndef _WIN32
int firmware_path()
{
    char path[] = "/tmp/pix-l3-fw-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) {
        return 1;
    }
    const std::uint8_t image[5] = {0x41U, 0x42U, 0x43U, 0x44U, 0x45U};
    if (write(fd, image, sizeof(image)) != static_cast<ssize_t>(sizeof(image))) {
        close(fd);
        unlink(path);
        return 2;
    }
    close(fd);
    dtv_l3_firmware firmware{};
    firmware.source = DTV_L3_FIRMWARE_PATH;
    firmware.path = path;
    FakeBackend backend;
    Session session;
    const bool started = session.start(backend, PIX_L3_MODEL_DT295, firmware);
    unlink(path);
    if (!started) {
        return 3;
    }
    if (backend.seen_firmware.size() != sizeof(image) ||
        backend.seen_firmware[0] != image[0] ||
        backend.seen_firmware[4] != image[4]) {
        return 4;
    }
    return session.finish() ? 0 : 5;
}
#endif

int firmware_required()
{
    FakeBackend backend;
    const dtv_l3_firmware none{};
    Session session;
    if (session.start(backend, PIX_L3_MODEL_DT295, none)) {
        return 1;
    }
    return session.finish() ? 0 : 2;
}

int route_rejection_keeps_lease()
{
    FakeBackend backend;
    backend.tunes = {DTV_L3_FAILED, DTV_L3_OK};
    const std::uint8_t image[1] = {0x01U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT295, firmware)) {
        return 1;
    }
    dtv_l3_lease *lease = nullptr;
    if (dtv_l3_lease_acquire(session.owner, 0U, &lease) != DTV_L3_OK ||
        lease == nullptr) {
        return 2;
    }
    uint64_t programmed = 0U;
    uint64_t revision = 0U;
    if (dtv_l3_lease_tune(lease, 522000000U, &programmed, &revision) !=
        DTV_L3_FAILED) {
        return 3;
    }
    dtv_l3_lease_status status{};
    if (dtv_l3_lease_status_get(lease, &status) != DTV_L3_OK ||
        status.state != DTV_L3_LEASE_FREE || status.tune_revision != 0U) {
        return 4;
    }
    if (dtv_l3_lease_tune(lease, 522000000U, &programmed, &revision) !=
            DTV_L3_OK ||
        programmed != 522000000U || revision != 1U) {
        return 5;
    }
    if (dtv_l3_lease_status_get(lease, &status) != DTV_L3_OK ||
        status.state != DTV_L3_LEASE_TUNED || status.tune_revision != 1U) {
        return 6;
    }
    if (dtv_l3_lease_release(lease) != DTV_L3_OK) {
        return 7;
    }
    return session.finish() ? 0 : 8;
}

int transfer_copy_and_repeat_poll()
{
    FakeBackend backend;
    backend.fill = 0x47U;
    const std::uint8_t image[1] = {0x01U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT181, firmware)) {
        return 1;
    }
    dtv_l3_transfer_request request{};
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81U;
    request.timeout_ms = 100U;
    request.in_capacity = 188U;
    dtv_l3_op *op = nullptr;
    if (dtv_l3_transfer_start(session.owner, &request, &op) != DTV_L3_ACCEPTED ||
        op == nullptr || !wait_op(op)) {
        return 2;
    }
    dtv_l3_op_result first{};
    dtv_l3_op_result second{};
    if (dtv_l3_op_poll(op, &first) != DTV_L3_OK ||
        dtv_l3_op_poll(op, &second) != DTV_L3_OK) {
        return 3;
    }
    if (first.result != DTV_L3_OK || first.data_size != 188U ||
        first.data == nullptr || second.data_size != 188U ||
        second.data == nullptr) {
        return 4;
    }
    for (std::size_t i = 0U; i < 188U; ++i) {
        if (first.data[i] != 0x47U || second.data[i] != 0x47U) {
            return 5;
        }
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK) {
        return 6;
    }

    std::uint8_t out[4] = {1U, 2U, 3U, 4U};
    dtv_l3_transfer_request outbound{};
    outbound.kind = DTV_L3_TRANSFER_BULK;
    outbound.endpoint = 0x01U;
    outbound.timeout_ms = 100U;
    outbound.out_data = out;
    outbound.out_size = sizeof(out);
    dtv_l3_op *out_op = nullptr;
    if (dtv_l3_transfer_start(session.owner, &outbound, &out_op) !=
            DTV_L3_ACCEPTED ||
        out_op == nullptr) {
        return 7;
    }
    out[0] = 0xAAU;
    if (!wait_op(out_op)) {
        return 8;
    }
    dtv_l3_op_result outbound_result{};
    if (dtv_l3_op_poll(out_op, &outbound_result) != DTV_L3_OK ||
        outbound_result.result != DTV_L3_OK) {
        return 9;
    }
    if (backend.last_out.size() != 4U || backend.last_out[0] != 1U ||
        backend.last_out[1] != 2U || backend.last_out[2] != 3U ||
        backend.last_out[3] != 4U) {
        return 10;
    }
    dtv_l3_op_release(out_op);
    return session.finish() ? 0 : 11;
}

int close_retention_and_cleanup_retry()
{
    FakeBackend backend;
    backend.releases.push_back({DTV_L3_RELEASE_QUIESCENCE_UNKNOWN,
                                DTV_L3_CLAIM_RETAINED,
                                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_FAILED});
    const std::uint8_t image[1] = {0x01U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT295, firmware)) {
        return 1;
    }
    dtv_l3_op_release(session.open_op);
    session.open_op = nullptr;

    if (dtv_l3_close(session.owner, 100U) != DTV_L3_TIMEOUT) {
        return 2;
    }
    dtv_l3_close_status status{};
    if (dtv_l3_close_status_get(session.owner, &status) != DTV_L3_OK ||
        status.physical_quiesced != 0U || status.cleanup_complete != 0U ||
        status.releasable != 0U) {
        return 3;
    }
    bool retried = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
        const dtv_l3_result result = dtv_l3_cleanup_retry(session.owner);
        if (result == DTV_L3_ACCEPTED || result == DTV_L3_OK) {
            retried = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!retried) {
        return 4;
    }
    if (dtv_l3_close(session.owner, 1000U) != DTV_L3_OK) {
        return 5;
    }
    if (dtv_l3_close_status_get(session.owner, &status) != DTV_L3_OK ||
        status.physical_quiesced == 0U || status.cleanup_complete == 0U ||
        status.releasable == 0U) {
        return 6;
    }
    if (backend.release_calls < 2U) {
        return 7;
    }
    return session.finish() ? 0 : 8;
}


dtv_l3_result start_in_transfer(dtv_l3_context *owner, dtv_l3_op **op)
{
    dtv_l3_transfer_request request{};
    request.kind = DTV_L3_TRANSFER_BULK;
    request.endpoint = 0x81U;
    request.timeout_ms = 100U;
    request.in_capacity = 188U;
    return dtv_l3_transfer_start(owner, &request, op);
}

int async_cancel_terminal_and_ownership()
{
    FakeBackend backend;
    backend.async = true;
    backend.cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE;
    const std::uint8_t image[1] = {0x01U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT181, firmware)) {
        return 1;
    }
    pix_l3_product_capabilities details{};
    if (pix_l3_factory_details(session.factory, &details) != DTV_L3_OK ||
        details.cancel_supported != 1U ||
        details.cancel_scope != DTV_L3_CANCEL_SCOPE_PIPE ||
        details.submit_nonblocking != 1U) {
        return 2;
    }

    dtv_l3_op *a = nullptr;
    dtv_l3_op *b = nullptr;
    dtv_l3_op_release(session.open_op);
    session.open_op = nullptr;
    const dtv_l3_result ra = start_in_transfer(session.owner, &a);
    const dtv_l3_result rb = start_in_transfer(session.owner, &b);
    const bool wp = wait_pending(backend, 2U);
    if (ra != DTV_L3_ACCEPTED || rb != DTV_L3_ACCEPTED ||
        a == nullptr || b == nullptr || !wp) {
        return 3;
    }
    if (dtv_l3_op_cancel(a) != DTV_L3_ACCEPTED ||
        backend.cancel_calls != 1U) {
        return 4;
    }
    if (dtv_l3_close(session.owner, 0U) == DTV_L3_OK) {
        return 5;
    }
    dtv_l3_close_status close_status{};
    if (dtv_l3_close_status_get(session.owner, &close_status) != DTV_L3_OK ||
        close_status.cleanup_complete != 0U || close_status.releasable != 0U) {
        return 6;
    }
    if (dtv_l3_context_release(session.owner) != DTV_L3_BUSY) {
        return 7;
    }

    if (!complete_pending(backend, 2U, DTV_L3_OK, 0x22U) ||
        !complete_pending(backend, 1U, DTV_L3_CANCELLED, 0x00U)) {
        return 8;
    }
    if (!wait_op(a) || !wait_op(b)) {
        return 9;
    }
    dtv_l3_op_result a_result{};
    dtv_l3_op_result b_result{};
    if (dtv_l3_op_poll(a, &a_result) != DTV_L3_OK ||
        dtv_l3_op_poll(b, &b_result) != DTV_L3_OK) {
        return 10;
    }
    if (a_result.result != DTV_L3_CANCELLED ||
        b_result.result != DTV_L3_OK || b_result.data_size != 188U ||
        b_result.data == nullptr || b_result.data[0] != 0x22U) {
        return 11;
    }
    if (dtv_l3_op_release(a) != DTV_L3_OK ||
        dtv_l3_op_release(b) != DTV_L3_OK) {
        return 12;
    }
    if (!session.finish()) {
        return 13;
    }
    return 0;
}

int async_normal_completion_wins_race()
{
    FakeBackend backend;
    backend.async = true;
    backend.cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE;
    const std::uint8_t image[1] = {0x01U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT295, firmware)) {
        return 1;
    }
    dtv_l3_op *op = nullptr;
    if (start_in_transfer(session.owner, &op) != DTV_L3_ACCEPTED ||
        op == nullptr || !wait_pending(backend, 1U)) {
        return 2;
    }
    if (dtv_l3_op_cancel(op) != DTV_L3_ACCEPTED) {
        return 3;
    }
    if (!complete_pending(backend, 1U, DTV_L3_OK, 0x33U) || !wait_op(op)) {
        return 4;
    }
    dtv_l3_op_result result{};
    if (dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK || result.data_size != 188U ||
        result.data == nullptr || result.data[0] != 0x33U) {
        return 5;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK) {
        return 6;
    }
    return session.finish() ? 0 : 7;
}

int async_cancel_unsupported_is_not_success()
{
    FakeBackend backend;
    backend.async = true;
    backend.cancel_scope = DTV_L3_CANCEL_SCOPE_PIPE;
    const std::uint8_t image[1] = {0x01U};
    const dtv_l3_firmware firmware = firmware_bytes(image, sizeof(image));
    Session session;
    if (!session.start(backend, PIX_L3_MODEL_DT181, firmware)) {
        return 1;
    }
    dtv_l3_op *op = nullptr;
    if (start_in_transfer(session.owner, &op) != DTV_L3_ACCEPTED ||
        !wait_pending(backend, 1U)) {
        return 2;
    }
    backend.cancel_result = DTV_L3_UNSUPPORTED;
    if (dtv_l3_op_cancel(op) != DTV_L3_UNSUPPORTED) {
        return 3;
    }
    if (!complete_pending(backend, 1U, DTV_L3_OK, 0x44U) || !wait_op(op)) {
        return 4;
    }
    dtv_l3_op_result result{};
    if (dtv_l3_op_poll(op, &result) != DTV_L3_OK ||
        result.result != DTV_L3_OK) {
        return 5;
    }
    if (dtv_l3_op_release(op) != DTV_L3_OK) {
        return 6;
    }
    return session.finish() ? 0 : 7;
}

}  // namespace

int main()
{
    if (const int rc = common_capabilities()) {
        return 10 + rc;
    }
    if (const int rc = factory_capability_differences()) {
        return 20 + rc;
    }
    if (const int rc = factory_rejects_invalid()) {
        return 30 + rc;
    }
    if (const int rc = firmware_bytes_and_callback()) {
        return 40 + rc;
    }
#ifndef _WIN32
    if (const int rc = firmware_path()) {
        return 50 + rc;
    }
#endif
    if (const int rc = firmware_required()) {
        return 60 + rc;
    }
    if (const int rc = route_rejection_keeps_lease()) {
        return 70 + rc;
    }
    if (const int rc = transfer_copy_and_repeat_poll()) {
        return 80 + rc;
    }
    if (const int rc = close_retention_and_cleanup_retry()) {
        return 90 + rc;
    }
    if (const int rc = async_cancel_terminal_and_ownership()) {
        return 100 + rc;
    }
    if (const int rc = async_normal_completion_wins_race()) {
        return 110 + rc;
    }
    if (const int rc = async_cancel_unsupported_is_not_success()) {
        return 120 + rc;
    }
    return 0;
}
