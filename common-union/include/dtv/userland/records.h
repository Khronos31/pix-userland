// SPDX-License-Identifier: MIT
#ifndef DTV_USERLAND_RECORDS_H
#define DTV_USERLAND_RECORDS_H

#include <dtv/userland/detail/text.h>
#include <dtv/userland/wire.h>

#include <array>
#include <cstddef>
#include <cstdint>

// Typed UDTV 1.0 wire records: HELLO response, error, LIST/STATUS snapshots,
// tune/stream lifecycle, TS_DATA, events and card operations. Every fixed and
// variable-length field is stored in an explicit bounded container; the codecs
// never allocate and never copy a host struct to or from the wire. See
// docs/wire-records.md for the field tables, the validation split between
// syntactic decoding and cross-record checks, and the open ambiguities.
//
// Encoding conventions (wire-record-layout.md):
//   str[N]    u16 byte length + UTF-8 bytes, at most N, no embedded NUL
//   bytes[N]  u32 length + bytes, at most N
//   arr<T,N>  u16 count + elements, at most N
//   opt<T>    u8 tag (0 absent / 1 present) + value when present
// Every length, count and cap is checked before the corresponding element is
// read or written. Encoders measure the whole record first and return
// BUFFER_TOO_SMALL without writing a partial record when the output is short.
namespace dtv::userland::wire {

// Feature bits for HelloRequest/HelloResponse. Unknown bits are preserved.
inline constexpr std::uint64_t kFeatureStateEvent = 1ULL << 0;
inline constexpr std::uint64_t kFeatureCardService = 1ULL << 1;
inline constexpr std::uint64_t kFeaturePidFilter = 1ULL << 2;

// Opcodes of the records in this header. 0x8050..0x80fe are unused here; the
// envelope already accepts unknown opcodes for the dispatcher.
inline constexpr std::uint16_t kOpcodeList = 0x0010U;
inline constexpr std::uint16_t kOpcodeStatus = 0x0011U;
inline constexpr std::uint16_t kOpcodeRelease = 0x0021U;
inline constexpr std::uint16_t kOpcodeTune = 0x0022U;
inline constexpr std::uint16_t kOpcodeStartStream = 0x0023U;
inline constexpr std::uint16_t kOpcodeStopStream = 0x0024U;
inline constexpr std::uint16_t kOpcodeCardStatus = 0x0030U;
inline constexpr std::uint16_t kOpcodeCardConnect = 0x0031U;
inline constexpr std::uint16_t kOpcodeCardReconnect = 0x0032U;
inline constexpr std::uint16_t kOpcodeCardDisconnect = 0x0033U;
inline constexpr std::uint16_t kOpcodeCardReset = 0x0034U;
inline constexpr std::uint16_t kOpcodeCardTransmit = 0x0035U;
inline constexpr std::uint16_t kOpcodeBeginTransaction = 0x0036U;
inline constexpr std::uint16_t kOpcodeEndTransaction = 0x0037U;
inline constexpr std::uint16_t kOpcodeCardHandleStatus = 0x0038U;
inline constexpr std::uint16_t kOpcodeAttachStream = 0x0040U;
inline constexpr std::uint16_t kOpcodeTsDataEvent = 0x8040U;
inline constexpr std::uint16_t kOpcodeStateEvent = 0x80f0U;
inline constexpr std::uint16_t kOpcodeStreamEndEvent = 0x80ffU;

// Resource caps from wire-record-layout.md section 1. The function count is
// the one shared definition used by the identity codecs as well.
inline constexpr std::size_t kMaxFunctions = detail::kMaxFunctions;
inline constexpr std::size_t kMaxPublicReceivers = 32U;
inline constexpr std::size_t kMaxReaders = 8U;
inline constexpr std::size_t kMaxPids = 8192U;
inline constexpr std::size_t kMaxAtrBytes = 33U;
inline constexpr std::size_t kMaxApduBytes = 4096U;

inline constexpr std::uint8_t kMetricIdLock = 1U;
inline constexpr std::uint8_t kMetricIdCnrMdb = 2U;
inline constexpr std::uint8_t kMetricIdSignalMdbm = 3U;
inline constexpr std::uint8_t kMetricMaskLock = 1U << 0;
inline constexpr std::uint8_t kMetricMaskCnrMdb = 1U << 1;
inline constexpr std::uint8_t kMetricMaskSignalMdbm = 1U << 2;
inline constexpr std::uint8_t kMetricMaskAll = 0x07U;

inline constexpr std::uint8_t kControlBitLna = 1U << 0;
inline constexpr std::uint8_t kControlBitLnb = 1U << 1;

inline constexpr std::uint8_t kCounterIdCapturedPackets = 1U;
inline constexpr std::uint8_t kCounterIdUsbErrors = 2U;
inline constexpr std::uint8_t kCounterIdFramingErrors = 3U;
inline constexpr std::uint8_t kCounterIdUpstreamLostPackets = 4U;
inline constexpr std::uint8_t kCounterIdTransmittedPackets = 5U;
inline constexpr std::uint8_t kCounterIdQueueDroppedPackets = 6U;

// Scalar enums (wire-record-layout.md section 2). Unknown values are rejected
// by the codecs; a future minor may widen them.
enum class System : std::uint8_t { isdb_t = 1U, isdb_s = 2U };
enum class ContextState : std::uint8_t {
    starting = 0U,
    ready = 1U,
    stopping = 2U,
    error = 3U,
    disconnected = 4U,
};
enum class TopologyState : std::uint8_t { unknown = 0U, partial = 1U, complete = 2U };
enum class ReceiverState : std::uint8_t {
    free = 0U,
    leased = 1U,
    tuned = 2U,
    streaming = 3U,
    error = 4U,
    absent = 5U,
};
enum class Operation : std::uint8_t { idle = 0U, tuning = 1U, stopping = 2U, releasing = 3U };
enum class StreamPhase : std::uint8_t {
    armed = 1U,
    attaching = 2U,
    active = 3U,
    closing = 4U,
};
enum class ScopeKind : std::uint8_t {
    enclosure = 0U,
    receiver = 1U,
    reader = 2U,
    stream = 3U,
};
enum class SerialState : std::uint8_t {
    not_queried = 0U,
    present = 1U,
    absent = 2U,
    unreadable = 3U,
    invalid = 4U,
};
enum class IdentitySerialState : std::uint8_t { unknown = 0U, present = 1U, absent = 2U };
enum class Uniqueness : std::uint8_t {
    unknown = 0U,
    unique_in_scan = 1U,
    duplicate = 2U,
    not_applicable = 3U,
};
enum class Mode : std::uint8_t { unknown = 0U, cold = 1U, runtime = 2U };
enum class UnknownReason : std::uint8_t {
    not_sampled = 1U,
    stale = 2U,
    tune_changed = 3U,
    device_lost = 4U,
    read_failed = 5U,
};
enum class IdentificationReason : std::uint8_t {
    none = 0U,
    cold_model = 1U,
    descriptor_unreadable = 2U,
    model_unconfirmed = 3U,
};
enum class Verification : std::uint8_t { commands = 0U, lock = 1U, ts_sync = 2U };
enum class Selection : std::uint8_t { automatic = 0U, tsid = 1U, slot = 2U };
enum class TsScope : std::uint8_t { multiplex = 0U, pid_subset = 1U };
enum class StreamEndReason : std::uint8_t {
    stopped = 0U,
    data_closed = 1U,
    owner_closed = 2U,
    attach_expired = 3U,
    slow_consumer = 4U,
    device_lost = 5U,
    backend_error = 6U,
    shutdown = 7U,
};
enum class CardState : std::uint8_t {
    unknown = 0U,
    absent = 1U,
    present = 2U,
    unavailable = 3U,
};
enum class CardOperation : std::uint8_t {
    idle = 0U,
    initializing = 1U,
    transmitting = 2U,
    resetting = 3U,
    stopping = 4U,
};
enum class Share : std::uint8_t { shared = 1U, exclusive = 2U };
enum class Disposition : std::uint8_t { leave = 0U, reset = 1U };
enum class CardProtocol : std::uint8_t { t1 = 1U };
enum class MeasureState : std::uint8_t { unsupported = 0U, unknown = 1U, valid = 2U };
enum class StateEventReason : std::uint8_t {
    context_changed = 1U,
    receiver_changed = 2U,
    reader_changed = 3U,
    device_changed = 4U,
};

// Bounded containers. length/count is the number of valid elements; the array
// is the storage cap. Nothing here allocates.
template <std::size_t N>
struct WString final {
    std::uint16_t length = 0U;
    std::array<char, N> bytes{};
};

template <std::size_t N>
struct BBytes final {
    std::uint32_t length = 0U;
    std::array<std::uint8_t, N> bytes{};
};

template <typename T, std::size_t N>
struct BArray final {
    std::uint16_t count = 0U;
    std::array<T, N> items{};
};

template <typename T>
struct Optional final {
    bool present = false;
    T value{};
};

template <typename T>
struct Measurement final {
    MeasureState state = MeasureState::unsupported;
    UnknownReason reason = UnknownReason::not_sampled;
    T value{};
    std::uint64_t sampled_at_ns = 0U;
};

// Counter is Measurement<u64> plus a trailing overflow byte on the valid path.
struct Counter final {
    Measurement<std::uint64_t> measurement;
    bool overflow = false;
};

// Systems is a bitmask: bit0 isdb-t, bit1 isdb-s, zero forbidden, other bits
// reserved. Stored as its raw byte so an unknown bit is not silently dropped.
struct Systems final {
    std::uint8_t mask = 0U;
};

using Uuid = Epoch;

struct Producer final {
    WString<32> product;
    WString<64> version;
    Optional<WString<64>> core_version;
};

struct Model final {
    WString<64> id;
    WString<128> name;
};

// Error record. code is u32 so an unknown nonzero code keeps its value; code 0
// is never valid in a present Error record.
struct ErrorRecord final {
    std::uint32_t code = 0U;
    WString<32> operation;
    WString<256> detail;
};

struct Identity final {
    IdentitySerialState serial_state = IdentitySerialState::unknown;
    Optional<WString<detail::kMaxSerialBytes>> serial;
    Uniqueness uniqueness = Uniqueness::unknown;
};

struct Location final {
    Optional<WString<detail::kMaxUsbPathBytes>> usb_path;
    Optional<std::uint8_t> bus;
    Optional<std::uint8_t> address;
};

struct Binding final {
    std::uint16_t function_id = 0U;
    std::uint16_t local_id = 0U;
};

struct Topology final {
    TopologyState state = TopologyState::unknown;
    Optional<std::uint16_t> expected_function_count;
};

struct Limits final {
    Optional<std::uint16_t> max_concurrent_streams;
};

struct LnaDefaultEntry final {
    System system = System::isdb_t;
    Optional<bool> default_value;
};

struct LnaDefaults final {
    BArray<LnaDefaultEntry, 2> entries;
};

struct LnbValues final {
    BArray<std::uint8_t, 2> values;
};

// Capabilities. controls bit0 carries a following LnaDefaults body and bit1 a
// following LnbValues body with no extra optional tag; when a bit is clear its
// body is absent. measurement_mask declares the metric IDs that must appear in
// Measurements.
struct Capabilities final {
    TsScope ts_scope = TsScope::multiplex;
    Optional<std::uint16_t> max_pids;
    std::uint8_t controls = 0U;
    LnaDefaults lna;
    LnbValues lnb;
    std::uint8_t measurement_mask = 0U;
};

struct ReceiverDescriptor final {
    std::uint16_t id = 0U;
    Systems systems;
    Optional<Binding> binding;
    Capabilities capabilities;
};

struct ReaderDescriptor final {
    std::uint16_t id = 0U;
    Optional<Binding> binding;
};

struct Enclosure final {
    std::uint16_t id = 0U;
    Optional<Model> model;
    Identity identity;
    Limits limits;
};

struct UsbFunction final {
    std::uint16_t id = 0U;
    std::uint16_t vid = 0U;
    std::uint16_t pid = 0U;
    Mode mode = Mode::unknown;
    Location location;
    SerialState serial_state = SerialState::not_queried;
    Optional<WString<detail::kMaxSerialBytes>> serial;
    Optional<Model> model;
    IdentificationReason identification_reason = IdentificationReason::none;
    bool present = false;
    Optional<ErrorRecord> last_error;
};

struct Scope final {
    ScopeKind kind = ScopeKind::enclosure;
    std::uint64_t id = 0U;
};

struct Snapshot final {
    std::uint64_t sequence = 0U;
    std::uint64_t observed_at_ns = 0U;
};

struct Context final {
    Uuid epoch{};
    Optional<WString<detail::kMaxInstanceBytes>> instance;
    ContextState state = ContextState::starting;
    Optional<ErrorRecord> last_error;
};

struct Tune final {
    std::uint64_t revision = 0U;
    System system = System::isdb_t;
    std::uint64_t frequency_hz = 0U;
    Optional<std::uint64_t> programmed_hz;
    Selection selection = Selection::automatic;
    std::uint16_t selection_value = 0U;
    Verification verification = Verification::commands;
};

struct Stream final {
    std::uint64_t id = 0U;
    std::uint64_t tune_revision = 0U;
    StreamPhase phase = StreamPhase::armed;
};

struct CounterEntry final {
    std::uint8_t id = 0U;
    Counter counter;
};

struct StreamCounters final {
    BArray<CounterEntry, 2> entries;
};

struct ReceiverCounters final {
    BArray<CounterEntry, 4> entries;
};

// A metric entry carries the id and the typed value chosen by that id: lock is a
// boolean measurement, cnr_mdb and signal_mdbm are i32 measurements.
struct MetricEntry final {
    std::uint8_t id = 0U;
    Measurement<bool> boolean;
    Measurement<std::int32_t> integer;
};

struct Measurements final {
    BArray<MetricEntry, 3> entries;
};

struct StreamFinal final {
    std::uint64_t id = 0U;
    std::uint64_t tune_revision = 0U;
    StreamEndReason reason = StreamEndReason::stopped;
    Optional<ErrorRecord> error;
    StreamCounters statistics;
};

struct CardObservation final {
    CardState state = CardState::unknown;
    Optional<std::uint64_t> sampled_at_ns;
    std::uint8_t reason = 0U;
};

struct ReceiverStatus final {
    ReceiverDescriptor descriptor;
    bool present = false;
    ReceiverState state = ReceiverState::free;
    Operation operation = Operation::idle;
    bool leased = false;
    std::uint64_t tune_revision = 0U;
    Optional<Tune> current_tune;
    Optional<Tune> last_successful_tune;
    Optional<Stream> stream;
    Optional<StreamFinal> last_stream;
    Measurements measurements;
    ReceiverCounters receiver_statistics;
    Optional<StreamCounters> stream_statistics;
    Optional<ErrorRecord> last_error;
};

struct ReaderStatus final {
    ReaderDescriptor descriptor;
    bool present = false;
    CardObservation card;
    CardOperation operation = CardOperation::idle;
    std::uint16_t handles_open = 0U;
    bool transaction_active = false;
    Optional<ErrorRecord> last_error;
};

// Payload records.
struct HelloResponse final {
    std::uint16_t major = 0U;
    std::uint16_t minor = 0U;
    std::uint64_t features = 0U;
    Uuid epoch{};
    Producer producer;
    std::uint32_t max_control_payload = 0U;
    std::uint32_t max_ts_bytes = 0U;
};

struct ErrorResponse final {
    Uuid epoch{};
    ErrorRecord error;
    Scope target;
    Optional<std::uint32_t> retry_after_ms;
    Optional<std::uint32_t> required_payload_bytes;
};

struct ListRequest final {
    Uuid epoch{};
};

struct ListResponse final {
    Uuid epoch{};
    Producer producer;
    Snapshot snapshot;
    Topology topology;
    Optional<Enclosure> enclosure;
    BArray<UsbFunction, kMaxFunctions> functions;
    Optional<BArray<ReceiverDescriptor, kMaxPublicReceivers>> receivers;
    Optional<BArray<ReaderDescriptor, kMaxReaders>> readers;
};

struct StatusRequest final {
    Uuid epoch{};
    Scope scope;
};

struct StatusResponse final {
    Producer producer;
    Context context;
    Snapshot snapshot;
    Scope scope;
    Optional<Enclosure> enclosure;
    Topology topology;
    Optional<BArray<UsbFunction, kMaxFunctions>> functions;
    Optional<BArray<ReceiverStatus, kMaxPublicReceivers>> receivers;
    Optional<BArray<ReaderStatus, kMaxReaders>> readers;
};

struct TuneRequest final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
    System system = System::isdb_t;
    std::uint64_t frequency_hz = 0U;
    Selection selection = Selection::automatic;
    std::uint16_t selection_value = 0U;
    Optional<std::uint8_t> lnb_voltage;
    Optional<bool> lna;
    Optional<BArray<std::uint16_t, kMaxPids>> pids;
    std::uint32_t timeout_ms = 0U;
};

