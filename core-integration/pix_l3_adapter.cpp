// SPDX-License-Identifier: GPL-2.0-or-later
#include "pix_l3_adapter.h"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kMaxFirmwareBytes = 64U * 1024U * 1024U;

struct Factory {
    pix_l3_factory_config config{};
};

struct AdapterInstance final {
    dtv_l3_product_adapter adapter{};
    pix_l3_product_ops product{};
    dtv_l3_firmware firmware_view{};
    std::vector<std::uint8_t> firmware_bytes;
    pix_l3_model model = PIX_L3_MODEL_DT181;
    std::uint64_t selector = 0U;
    dtv_l3_context *owner = nullptr;
    dtv_l3_capabilities capabilities{};
    dtv_l3_capability_catalog catalog{};
};

dtv_l3_claim_result backend_claim(void *context,
                                 std::uint32_t claim_index) noexcept;
dtv_l3_release_result backend_release_claim(
    void *context, std::uint32_t claim_index) noexcept;

dtv_l3_claim_result usb_claim(void *context,
                              dtv_l3_provider_binding *binding,
                              std::uint32_t claim_index) noexcept
{
    (void)binding;
    return backend_claim(context, claim_index);
}

dtv_l3_release_result usb_release_claim(void *context,
                                         dtv_l3_provider_binding *binding,
                                         std::uint32_t claim_index) noexcept
{
    (void)binding;
    return backend_release_claim(context, claim_index);
}

Factory *factory_from(void *context) noexcept
{
    return static_cast<Factory *>(context);
}

bool usb_backend_complete(const pix_l3_usb_backend_ops &usb) noexcept
{
    return usb.claim != nullptr && usb.release_claim != nullptr &&
           (usb.transfer != nullptr || usb.submit != nullptr);
}

const dtv_l3_receiver_capability kReceivers[] = {
    {0U, DTV_L3_SYSTEM_ISDB_T | DTV_L3_SYSTEM_ISDB_S,
     DTV_L3_PID_FILTER_SOFTWARE, DTV_L3_SUPPORT_SUPPORTED}};
const dtv_l3_reader_capability kReaders[] = {
    {0U, DTV_L3_SUPPORT_UNSUPPORTED, DTV_L3_SUPPORT_SUPPORTED,
     DTV_L3_SUPPORT_SUPPORTED, 0U, 0U, 0U, DTV_L3_SUPPORT_UNKNOWN}};

dtv_l3_result read_firmware_file(const char *path,
                                 std::vector<std::uint8_t> &out) noexcept
{
    if (path == nullptr || path[0] == '\0') {
        return DTV_L3_INVALID;
    }
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return DTV_L3_FAILED;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return DTV_L3_FAILED;
    }
    const long length = std::ftell(file);
    if (length <= 0 || static_cast<unsigned long>(length) > kMaxFirmwareBytes) {
        std::fclose(file);
        return length == 0 ? DTV_L3_INVALID : DTV_L3_FAILED;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return DTV_L3_FAILED;
    }
    out.resize(static_cast<std::size_t>(length));
    const std::size_t read = std::fread(out.data(), 1U, out.size(), file);
    const int closed = std::fclose(file);
    if (read != out.size() || closed != 0) {
        out.clear();
        return DTV_L3_FAILED;
    }
    return DTV_L3_OK;
}

dtv_l3_result load_firmware(const dtv_l3_firmware *firmware,
                            std::vector<std::uint8_t> &out) noexcept
{
    if (firmware == nullptr) {
        return DTV_L3_INVALID;
    }
    switch (firmware->source) {
    case DTV_L3_FIRMWARE_PATH:
        return read_firmware_file(firmware->path, out);
    case DTV_L3_FIRMWARE_BYTES:
        if (firmware->bytes == nullptr || firmware->size == 0U ||
            firmware->size > kMaxFirmwareBytes) {
            return DTV_L3_INVALID;
        }
        out.assign(firmware->bytes, firmware->bytes + firmware->size);
        return DTV_L3_OK;
    case DTV_L3_FIRMWARE_READ_CALLBACK: {
        if (firmware->read == nullptr) {
            return DTV_L3_INVALID;
        }
        const std::uint8_t *data = nullptr;
        std::size_t size = 0U;
        const dtv_l3_result result =
            firmware->read(firmware->read_context, &data, &size);
        if (result != DTV_L3_OK) {
            return result;
        }
        if (data == nullptr || size == 0U || size > kMaxFirmwareBytes) {
            return DTV_L3_INVALID;
        }
        out.assign(data, data + size);
        return DTV_L3_OK;
    }
    case DTV_L3_FIRMWARE_NONE:
    default:
        return DTV_L3_INVALID;
    }
}

