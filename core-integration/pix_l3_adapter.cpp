// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_adapter.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace {

struct Factory {
    pix_l3_factory_config config{};
    std::mutex mutex;
    std::map<std::uint64_t, std::uint32_t> active_transfers;
    bool released = false;
};

Factory *factory_from(void *context) noexcept
{
    return static_cast<Factory *>(context);
}

dtv_l3_result require_backend(const pix_l3_backend_ops &backend) noexcept
{
    if (backend.claim == nullptr || backend.release_claim == nullptr ||
        backend.transfer == nullptr || backend.cancel == nullptr) {
        return DTV_L3_INVALID;
    }
    return DTV_L3_OK;
}

dtv_l3_claim_result claim(void *context, dtv_l3_provider_binding *binding,
                          std::uint32_t claim_index) noexcept
{
    (void)binding;
    Factory *factory = factory_from(context);
    if (factory == nullptr) {
        return {DTV_L3_INVALID, DTV_L3_CLAIM_NONE};
    }
    if (factory->config.backend.claim == nullptr) {
        return {DTV_L3_UNSUPPORTED, DTV_L3_CLAIM_NONE};
    }
    return factory->config.backend.claim(factory->config.backend.context,
                                         claim_index);
}

dtv_l3_release_result release_claim(void *context,
                                    dtv_l3_provider_binding *binding,
                                    std::uint32_t claim_index) noexcept
{
    (void)binding;
    Factory *factory = factory_from(context);
    if (factory == nullptr) {
        return {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_NONE,
                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_INVALID};
    }
    if (factory->config.backend.release_claim == nullptr) {
        return {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_RETAINED,
                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_UNSUPPORTED};
    }
    return factory->config.backend.release_claim(
        factory->config.backend.context, claim_index);
}

dtv_l3_result submit(void *context, dtv_l3_provider_binding *binding,
                     std::uint64_t transfer_id,
                     const dtv_l3_transfer_request *request,
                     dtv_l3_transfer_complete_fn complete,
                     void *callback_context) noexcept
{
    Factory *factory = factory_from(context);
    if (factory == nullptr || binding == nullptr || request == nullptr ||
        complete == nullptr) {
        return DTV_L3_INVALID;
    }

    {
        std::lock_guard<std::mutex> lock(factory->mutex);
        if (factory->released) {
            return DTV_L3_CLOSING;
        }
        if (factory->active_transfers.count(transfer_id) != 0U) {
            return DTV_L3_INVALID;
        }
        factory->active_transfers.emplace(transfer_id, request->claim_index);
    }

    if (request->kind != DTV_L3_TRANSFER_BULK &&
        request->kind != DTV_L3_TRANSFER_CONTROL) {
        std::lock_guard<std::mutex> lock(factory->mutex);
        factory->active_transfers.erase(transfer_id);
        return DTV_L3_UNSUPPORTED;
    }

    pix_l3_transfer transfer{};
    transfer.kind = request->kind;
    transfer.endpoint = request->endpoint;
    transfer.timeout_ms = request->timeout_ms;
    transfer.out_data = request->out_data;
    transfer.out_size = request->out_size;
    transfer.in_capacity = request->in_capacity;
    transfer.claim_index = request->claim_index;
    transfer.request_type = request->control.request_type;
    transfer.request = request->control.request;
    transfer.value = request->control.value;
    transfer.index = request->control.index;
    transfer.deadline_ns = request->deadline_ns;

    std::vector<std::uint8_t> in_buffer(request->in_capacity);
    pix_l3_transfer_result completion{};
    completion.result = DTV_L3_FAILED;
    const dtv_l3_result submitted =
        factory->config.backend.transfer(factory->config.backend.context,
                                         transfer_id, &transfer,
                                         in_buffer.data(), in_buffer.size(),
                                         &completion);

    dtv_l3_transfer_completion delivered{};
    if (submitted == DTV_L3_OK) {
        delivered.result = completion.result;
        delivered.data = completion.data;
        delivered.size = completion.size;
        delivered.actual_length = completion.actual_length;
        delivered.actual_length_known = completion.actual_length_known;
    } else {
        delivered.result = submitted;
    }

    {
        std::lock_guard<std::mutex> lock(factory->mutex);
        factory->active_transfers.erase(transfer_id);
    }
    complete(binding, transfer_id, &delivered, callback_context);
    return DTV_L3_ACCEPTED;
}