struct TuneResult final {
    Uuid epoch{};
    std::uint64_t tune_revision = 0U;
    std::uint64_t requested_hz = 0U;
    Optional<std::uint64_t> programmed_hz;
    Verification verification = Verification::commands;
    Measurement<bool> lock;
    Optional<std::uint16_t> selected_tsid;
    Optional<std::uint16_t> selected_slot;
};

struct StartRequest final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
    std::uint64_t tune_revision = 0U;
};

struct StartResponse final {
    Uuid epoch{};
    std::uint64_t stream_id = 0U;
    Uuid attach_token{};
    std::uint32_t expires_after_ms = 0U;
};

struct AttachRequest final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
    std::uint64_t stream_id = 0U;
    Uuid attach_token{};
};

struct AttachResponse final {
    Uuid epoch{};
    std::uint64_t stream_id = 0U;
};

struct StopRequest final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
    std::uint64_t stream_id = 0U;
};

struct StopResponse final {
    Uuid epoch{};
    StreamFinal final;
    bool quiesced = false;
};

struct ReleaseRequest final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
};

struct ReleaseResponse final {
    Uuid epoch{};
    std::uint64_t lease_id = 0U;
    bool released = false;
    bool quiesced = false;
    Optional<ErrorRecord> error;
};

// TS_DATA aliases the input packet bytes instead of storing a 1 MiB array.
struct TsData final {
    std::uint64_t stream_id = 0U;
    std::uint64_t sequence = 0U;
    std::uint64_t cumulative_queue_dropped_packets = 0U;
    ByteView packets{nullptr, 0U};
};

