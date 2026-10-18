// SPDX-License-Identifier: MIT
#include <dtv/userland/tuning.h>

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>

namespace {

namespace wire = dtv::userland::wire;
namespace tuning = dtv::tuning;
using dtv::userland::Error;

int failures = 0;
int checks = 0;

#define CHECK(expr)                                                               \
    do {                                                                          \
        ++checks;                                                                 \
        if (!(expr)) {                                                            \
            ++failures;                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #expr);                                                  \
        }                                                                         \
    } while (false)

tuning::Request channel_request(std::string_view channel)
{
    tuning::Request request;
    request.channel = channel;
    return request;
}

tuning::Request frequency_request(std::uint64_t hz, wire::System system)
{
    tuning::Request request;
    request.frequency_hz = hz;
    request.system = system;
    return request;
}

void check_channel(std::string_view channel, wire::System system, std::uint64_t hz,
                   wire::Selection selection, std::uint16_t value)
{
    const auto result = tuning::resolve(channel_request(channel));
    CHECK(result.has_value());
    if (result) {
        CHECK(result.value().system == system);
        CHECK(result.value().frequency_hz == hz);
        CHECK(result.value().selection == selection);
        CHECK(result.value().selection_value == value);
    }
}

void check_invalid(const tuning::Request& request)
{
    const auto result = tuning::resolve(request);
    CHECK(!result.has_value());
    CHECK(result.error() == Error::INVALID_ARGUMENT);
}

void check_cs_selection(std::string_view channel, wire::Selection selection,
                        std::uint16_t value, std::optional<std::uint16_t> slot,
                        std::optional<std::uint16_t> tsid)
{
    auto request = channel_request(channel);
    request.slot = slot;
    request.tsid = tsid;
    const auto result = tuning::resolve(request);
    CHECK(result.has_value());
    if (result) {
        CHECK(result.value().system == wire::System::isdb_s);
        CHECK(result.value().frequency_hz == 1613000000ULL);
        CHECK(result.value().selection == selection);
        CHECK(result.value().selection_value == value);
    }
}

void test_literal_vectors()
{
    check_channel("T13", wire::System::isdb_t, 473142857ULL,
                  wire::Selection::automatic, 0U);
    check_channel("T27", wire::System::isdb_t, 557142857ULL,
                  wire::Selection::automatic, 0U);
    check_channel("T62", wire::System::isdb_t, 767142857ULL,
                  wire::Selection::automatic, 0U);
    for (const auto& vector : {std::pair{"BS01", 1049480000ULL},
                               std::pair{"BS15", 1318000000ULL},
                               std::pair{"BS23", 1471440000ULL}}) {
        auto request = channel_request(vector.first);
        request.slot = 3U;
        const auto result = tuning::resolve(request);
        CHECK(result.has_value());
        if (result) {
            CHECK(result.value().frequency_hz == vector.second);
            CHECK(result.value().selection == wire::Selection::slot);
            CHECK(result.value().selection_value == 3U);
        }
    }
    check_channel("CS2", wire::System::isdb_s, 1613000000ULL,
                  wire::Selection::slot, 0U);
    check_channel("CS02", wire::System::isdb_s, 1613000000ULL,
                  wire::Selection::slot, 0U);
    check_channel("CS04", wire::System::isdb_s, 1653000000ULL,
                  wire::Selection::slot, 0U);
    check_channel("CS06", wire::System::isdb_s, 1693000000ULL,
                  wire::Selection::slot, 0U);
    check_channel("CS08", wire::System::isdb_s, 1733000000ULL,
                  wire::Selection::slot, 0U);
    check_channel("CS24", wire::System::isdb_s, 2053000000ULL,
                  wire::Selection::slot, 0U);

    for (const auto channel : {"BS01", "BS15", "BS23"}) {
        auto request = channel_request(channel);
        request.slot = 3U;
        const auto result = tuning::resolve(request);
        CHECK(result.has_value());
        if (result) {
            CHECK(result.value().selection == wire::Selection::slot);
            CHECK(result.value().selection_value == 3U);
        }
    }
}

void test_domain()
{
    for (unsigned number = 13U; number <= 62U; ++number) {
        char digits[] = {static_cast<char>('0' + number / 10U),
                         static_cast<char>('0' + number % 10U), '\0'};
        const auto expected = 473142857ULL + (number - 13U) * 6000000ULL;
        char t_digits[] = {'T', digits[0], digits[1], '\0'};
        for (const auto channel : {std::string_view(digits), std::string_view(t_digits)}) {
            const auto result = tuning::resolve(channel_request(channel));
            CHECK(result.has_value());
            if (result) {
                CHECK(result.value().system == wire::System::isdb_t);
                CHECK(result.value().frequency_hz == expected);
                CHECK(result.value().selection == wire::Selection::automatic);
            }
        }
    }
    for (unsigned number = 1U; number <= 23U; number += 2U) {
        char channel[5] = {'B', 'S', static_cast<char>('0' + number / 10U),
                           static_cast<char>('0' + number % 10U), '\0'};
        auto request = channel_request(channel);
        request.tsid = 1U;
        const auto result = tuning::resolve(request);
        CHECK(result.has_value());
        if (result) {
            CHECK(result.value().system == wire::System::isdb_s);
            CHECK(result.value().frequency_hz ==
                  1049480000ULL + ((number - 1U) / 2U) * 38360000ULL);
            CHECK(result.value().selection == wire::Selection::tsid);
            CHECK(result.value().selection_value == 1U);
        }
    }
    for (unsigned slot = 0U; slot <= 11U; ++slot) {
        char channel[8] = {'B', 'S', '0', '1', '_',
                           static_cast<char>(slot < 10U ? '0' + slot : '1'),
                           static_cast<char>(slot < 10U ? '\0' : '0' + slot - 10U), '\0'};
        const auto text = std::string_view(channel, slot < 10U ? 6U : 7U);
        const auto result = tuning::resolve(channel_request(text));
        CHECK(result.has_value());
        if (result) {
            CHECK(result.value().selection == wire::Selection::slot);
            CHECK(result.value().selection_value == slot);
        }
    }
    for (unsigned number = 2U; number <= 24U; number += 2U) {
        char channel[5] = {'C', 'S', static_cast<char>('0' + number / 10U),
                           static_cast<char>('0' + number % 10U), '\0'};
        const auto result = tuning::resolve(channel_request(channel));
        CHECK(result.has_value());
        if (result) {
            CHECK(result.value().system == wire::System::isdb_s);
            CHECK(result.value().frequency_hz ==
                  1613000000ULL + ((number - 2U) / 2U) * 40000000ULL);
            CHECK(result.value().selection == wire::Selection::slot);
            CHECK(result.value().selection_value == 0U);
        }
    }
}

void test_selection_table()
{
    for (const auto channel : {"13", "T62"}) {
        auto request = channel_request(channel);
        request.slot = 0U;
        check_invalid(request);
        request.slot.reset();
        request.tsid = 1U;
        check_invalid(request);
    }
    auto bs = channel_request("BS01");
    check_invalid(bs);
    bs.slot = 0U;
    CHECK(tuning::resolve(bs).has_value());
    bs.slot.reset();
    bs.tsid = 65534U;
    CHECK(tuning::resolve(bs).has_value());
    bs.slot = 0U;
    check_invalid(bs);

    auto suffixed = channel_request("BS01_0");
    CHECK(tuning::resolve(suffixed).has_value());
    suffixed.slot = 0U;
    check_invalid(suffixed);
    suffixed.slot.reset();
    suffixed.tsid = 1U;
    check_invalid(suffixed);

    for (const auto channel : {"CS2", "CS02"}) {
        check_cs_selection(channel, wire::Selection::slot, 11U, 11U, std::nullopt);
        check_cs_selection(channel, wire::Selection::tsid, 1U, std::nullopt, 1U);
        check_cs_selection(channel, wire::Selection::tsid, 65534U, std::nullopt, 65534U);
        auto conflict = channel_request(channel);
        conflict.slot = 11U;
        conflict.tsid = 1U;
        check_invalid(conflict);
    }
    auto cs = channel_request("CS10");
    cs.slot = 12U;
    check_invalid(cs);

    auto satellite_frequency = frequency_request(1U, wire::System::isdb_s);
    check_invalid(satellite_frequency);
    satellite_frequency.slot = 11U;
    auto result = tuning::resolve(satellite_frequency);
    CHECK(result.has_value());
    if (result) {
        CHECK(result.value().selection == wire::Selection::slot);
        CHECK(result.value().selection_value == 11U);
    }
    satellite_frequency.slot.reset();
    satellite_frequency.tsid = 65534U;
    result = tuning::resolve(satellite_frequency);
    CHECK(result.has_value());
    if (result) {
        CHECK(result.value().selection == wire::Selection::tsid);
        CHECK(result.value().selection_value == 65534U);
    }
}

void test_invalid_inputs()
{
    check_invalid(tuning::Request{});
    auto both = channel_request("T13");
    both.frequency_hz = 473142857ULL;
    check_invalid(both);
    auto empty = channel_request("");
    check_invalid(empty);

    for (const auto channel : {"12", "63", "T12", "t13", "13 ", "+13", "013",
                               "BS00", "BS02", "BS24", "BS01_", "BS01_00",
                               "BS01_01", "BS01_12", "BS01_0x", "BS01_0 ",
                               "BS01_000", "CS", "CS02 ", "CS002", "CS03",
                               "CS26", "cs2", "T１３", "BS01_11x"}) {
        check_invalid(channel_request(channel));
    }
    check_invalid(channel_request("BS23_11x"));
    check_invalid(channel_request(std::string_view("BS23_11\0", 8U)));

    auto mismatch = channel_request("T13");
    mismatch.system = wire::System::isdb_s;
    check_invalid(mismatch);
    mismatch.system = static_cast<wire::System>(0U);
    check_invalid(mismatch);
    mismatch = frequency_request(10U, static_cast<wire::System>(3U));
    check_invalid(mismatch);
    mismatch.system.reset();
    check_invalid(mismatch);

    check_invalid(frequency_request(0U, wire::System::isdb_t));
    CHECK(tuning::resolve(frequency_request(9007199254740991ULL,
                                           wire::System::isdb_t)).has_value());
    check_invalid(frequency_request(9007199254740992ULL, wire::System::isdb_t));

    auto ignored = channel_request("T13");
    ignored.system.reset();
    ignored.slot.reset();
    ignored.tsid.reset();
    CHECK(tuning::resolve(ignored).has_value());

    auto bad_slot = channel_request("BS01");
    bad_slot.slot = 12U;
    check_invalid(bad_slot);
    auto bad_tsid = channel_request("BS01");
    bad_tsid.tsid = 0U;
    check_invalid(bad_tsid);
    bad_tsid.tsid = 65535U;
    check_invalid(bad_tsid);
}

}  // namespace

int main()
{
    test_literal_vectors();
    test_domain();
    test_selection_table();
    test_invalid_inputs();
    std::fprintf(stderr, "tuning_tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
