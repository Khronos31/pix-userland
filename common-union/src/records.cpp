// SPDX-License-Identifier: MIT
#include <dtv/userland/records.h>

#include <cstring>

namespace dtv::userland::wire {
namespace {

bool valid_view(ByteView view) noexcept
{
    return view.data != nullptr || view.size == 0U;
}

bool valid_view(MutableByteView view) noexcept
{
    return view.data != nullptr || view.size == 0U;
}

std::uint16_t load_u16(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8U));
}

std::uint32_t load_u32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) | (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint64_t load_u64(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint64_t>(load_u32(p)) |
           (static_cast<std::uint64_t>(load_u32(p + 4)) << 32U);
}

void store_u32(std::uint8_t* p, std::uint32_t value) noexcept
{
    for (std::size_t i = 0U; i < 4U; ++i) {
        p[i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU);
    }
}

void store_u64(std::uint8_t* p, std::uint64_t value) noexcept
{
    for (std::size_t i = 0U; i < 8U; ++i) {
        p[i] = static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU);
    }
}

// Encoder measures with out == nullptr and then writes with a real buffer.
// Copying nothing during the measure pass is what makes a short output leave the
// caller buffer untouched.
struct Encoder final {
    // The shared codec never mutates a record while encoding; the flag lets the
    // codec skip the decode-only assignments instead of writing through a
    // const_cast, which would be undefined for a genuinely const input.
    static constexpr bool writes = false;

    std::uint8_t* out = nullptr;
    std::size_t pos = 0U;

    bool reject(Error) noexcept { return false; }

    void put(std::uint8_t value) noexcept
    {
        if (out != nullptr) {
            out[pos] = value;
        }
        ++pos;
    }

    bool u8(std::uint8_t& value) noexcept
    {
        put(value);
        return true;
    }