struct StateEvent final {
    Uuid epoch{};
    std::uint64_t snapshot_sequence = 0U;
    Scope target;
    StateEventReason reason = StateEventReason::context_changed;
};

struct StreamEndEvent final {
    StreamFinal final;
};

// Card records. ReaderKey binds to a reader, CardHandleKey adds the saved
// generation so a stale handle is not silently moved to a new generation.
struct ReaderKey final {
    Uuid epoch{};
    std::uint16_t reader_id = 0U;
};

struct CardHandleKey final {
    Uuid epoch{};
    std::uint16_t reader_id = 0U;
    std::uint64_t bound_generation = 0U;
    std::uint64_t card_handle = 0U;
};

struct CardSessionResult final {
    Uuid epoch{};
    std::uint16_t reader_id = 0U;
    std::uint64_t card_handle = 0U;
    std::uint64_t bound_generation = 0U;
    std::uint64_t current_generation = 0U;
    bool present = false;
    bool initialized = false;
    bool usable = false;
    Optional<CardProtocol> protocol;
    Optional<BBytes<kMaxAtrBytes>> atr;
};

struct CardStatusRequest final {
    ReaderKey key;
};

struct CardStatusResponse final {
    ReaderKey key;
    std::uint64_t current_generation = 0U;
    bool present = false;
    bool initialized = false;
    Optional<CardProtocol> protocol;
    Optional<BBytes<kMaxAtrBytes>> atr;
};