dtv_l3_claim_result backend_claim(void *context,
                                 std::uint32_t claim_index) noexcept
{
    Factory *factory = factory_from(context);
    if (factory == nullptr || factory->config.usb.claim == nullptr) {
        return {DTV_L3_INVALID, DTV_L3_CLAIM_NONE};
    }
    return factory->config.usb.claim(factory->config.usb.context, claim_index);
}

dtv_l3_release_result backend_release_claim(void *context,
                                           std::uint32_t claim_index) noexcept
{
    Factory *factory = factory_from(context);
    if (factory == nullptr || factory->config.usb.release_claim == nullptr) {
        return {DTV_L3_RELEASE_QUIESCENCE_UNKNOWN, DTV_L3_CLAIM_RETAINED,
                DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_INVALID};
    }
    return factory->config.usb.release_claim(factory->config.usb.context,
                                             claim_index);
}

dtv_l3_result usb_submit(void *context, dtv_l3_provider_binding *binding,
                         std::uint64_t transfer_id,
                         const dtv_l3_transfer_request *request,
                         dtv_l3_transfer_complete_fn complete,
                         void *callback_context) noexcept
{
    Factory *factory = factory_from(context);
    if (factory == nullptr || binding == nullptr || request == nullptr ||
        complete == nullptr ||
        (factory->config.usb.submit == nullptr &&
         factory->config.usb.transfer == nullptr)) {
        return DTV_L3_INVALID;
    }

    if (factory->config.usb.submit != nullptr) {
        return factory->config.usb.submit(factory->config.usb.context, binding,
                                          transfer_id, request, complete,
                                          callback_context);
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
        factory->config.usb.transfer(factory->config.usb.context, transfer_id,
                                     &transfer, in_buffer.data(),
                                     in_buffer.size(), &completion);

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
    complete(binding, transfer_id, &delivered, callback_context);
    return DTV_L3_ACCEPTED;
}

dtv_l3_result usb_cancel(void *context, dtv_l3_provider_binding *binding,
                         std::uint64_t transfer_id) noexcept
{
    Factory *factory = factory_from(context);
    if (factory == nullptr) {
        return DTV_L3_INVALID;
    }
    if (factory->config.usb.cancel_async != nullptr) {
        return factory->config.usb.cancel_async(factory->config.usb.context,
                                                binding, transfer_id);
    }
    if (factory->config.usb.cancel == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return factory->config.usb.cancel(factory->config.usb.context, transfer_id);
}

dtv_l3_result adapter_bind(void *context, dtv_l3_context *owner) noexcept
{
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr) {
        return DTV_L3_INVALID;
    }
    instance->owner = owner;
    if (instance->product.bind == nullptr) {
        return DTV_L3_OK;
    }
    return instance->product.bind(instance->product.context, owner);
}

dtv_l3_result adapter_stream_attach(void *context,
                                    dtv_l3_provider_binding *binding,
                                    std::uint64_t stream_id,
                                    dtv_l3_ts_deliver_fn deliver,
                                    void *callback_context) noexcept
{
    (void)binding;
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr || instance->owner == nullptr ||
        deliver == nullptr) {
        return DTV_L3_INVALID;
    }
    if (instance->product.stream_attach == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return instance->product.stream_attach(instance->product.context,
                                           instance->owner, stream_id,
                                           deliver, callback_context);
}

dtv_l3_result adapter_stream_stop(void *context,
                                  dtv_l3_provider_binding *binding,
                                  std::uint64_t stream_id) noexcept
{
    (void)binding;
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr || instance->owner == nullptr) {
        return DTV_L3_INVALID;
    }
    if (instance->product.stream_stop == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return instance->product.stream_stop(instance->product.context,
                                         instance->owner, stream_id);
}

dtv_l3_result adapter_bridge_start(void *context,
                                   dtv_l3_provider_binding *binding,
                                   std::uint32_t bridge_id,
                                   std::uint32_t receiver_id) noexcept
{
    (void)binding;
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr) {
        return DTV_L3_INVALID;
    }
    if (instance->product.bridge_start == nullptr) {
        return DTV_L3_OK;
    }
    return instance->product.bridge_start(instance->product.context, bridge_id,
                                          receiver_id);
}