    bool u16(std::uint16_t& value) noexcept
    {
        put(static_cast<std::uint8_t>(value & 0xffU));
        put(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
        return true;
    }

    bool u32(std::uint32_t& value) noexcept
    {
        for (std::size_t i = 0U; i < 4U; ++i) {
            put(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
        }
        return true;
    }

    bool u64(std::uint64_t& value) noexcept
    {
        for (std::size_t i = 0U; i < 8U; ++i) {
            put(static_cast<std::uint8_t>((value >> (8U * i)) & 0xffU));
        }
        return true;
    }

    bool i32(std::int32_t& value) noexcept
    {
        std::uint32_t bits = 0U;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32(bits);
    }

    bool byte(std::uint8_t& value) noexcept { return u8(value); }

    bool byte(char& value) noexcept
    {
        put(static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
        return true;
    }
};

struct Decoder final {
    static constexpr bool writes = true;

    const std::uint8_t* in = nullptr;
    std::size_t size = 0U;
    std::size_t pos = 0U;
    Error error = Error::OK;

    bool reject(Error error_code) noexcept
    {
        if (error == Error::OK) {
            error = error_code;
        }
        return false;
    }

    bool u8(std::uint8_t& value) noexcept
    {
        if (size - pos < 1U) {
            return reject(Error::PROTOCOL_ERROR);
        }
        value = in[pos++];
        return true;
    }

    bool u16(std::uint16_t& value) noexcept
    {
        if (size - pos < 2U) {
            return reject(Error::PROTOCOL_ERROR);
        }
        value = load_u16(in + pos);
        pos += 2U;
        return true;
    }

    bool u32(std::uint32_t& value) noexcept
    {
        if (size - pos < 4U) {
            return reject(Error::PROTOCOL_ERROR);
        }
        value = load_u32(in + pos);
        pos += 4U;
        return true;
    }

    bool u64(std::uint64_t& value) noexcept
    {
        if (size - pos < 8U) {
            return reject(Error::PROTOCOL_ERROR);
        }
        value = load_u64(in + pos);
        pos += 8U;
        return true;
    }

    bool i32(std::int32_t& value) noexcept
    {
        std::uint32_t bits = 0U;
        if (!u32(bits)) {
            return false;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }

    bool byte(std::uint8_t& value) noexcept { return u8(value); }

    bool byte(char& value) noexcept
    {
        std::uint8_t raw = 0U;
        if (!u8(raw)) {
            return false;
        }
        value = static_cast<char>(raw);
        return true;
    }
};


template <std::size_t N, typename Ar>
bool string_field(Ar& ar, WString<N>& value) noexcept
{
    if (!ar.u16(value.length)) {
        return false;
    }
    if (value.length > N) {
        return ar.reject(Error::PROTOCOL_ERROR);
    }
    for (std::size_t i = 0U; i < value.length; ++i) {
        if (!ar.byte(value.bytes[i])) {
            return false;
        }
    }
    if (value.length != 0U) {
        if (!detail::strict_utf8(reinterpret_cast<const std::uint8_t*>(value.bytes.data()), value.length)) {
            return ar.reject(Error::PROTOCOL_ERROR);
        }
        for (std::size_t i = 0U; i < value.length; ++i) {
            if (value.bytes[i] == '\0') {
                return ar.reject(Error::PROTOCOL_ERROR);
            }
        }
    }
    return true;
}

template <std::size_t N, typename Ar>
bool bytes_field(Ar& ar, BBytes<N>& value) noexcept
{
    if (!ar.u32(value.length)) {
        return false;
    }
    if (value.length > N) {
        return ar.reject(Error::PROTOCOL_ERROR);
    }
    for (std::size_t i = 0U; i < value.length; ++i) {
        if (!ar.byte(value.bytes[i])) {
            return false;
        }
    }
    return true;
}

template <typename T, std::size_t N, typename Ar>
bool array_field(Ar& ar, BArray<T, N>& value) noexcept
{
    if (!ar.u16(value.count)) {
        return false;
    }
    if (value.count > N) {
        return ar.reject(Error::PROTOCOL_ERROR);
    }
    for (std::size_t i = 0U; i < value.count; ++i) {
        if (!codec(ar, value.items[i])) {
            return false;
        }
    }
    return true;
}

template <typename T, typename Ar>
bool optional_field(Ar& ar, Optional<T>& value) noexcept
{
    std::uint8_t tag = value.present ? 1U : 0U;
    if (!ar.u8(tag)) {
        return false;
    }
    if (tag > 1U) {
        return ar.reject(Error::PROTOCOL_ERROR);
    }
    if (Ar::writes) {
        value.present = tag != 0U;
    }
    if (value.present) {
        return codec(ar, value.value);
    }
    return true;
}

template <typename Ar>
bool uuid_field(Ar& ar, Uuid& value) noexcept
{
    for (std::size_t i = 0U; i < kEpochSize; ++i) {
        if (!ar.u8(value[i])) {
            return false;
        }
    }
    return true;
}

template <typename T, typename Ar>
bool measurement_field(Ar& ar, Measurement<T>& value) noexcept
{
    std::uint8_t state = static_cast<std::uint8_t>(value.state);
    if (!ar.u8(state)) {
        return false;
    }
    if (state > static_cast<std::uint8_t>(MeasureState::valid)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (Ar::writes) {
        value.state = static_cast<MeasureState>(state);
    }
    if (value.state == MeasureState::unknown) {
        std::uint8_t reason = static_cast<std::uint8_t>(value.reason);
        if (!ar.u8(reason)) {
            return false;
        }
        if (reason < 1U || reason > 5U) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
        if (Ar::writes) {
            value.reason = static_cast<UnknownReason>(reason);
        }
    } else if (value.state == MeasureState::valid) {
        if (!codec(ar, value.value)) {
            return false;
        }
        if (!ar.u64(value.sampled_at_ns)) {
            return false;
        }
    }
    return true;
}

template <typename Ar>
bool counter_field(Ar& ar, Counter& value) noexcept
{
    if (!measurement_field(ar, value.measurement)) {
        return false;
    }
    if (value.measurement.state == MeasureState::valid) {
        std::uint8_t overflow = value.overflow ? 1U : 0U;
        if (!ar.u8(overflow)) {
            return false;
        }
        if (overflow > 1U) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
        if (overflow != 0U && value.measurement.value != 0xffffffffffffffffULL) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
        if (Ar::writes) {
            value.overflow = overflow != 0U;
        }
    }
    return true;
}

template <typename T, typename Ar>
bool codec(Ar& ar, Measurement<T>& value) noexcept
{
    return measurement_field(ar, value);
}

template <typename Ar>
bool codec(Ar& ar, Counter& value) noexcept
{
    return counter_field(ar, value);
}

// Leaf codecs.
template <typename Ar>
bool codec(Ar& ar, std::uint8_t& value) noexcept
{
    return ar.u8(value);
}

template <typename Ar>
bool codec(Ar& ar, std::uint16_t& value) noexcept
{
    return ar.u16(value);
}

template <typename Ar>
bool codec(Ar& ar, std::uint32_t& value) noexcept
{
    return ar.u32(value);
}

template <typename Ar>
bool codec(Ar& ar, std::uint64_t& value) noexcept
{
    return ar.u64(value);
}

template <typename Ar>
bool codec(Ar& ar, std::int32_t& value) noexcept
{
    return ar.i32(value);
}

template <typename Ar>
bool codec(Ar& ar, bool& value) noexcept
{
    std::uint8_t raw = value ? 1U : 0U;
    if (!ar.u8(raw)) {
        return false;
    }
    if (raw > 1U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (Ar::writes) {
        value = raw != 0U;
    }
    return true;
}

template <std::size_t N, typename Ar>
bool codec(Ar& ar, WString<N>& value) noexcept
{
    return string_field(ar, value);
}

template <std::size_t N, typename Ar>
bool codec(Ar& ar, BBytes<N>& value) noexcept
{
    return bytes_field(ar, value);
}

template <typename T, std::size_t N, typename Ar>
bool codec(Ar& ar, BArray<T, N>& value) noexcept
{
    return array_field(ar, value);
}

template <typename T, typename Ar>
bool codec(Ar& ar, Optional<T>& value) noexcept
{
    return optional_field(ar, value);
}

#define DTV_USERLAND_ENUM_CODEC(NAME, LO, HI)                                     \
    template <typename Ar>                                                        \
    bool codec(Ar& ar, NAME& value) noexcept                                      \
    {                                                                             \
        std::uint8_t raw = static_cast<std::uint8_t>(value);                      \
        if (!ar.u8(raw)) {                                                        \
            return false;                                                         \
        }                                                                         \
        if (raw < static_cast<std::uint8_t>(LO) || raw > static_cast<std::uint8_t>(HI)) { \
            return ar.reject(Error::INVALID_ARGUMENT);                            \
        }                                                                         \
        if (Ar::writes) {                                                         \
            value = static_cast<NAME>(raw);                                       \
        }                                                                         \
        return true;                                                              \
    }

DTV_USERLAND_ENUM_CODEC(System, 1U, 2U)
DTV_USERLAND_ENUM_CODEC(ContextState, 0U, 4U)
DTV_USERLAND_ENUM_CODEC(TopologyState, 0U, 2U)
DTV_USERLAND_ENUM_CODEC(ReceiverState, 0U, 5U)
DTV_USERLAND_ENUM_CODEC(Operation, 0U, 3U)
DTV_USERLAND_ENUM_CODEC(StreamPhase, 1U, 4U)
DTV_USERLAND_ENUM_CODEC(ScopeKind, 0U, 3U)
DTV_USERLAND_ENUM_CODEC(SerialState, 0U, 4U)
DTV_USERLAND_ENUM_CODEC(IdentitySerialState, 0U, 2U)
DTV_USERLAND_ENUM_CODEC(Uniqueness, 0U, 3U)
DTV_USERLAND_ENUM_CODEC(Mode, 0U, 2U)
DTV_USERLAND_ENUM_CODEC(UnknownReason, 1U, 5U)
DTV_USERLAND_ENUM_CODEC(IdentificationReason, 0U, 3U)
DTV_USERLAND_ENUM_CODEC(Verification, 0U, 2U)
DTV_USERLAND_ENUM_CODEC(Selection, 0U, 2U)
DTV_USERLAND_ENUM_CODEC(TsScope, 0U, 1U)
DTV_USERLAND_ENUM_CODEC(StreamEndReason, 0U, 7U)
DTV_USERLAND_ENUM_CODEC(CardState, 0U, 3U)
DTV_USERLAND_ENUM_CODEC(CardOperation, 0U, 4U)
DTV_USERLAND_ENUM_CODEC(Share, 1U, 2U)
DTV_USERLAND_ENUM_CODEC(Disposition, 0U, 1U)
DTV_USERLAND_ENUM_CODEC(CardProtocol, 1U, 1U)
DTV_USERLAND_ENUM_CODEC(MeasureState, 0U, 2U)
DTV_USERLAND_ENUM_CODEC(StateEventReason, 1U, 4U)

#undef DTV_USERLAND_ENUM_CODEC

template <typename Ar>
bool codec(Ar& ar, Systems& value) noexcept
{
    if (!ar.u8(value.mask)) {
        return false;
    }
    if (value.mask == 0U || (value.mask & static_cast<std::uint8_t>(~0x03U)) != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

// Bounded string helpers used by the record codecs to require non-empty values.
template <typename Ar, std::size_t N>
bool optional_nonempty_string(Ar& ar, Optional<WString<N>>& value) noexcept
{
    if (!optional_field(ar, value)) {
        return false;
    }
    if (value.present && value.value.length == 0U) {
        return ar.reject(Error::PROTOCOL_ERROR);
    }
    return true;
}

// preferred_protocols is a bitmask: bit0 is T1 and every other bit is reserved,
// so the only valid value in this version is 1.
template <typename Ar>
bool preferred_protocols_field(Ar& ar, std::uint8_t& value) noexcept
{
    if (!ar.u8(value)) {
        return false;
    }
    if (value != 1U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, Producer& value) noexcept
{
    if (!string_field(ar, value.product)) {
        return false;
    }
    if (!string_field(ar, value.version)) {
        return false;
    }
    return optional_field(ar, value.core_version);
}

template <typename Ar>
bool codec(Ar& ar, Model& value) noexcept
{
    return string_field(ar, value.id) && string_field(ar, value.name);
}

template <typename Ar>
bool codec(Ar& ar, ErrorRecord& value) noexcept
{
    if (!ar.u32(value.code)) {
        return false;
    }
    if (value.code == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return string_field(ar, value.operation) && string_field(ar, value.detail);
}

template <typename Ar>
bool codec(Ar& ar, Identity& value) noexcept
{
    if (!codec(ar, value.serial_state)) {
        return false;
    }
    if (!optional_nonempty_string(ar, value.serial)) {
        return false;
    }
    if (value.serial.present != (value.serial_state == IdentitySerialState::present)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return codec(ar, value.uniqueness);
}

template <typename Ar>
bool codec(Ar& ar, Location& value) noexcept
{
    return optional_field(ar, value.usb_path) && optional_field(ar, value.bus) &&
           optional_field(ar, value.address);
}

template <typename Ar>
bool codec(Ar& ar, Binding& value) noexcept
{
    return ar.u16(value.function_id) && ar.u16(value.local_id);
}

template <typename Ar>
bool codec(Ar& ar, Topology& value) noexcept
{
    if (!codec(ar, value.state)) {
        return false;
    }
    if (!optional_field(ar, value.expected_function_count)) {
        return false;
    }
    if (value.expected_function_count.present &&
        (value.expected_function_count.value == 0U ||
         value.expected_function_count.value > kMaxFunctions)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, Limits& value) noexcept
{
    if (!optional_field(ar, value.max_concurrent_streams)) {
        return false;
    }
    if (value.max_concurrent_streams.present &&
        (value.max_concurrent_streams.value == 0U ||
         value.max_concurrent_streams.value > kMaxPublicReceivers)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, LnaDefaultEntry& value) noexcept
{
    return codec(ar, value.system) && optional_field(ar, value.default_value);
}

template <typename Ar>
bool codec(Ar& ar, LnaDefaults& value) noexcept
{
    if (!array_field(ar, value.entries)) {
        return false;
    }
    if (value.entries.count == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    for (std::size_t i = 1U; i < value.entries.count; ++i) {
        if (static_cast<std::uint8_t>(value.entries.items[i].system) <=
            static_cast<std::uint8_t>(value.entries.items[i - 1U].system)) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, LnbValues& value) noexcept
{
    if (!array_field(ar, value.values)) {
        return false;
    }
    if (value.values.count == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    for (std::size_t i = 0U; i < value.values.count; ++i) {
        const std::uint8_t volts = value.values.items[i];
        if (volts != 0U && volts != 15U) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
        if (i != 0U && volts <= value.values.items[i - 1U]) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, Capabilities& value) noexcept
{
    if (!codec(ar, value.ts_scope)) {
        return false;
    }
    if (!optional_field(ar, value.max_pids)) {
        return false;
    }
    if (value.max_pids.present && (value.max_pids.value == 0U || value.max_pids.value > kMaxPids)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u8(value.controls)) {
        return false;
    }
    if ((value.controls & static_cast<std::uint8_t>(~(kControlBitLna | kControlBitLnb))) != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    // The controls bits govern the LNA/LNB bodies directly; there is no extra
    // presence tag. A clear bit means the body is absent, so a non-empty body
    // with the bit clear is an inconsistent value.
    if ((value.controls & kControlBitLna) != 0U) {
        if (!codec(ar, value.lna)) {
            return false;
        }
    } else if (value.lna.entries.count != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if ((value.controls & kControlBitLnb) != 0U) {
        if (!codec(ar, value.lnb)) {
            return false;
        }
    } else if (value.lnb.values.count != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u8(value.measurement_mask)) {
        return false;
    }
    if ((value.measurement_mask & static_cast<std::uint8_t>(~kMetricMaskAll)) != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, ReceiverDescriptor& value) noexcept
{
    return ar.u16(value.id) && codec(ar, value.systems) && optional_field(ar, value.binding) &&
           codec(ar, value.capabilities);
}

template <typename Ar>
bool codec(Ar& ar, ReaderDescriptor& value) noexcept
{
    return ar.u16(value.id) && optional_field(ar, value.binding);
}

template <typename Ar>
bool codec(Ar& ar, Enclosure& value) noexcept
{
    return ar.u16(value.id) && optional_field(ar, value.model) && codec(ar, value.identity) &&
           codec(ar, value.limits);
}

template <typename Ar>
bool codec(Ar& ar, UsbFunction& value) noexcept
{
    if (!ar.u16(value.id) || !ar.u16(value.vid) || !ar.u16(value.pid)) {
        return false;
    }
    if (!codec(ar, value.mode) || !codec(ar, value.location)) {
        return false;
    }
    if (!codec(ar, value.serial_state)) {
        return false;
    }
    if (!optional_nonempty_string(ar, value.serial)) {
        return false;
    }
    if (value.serial.present != (value.serial_state == SerialState::present)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!optional_field(ar, value.model)) {
        return false;
    }
    if (!codec(ar, value.identification_reason)) {
        return false;
    }
    // An absent model must carry a reason other than none; a known cold model may
    // still carry cold_model.
    if (!value.model.present && value.identification_reason == IdentificationReason::none) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!codec(ar, value.present)) {
        return false;
    }
    return optional_field(ar, value.last_error);
}

template <typename Ar>
bool codec(Ar& ar, Scope& value) noexcept
{
    if (!codec(ar, value.kind)) {
        return false;
    }
    if (!ar.u64(value.id)) {
        return false;
    }
    if (value.kind == ScopeKind::enclosure && value.id != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if ((value.kind == ScopeKind::receiver || value.kind == ScopeKind::reader) &&
        value.id > 0xffffU) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, Snapshot& value) noexcept
{
    return ar.u64(value.sequence) && ar.u64(value.observed_at_ns);
}

template <typename Ar>
bool codec(Ar& ar, Context& value) noexcept
{
    return uuid_field(ar, value.epoch) && optional_field(ar, value.instance) &&
           codec(ar, value.state) && optional_field(ar, value.last_error);
}

template <typename Ar>
bool codec(Ar& ar, Tune& value) noexcept
{
    if (!ar.u64(value.revision) || value.revision == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!codec(ar, value.system) || !ar.u64(value.frequency_hz)) {
        return false;
    }
    if (value.frequency_hz == 0U || value.frequency_hz > 9007199254740991ULL) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!optional_field(ar, value.programmed_hz)) {
        return false;
    }
    // programmed_hz is a frequency like frequency_hz: positive and <= 2^53-1.
    if (value.programmed_hz.present &&
        (value.programmed_hz.value == 0U || value.programmed_hz.value > 9007199254740991ULL)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!codec(ar, value.selection) || !ar.u16(value.selection_value) ||
        !codec(ar, value.verification)) {
        return false;
    }
    if (value.selection == Selection::automatic && value.selection_value != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    // ISDB-T is terrestrial: a tuned record cannot carry a TSID/slot selection.
    if (value.system == System::isdb_t && value.selection != Selection::automatic) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, Stream& value) noexcept
{
    if (!ar.u64(value.id) || value.id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.tune_revision) || value.tune_revision == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return codec(ar, value.phase);
}

template <typename Ar>
bool codec(Ar& ar, CounterEntry& value) noexcept
{
    return ar.u8(value.id) && counter_field(ar, value.counter);
}

template <typename Ar>
bool codec(Ar& ar, StreamCounters& value) noexcept
{
    if (!array_field(ar, value.entries)) {
        return false;
    }
    for (std::size_t i = 1U; i < value.entries.count; ++i) {
        if (value.entries.items[i].id <= value.entries.items[i - 1U].id) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, ReceiverCounters& value) noexcept
{
    if (!array_field(ar, value.entries)) {
        return false;
    }
    for (std::size_t i = 0U; i < value.entries.count; ++i) {
        if (i != 0U && value.entries.items[i].id <= value.entries.items[i - 1U].id) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, MetricEntry& value) noexcept
{
    if (!ar.u8(value.id)) {
        return false;
    }
    if (value.id == kMetricIdLock) {
        return codec(ar, value.boolean);
    }
    if (value.id == kMetricIdCnrMdb || value.id == kMetricIdSignalMdbm) {
        return codec(ar, value.integer);
    }
    return ar.reject(Error::INVALID_ARGUMENT);
}

template <typename Ar>
bool codec(Ar& ar, Measurements& value) noexcept
{
    if (!array_field(ar, value.entries)) {
        return false;
    }
    for (std::size_t i = 1U; i < value.entries.count; ++i) {
        if (value.entries.items[i].id <= value.entries.items[i - 1U].id) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, StreamFinal& value) noexcept
{
    if (!ar.u64(value.id) || value.id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.tune_revision) || value.tune_revision == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!codec(ar, value.reason)) {
        return false;
    }
    return optional_field(ar, value.error) && codec(ar, value.statistics);
}

template <typename Ar>
bool codec(Ar& ar, CardObservation& value) noexcept
{
    if (!codec(ar, value.state) || !optional_field(ar, value.sampled_at_ns) ||
        !ar.u8(value.reason)) {
        return false;
    }
    if (value.reason > static_cast<std::uint8_t>(UnknownReason::read_failed)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, ReceiverStatus& value) noexcept
{
    return codec(ar, value.descriptor) && codec(ar, value.present) && codec(ar, value.state) &&
           codec(ar, value.operation) && codec(ar, value.leased) && ar.u64(value.tune_revision) &&
           optional_field(ar, value.current_tune) &&
           optional_field(ar, value.last_successful_tune) && optional_field(ar, value.stream) &&
           optional_field(ar, value.last_stream) && codec(ar, value.measurements) &&
           codec(ar, value.receiver_statistics) && optional_field(ar, value.stream_statistics) &&
           optional_field(ar, value.last_error);
}

template <typename Ar>
bool codec(Ar& ar, ReaderStatus& value) noexcept
{
    return codec(ar, value.descriptor) && codec(ar, value.present) && codec(ar, value.card) &&
           codec(ar, value.operation) && ar.u16(value.handles_open) &&
           codec(ar, value.transaction_active) && optional_field(ar, value.last_error);
}

template <typename Ar>
bool codec(Ar& ar, HelloResponse& value) noexcept
{
    return ar.u16(value.major) && ar.u16(value.minor) && ar.u64(value.features) &&
           uuid_field(ar, value.epoch) && codec(ar, value.producer) &&
           ar.u32(value.max_control_payload) && ar.u32(value.max_ts_bytes);
}

template <typename Ar>
bool codec(Ar& ar, ErrorResponse& value) noexcept
{
    return uuid_field(ar, value.epoch) && codec(ar, value.error) && codec(ar, value.target) &&
           optional_field(ar, value.retry_after_ms) &&
           optional_field(ar, value.required_payload_bytes);
}

template <typename Ar>
bool codec(Ar& ar, ListRequest& value) noexcept
{
    return uuid_field(ar, value.epoch);
}

template <typename Ar>
bool codec(Ar& ar, ListResponse& value) noexcept
{
    return uuid_field(ar, value.epoch) && codec(ar, value.producer) && codec(ar, value.snapshot) &&
           codec(ar, value.topology) && optional_field(ar, value.enclosure) &&
           array_field(ar, value.functions) && optional_field(ar, value.receivers) &&
           optional_field(ar, value.readers);
}

template <typename Ar>
bool codec(Ar& ar, StatusRequest& value) noexcept
{
    return uuid_field(ar, value.epoch) && codec(ar, value.scope);
}

template <typename Ar>
bool codec(Ar& ar, StatusResponse& value) noexcept
{
    return codec(ar, value.producer) && codec(ar, value.context) && codec(ar, value.snapshot) &&
           codec(ar, value.scope) && optional_field(ar, value.enclosure) &&
           codec(ar, value.topology) && optional_field(ar, value.functions) &&
           optional_field(ar, value.receivers) && optional_field(ar, value.readers);
}

template <typename Ar>
bool codec(Ar& ar, TuneRequest& value) noexcept
{
    if (!uuid_field(ar, value.epoch)) {
        return false;
    }
    if (!ar.u64(value.lease_id) || value.lease_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!codec(ar, value.system)) {
        return false;
    }
    if (!ar.u64(value.frequency_hz)) {
        return false;
    }
    if (value.frequency_hz == 0U || value.frequency_hz > 9007199254740991ULL) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!codec(ar, value.selection) || !ar.u16(value.selection_value)) {
        return false;
    }
    if (value.selection == Selection::automatic && value.selection_value != 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    // ISDB-T is terrestrial: no TSID or slot selection.
    if (value.system == System::isdb_t && value.selection != Selection::automatic) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!optional_field(ar, value.lnb_voltage)) {
        return false;
    }
    if (value.lnb_voltage.present && value.lnb_voltage.value != 0U && value.lnb_voltage.value != 15U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!optional_field(ar, value.lna)) {
        return false;
    }
    if (!optional_field(ar, value.pids)) {
        return false;
    }
    if (value.pids.present) {
        if (value.pids.value.count == 0U) {
            return ar.reject(Error::INVALID_ARGUMENT);
        }
        std::array<bool, kMaxPids> seen{};
        for (std::size_t i = 0U; i < value.pids.value.count; ++i) {
            const std::uint16_t pid = value.pids.value.items[i];
            if (pid > 8191U || seen[pid]) {
                return ar.reject(Error::INVALID_ARGUMENT);
            }
            seen[pid] = true;
        }
    }
    return ar.u32(value.timeout_ms);
}

template <typename Ar>
bool codec(Ar& ar, TuneResult& value) noexcept
{
    if (!uuid_field(ar, value.epoch)) {
        return false;
    }
    if (!ar.u64(value.tune_revision) || value.tune_revision == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.requested_hz) || value.requested_hz == 0U ||
        value.requested_hz > 9007199254740991ULL) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!optional_field(ar, value.programmed_hz)) {
        return false;
    }
    if (value.programmed_hz.present &&
        (value.programmed_hz.value == 0U || value.programmed_hz.value > 9007199254740991ULL)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return codec(ar, value.verification) && measurement_field(ar, value.lock) &&
           optional_field(ar, value.selected_tsid) && optional_field(ar, value.selected_slot);
}

template <typename Ar>
bool codec(Ar& ar, StartRequest& value) noexcept
{
    if (!uuid_field(ar, value.epoch)) {
        return false;
    }
    if (!ar.u64(value.lease_id) || value.lease_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.tune_revision) || value.tune_revision == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, StartResponse& value) noexcept
{
    if (!uuid_field(ar, value.epoch)) {
        return false;
    }
    if (!ar.u64(value.stream_id) || value.stream_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return uuid_field(ar, value.attach_token) && ar.u32(value.expires_after_ms);
}

template <typename Ar>
bool codec(Ar& ar, AttachRequest& value) noexcept
{
    if (!uuid_field(ar, value.epoch) || !ar.u64(value.lease_id) || value.lease_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.stream_id) || value.stream_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return uuid_field(ar, value.attach_token);
}

template <typename Ar>
bool codec(Ar& ar, AttachResponse& value) noexcept
{
    if (!uuid_field(ar, value.epoch)) {
        return false;
    }
    if (!ar.u64(value.stream_id) || value.stream_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, StopRequest& value) noexcept
{
    if (!uuid_field(ar, value.epoch) || !ar.u64(value.lease_id) || value.lease_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.stream_id) || value.stream_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, StopResponse& value) noexcept
{
    if (!uuid_field(ar, value.epoch) || !codec(ar, value.final) || !codec(ar, value.quiesced)) {
        return false;
    }
    // A non-quiesced completion means the resources are not settled; the final
    // must carry the error that explains it.
    if (!value.quiesced && !value.final.error.present) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, ReleaseRequest& value) noexcept
{
    if (!uuid_field(ar, value.epoch) || !ar.u64(value.lease_id) || value.lease_id == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, ReleaseResponse& value) noexcept
{
    if (!uuid_field(ar, value.epoch) || !ar.u64(value.lease_id) || value.lease_id == 0U ||
        !codec(ar, value.released) || !codec(ar, value.quiesced)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!optional_field(ar, value.error)) {
        return false;
    }
    if (value.released && (!value.quiesced || value.error.present)) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, StateEvent& value) noexcept
{
    return uuid_field(ar, value.epoch) && ar.u64(value.snapshot_sequence) &&
           codec(ar, value.target) && codec(ar, value.reason);
}

template <typename Ar>
bool codec(Ar& ar, StreamEndEvent& value) noexcept
{
    return codec(ar, value.final);
}

template <typename Ar>
bool codec(Ar& ar, ReaderKey& value) noexcept
{
    return uuid_field(ar, value.epoch) && ar.u16(value.reader_id);
}

template <typename Ar>
bool codec(Ar& ar, CardHandleKey& value) noexcept
{
    if (!uuid_field(ar, value.epoch) || !ar.u16(value.reader_id)) {
        return false;
    }
    // A handle key references an already issued generation and handle, both nonzero.
    if (!ar.u64(value.bound_generation) || value.bound_generation == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.card_handle) || value.card_handle == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, CardSessionResult& value) noexcept
{
    if (!uuid_field(ar, value.epoch)) {
        return false;
    }
    if (!ar.u16(value.reader_id)) {
        return false;
    }
    if (!ar.u64(value.card_handle) || value.card_handle == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.bound_generation) || value.bound_generation == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!ar.u64(value.current_generation) || value.current_generation == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    if (!codec(ar, value.present) || !codec(ar, value.initialized) ||
        !codec(ar, value.usable)) {
        return false;
    }
    return optional_field(ar, value.protocol) && optional_field(ar, value.atr);
}

template <typename Ar>
bool codec(Ar& ar, CardStatusRequest& value) noexcept
{
    return codec(ar, value.key);
}

template <typename Ar>
bool codec(Ar& ar, CardStatusResponse& value) noexcept
{
    if (!codec(ar, value.key)) {
        return false;
    }
    if (!ar.u64(value.current_generation) || value.current_generation == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return codec(ar, value.present) && codec(ar, value.initialized) &&
           optional_field(ar, value.protocol) && optional_field(ar, value.atr);
}

template <typename Ar>
bool codec(Ar& ar, CardConnectRequest& value) noexcept
{
    return codec(ar, value.key) && codec(ar, value.share) &&
           preferred_protocols_field(ar, value.preferred_protocols);
}

template <typename Ar>
bool codec(Ar& ar, CardReconnectRequest& value) noexcept
{
    return codec(ar, value.key) && codec(ar, value.share) && codec(ar, value.disposition) &&
           preferred_protocols_field(ar, value.preferred_protocols);
}

template <typename Ar>
bool codec(Ar& ar, CardDisconnectRequest& value) noexcept
{
    return codec(ar, value.key) && codec(ar, value.disposition);
}

template <typename Ar>
bool codec(Ar& ar, CardDisconnectResponse& value) noexcept
{
    if (!codec(ar, value.key)) {
        return false;
    }
    if (!ar.u64(value.card_handle) || value.card_handle == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return codec(ar, value.consumed) && codec(ar, value.quiesced) &&
           optional_field(ar, value.error);
}

template <typename Ar>
bool codec(Ar& ar, CardResetRequest& value) noexcept
{
    return codec(ar, value.key);
}

template <typename Ar>
bool codec(Ar& ar, CardTransmitRequest& value) noexcept
{
    if (!codec(ar, value.key) || !codec(ar, value.protocol)) {
        return false;
    }
    if (!bytes_field(ar, value.command)) {
        return false;
    }
    // TRANSMIT command is 1..4096 bytes; an empty command is not an APDU.
    if (value.command.length == 0U) {
        return ar.reject(Error::INVALID_ARGUMENT);
    }
    return true;
}

template <typename Ar>
bool codec(Ar& ar, CardTransmitResponse& value) noexcept
{
    return codec(ar, value.key) && bytes_field(ar, value.response);
}

template <typename Ar>
bool codec(Ar& ar, BeginTransactionRequest& value) noexcept
{
    return codec(ar, value.key);
}

template <typename Ar>
bool codec(Ar& ar, BeginTransactionResponse& value) noexcept
{
    return codec(ar, value.key);
}

template <typename Ar>
bool codec(Ar& ar, EndTransactionRequest& value) noexcept
{
    return codec(ar, value.key) && codec(ar, value.disposition);
}

template <typename Ar>
bool codec(Ar& ar, CardHandleStatusRequest& value) noexcept
{
    return codec(ar, value.key);
}

// Runs the exact codec local checks (sizes, counts, enum ranges, state/reason,
// reserved bits, caps, ordering, saturation) over a value without producing
// bytes. Validators call this first so a direct typed caller cannot bypass the
// local syntax/value rules with a static_cast that the codec would reject. The
// counting encoder never writes through the const_cast.
template <typename T>
bool syntax_ok(const T& value) noexcept
{
    Encoder counter;
    T& mutable_value = const_cast<T&>(value);
    return codec(counter, mutable_value);
}

}  // namespace

#define DTV_USERLAND_DEFINE_RECORD(NAME, FUNC)                                       \
    Result<std::size_t> encode_##FUNC(const NAME& value, MutableByteView output) noexcept \
    {                                                                                \
        if (!valid_view(output)) {                                                   \
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);            \
        }                                                                            \
        NAME& mutable_value = const_cast<NAME&>(value);                              \
        Encoder measure;                                                             \
        if (!codec(measure, mutable_value)) {                                        \
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);            \
        }                                                                            \
        if (measure.pos > kMaxControlPayload) {                                      \
            return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);              \
        }                                                                            \
        if (output.size < measure.pos) {                                             \
            return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);            \
        }                                                                            \
        Encoder writer;                                                              \
        writer.out = output.data;                                                    \
        if (!codec(writer, mutable_value)) {                                         \
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);            \
        }                                                                            \
        return Result<std::size_t>::success(measure.pos);                            \
    }                                                                                \
    Result<NAME> decode_##FUNC(ByteView input) noexcept                              \
    {                                                                                \
        if (!valid_view(input)) {                                                    \
            return Result<NAME>::failure(Error::INVALID_ARGUMENT);                   \
        }                                                                            \
        Decoder reader;                                                              \
        reader.in = input.data;                                                      \
        reader.size = input.size;                                                    \
        NAME value{};                                                                \
        if (!codec(reader, value)) {                                                 \
            return Result<NAME>::failure(reader.error);                              \
        }                                                                            \
        if (reader.pos != input.size) {                                              \
            return Result<NAME>::failure(Error::PROTOCOL_ERROR);                     \
        }                                                                            \
        return Result<NAME>::success(value);                                         \
    }

DTV_USERLAND_DEFINE_RECORD(HelloResponse, hello_response)
DTV_USERLAND_DEFINE_RECORD(ErrorResponse, error_response)
DTV_USERLAND_DEFINE_RECORD(ListRequest, list_request)
DTV_USERLAND_DEFINE_RECORD(ListResponse, list_response)
DTV_USERLAND_DEFINE_RECORD(StatusRequest, status_request)
DTV_USERLAND_DEFINE_RECORD(StatusResponse, status_response)
DTV_USERLAND_DEFINE_RECORD(TuneRequest, tune_request)
DTV_USERLAND_DEFINE_RECORD(TuneResult, tune_result)
DTV_USERLAND_DEFINE_RECORD(StartRequest, start_request)
DTV_USERLAND_DEFINE_RECORD(StartResponse, start_response)
DTV_USERLAND_DEFINE_RECORD(AttachRequest, attach_request)
DTV_USERLAND_DEFINE_RECORD(AttachResponse, attach_response)
DTV_USERLAND_DEFINE_RECORD(StopRequest, stop_request)
DTV_USERLAND_DEFINE_RECORD(StopResponse, stop_response)
DTV_USERLAND_DEFINE_RECORD(ReleaseRequest, release_request)
DTV_USERLAND_DEFINE_RECORD(ReleaseResponse, release_response)
DTV_USERLAND_DEFINE_RECORD(StateEvent, state_event)
DTV_USERLAND_DEFINE_RECORD(StreamEndEvent, stream_end_event)
DTV_USERLAND_DEFINE_RECORD(CardSessionResult, card_session_result)
DTV_USERLAND_DEFINE_RECORD(CardStatusRequest, card_status_request)
DTV_USERLAND_DEFINE_RECORD(CardStatusResponse, card_status_response)
DTV_USERLAND_DEFINE_RECORD(CardConnectRequest, card_connect_request)
DTV_USERLAND_DEFINE_RECORD(CardReconnectRequest, card_reconnect_request)
DTV_USERLAND_DEFINE_RECORD(CardDisconnectRequest, card_disconnect_request)
DTV_USERLAND_DEFINE_RECORD(CardDisconnectResponse, card_disconnect_response)
DTV_USERLAND_DEFINE_RECORD(CardResetRequest, card_reset_request)
DTV_USERLAND_DEFINE_RECORD(CardTransmitRequest, card_transmit_request)
DTV_USERLAND_DEFINE_RECORD(CardTransmitResponse, card_transmit_response)
DTV_USERLAND_DEFINE_RECORD(BeginTransactionRequest, begin_transaction_request)
DTV_USERLAND_DEFINE_RECORD(BeginTransactionResponse, begin_transaction_response)
DTV_USERLAND_DEFINE_RECORD(EndTransactionRequest, end_transaction_request)
DTV_USERLAND_DEFINE_RECORD(CardHandleStatusRequest, card_handle_status_request)
DTV_USERLAND_DEFINE_RECORD(Producer, producer)
DTV_USERLAND_DEFINE_RECORD(Model, model)
DTV_USERLAND_DEFINE_RECORD(ErrorRecord, error_record)
DTV_USERLAND_DEFINE_RECORD(Identity, identity)
DTV_USERLAND_DEFINE_RECORD(Location, location)
DTV_USERLAND_DEFINE_RECORD(Binding, binding)
DTV_USERLAND_DEFINE_RECORD(Topology, topology)
DTV_USERLAND_DEFINE_RECORD(Limits, limits)
DTV_USERLAND_DEFINE_RECORD(LnaDefaults, lna_defaults)
DTV_USERLAND_DEFINE_RECORD(LnbValues, lnb_values)
DTV_USERLAND_DEFINE_RECORD(Capabilities, capabilities)
DTV_USERLAND_DEFINE_RECORD(ReceiverDescriptor, receiver_descriptor)
DTV_USERLAND_DEFINE_RECORD(ReaderDescriptor, reader_descriptor)
DTV_USERLAND_DEFINE_RECORD(Enclosure, enclosure)
DTV_USERLAND_DEFINE_RECORD(UsbFunction, usb_function)
DTV_USERLAND_DEFINE_RECORD(Scope, scope)
DTV_USERLAND_DEFINE_RECORD(Snapshot, snapshot)
DTV_USERLAND_DEFINE_RECORD(Context, context)
DTV_USERLAND_DEFINE_RECORD(Tune, tune)
DTV_USERLAND_DEFINE_RECORD(Stream, stream)
DTV_USERLAND_DEFINE_RECORD(StreamCounters, stream_counters)
DTV_USERLAND_DEFINE_RECORD(ReceiverCounters, receiver_counters)
DTV_USERLAND_DEFINE_RECORD(Measurements, measurements)
DTV_USERLAND_DEFINE_RECORD(StreamFinal, stream_final)
DTV_USERLAND_DEFINE_RECORD(CardObservation, card_observation)
DTV_USERLAND_DEFINE_RECORD(ReceiverStatus, receiver_status)
DTV_USERLAND_DEFINE_RECORD(ReaderStatus, reader_status)
DTV_USERLAND_DEFINE_RECORD(ReaderKey, reader_key)
DTV_USERLAND_DEFINE_RECORD(CardHandleKey, card_handle_key)

#undef DTV_USERLAND_DEFINE_RECORD

Result<std::size_t> encode_ts_data(const TsData& value, MutableByteView output) noexcept
{
    if (!valid_view(output) || (value.packets.data == nullptr && value.packets.size != 0U)) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (value.stream_id == 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (value.packets.size > kMaxTsPacketBytes || (value.packets.size % kTsPacketSize) != 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    const std::size_t required = kTsDataPrefixSize + value.packets.size;
    if (output.size < required) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    store_u64(output.data, value.stream_id);
    store_u64(output.data + 8U, value.sequence);
    store_u64(output.data + 16U, value.cumulative_queue_dropped_packets);
    store_u32(output.data + 24U, static_cast<std::uint32_t>(value.packets.size));
    for (std::size_t i = 0U; i < value.packets.size; ++i) {
        output.data[kTsDataPrefixSize + i] = value.packets.data[i];
    }
    return Result<std::size_t>::success(required);
}

Result<TsData> decode_ts_data(ByteView input) noexcept
{
    if (!valid_view(input)) {
        return Result<TsData>::failure(Error::INVALID_ARGUMENT);
    }
    if (input.size < kTsDataPrefixSize) {
        return Result<TsData>::failure(Error::PROTOCOL_ERROR);
    }
    TsData value;
    value.stream_id = load_u64(input.data);
    value.sequence = load_u64(input.data + 8U);
    value.cumulative_queue_dropped_packets = load_u64(input.data + 16U);
    const std::uint32_t length = load_u32(input.data + 24U);
    if (value.stream_id == 0U) {
        return Result<TsData>::failure(Error::INVALID_ARGUMENT);
    }
    if (length > kMaxTsPacketBytes || (length % kTsPacketSize) != 0U) {
        return Result<TsData>::failure(Error::PROTOCOL_ERROR);
    }
    if (input.size - kTsDataPrefixSize != length) {
        return Result<TsData>::failure(Error::PROTOCOL_ERROR);
    }
    value.packets = ByteView{input.data + kTsDataPrefixSize, length};
    return Result<TsData>::success(value);
}

// Cross-record validation.
Error validate_error_record(const ErrorRecord& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.code == 0U) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

Error validate_scope(const Scope& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    switch (value.kind) {
    case ScopeKind::enclosure:
        return value.id == 0U ? Error::OK : Error::INVALID_ARGUMENT;
    case ScopeKind::receiver:
    case ScopeKind::reader:
        return value.id <= 0xffffU ? Error::OK : Error::INVALID_ARGUMENT;
    case ScopeKind::stream:
        // Stream IDs are issued nonzero and never reused; a zero stream scope is
        // not a valid target.
        return value.id != 0U ? Error::OK : Error::INVALID_ARGUMENT;
    }
    return Error::INVALID_ARGUMENT;
}

Error validate_capabilities(const Capabilities& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if ((value.controls & static_cast<std::uint8_t>(~(kControlBitLna | kControlBitLnb))) != 0U) {
        return Error::INVALID_ARGUMENT;
    }
    if ((value.measurement_mask & static_cast<std::uint8_t>(~kMetricMaskAll)) != 0U) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.max_pids.present && (value.max_pids.value == 0U || value.max_pids.value > kMaxPids)) {
        return Error::INVALID_ARGUMENT;
    }
    const bool has_lna = (value.controls & kControlBitLna) != 0U;
    const bool has_lnb = (value.controls & kControlBitLnb) != 0U;
    if (has_lna) {
        if (value.lna.entries.count == 0U || value.lna.entries.count > 2U) {
            return Error::INVALID_ARGUMENT;
        }
        for (std::size_t i = 0U; i < value.lna.entries.count; ++i) {
            if (i != 0U && static_cast<std::uint8_t>(value.lna.entries.items[i].system) <=
                               static_cast<std::uint8_t>(value.lna.entries.items[i - 1U].system)) {
                return Error::INVALID_ARGUMENT;
            }
        }
    } else if (value.lna.entries.count != 0U) {
        return Error::INVALID_ARGUMENT;
    }
    if (has_lnb) {
        if (value.lnb.values.count == 0U || value.lnb.values.count > 2U) {
            return Error::INVALID_ARGUMENT;
        }
        for (std::size_t i = 0U; i < value.lnb.values.count; ++i) {
            const std::uint8_t volts = value.lnb.values.items[i];
            if (volts != 0U && volts != 15U) {
                return Error::INVALID_ARGUMENT;
            }
            if (i != 0U && volts <= value.lnb.values.items[i - 1U]) {
                return Error::INVALID_ARGUMENT;
            }
        }
    } else if (value.lnb.values.count != 0U) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

Error validate_receiver_descriptor(const ReceiverDescriptor& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.systems.mask == 0U || (value.systems.mask & static_cast<std::uint8_t>(~0x03U)) != 0U) {
        return Error::INVALID_ARGUMENT;
    }
    const Error capabilities = validate_capabilities(value.capabilities);
    if (capabilities != Error::OK) {
        return capabilities;
    }
    // When LNA is provided, its defaults must cover exactly the descriptor's
    // supported systems: one entry per supported system, no missing or extra.
    if ((value.capabilities.controls & kControlBitLna) != 0U) {
        std::size_t index = 0U;
        for (std::uint8_t bit = 0U; bit < 2U; ++bit) {
            if ((value.systems.mask & static_cast<std::uint8_t>(1U << bit)) == 0U) {
                continue;
            }
            const System expected = bit == 0U ? System::isdb_t : System::isdb_s;
            if (index >= value.capabilities.lna.entries.count ||
                value.capabilities.lna.entries.items[index].system != expected) {
                return Error::INVALID_ARGUMENT;
            }
            ++index;
        }
        if (index != value.capabilities.lna.entries.count) {
            return Error::INVALID_ARGUMENT;
        }
    }
    return Error::OK;
}

Error validate_reader_descriptor(const ReaderDescriptor& value) noexcept
{
    return syntax_ok(value) ? Error::OK : Error::INVALID_ARGUMENT;
}

Error validate_topology(const Topology& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.expected_function_count.present &&
        (value.expected_function_count.value == 0U ||
         value.expected_function_count.value > kMaxFunctions)) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

Error validate_limits(const Limits& value, std::size_t public_receiver_count) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (!value.max_concurrent_streams.present) {
        return Error::OK;
    }
    const std::uint16_t streams = value.max_concurrent_streams.value;
    if (streams == 0U || streams > kMaxPublicReceivers || streams > public_receiver_count) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

Error validate_measurements(const Measurements& value, std::uint8_t measurement_mask) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.entries.count > 3U) {
        return Error::INVALID_ARGUMENT;
    }
    if ((measurement_mask & static_cast<std::uint8_t>(~kMetricMaskAll)) != 0U) {
        return Error::INVALID_ARGUMENT;
    }
    std::uint8_t declared = 0U;
    if ((measurement_mask & kMetricMaskLock) != 0U) {
        declared |= kMetricMaskLock;
    }
    if ((measurement_mask & kMetricMaskCnrMdb) != 0U) {
        declared |= kMetricMaskCnrMdb;
    }
    if ((measurement_mask & kMetricMaskSignalMdbm) != 0U) {
        declared |= kMetricMaskSignalMdbm;
    }
    std::uint8_t present = 0U;
    for (std::size_t i = 0U; i < value.entries.count; ++i) {
        const MetricEntry& entry = value.entries.items[i];
        if (i != 0U && entry.id <= value.entries.items[i - 1U].id) {
            return Error::INVALID_ARGUMENT;
        }
        switch (entry.id) {
        case kMetricIdLock:
            if (entry.boolean.state == MeasureState::unsupported) {
                return Error::INVALID_ARGUMENT;
            }
            present |= kMetricMaskLock;
            break;
        case kMetricIdCnrMdb:
            if (entry.integer.state == MeasureState::unsupported) {
                return Error::INVALID_ARGUMENT;
            }
            present |= kMetricMaskCnrMdb;
            break;
        case kMetricIdSignalMdbm:
            if (entry.integer.state == MeasureState::unsupported) {
                return Error::INVALID_ARGUMENT;
            }
            present |= kMetricMaskSignalMdbm;
            break;
        default:
            return Error::INVALID_ARGUMENT;
        }
    }
    return present == declared ? Error::OK : Error::INVALID_ARGUMENT;
}

// A saturated valid counter carries value == UINT64_MAX together with
// overflow=true; a wrap is never represented.
Error validate_counter(const Counter& value) noexcept
{
    if (value.measurement.state != MeasureState::valid) {
        return Error::OK;
    }
    if (value.overflow && value.measurement.value != 0xffffffffffffffffULL) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

Error validate_stream_counters(const StreamCounters& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.entries.count != 2U) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.entries.items[0].id != kCounterIdTransmittedPackets ||
        value.entries.items[1].id != kCounterIdQueueDroppedPackets) {
        return Error::INVALID_ARGUMENT;
    }
    for (std::size_t i = 0U; i < value.entries.count; ++i) {
        if (value.entries.items[i].counter.measurement.state == MeasureState::unsupported) {
            return Error::INVALID_ARGUMENT;
        }
        if (validate_counter(value.entries.items[i].counter) != Error::OK) {
            return Error::INVALID_ARGUMENT;
        }
    }
    return Error::OK;
}

Error validate_receiver_counters(const ReceiverCounters& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.entries.count > 4U) {
        return Error::INVALID_ARGUMENT;
    }
    for (std::size_t i = 0U; i < value.entries.count; ++i) {
        const std::uint8_t id = value.entries.items[i].id;
        if (id < kCounterIdCapturedPackets || id > kCounterIdUpstreamLostPackets) {
            return Error::INVALID_ARGUMENT;
        }
        if (i != 0U && id <= value.entries.items[i - 1U].id) {
            return Error::INVALID_ARGUMENT;
        }
        if (value.entries.items[i].counter.measurement.state == MeasureState::unsupported) {
            return Error::INVALID_ARGUMENT;
        }
        if (validate_counter(value.entries.items[i].counter) != Error::OK) {
            return Error::INVALID_ARGUMENT;
        }
    }
    return Error::OK;
}

Error validate_stream_final(const StreamFinal& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.id == 0U || value.tune_revision == 0U) {
        return Error::INVALID_ARGUMENT;
    }
    const Error counters = validate_stream_counters(value.statistics);
    if (counters != Error::OK) {
        return counters;
    }
    if (value.reason == StreamEndReason::stopped || value.reason == StreamEndReason::shutdown) {
        return Error::OK;
    }
    if (!value.error.present) {
        return Error::INVALID_ARGUMENT;
    }
    return validate_error_record(value.error.value);
}

Error validate_receiver_status(const ReceiverStatus& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    const Error descriptor = validate_receiver_descriptor(value.descriptor);
    if (descriptor != Error::OK) {
        return descriptor;
    }
    const Error metrics =
        validate_measurements(value.measurements, value.descriptor.capabilities.measurement_mask);
    if (metrics != Error::OK) {
        return metrics;
    }
    const Error counters = validate_receiver_counters(value.receiver_statistics);
    if (counters != Error::OK) {
        return counters;
    }
    if (value.stream_statistics.present) {
        const Error stream = validate_stream_counters(value.stream_statistics.value);
        if (stream != Error::OK) {
            return stream;
        }
    }
    if (value.last_stream.present) {
        const Error last = validate_stream_final(value.last_stream.value);
        if (last != Error::OK) {
            return last;
        }
    }
    return Error::OK;
}

Error validate_reader_status(const ReaderStatus& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.handles_open > 64U) {
        return Error::INVALID_ARGUMENT;
    }
    return validate_card_observation(value.card);
}

Error validate_card_observation(const CardObservation& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    switch (value.state) {
    case CardState::present:
    case CardState::absent:
        if (value.reason != 0U || !value.sampled_at_ns.present) {
            return Error::INVALID_ARGUMENT;
        }
        return Error::OK;
    case CardState::unknown:
        if (value.sampled_at_ns.present) {
            return Error::INVALID_ARGUMENT;
        }
        return (value.reason == static_cast<std::uint8_t>(UnknownReason::not_sampled) ||
                value.reason == static_cast<std::uint8_t>(UnknownReason::stale))
                   ? Error::OK
                   : Error::INVALID_ARGUMENT;
    case CardState::unavailable:
        if (value.sampled_at_ns.present) {
            return Error::INVALID_ARGUMENT;
        }
        return (value.reason == static_cast<std::uint8_t>(UnknownReason::read_failed) ||
                value.reason == static_cast<std::uint8_t>(UnknownReason::device_lost))
                   ? Error::OK
                   : Error::INVALID_ARGUMENT;
    }
    return Error::INVALID_ARGUMENT;
}

Error validate_card_session_result(const CardSessionResult& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.card_handle == 0U || value.bound_generation == 0U ||
        value.current_generation == 0U) {
        return Error::INVALID_ARGUMENT;
    }
    if (!value.present && value.initialized) {
        return Error::INVALID_ARGUMENT;
    }
    // protocol/ATR describe this handle only; a stale (usable=false) or
    // uninitialized handle must not carry them.
    if (value.usable) {
        if (!value.present || !value.initialized) {
            return Error::INVALID_ARGUMENT;
        }
        if (value.bound_generation != value.current_generation) {
            return Error::INVALID_ARGUMENT;
        }
        if (!value.protocol.present || !value.atr.present || value.atr.value.length == 0U) {
            return Error::INVALID_ARGUMENT;
        }
    } else if (value.protocol.present || value.atr.present) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

Error validate_card_status_response(const CardStatusResponse& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.initialized) {
        if (!value.present || !value.protocol.present || !value.atr.present ||
            value.atr.value.length == 0U) {
            return Error::INVALID_ARGUMENT;
        }
    } else if (value.protocol.present || value.atr.present) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

Error validate_binding(const Binding& value, const std::uint16_t* function_ids,
                       std::size_t function_count) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    for (std::size_t i = 0U; i < function_count; ++i) {
        if (function_ids[i] == value.function_id) {
            return Error::OK;
        }
    }
    return Error::INVALID_ARGUMENT;
}

Error validate_list_response(const ListResponse& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.functions.count > kMaxFunctions ||
        (value.receivers.present && value.receivers.value.count > kMaxPublicReceivers) ||
        (value.readers.present && value.readers.value.count > kMaxReaders)) {
        return Error::INVALID_ARGUMENT;
    }
    std::uint16_t function_ids[kMaxFunctions]{};
    for (std::size_t i = 0U; i < value.functions.count; ++i) {
        function_ids[i] = value.functions.items[i].id;
        for (std::size_t j = 0U; j < i; ++j) {
            if (function_ids[j] == function_ids[i]) {
                return Error::INVALID_ARGUMENT;
            }
        }
    }
    if (value.topology.state == TopologyState::unknown) {
        if (value.receivers.present || value.readers.present) {
            return Error::INVALID_ARGUMENT;
        }
        return Error::OK;
    }
    // Known topology (partial or complete) returns both collections; a
    // known-empty reader set is a present array with count 0, not an absent one.
    if (!value.receivers.present || !value.readers.present) {
        return Error::INVALID_ARGUMENT;
    }
    {
        for (std::size_t i = 0U; i < value.receivers.value.count; ++i) {
            const ReceiverDescriptor& receiver = value.receivers.value.items[i];
            const Error descriptor = validate_receiver_descriptor(receiver);
            if (descriptor != Error::OK) {
                return descriptor;
            }
            if (receiver.binding.present) {
                const Error binding = validate_binding(receiver.binding.value, function_ids,
                                                       value.functions.count);
                if (binding != Error::OK) {
                    return binding;
                }
            }
            for (std::size_t j = 0U; j < i; ++j) {
                if (value.receivers.value.items[j].id == receiver.id) {
                    return Error::INVALID_ARGUMENT;
                }
            }
        }
    }
    if (value.readers.present) {
        for (std::size_t i = 0U; i < value.readers.value.count; ++i) {
            const ReaderDescriptor& reader = value.readers.value.items[i];
            if (reader.binding.present) {
                const Error binding = validate_binding(reader.binding.value, function_ids,
                                                       value.functions.count);
                if (binding != Error::OK) {
                    return binding;
                }
            }
            for (std::size_t j = 0U; j < i; ++j) {
                if (value.readers.value.items[j].id == reader.id) {
                    return Error::INVALID_ARGUMENT;
                }
            }
        }
    }
    if (value.enclosure.present) {
        const Error limits = validate_limits(
            value.enclosure.value.limits, value.receivers.present ? value.receivers.value.count : 0U);
        if (limits != Error::OK) {
            return limits;
        }
    }
    return validate_topology(value.topology);
}

Error validate_status_response_scope(const StatusResponse& value) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    const Error scope = validate_scope(value.scope);
    if (scope != Error::OK) {
        return scope;
    }
    if (value.scope.kind == ScopeKind::enclosure) {
        if (!value.functions.present) {
            return Error::INVALID_ARGUMENT;
        }
        if (value.topology.state == TopologyState::unknown) {
            return (value.receivers.present || value.readers.present) ? Error::INVALID_ARGUMENT
                                                                      : Error::OK;
        }
        return (value.receivers.present && value.readers.present) ? Error::OK
                                                                  : Error::INVALID_ARGUMENT;
    }
    if (value.functions.present) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.scope.kind == ScopeKind::reader) {
        if (!value.readers.present || value.readers.value.count != 1U ||
            value.readers.value.items[0].descriptor.id != static_cast<std::uint16_t>(value.scope.id)) {
            return Error::INVALID_ARGUMENT;
        }
        return value.receivers.present ? Error::INVALID_ARGUMENT : Error::OK;
    }
    if (!value.receivers.present || value.receivers.value.count != 1U) {
        return Error::INVALID_ARGUMENT;
    }
    if (value.readers.present) {
        return Error::INVALID_ARGUMENT;
    }
    const ReceiverStatus& receiver = value.receivers.value.items[0];
    if (value.scope.kind == ScopeKind::receiver) {
        if (receiver.descriptor.id != static_cast<std::uint16_t>(value.scope.id)) {
            return Error::INVALID_ARGUMENT;
        }
        return Error::OK;
    }
    // Stream scope: the receiver owning the stream is returned, but the scope id
    // is a stream id, not a receiver id, so it is matched against the stream.
    const bool in_current = receiver.stream.present && receiver.stream.value.id == value.scope.id;
    const bool in_last =
        receiver.last_stream.present && receiver.last_stream.value.id == value.scope.id;
    if (!in_current && !in_last) {
        return Error::INVALID_ARGUMENT;
    }
    return Error::OK;
}

namespace {

Error check_sampled_time(std::uint64_t sampled, std::uint64_t observed) noexcept
{
    return sampled <= observed ? Error::OK : Error::INVALID_ARGUMENT;
}

Error check_measurement_time(const Measurement<std::uint64_t>& measurement,
                             std::uint64_t observed) noexcept
{
    if (measurement.state != MeasureState::valid) {
        return Error::OK;
    }
    return check_sampled_time(measurement.sampled_at_ns, observed);
}

Error check_metric_time(const MetricEntry& entry, std::uint64_t observed) noexcept
{
    if (entry.id == kMetricIdLock) {
        if (entry.boolean.state != MeasureState::valid) {
            return Error::OK;
        }
        return check_sampled_time(entry.boolean.sampled_at_ns, observed);
    }
    if (entry.integer.state != MeasureState::valid) {
        return Error::OK;
    }
    return check_sampled_time(entry.integer.sampled_at_ns, observed);
}

Error check_counter_set_time(const BArray<CounterEntry, 2>& entries, std::uint64_t observed) noexcept
{
    for (std::size_t i = 0U; i < entries.count; ++i) {
        const Error error = check_measurement_time(entries.items[i].counter.measurement, observed);
        if (error != Error::OK) {
            return error;
        }
    }
    return Error::OK;
}

Error check_counter_set_time(const BArray<CounterEntry, 4>& entries, std::uint64_t observed) noexcept
{
    for (std::size_t i = 0U; i < entries.count; ++i) {
        const Error error = check_measurement_time(entries.items[i].counter.measurement, observed);
        if (error != Error::OK) {
            return error;
        }
    }
    return Error::OK;
}

Error check_receiver_status_time(const ReceiverStatus& receiver, std::uint64_t observed) noexcept
{
    for (std::size_t i = 0U; i < receiver.measurements.entries.count; ++i) {
        const Error error = check_metric_time(receiver.measurements.entries.items[i], observed);
        if (error != Error::OK) {
            return error;
        }
    }
    Error error = check_counter_set_time(receiver.receiver_statistics.entries, observed);
    if (error != Error::OK) {
        return error;
    }
    if (receiver.stream_statistics.present) {
        error = check_counter_set_time(receiver.stream_statistics.value.entries, observed);
        if (error != Error::OK) {
            return error;
        }
    }
    if (receiver.last_stream.present) {
        error = check_counter_set_time(receiver.last_stream.value.statistics.entries, observed);
        if (error != Error::OK) {
            return error;
        }
    }
    return Error::OK;
}

}  // namespace

namespace {

bool same_capabilities(const Capabilities& a, const Capabilities& b) noexcept
{
    if (a.ts_scope != b.ts_scope || a.controls != b.controls ||
        a.measurement_mask != b.measurement_mask) {
        return false;
    }
    if (a.max_pids.present != b.max_pids.present ||
        (a.max_pids.present && a.max_pids.value != b.max_pids.value)) {
        return false;
    }
    if (a.lna.entries.count != b.lna.entries.count ||
        a.lnb.values.count != b.lnb.values.count) {
        return false;
    }
    for (std::size_t i = 0U; i < a.lna.entries.count; ++i) {
        const LnaDefaultEntry& left = a.lna.entries.items[i];
        const LnaDefaultEntry& right = b.lna.entries.items[i];
        if (left.system != right.system ||
            left.default_value.present != right.default_value.present ||
            (left.default_value.present && left.default_value.value != right.default_value.value)) {
            return false;
        }
    }
    for (std::size_t i = 0U; i < a.lnb.values.count; ++i) {
        if (a.lnb.values.items[i] != b.lnb.values.items[i]) {
            return false;
        }
    }
    return true;
}

bool same_binding(const Optional<Binding>& a, const Optional<Binding>& b) noexcept
{
    if (a.present != b.present) {
        return false;
    }
    if (!a.present) {
        return true;
    }
    return a.value.function_id == b.value.function_id && a.value.local_id == b.value.local_id;
}

// Membership plus identity: the STATUS descriptor must equal the canonical
// published descriptor for that ID in systems, binding and capabilities.
const ReceiverDescriptor* find_canonical_receiver(const ListResponse& list,
                                                  std::uint16_t id) noexcept
{
    if (!list.receivers.present) {
        return nullptr;
    }
    for (std::size_t i = 0U; i < list.receivers.value.count; ++i) {
        if (list.receivers.value.items[i].id == id) {
            return &list.receivers.value.items[i];
        }
    }
    return nullptr;
}

const ReaderDescriptor* find_canonical_reader(const ListResponse& list, std::uint16_t id) noexcept
{
    if (!list.readers.present) {
        return nullptr;
    }
    for (std::size_t i = 0U; i < list.readers.value.count; ++i) {
        if (list.readers.value.items[i].id == id) {
            return &list.readers.value.items[i];
        }
    }
    return nullptr;
}

const UsbFunction* find_canonical_function(const ListResponse& list, std::uint16_t id) noexcept
{
    for (std::size_t i = 0U; i < list.functions.count; ++i) {
        if (list.functions.items[i].id == id) {
            return &list.functions.items[i];
        }
    }
    return nullptr;
}

}  // namespace

Error validate_status_response(const StatusResponse& value,
                               const StatusContext& context) noexcept
{
    if (!syntax_ok(value)) {
        return Error::INVALID_ARGUMENT;
    }
    // Shape: which collections the scope must carry.
    const Error shape = validate_status_response_scope(value);
    if (shape != Error::OK) {
        return shape;
    }
    // A canonical same-epoch LIST is required. The partial record's own claims
    // never substitute for it.
    if (context.list == nullptr) {
        return Error::INVALID_ARGUMENT;
    }
    const ListResponse& list = *context.list;
    const Error list_status = validate_list_response(list);
    if (list_status != Error::OK) {
        return list_status;
    }
    // Same epoch: the response context must belong to the canonical list epoch.
    if (value.context.epoch != list.epoch) {
        return Error::INVALID_ARGUMENT;
    }
    const bool global = value.scope.kind == ScopeKind::enclosure;

    // A global snapshot covers the whole context, so its collection presence
    // must match the canonical same-epoch LIST: a known canonical topology
    // requires the response to return both collections, and a genuinely unknown
    // canonical topology requires both to be omitted. This is enforced on
    // presence, not only when the response happens to supply a collection.
    if (global) {
        const bool canonical_known = list.topology.state != TopologyState::unknown;
        if (canonical_known) {
            if (value.topology.state == TopologyState::unknown || !value.receivers.present ||
                !value.readers.present) {
                return Error::INVALID_ARGUMENT;
            }
        } else if (value.topology.state != TopologyState::unknown || value.receivers.present ||
                   value.readers.present) {
            return Error::INVALID_ARGUMENT;
        }
    }

    // STATUS never asserts scan uniqueness and must not conflate an unknown
    // serial state with confirmed absence: uniqueness is unknown while the
    // serial state is present or unknown, and not_applicable only on confirmed
    // absence. unique_in_scan and duplicate stay LIST/discovery values.
    if (value.enclosure.present) {
        const Identity& identity = value.enclosure.value.identity;
        switch (identity.serial_state) {
        case IdentitySerialState::unknown:
        case IdentitySerialState::present:
            if (identity.uniqueness != Uniqueness::unknown) {
                return Error::INVALID_ARGUMENT;
            }
            break;
        case IdentitySerialState::absent:
            if (identity.uniqueness != Uniqueness::not_applicable) {
                return Error::INVALID_ARGUMENT;
            }
            break;
        }
    }

    // Functions: every published function in the response must be canonical.
    if (value.functions.present) {
        if (value.functions.value.count > kMaxFunctions) {
            return Error::INVALID_ARGUMENT;
        }
        for (std::size_t i = 0U; i < value.functions.value.count; ++i) {
            for (std::size_t j = 0U; j < i; ++j) {
                if (value.functions.value.items[j].id == value.functions.value.items[i].id) {
                    return Error::INVALID_ARGUMENT;
                }
            }
            if (find_canonical_function(list, value.functions.value.items[i].id) == nullptr) {
                return Error::INVALID_ARGUMENT;
            }
        }
        // The global scope returns the complete same-epoch function set.
        if (global && value.functions.value.count != list.functions.count) {
            return Error::INVALID_ARGUMENT;
        }
    }

    // Receivers: membership, descriptor identity and sample times.
    if (value.receivers.present) {
        if (value.receivers.value.count > kMaxPublicReceivers) {
            return Error::INVALID_ARGUMENT;
        }
        for (std::size_t i = 0U; i < value.receivers.value.count; ++i) {
            const ReceiverStatus& receiver = value.receivers.value.items[i];
            for (std::size_t j = 0U; j < i; ++j) {
                if (value.receivers.value.items[j].descriptor.id == receiver.descriptor.id) {
                    return Error::INVALID_ARGUMENT;
                }
            }
            const Error status = validate_receiver_status(receiver);
            if (status != Error::OK) {
                return status;
            }
            const ReceiverDescriptor* canonical =
                find_canonical_receiver(list, receiver.descriptor.id);
            if (canonical == nullptr) {
                return Error::INVALID_ARGUMENT;
            }
            if (!same_binding(receiver.descriptor.binding, canonical->binding) ||
                !same_capabilities(receiver.descriptor.capabilities, canonical->capabilities) ||
                receiver.descriptor.systems.mask != canonical->systems.mask) {
                return Error::INVALID_ARGUMENT;
            }
            if (receiver.descriptor.binding.present &&
                find_canonical_function(list, receiver.descriptor.binding.value.function_id) ==
                    nullptr) {
                return Error::INVALID_ARGUMENT;
            }
            const Error times = check_receiver_status_time(receiver, value.snapshot.observed_at_ns);
            if (times != Error::OK) {
                return times;
            }
        }
        // The global scope returns the complete same-epoch receiver set.
        if (global && value.receivers.value.count !=
                          (list.receivers.present ? list.receivers.value.count : 0U)) {
            return Error::INVALID_ARGUMENT;
        }
    }

    // Readers: membership, descriptor identity and sample times.
    if (value.readers.present) {
        if (value.readers.value.count > kMaxReaders) {
            return Error::INVALID_ARGUMENT;
        }
        for (std::size_t i = 0U; i < value.readers.value.count; ++i) {
            const ReaderStatus& reader = value.readers.value.items[i];
            for (std::size_t j = 0U; j < i; ++j) {
                if (value.readers.value.items[j].descriptor.id == reader.descriptor.id) {
                    return Error::INVALID_ARGUMENT;
                }
            }
            const Error status = validate_reader_status(reader);
            if (status != Error::OK) {
                return status;
            }
            const ReaderDescriptor* canonical = find_canonical_reader(list, reader.descriptor.id);
            if (canonical == nullptr) {
                return Error::INVALID_ARGUMENT;
            }
            if (!same_binding(reader.descriptor.binding, canonical->binding)) {
                return Error::INVALID_ARGUMENT;
            }
            if (reader.descriptor.binding.present &&
                find_canonical_function(list, reader.descriptor.binding.value.function_id) ==
                    nullptr) {
                return Error::INVALID_ARGUMENT;
            }
            if (reader.card.sampled_at_ns.present &&
                reader.card.sampled_at_ns.value > value.snapshot.observed_at_ns) {
                return Error::INVALID_ARGUMENT;
            }
        }
        if (global && value.readers.value.count !=
                          (list.readers.present ? list.readers.value.count : 0U)) {
            return Error::INVALID_ARGUMENT;
        }
    }

    const Error topology = validate_topology(value.topology);
    if (topology != Error::OK) {
        return topology;
    }
    if (value.enclosure.present &&
        value.enclosure.value.limits.max_concurrent_streams.present) {
        const std::size_t public_receivers =
            list.receivers.present ? list.receivers.value.count : 0U;
        const Error limits = validate_limits(value.enclosure.value.limits, public_receivers);
        if (limits != Error::OK) {
            return limits;
        }
    }

    // A retained (last) stream may only be queried while the receiver is still
    // leased; a released receiver's stream is no longer a valid target.
    if (value.scope.kind == ScopeKind::stream) {
        const ReceiverStatus& receiver = value.receivers.value.items[0];
        if (!receiver.leased) {
            return Error::INVALID_ARGUMENT;
        }
    }
    return Error::OK;
}

}  // namespace dtv::userland::wire