struct CardConnectRequest final {
    ReaderKey key;
    Share share = Share::shared;
    std::uint8_t preferred_protocols = 0U;
};

struct CardReconnectRequest final {
    CardHandleKey key;
    Share share = Share::shared;
    Disposition disposition = Disposition::leave;
    std::uint8_t preferred_protocols = 0U;
};

struct CardDisconnectRequest final {
    CardHandleKey key;
    Disposition disposition = Disposition::leave;
};

struct CardDisconnectResponse final {
    ReaderKey key;
    std::uint64_t card_handle = 0U;
    bool consumed = false;
    bool quiesced = false;
    Optional<ErrorRecord> error;
};

struct CardResetRequest final {
    CardHandleKey key;
};

struct CardTransmitRequest final {
    CardHandleKey key;
    CardProtocol protocol = CardProtocol::t1;
    BBytes<kMaxApduBytes> command;
};

struct CardTransmitResponse final {
    CardHandleKey key;
    BBytes<kMaxApduBytes> response;
};

struct BeginTransactionRequest final {
    CardHandleKey key;
};

struct BeginTransactionResponse final {
    CardHandleKey key;
};

struct EndTransactionRequest final {
    CardHandleKey key;
    Disposition disposition = Disposition::leave;
};

struct CardHandleStatusRequest final {
    CardHandleKey key;
};

