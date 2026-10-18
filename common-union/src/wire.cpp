// SPDX-License-Identifier: MIT
#include <dtv/userland/wire.h>

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

void store_u16(std::uint8_t* p, std::uint16_t value) noexcept
{
    p[0] = static_cast<std::uint8_t>(value & 0xffU);
    p[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
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

void store_epoch(std::uint8_t* p, const Epoch& epoch) noexcept
{
    for (std::size_t i = 0U; i < kEpochSize; ++i) {
        p[i] = epoch[i];
    }
}

void load_epoch(const std::uint8_t* p, Epoch& epoch) noexcept
{
    for (std::size_t i = 0U; i < kEpochSize; ++i) {
        epoch[i] = p[i];
    }
}

bool magic_equals(const std::uint8_t* p, const std::array<std::uint8_t, 4>& magic) noexcept
{
    return p[0] == magic[0] && p[1] == magic[1] && p[2] == magic[2] && p[3] == magic[3];
}

std::uint16_t flags_for(Kind kind) noexcept
{
    switch (kind) {
    case Kind::response:
        return kFlagResponse;
    case Kind::error_response:
        return static_cast<std::uint16_t>(kFlagResponse | kFlagError);
    case Kind::request:
    case Kind::event:
        break;
    }
    return 0U;
}

// Shared by decode and encode so both sides enforce one rule set. The event
// rule is generic: opcode bit 15 set exactly for events, for every opcode.
HeaderStatus check_fields(const Header& header) noexcept
{
    if (header.kind != Kind::request && header.kind != Kind::response &&
        header.kind != Kind::error_response && header.kind != Kind::event) {
        return HeaderStatus::bad_flags;
    }
    const bool event = header.kind == Kind::event;
    if (event != is_event_opcode(header.opcode)) {
        return HeaderStatus::bad_kind_for_opcode;
    }
    if (event != (header.request_id == 0U)) {
        return HeaderStatus::bad_request_id;
    }
    if (header.payload_length > payload_limit(header.kind, header.opcode)) {
        return HeaderStatus::payload_too_large;
    }
    return HeaderStatus::ok;
}

bool version_less(Version a, Version b) noexcept
{
    return a.major < b.major || (a.major == b.major && a.minor < b.minor);
}

}  // namespace

Error header_error(HeaderStatus status) noexcept
{
    switch (status) {
    case HeaderStatus::ok:
        return Error::OK;
    case HeaderStatus::invalid_view:
    case HeaderStatus::wrong_size:
        return Error::INVALID_ARGUMENT;
    case HeaderStatus::unsupported_version:
        return Error::VERSION_MISMATCH;
    case HeaderStatus::legacy_magic:
    case HeaderStatus::bad_magic:
    case HeaderStatus::bad_flags:
    case HeaderStatus::bad_request_id:
    case HeaderStatus::bad_kind_for_opcode:
    case HeaderStatus::payload_too_large:
        return Error::PROTOCOL_ERROR;
    }
    return Error::INTERNAL;
}

std::size_t payload_limit(Kind kind, std::uint16_t opcode) noexcept
{
    if (kind == Kind::event && opcode == kOpcodeTsData) {
        return kMaxTsDataPayload;
    }
    return kMaxControlPayload;
}

HeaderStatus check_header(ByteView input, Header* header) noexcept
{
    if (!valid_view(input) || header == nullptr) {
        return HeaderStatus::invalid_view;
    }
    if (input.size != kHeaderSize) {
        return HeaderStatus::wrong_size;
    }
    const std::uint8_t* p = input.data;
    if (magic_equals(p, kLegacyPx4Magic) || magic_equals(p, kLegacyAsicenMagic)) {
        return HeaderStatus::legacy_magic;
    }
    if (!magic_equals(p, kMagic)) {
        return HeaderStatus::bad_magic;
    }
    if (load_u16(p + 4) != kVersionMajor || load_u16(p + 6) != kVersionMinor) {
        return HeaderStatus::unsupported_version;
    }

    Header decoded;
    decoded.opcode = load_u16(p + 8);
    const std::uint16_t flags = load_u16(p + 10);
    decoded.request_id = load_u32(p + 12);
    decoded.payload_length = load_u32(p + 16);

    // flags 0 is an event or a request depending only on opcode bit 15;
    // check_fields then requires request_id to match that kind.
    if (flags == 0U) {
        decoded.kind = is_event_opcode(decoded.opcode) ? Kind::event : Kind::request;
    } else if (flags == kFlagResponse) {
        decoded.kind = Kind::response;
    } else if (flags == static_cast<std::uint16_t>(kFlagResponse | kFlagError)) {
        decoded.kind = Kind::error_response;
    } else {
        return HeaderStatus::bad_flags;
    }

    const HeaderStatus status = check_fields(decoded);
    if (status != HeaderStatus::ok) {
        return status;
    }
    *header = decoded;
    return HeaderStatus::ok;
}

Result<Header> decode_header(ByteView input) noexcept
{
    Header header;
    const HeaderStatus status = check_header(input, &header);
    if (status != HeaderStatus::ok) {
        return Result<Header>::failure(header_error(status));
    }
    return Result<Header>::success(header);
}

Result<std::size_t> encode_header(const Header& header, MutableByteView output) noexcept
{
    if (!valid_view(output)) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (check_fields(header) != HeaderStatus::ok) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (output.size < kHeaderSize) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    std::uint8_t* p = output.data;
    for (std::size_t i = 0U; i < kMagic.size(); ++i) {
        p[i] = kMagic[i];
    }
    store_u16(p + 4, kVersionMajor);
    store_u16(p + 6, kVersionMinor);
    store_u16(p + 8, header.opcode);
    store_u16(p + 10, flags_for(header.kind));
    store_u32(p + 12, header.request_id);
    store_u32(p + 16, header.payload_length);
    return Result<std::size_t>::success(kHeaderSize);
}

Result<Frame> decode_frame(ByteView input) noexcept
{
    if (!valid_view(input)) {
        return Result<Frame>::failure(Error::INVALID_ARGUMENT);
    }
    if (input.size < kHeaderSize) {
        return Result<Frame>::failure(Error::PROTOCOL_ERROR);
    }
    const auto header = decode_header(ByteView{input.data, kHeaderSize});
    if (!header) {
        return Result<Frame>::failure(header.error());
    }
    // payload_length is already bounded by the cap, so this cannot overflow.
    if (input.size - kHeaderSize != header.value().payload_length) {
        return Result<Frame>::failure(Error::PROTOCOL_ERROR);
    }
    Frame frame;
    frame.header = header.value();
    frame.payload = ByteView{input.data + kHeaderSize, header.value().payload_length};
    return Result<Frame>::success(frame);
}

Result<std::size_t> encode_frame(const Header& header, ByteView payload,
                                 MutableByteView output) noexcept
{
    if (!valid_view(payload) || !valid_view(output) ||
        payload.size != header.payload_length || check_fields(header) != HeaderStatus::ok) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    // Nothing is written unless the whole frame fits.
    if (output.size < kHeaderSize || output.size - kHeaderSize < payload.size) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    const auto written = encode_header(header, output);
    if (!written) {
        return written;
    }
    for (std::size_t i = 0U; i < payload.size; ++i) {
        output.data[kHeaderSize + i] = payload.data[i];
    }
    return Result<std::size_t>::success(kHeaderSize + payload.size);
}

FrameReceiver::FrameReceiver(MutableByteView payload_storage) noexcept
    : storage_(payload_storage)
{
    if (!valid_view(payload_storage)) {
        failure_ = Error::INVALID_ARGUMENT;
    }
}

Result<std::size_t> FrameReceiver::fail(Error error) noexcept
{
    failure_ = error;
    return Result<std::size_t>::failure(error);
}

void FrameReceiver::reset() noexcept
{
    header_filled_ = 0U;
    have_header_ = false;
    header_ = Header{};
    payload_filled_ = 0U;
    header_status_ = HeaderStatus::ok;
    truncated_ = false;
    failure_ = valid_view(storage_) ? Error::OK : Error::INVALID_ARGUMENT;
}

std::size_t FrameReceiver::pending_size() const noexcept
{
    return have_header_ ? kHeaderSize + payload_filled_ : header_filled_;
}

Result<void> FrameReceiver::finish() noexcept
{
    if (failed()) {
        return Result<void>::failure(failure_);
    }
    if (pending_size() != 0U) {
        truncated_ = true;
        failure_ = Error::PROTOCOL_ERROR;
        return Result<void>::failure(failure_);
    }
    return Result<void>::success();
}

Result<std::size_t> FrameReceiver::feed(ByteView input, FrameHandler& handler) noexcept
{
    if (failed()) {
        return Result<std::size_t>::failure(failure_);
    }
    if (!valid_view(input)) {
        return fail(Error::INVALID_ARGUMENT);
    }
    std::size_t offset = 0U;
    std::size_t delivered = 0U;
    for (;;) {
        if (!have_header_) {
            while (header_filled_ < kHeaderSize && offset < input.size) {
                header_bytes_[header_filled_++] = input.data[offset++];
            }
            if (header_filled_ < kHeaderSize) {
                break;
            }
            header_status_ =
                check_header(ByteView{header_bytes_.data(), kHeaderSize}, &header_);
            if (header_status_ != HeaderStatus::ok) {
                return fail(header_error(header_status_));
            }
            if (header_.payload_length > storage_.size) {
                return fail(Error::BUFFER_TOO_SMALL);
            }
            have_header_ = true;
            payload_filled_ = 0U;
        }

        const std::size_t wanted = header_.payload_length - payload_filled_;
        const std::size_t available = input.size - offset;
        const std::size_t take = available < wanted ? available : wanted;
        for (std::size_t i = 0U; i < take; ++i) {
            storage_.data[payload_filled_ + i] = input.data[offset + i];
        }
        payload_filled_ += take;
        offset += take;
        if (payload_filled_ < header_.payload_length) {
            break;
        }

        Frame frame;
        frame.header = header_;
        frame.payload = ByteView{storage_.data, header_.payload_length};
        have_header_ = false;
        header_filled_ = 0U;
        payload_filled_ = 0U;
        const auto handled = handler.on_frame(frame);
        if (!handled) {
            return fail(handled.error());
        }
        ++delivered;
        if (offset == input.size) {
            break;
        }
    }
    return Result<std::size_t>::success(delivered);
}

Result<std::size_t> encode_hello_request(const HelloRequest& value,
                                         MutableByteView output) noexcept
{
    if (!valid_view(output) || version_less(value.max, value.min)) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (output.size < kHelloRequestSize) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    std::uint8_t* p = output.data;
    store_u16(p, value.min.major);
    store_u16(p + 2, value.min.minor);
    store_u16(p + 4, value.max.major);
    store_u16(p + 6, value.max.minor);
    store_u64(p + 8, value.required_features);
    store_u64(p + 16, value.optional_features);
    return Result<std::size_t>::success(kHelloRequestSize);
}

Result<HelloRequest> decode_hello_request(ByteView input) noexcept
{
    if (!valid_view(input)) {
        return Result<HelloRequest>::failure(Error::INVALID_ARGUMENT);
    }
    if (input.size != kHelloRequestSize) {
        return Result<HelloRequest>::failure(Error::PROTOCOL_ERROR);
    }
    const std::uint8_t* p = input.data;
    HelloRequest value;
    value.min.major = load_u16(p);
    value.min.minor = load_u16(p + 2);
    value.max.major = load_u16(p + 4);
    value.max.minor = load_u16(p + 6);
    value.required_features = load_u64(p + 8);
    value.optional_features = load_u64(p + 16);
    if (version_less(value.max, value.min)) {
        return Result<HelloRequest>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<HelloRequest>::success(value);
}

Result<Version> negotiate_version(const HelloRequest& request) noexcept
{
    if (version_less(request.max, request.min)) {
        return Result<Version>::failure(Error::INVALID_ARGUMENT);
    }
    const Version current{kVersionMajor, kVersionMinor};
    if (version_less(current, request.min) || version_less(request.max, current)) {
        return Result<Version>::failure(Error::VERSION_MISMATCH);
    }
    return Result<Version>::success(current);
}

Result<std::size_t> encode_acquire_request(const AcquireRequest& value,
                                           MutableByteView output) noexcept
{
    if (!valid_view(output)) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (output.size < kAcquireRequestSize) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    store_epoch(output.data, value.epoch);
    store_u16(output.data + kEpochSize, value.receiver_id);
    return Result<std::size_t>::success(kAcquireRequestSize);
}

Result<AcquireRequest> decode_acquire_request(ByteView input) noexcept
{
    if (!valid_view(input)) {
        return Result<AcquireRequest>::failure(Error::INVALID_ARGUMENT);
    }
    if (input.size != kAcquireRequestSize) {
        return Result<AcquireRequest>::failure(Error::PROTOCOL_ERROR);
    }
    AcquireRequest value;
    load_epoch(input.data, value.epoch);
    value.receiver_id = load_u16(input.data + kEpochSize);
    return Result<AcquireRequest>::success(value);
}

Result<std::size_t> encode_acquire_response(const AcquireResponse& value,
                                            MutableByteView output) noexcept
{
    if (!valid_view(output) || value.lease_id == 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    if (output.size < kAcquireResponseSize) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    store_epoch(output.data, value.epoch);
    store_u64(output.data + kEpochSize, value.lease_id);
    return Result<std::size_t>::success(kAcquireResponseSize);
}

Result<AcquireResponse> decode_acquire_response(ByteView input) noexcept
{
    if (!valid_view(input)) {
        return Result<AcquireResponse>::failure(Error::INVALID_ARGUMENT);
    }
    if (input.size != kAcquireResponseSize) {
        return Result<AcquireResponse>::failure(Error::PROTOCOL_ERROR);
    }
    AcquireResponse value;
    load_epoch(input.data, value.epoch);
    value.lease_id = load_u64(input.data + kEpochSize);
    if (value.lease_id == 0U) {
        return Result<AcquireResponse>::failure(Error::INVALID_ARGUMENT);
    }
    return Result<AcquireResponse>::success(value);
}

}  // namespace dtv::userland::wire
