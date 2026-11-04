// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_adapter.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct FakeBackend final {
    uint64_t claim_calls = 0U;
    uint64_t release_calls = 0U;
    uint64_t transfer_calls = 0U;
    uint64_t tune_calls = 0U;
    dtv_l3_claim_result claim_result{DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    std::vector<dtv_l3_release_result> releases;
    std::vector<dtv_l3_result> tunes;
    std::vector<std::uint8_t> last_out;
    std::uint8_t fill = 0x47U;
};

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

dtv_l3_result tune(void *context, std::uint32_t receiver_id,
                   std::uint64_t requested_hz, std::uint64_t *programmed_hz)
{
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

bool wait_open(dtv_l3_context *owner, dtv_l3_op *op)
{
    if (dtv_l3_op_wait(op, 3000U) != DTV_L3_OK) {
        return false;
    }
    dtv_l3_op_result result{};
    if (dtv_l3_op_poll(op, &result) != DTV_L3_OK) {
        return false;
    }
    if (result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK) {
        return false;
    }
    dtv_l3_open_status status{};
    return dtv_l3_open_status_get(owner, &status) == DTV_L3_OK &&
           status.state == DTV_L3_OPEN_READY;
}

bool wait_op(dtv_l3_op *op)
{
    if (dtv_l3_op_wait(op, 3000U) != DTV_L3_OK) {
        return false;
    }
    dtv_l3_op_result result{};
    return dtv_l3_op_poll(op, &result) == DTV_L3_OK &&
           result.state == DTV_L3_OP_DONE;
}

struct Session final {
    pix_l3_factory *factory = nullptr;
    dtv_l3_provider provider{};
    dtv_l3_context *owner = nullptr;
    dtv_l3_op *open_op = nullptr;

    bool start(const pix_l3_backend_ops &backend, pix_l3_model model)
    {
        pix_l3_factory_config config{};
        config.model = model;
        config.backend = backend;
        if (pix_l3_factory_create(&config, &factory) != DTV_L3_OK) {
            return false;
        }
        if (pix_l3_factory_provider(factory, &provider) != DTV_L3_OK) {
            return false;
        }
        dtv_l3_open_result opened = dtv_l3_open(&provider, 1U);
        owner = opened.owner;
        open_op = opened.op;
        if (opened.result != DTV_L3_ACCEPTED || owner == nullptr ||
            open_op == nullptr) {
            return false;
        }
        return wait_open(owner, open_op);
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
                status.cleanup_complete != 0U && status.physical_quiesced == 0U) {
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

pix_l3_backend_ops ops_for(FakeBackend *backend)
{
    pix_l3_backend_ops ops{};
    ops.context = backend;
    ops.claim = claim;
    ops.release_claim = release;
    ops.transfer = transfer;
    ops.cancel = cancel;
    ops.tune = tune;
    return ops;
}

int capability_differences()
{
    FakeBackend backend;
    Session dt181;
    if (!dt181.start(ops_for(&backend), PIX_L3_MODEL_DT181)) {
        return 1;
    }
    pix_l3_capabilities a{};
    if (pix_l3_factory_capabilities(dt181.factory, &a) != DTV_L3_OK ||
        a.model != PIX_L3_MODEL_DT181 ||
        a.tuners[0].selects_satellite_stream_before_tune != 1U ||
        a.custom_firmware_candidate != 0U) {
        return 2;
    }
    if (!dt181.finish()) {
        return 5;
    }

    Session dt295;
    if (!dt295.start(ops_for(&backend), PIX_L3_MODEL_DT295)) {
        return 3;
    }
    pix_l3_capabilities b{};
    if (pix_l3_factory_capabilities(dt295.factory, &b) != DTV_L3_OK ||
        b.model != PIX_L3_MODEL_DT295 ||
        b.tuners[0].selects_satellite_stream_before_tune != 0U ||
        b.custom_firmware_candidate != 1U ||
        b.tuners[0].satellite_slot_count != 8U ||
        b.tuners[0].bridge_shared != 0U ||
        b.readers[0].external_apdu != PIX_L3_UNSUPPORTED ||
        b.readers[0].presence_observation != PIX_L3_SUPPORTED ||
        b.lnb_power != PIX_L3_UNSUPPORTED ||
        b.route_owned_by_product != 1U ||
        b.card_ts_concurrency != PIX_L3_CARD_TS_UNVERIFIED ||
        b.fence_ts_effect != PIX_L3_FENCE_TS_UNKNOWN) {
        return 4;
    }
    if (!dt295.finish()) {
        return 6;
    }
    return 0;
}

int factory_rejects_incomplete()
{
    pix_l3_factory_config config{};
    config.model = PIX_L3_MODEL_DT295;
    pix_l3_factory *factory = nullptr;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_INVALID ||
        factory != nullptr) {
        return 1;
    }
    config.model = static_cast<pix_l3_model>(1);
    config.backend.claim = claim;
    config.backend.release_claim = release;
    config.backend.transfer = transfer;
    config.backend.cancel = cancel;
    if (pix_l3_factory_create(&config, &factory) != DTV_L3_INVALID) {
        return 2;
    }
    return 0;
}

int route_rejection_keeps_lease()
{
    FakeBackend backend;
    backend.tunes = {DTV_L3_FAILED, DTV_L3_OK};
    Session session;
    if (!session.start(ops_for(&backend), PIX_L3_MODEL_DT295)) {
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
    Session session;
    if (!session.start(ops_for(&backend), PIX_L3_MODEL_DT181)) {
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
    Session session;
    if (!session.start(ops_for(&backend), PIX_L3_MODEL_DT295)) {
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

}  // namespace

int main()
{
    if (const int rc = capability_differences()) {
        return 10 + rc;
    }
    if (const int rc = factory_rejects_incomplete()) {
        return 20 + rc;
    }
    if (const int rc = route_rejection_keeps_lease()) {
        return 30 + rc;
    }
    if (const int rc = transfer_copy_and_repeat_poll()) {
        return 40 + rc;
    }
    if (const int rc = close_retention_and_cleanup_retry()) {
        return 50 + rc;
    }
    return 0;
}