// Codec pair per record. Decoders require the exact payload size, reject
// trailing bytes, and map a structural problem to PROTOCOL_ERROR and an
// out-of-range value to INVALID_ARGUMENT. Encoders return INVALID_ARGUMENT for
// an invalid value and BUFFER_TOO_SMALL for a short output.
#define DTV_USERLAND_RECORD_API(NAME, FUNC)                                        \
    Result<std::size_t> encode_##FUNC(const NAME& value, MutableByteView output) noexcept; \
    Result<NAME> decode_##FUNC(ByteView input) noexcept;

DTV_USERLAND_RECORD_API(HelloResponse, hello_response)
DTV_USERLAND_RECORD_API(ErrorResponse, error_response)
DTV_USERLAND_RECORD_API(ListRequest, list_request)
DTV_USERLAND_RECORD_API(ListResponse, list_response)
DTV_USERLAND_RECORD_API(StatusRequest, status_request)
DTV_USERLAND_RECORD_API(StatusResponse, status_response)
DTV_USERLAND_RECORD_API(TuneRequest, tune_request)
DTV_USERLAND_RECORD_API(TuneResult, tune_result)
DTV_USERLAND_RECORD_API(StartRequest, start_request)
DTV_USERLAND_RECORD_API(StartResponse, start_response)
DTV_USERLAND_RECORD_API(AttachRequest, attach_request)
DTV_USERLAND_RECORD_API(AttachResponse, attach_response)
DTV_USERLAND_RECORD_API(StopRequest, stop_request)
DTV_USERLAND_RECORD_API(StopResponse, stop_response)
DTV_USERLAND_RECORD_API(ReleaseRequest, release_request)
DTV_USERLAND_RECORD_API(ReleaseResponse, release_response)
DTV_USERLAND_RECORD_API(TsData, ts_data)
DTV_USERLAND_RECORD_API(StateEvent, state_event)
DTV_USERLAND_RECORD_API(StreamEndEvent, stream_end_event)
DTV_USERLAND_RECORD_API(CardSessionResult, card_session_result)
DTV_USERLAND_RECORD_API(CardStatusRequest, card_status_request)
DTV_USERLAND_RECORD_API(CardStatusResponse, card_status_response)
DTV_USERLAND_RECORD_API(CardConnectRequest, card_connect_request)
DTV_USERLAND_RECORD_API(CardReconnectRequest, card_reconnect_request)
DTV_USERLAND_RECORD_API(CardDisconnectRequest, card_disconnect_request)
DTV_USERLAND_RECORD_API(CardDisconnectResponse, card_disconnect_response)
DTV_USERLAND_RECORD_API(CardResetRequest, card_reset_request)
DTV_USERLAND_RECORD_API(CardTransmitRequest, card_transmit_request)
DTV_USERLAND_RECORD_API(CardTransmitResponse, card_transmit_response)
DTV_USERLAND_RECORD_API(BeginTransactionRequest, begin_transaction_request)
DTV_USERLAND_RECORD_API(BeginTransactionResponse, begin_transaction_response)
DTV_USERLAND_RECORD_API(EndTransactionRequest, end_transaction_request)
DTV_USERLAND_RECORD_API(CardHandleStatusRequest, card_handle_status_request)