dtv_l3_result adapter_bridge_stop(void *context,
                                  dtv_l3_provider_binding *binding,
                                  std::uint32_t bridge_id) noexcept
{
    (void)binding;
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr) {
        return DTV_L3_INVALID;
    }
    if (instance->product.bridge_stop == nullptr) {
        return DTV_L3_OK;
    }
    return instance->product.bridge_stop(instance->product.context, bridge_id);
}

dtv_l3_result adapter_initialize(void *context,
                                 dtv_l3_context *owner) noexcept
{
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr) {
        return DTV_L3_INVALID;
    }
    if (instance->product.initialize == nullptr) {
        return DTV_L3_OK;
    }
    return instance->product.initialize(instance->product.context, owner);
}

dtv_l3_result adapter_scope_query(void *context, std::uint32_t receiver_id,
                                  dtv_l3_scope_info *scope) noexcept
{
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr || scope == nullptr) {
        return DTV_L3_INVALID;
    }
    if (instance->product.scope_query != nullptr) {
        return instance->product.scope_query(instance->product.context,
                                             receiver_id, scope);
    }
    if (receiver_id != 0U) {
        return DTV_L3_UNSUPPORTED;
    }
    /*
     * PIX has one physical receiver and no shared bridge. Route/plan remains
     * in the product tune path; this scope only describes exclusivity.
     */
    scope->bridge_id = 0U;
    scope->compatibility = DTV_L3_LEASE_EXCLUSIVE;
    return DTV_L3_OK;
}

dtv_l3_result adapter_tune(void *context, dtv_l3_provider_binding *binding,
                           std::uint32_t receiver_id,
                           std::uint64_t requested_hz,
                           std::uint64_t *programmed_hz) noexcept
{
    AdapterInstance *instance = static_cast<AdapterInstance *>(context);
    if (instance == nullptr || programmed_hz == nullptr) {
        return DTV_L3_INVALID;
    }
    if (instance->product.tune == nullptr) {
        return DTV_L3_UNSUPPORTED;
    }
    return instance->product.tune(instance->product.context, binding,
                                  receiver_id, requested_hz, programmed_hz);
}

dtv_l3_result factory_create(void *context,
                             const dtv_l3_open_config *config,
                             const dtv_l3_product_adapter **adapter_out,
                             void **adapter_owner_out) noexcept
{
    Factory *factory = factory_from(context);
    if (adapter_out == nullptr || adapter_owner_out == nullptr) {
        return DTV_L3_INVALID;
    }
    *adapter_out = nullptr;
    *adapter_owner_out = nullptr;
    if (factory == nullptr || config == nullptr) {
        return DTV_L3_INVALID;
    }
    if (config->model != static_cast<std::uint32_t>(factory->config.model)) {
        return DTV_L3_INVALID;
    }

    std::vector<std::uint8_t> firmware;
    const dtv_l3_result firmware_result =
        load_firmware(config->firmware, firmware);
    if (firmware_result != DTV_L3_OK) {
        return firmware_result;
    }

    std::unique_ptr<AdapterInstance> instance(new AdapterInstance);
    instance->product = factory->config.product;
    instance->model = factory->config.model;
    instance->selector = config->selector;
    instance->firmware_bytes = std::move(firmware);
    instance->firmware_view.source = DTV_L3_FIRMWARE_BYTES;
    instance->firmware_view.bytes = instance->firmware_bytes.data();
    instance->firmware_view.size = instance->firmware_bytes.size();

    if (instance->product.configure != nullptr) {
        const dtv_l3_result configured = instance->product.configure(
            instance->product.context, config->model, config->selector,
            instance->firmware_bytes.data(), instance->firmware_bytes.size());
        if (configured != DTV_L3_OK) {
            return configured;
        }
    }

    instance->adapter.product_context = instance.get();
    instance->adapter.capabilities.receiver_count = 1U;
    instance->adapter.capabilities.reader_count = 1U;
    instance->adapter.capabilities.lnb_power = 0U;
    instance->adapter.capabilities.serial = 0U;
    instance->adapter.capabilities.cancel_scope =
        factory->config.usb.cancel_scope;
    instance->catalog.receivers = kReceivers;
    instance->catalog.receiver_count = 1U;
    instance->catalog.readers = kReaders;
    instance->catalog.reader_count = 1U;
    instance->catalog.lnb_scopes = nullptr;
    instance->catalog.lnb_scope_count = 0U;
    instance->catalog.shared_scopes = nullptr;
    instance->catalog.shared_scope_count = 0U;
    instance->catalog.ts_form = DTV_L3_SUPPORT_SUPPORTED;
    instance->adapter.catalog = &instance->catalog;
    instance->adapter.firmware = &instance->firmware_view;
    instance->adapter.bind = adapter_bind;
    instance->adapter.initialize = adapter_initialize;
    instance->adapter.scope_query = adapter_scope_query;
    instance->adapter.tune = adapter_tune;
    instance->adapter.bridge_start = adapter_bridge_start;
    instance->adapter.bridge_stop = adapter_bridge_stop;
    instance->adapter.stream_attach = adapter_stream_attach;
    instance->adapter.stream_stop = adapter_stream_stop;

    *adapter_out = &instance->adapter;
    *adapter_owner_out = instance.get();
    instance.release();
    return DTV_L3_OK;
}

