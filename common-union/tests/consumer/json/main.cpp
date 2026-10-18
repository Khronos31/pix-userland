// SPDX-License-Identifier: MIT
#include <dtv/userland/error.h>
#include <dtv/userland/json.h>

#include <array>
#include <cstdint>

int main()
{
    using namespace dtv::userland;
    using namespace dtv::userland::wire;
    StatusResponse status;
    status.producer.product.length = 1U;
    status.producer.product.bytes[0] = 'p';
    status.producer.version.length = 1U;
    status.producer.version.bytes[0] = '1';
    status.context.state = ContextState::starting;
    status.scope.kind = ScopeKind::enclosure;
    status.topology.state = TopologyState::unknown;
    status.functions.present = true;

    ListResponse list;
    list.producer = status.producer;
    list.topology.state = TopologyState::unknown;
    StatusContext context{&list};
    const auto size = status_json_size(status, context);
    if (!size || error_symbol(16U) == nullptr || error_symbol(256U) != nullptr) return 1;
    std::array<std::uint8_t, 1024> output{};
    const auto written = encode_status_json(status, context, {output.data(), output.size()});
    return written && written.value() == size.value() && output[written.value() - 1U] == '\n'
               ? 0
               : 2;
}