// Sub-records are typed wire records too and are exposed so a consumer can
// validate or re-encode one snapshot piece in isolation.
DTV_USERLAND_RECORD_API(Producer, producer)
DTV_USERLAND_RECORD_API(Model, model)
DTV_USERLAND_RECORD_API(ErrorRecord, error_record)
DTV_USERLAND_RECORD_API(Identity, identity)
DTV_USERLAND_RECORD_API(Location, location)
DTV_USERLAND_RECORD_API(Binding, binding)
DTV_USERLAND_RECORD_API(Topology, topology)
DTV_USERLAND_RECORD_API(Limits, limits)
DTV_USERLAND_RECORD_API(LnaDefaults, lna_defaults)
DTV_USERLAND_RECORD_API(LnbValues, lnb_values)
DTV_USERLAND_RECORD_API(Capabilities, capabilities)
DTV_USERLAND_RECORD_API(ReceiverDescriptor, receiver_descriptor)
DTV_USERLAND_RECORD_API(ReaderDescriptor, reader_descriptor)
DTV_USERLAND_RECORD_API(Enclosure, enclosure)
DTV_USERLAND_RECORD_API(UsbFunction, usb_function)
DTV_USERLAND_RECORD_API(Scope, scope)
DTV_USERLAND_RECORD_API(Snapshot, snapshot)
DTV_USERLAND_RECORD_API(Context, context)
DTV_USERLAND_RECORD_API(Tune, tune)
DTV_USERLAND_RECORD_API(Stream, stream)
DTV_USERLAND_RECORD_API(StreamCounters, stream_counters)
DTV_USERLAND_RECORD_API(ReceiverCounters, receiver_counters)
DTV_USERLAND_RECORD_API(Measurements, measurements)
DTV_USERLAND_RECORD_API(StreamFinal, stream_final)
DTV_USERLAND_RECORD_API(CardObservation, card_observation)
DTV_USERLAND_RECORD_API(ReceiverStatus, receiver_status)
DTV_USERLAND_RECORD_API(ReaderStatus, reader_status)
DTV_USERLAND_RECORD_API(ReaderKey, reader_key)
DTV_USERLAND_RECORD_API(CardHandleKey, card_handle_key)