dtv_l3_result cancel(void *context, dtv_l3_provider_binding *binding,
                     std::uint64_t transfer_id) noexcept
{
    (void)binding;
    Factory *factory = factory_from(context);
    if (factory == nullptr) {
        return DTV_L3_INVALID;
    }
    if (factory->config.backend.cancel == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return factory->config.backend.cancel(factory->config.backend.context,
                                          transfer_id);
}

dtv_l3_result scope_query(void *context, std::uint32_t receiver_id,
                          dtv_l3_scope_info *scope) noexcept
{
    Factory *factory = factory_from(context);
    if (factory == nullptr || scope == nullptr) {
        return DTV_L3_INVALID;
    }
    if (receiver_id != 0U) {
        return DTV_L3_UNSUPPORTED;
    }
    /*
     * PIX has one physical receiver and no shared bridge. The route/plan
     * decision stays in the product backend tune path; this scope only
     * describes exclusivity of the single receiver.
     */
    scope->bridge_id = 0U;
    scope->compatibility = DTV_L3_LEASE_EXCLUSIVE;
    return DTV_L3_OK;
}

dtv_l3_result tune(void *context, dtv_l3_provider_binding *binding,
                   std::uint32_t receiver_id, std::uint64_t requested_hz,
                   std::uint64_t *programmed_hz) noexcept
{
    (void)binding;
    Factory *factory = factory_from(context);
    if (factory == nullptr || programmed_hz == nullptr) {
        return DTV_L3_INVALID;
    }
    if (receiver_id != 0U) {
        return DTV_L3_UNSUPPORTED;
    }
    if (factory->config.backend.tune == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return factory->config.backend.tune(factory->config.backend.context,
                                        receiver_id, requested_hz,
                                        programmed_hz);
}

}  // namespace

struct pix_l3_factory final : Factory {
};

extern "C" dtv_l3_result pix_l3_factory_create(
    const pix_l3_factory_config *config, pix_l3_factory **factory_out)
{
    if (config == nullptr || factory_out == nullptr) {
        return DTV_L3_INVALID;
    }
    if (config->model != PIX_L3_MODEL_DT181 &&
        config->model != PIX_L3_MODEL_DT295) {
        return DTV_L3_INVALID;
    }
    if (require_backend(config->backend) != DTV_L3_OK) {
        return DTV_L3_INVALID;
    }
    std::unique_ptr<pix_l3_factory> factory(new pix_l3_factory);
    factory->config = *config;
    *factory_out = factory.release();
    return DTV_L3_OK;
}

extern "C" void pix_l3_factory_destroy(pix_l3_factory *factory)
{
    delete static_cast<pix_l3_factory *>(factory);
}

extern "C" dtv_l3_result pix_l3_factory_capabilities(
    const pix_l3_factory *factory, pix_l3_capabilities *capabilities)
{
    if (factory == nullptr || capabilities == nullptr) {
        return DTV_L3_INVALID;
    }
    const Factory *value = static_cast<const Factory *>(factory);
    *capabilities = pix_l3_capabilities{};
    capabilities->model = value->config.model;
    capabilities->tuner_count = 1U;
    capabilities->reader_count = 1U;
    capabilities->lnb_power = PIX_L3_UNSUPPORTED;
    capabilities->route_owned_by_product = 1U;
    capabilities->custom_firmware_candidate =
        value->config.model == PIX_L3_MODEL_DT295 ? 1U : 0U;
    capabilities->card_ts_concurrency = PIX_L3_CARD_TS_UNVERIFIED;
    capabilities->fence_ts_effect = PIX_L3_FENCE_TS_UNKNOWN;

    pix_l3_tuner_capability &tuner = capabilities->tuners[0];
    tuner.receiver_id = 0U;
    tuner.terrestrial = PIX_L3_SUPPORTED;
    tuner.satellite = PIX_L3_SUPPORTED;
    tuner.satellite_slot_count = 8U;
    tuner.selects_satellite_stream_before_tune =
        value->config.model == PIX_L3_MODEL_DT181 ? 1U : 0U;
    tuner.bridge_shared = 0U;

    pix_l3_reader_capability &reader = capabilities->readers[0];
    reader.reader_id = 0U;
    reader.present = 1U;
    reader.presence_observation = PIX_L3_SUPPORTED;
    reader.external_apdu = PIX_L3_UNSUPPORTED;
    reader.protocol_t1 = PIX_L3_UNKNOWN;
    return DTV_L3_OK;
}

extern "C" dtv_l3_result pix_l3_factory_provider(pix_l3_factory *factory,
                                                 dtv_l3_provider *provider)
{
    if (factory == nullptr || provider == nullptr) {
        return DTV_L3_INVALID;
    }
    Factory *value = static_cast<Factory *>(factory);
    if (require_backend(value->config.backend) != DTV_L3_OK) {
        return DTV_L3_INVALID;
    }
    *provider = dtv_l3_provider{};
    provider->provider_context = factory;
    provider->claim = claim;
    provider->release_claim = release_claim;
    provider->submit = submit;
    provider->cancel = cancel;
    provider->scope_query = scope_query;
    provider->tune = tune;
    provider->cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    provider->logger.fn = nullptr;
    provider->logger.userdata = nullptr;
    provider->logger.min_level = DTV_L3_LOG_ERROR;
    return DTV_L3_OK;
}