void factory_release(void *adapter_owner)
{
    delete static_cast<AdapterInstance *>(adapter_owner);
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
    if (!usb_backend_complete(config->usb)) {
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
    const pix_l3_factory *factory, dtv_l3_capabilities *capabilities)
{
    if (factory == nullptr || capabilities == nullptr) {
        return DTV_L3_INVALID;
    }
    *capabilities = dtv_l3_capabilities{};
    capabilities->receiver_count = 1U;
    capabilities->reader_count = 1U;
    capabilities->lnb_power = 0U;
    capabilities->serial = 0U;
    const Factory *value = static_cast<const Factory *>(factory);
    capabilities->cancel_scope = value->config.usb.cancel_scope;
    return DTV_L3_OK;
}

extern "C" dtv_l3_result pix_l3_factory_details(
    const pix_l3_factory *factory, pix_l3_product_capabilities *capabilities)
{
    if (factory == nullptr || capabilities == nullptr) {
        return DTV_L3_INVALID;
    }
    const Factory *value = static_cast<const Factory *>(factory);
    *capabilities = pix_l3_product_capabilities{};
    capabilities->model = value->config.model;
    capabilities->tuner_count = 1U;
    capabilities->reader_count = 1U;
    capabilities->lnb_power = PIX_L3_UNSUPPORTED;
    capabilities->route_owned_by_product = 1U;
    capabilities->custom_firmware_candidate =
        value->config.model == PIX_L3_MODEL_DT295 ? 1U : 0U;
    capabilities->card_ts_concurrency = 0U;
    capabilities->fence_ts_effect = 0U;
    capabilities->firmware_required = 1U;
    capabilities->selects_satellite_stream_before_tune =
        value->config.model == PIX_L3_MODEL_DT181 ? 1U : 0U;
    capabilities->satellite_slot_count = 8U;
    capabilities->bridge_shared = 0U;
    capabilities->external_apdu = PIX_L3_UNSUPPORTED;
    capabilities->presence_observation = PIX_L3_SUPPORTED;
    capabilities->cancel_supported = value->config.usb.cancel_supported;
    capabilities->cancel_scope =
        static_cast<std::uint8_t>(value->config.usb.cancel_scope);
    capabilities->call_flags = value->config.usb.call_flags;
    return DTV_L3_OK;
}

extern "C" dtv_l3_result pix_l3_factory_usb_provider(
    pix_l3_factory *factory, dtv_l3_usb_provider *provider)
{
    if (factory == nullptr || provider == nullptr) {
        return DTV_L3_INVALID;
    }
    Factory *value = static_cast<Factory *>(factory);
    if (!usb_backend_complete(value->config.usb)) {
        return DTV_L3_INVALID;
    }
    *provider = dtv_l3_usb_provider{};
    provider->provider_context = factory;
    provider->claim = usb_claim;
    provider->release_claim = usb_release_claim;
    provider->submit = usb_submit;
    provider->cancel = usb_cancel;
    provider->claim_cancel = nullptr;
    provider->cancel_scope = value->config.usb.cancel_scope;
    provider->call_flags = value->config.usb.call_flags;
    provider->logger.fn = nullptr;
    provider->logger.userdata = nullptr;
    provider->logger.min_level = DTV_L3_LOG_ERROR;
    return DTV_L3_OK;
}

extern "C" dtv_l3_result pix_l3_factory_adapter_factory(
    pix_l3_factory *factory, dtv_l3_adapter_factory *adapter_factory)
{
    if (factory == nullptr || adapter_factory == nullptr) {
        return DTV_L3_INVALID;
    }
    *adapter_factory = dtv_l3_adapter_factory{};
    adapter_factory->factory_context = factory;
    adapter_factory->create = factory_create;
    adapter_factory->release = factory_release;
    return DTV_L3_OK;
}