#undef DTV_USERLAND_RECORD_API

// Cross-record and context validation. These run after a syntactic decode and
// before a snapshot is published or an operation is dispatched; the byte codecs
// alone cannot check a reference to a sibling record or a declared capability.
// Every function returns Error::OK on success.
Error validate_error_record(const ErrorRecord& value) noexcept;
Error validate_scope(const Scope& value) noexcept;
Error validate_capabilities(const Capabilities& value) noexcept;
Error validate_receiver_descriptor(const ReceiverDescriptor& value) noexcept;
Error validate_reader_descriptor(const ReaderDescriptor& value) noexcept;
Error validate_topology(const Topology& value) noexcept;
// max_concurrent_streams must be 1..32 and not exceed the public receiver count.
Error validate_limits(const Limits& value, std::size_t public_receiver_count) noexcept;
// The metric ID set must equal the declared capabilities measurement_mask; every
// declared metric must be present and unknown or valid, never unsupported.
Error validate_measurements(const Measurements& value, std::uint8_t measurement_mask) noexcept;
// StreamCounters must contain exactly ID5 transmitted_packets and ID6
// queue_dropped_packets, ascending, each unknown or valid.
Error validate_stream_counters(const StreamCounters& value) noexcept;
// ReceiverCounters is a subset of ID1..4, ascending, each unknown or valid.
Error validate_receiver_counters(const ReceiverCounters& value) noexcept;
Error validate_stream_final(const StreamFinal& value) noexcept;
Error validate_receiver_status(const ReceiverStatus& value) noexcept;
Error validate_reader_status(const ReaderStatus& value) noexcept;
Error validate_card_observation(const CardObservation& value) noexcept;
Error validate_card_session_result(const CardSessionResult& value) noexcept;
Error validate_card_status_response(const CardStatusResponse& value) noexcept;
// function_id must be one of the functions in the same snapshot.
Error validate_binding(const Binding& value, const std::uint16_t* function_ids,
                       std::size_t function_count) noexcept;
Error validate_list_response(const ListResponse& value) noexcept;
// Shape-only scope pre-check: which collections a scope must carry. It does not
// prove references, limits or sample times; use validate_status_response for the
// full aggregate check.
Error validate_status_response_scope(const StatusResponse& value) noexcept;

// Canonical same-epoch context for a STATUS response, supplied as a validated
// LIST snapshot. It carries the epoch the scope belongs to and the complete
// published function/receiver/reader descriptors, so membership and descriptor
// identity/capability/binding semantics can be checked instead of trusting the
// partial record's own claims. A null pointer means "no canonical context".
struct StatusContext final {
    const ListResponse* list = nullptr;
};

// Full aggregate STATUS validation: local syntax/value checks, scope shape,
// duplicate IDs, membership and descriptor identity against the canonical
// context, binding references, `Limits` versus the public receiver count and
// sample times versus `snapshot.observed_at_ns`. The context epoch must match
// `context.epoch`, and a retained (last) stream target requires the receiver
// still leased. A missing context or a missing membership proof returns
// INVALID_ARGUMENT rather than pretending the partial record proves the full
// context. Callers must run this before publishing a snapshot or dispatching an
// operation; the global and partial paths are documented in docs/wire-records.md.
Error validate_status_response(const StatusResponse& value,
                               const StatusContext& context) noexcept;

}  // namespace dtv::userland::wire

#endif  // DTV_USERLAND_RECORDS_H
