#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>
#ifdef SDK_TEST_THREADED_CRITICAL_SECTIONS
#include <atomic>
#include <thread>
#endif

#include "../audio/fx3_audio_stream.h"
#include "test_support.h"

namespace {
struct TestSource {
    std::vector<uint8_t> bytes;
    uint32_t offset = 0;
    uint16_t opened_asset = 0;
    uint32_t max_read = 4096;
    bool open_ok = true;
    bool read_error = false;
    bool closed = false;
    uint32_t closes = 0;
    uint32_t retries = 0;
    uint32_t reads = 0;

    ~TestSource() {
        fx3_audio_request_reset();
        fx3_audio_task();
    }
};

bool open(void* context, uint16_t asset_id) {
    auto& source = *static_cast<TestSource*>(context);
    source.offset = 0;
    source.opened_asset = asset_id;
    source.closed = false;
    return source.open_ok;
}

int32_t read(void* context, uint8_t* data, uint32_t size) {
    auto& source = *static_cast<TestSource*>(context);
    ++source.reads;
    if (source.retries) {
        --source.retries;
        return -2;
    }
    if (source.read_error)
        return -1;
    if (source.offset == source.bytes.size())
        return 0;
    const uint32_t remaining = static_cast<uint32_t>(source.bytes.size()) - source.offset;
    const uint32_t count = std::min({size, remaining, source.max_read});
    std::copy_n(source.bytes.data() + source.offset, count, data);
    source.offset += count;
    return static_cast<int32_t>(count);
}

void close(void* context) {
    auto& source = *static_cast<TestSource*>(context);
    source.closed = true;
    ++source.closes;
}

Fx3AudioSource hooks(TestSource& source) {
    return {&source, open, read, close};
}

uint16_t reg(uint8_t offset) {
    return static_cast<uint16_t>(fx3_audio::MMIO_BASE + offset);
}

uint32_t counter(uint8_t index) {
    uint32_t value = 0;
    for (uint8_t byte = 0; byte < 4; ++byte) {
        value |= static_cast<uint32_t>(
            fx3_audio_host_read(reg(static_cast<uint8_t>(
                fx3_audio::CounterBase + index * 4u + byte)))) << (byte * 8u);
    }
    return value;
}

void play(uint16_t asset_id) {
    fx3_audio_host_write(reg(fx3_audio::AssetIdLow), static_cast<uint8_t>(asset_id));
    fx3_audio_host_write(reg(fx3_audio::AssetIdHigh), static_cast<uint8_t>(asset_id >> 8));
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Play));
    fx3_audio_task();
    fx3_audio_task();
}

void test_unavailable_source_is_safe() {
    fx3_audio_init({});
    play(1);
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Error),
                 "missing SD source did not enter a recoverable error state");
    test_require(fx3_audio_host_read(reg(fx3_audio::Error)) ==
                     static_cast<uint8_t>(Fx3AudioError::Unavailable),
                 "missing SD source reported the wrong error");
    test_require(fx3_audio_host_read(reg(fx3_audio::HdmaPage)) == fx3_audio::NO_PAGE &&
                     fx3_audio_hdma_read(fx3_audio::HDMA_BASE) == 0,
                 "missing SD source exposed stale audio data");
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Pause));
    fx3_audio_task();
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Resume));
    fx3_audio_task();
    fx3_audio_host_write(reg(fx3_audio::HdmaPage), 0);
    fx3_audio_task();
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Error),
                 "PAUSE/RESUME or a stale page claim restarted a missing source");
    test_require(fx3_audio_hdma_read(fx3_audio::HDMA_EMPTY) == 0 &&
                     fx3_audio_hdma_read(0x5FFF) == 0 &&
                     fx3_audio_hdma_read(0x7000) == 0,
                 "empty/out-of-range HDMA read was not safe");
}

void test_hdma_table_and_page_ownership() {
    TestSource source;
    source.bytes.resize(40u * 1024u);
    for (uint32_t i = 0; i < source.bytes.size(); ++i)
        source.bytes[i] = static_cast<uint8_t>(i);
    source.max_read = 1024;
    fx3_audio_init(hooks(source));
    play(0x1234);

    test_require(source.opened_asset == 0x1234,
                 "PLAY did not pass the 16-bit asset ID to the source");
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Priming),
                 "stream did not enter PRIMING");
    const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
    test_require(page < fx3_audio::HDMA_PAGE_COUNT, "stream did not publish an HDMA page");
    test_require(fx3_audio_host_read(reg(fx3_audio::HdmaSequence)) == 0,
                 "first HDMA page has the wrong starting sequence");

    uint32_t offset = static_cast<uint32_t>(page) * fx3_audio::HDMA_PAGE_SIZE;
    uint32_t source_offset = 0;
    uint32_t lines = 0;
    uint8_t packets = 0;
    while (true) {
        const uint8_t delay = fx3_audio_hdma_read(
            static_cast<uint16_t>(fx3_audio::HDMA_BASE + offset++));
        if (!delay)
            break;
        lines += delay;
        for (uint8_t byte = 0; byte < 3; ++byte) {
            test_require(fx3_audio_hdma_read(
                             static_cast<uint16_t>(fx3_audio::HDMA_BASE + offset++)) ==
                             source.bytes[source_offset++],
                         "HDMA table payload changed the BRR stream");
        }
        test_require(fx3_audio_hdma_read(
                         static_cast<uint16_t>(fx3_audio::HDMA_BASE + offset++)) == packets,
                     "HDMA table packet sequence is wrong");
        ++packets;
    }
    test_require(fx3_audio_host_read(reg(fx3_audio::HdmaLastSequence)) == packets - 1u,
                 "offered-page last strobe differs from its HDMA table");
    test_require(packets == 51 && lines == 224,
                 "HDMA table does not spread one frame across 224 lines");

    fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
    std::vector<uint8_t> active(offset);
    for (uint32_t i = 0; i < offset; ++i) {
        active[i] = fx3_audio_hdma_read(static_cast<uint16_t>(
            fx3_audio::HDMA_BASE + static_cast<uint32_t>(page) *
                fx3_audio::HDMA_PAGE_SIZE + i));
    }
    for (unsigned i = 0; i < 32; ++i)
        fx3_audio_task();
    for (uint32_t i = 0; i < offset; ++i) {
        test_require(fx3_audio_hdma_read(static_cast<uint16_t>(
                         fx3_audio::HDMA_BASE + static_cast<uint32_t>(page) *
                             fx3_audio::HDMA_PAGE_SIZE + i)) == active[i],
                     "Pico modified an ACTIVE HDMA page");
    }

    test_require(counter(fx3_audio::SdReads) >= 6 &&
                     counter(fx3_audio::SdBytes) == fx3_audio::FIFO_HIGH_WATER + 4u * 153u &&
                     counter(fx3_audio::HdmaPagesBuilt) == 4 &&
                     counter(fx3_audio::StreamPacketsSent) == 51,
                 "stream instrumentation did not track source and page activity");

    fx3_audio_host_write(reg(fx3_audio::SpcLevel), 4);
    fx3_audio_task();
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Playing),
                 "SPC fill feedback did not complete priming");

    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Stop));
    fx3_audio_task();
    test_require(source.closed &&
                     fx3_audio_host_read(reg(fx3_audio::Status)) ==
                         static_cast<uint8_t>(Fx3AudioStatus::Idle),
                 "STOP did not close and reset the stream");
    for (uint32_t i = 0; i < offset; ++i) {
        test_require(fx3_audio_hdma_read(static_cast<uint16_t>(
                         fx3_audio::HDMA_BASE + static_cast<uint32_t>(page) *
                             fx3_audio::HDMA_PAGE_SIZE + i)) == active[i],
                     "STOP modified an ACTIVE HDMA page before VBlank release");
    }
    fx3_audio_host_write(reg(fx3_audio::HdmaPage), fx3_audio::NO_PAGE);
    test_require(fx3_audio_hdma_read(static_cast<uint16_t>(
                     fx3_audio::HDMA_BASE + page * fx3_audio::HDMA_PAGE_SIZE)) == 0,
                 "explicit page release left an old table visible");
}

void test_eof_and_failures() {
    TestSource source;
    source.bytes.resize(18);
    fx3_audio_init(hooks(source));
    play(2);
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Draining),
                 "short BRR source did not enter DRAINING");
    const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
    test_require(page < fx3_audio::HDMA_PAGE_COUNT, "EOF discarded complete BRR blocks");
    fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
    fx3_audio_host_write(reg(fx3_audio::HdmaPage), fx3_audio::NO_PAGE);
    fx3_audio_host_write(reg(fx3_audio::SpcLevel), 0);
    fx3_audio_task();
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Eof),
                 "EOF did not wait for queued data to drain");

    source.read_error = true;
    fx3_audio_init(hooks(source));
    play(3);
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Error) &&
                     fx3_audio_host_read(reg(fx3_audio::Error)) ==
                         static_cast<uint8_t>(Fx3AudioError::ReadFailed),
                 "SD read failure did not remain contained in the audio module");
}

void test_sixty_second_stream_is_contiguous() {
    TestSource source;
    source.bytes.resize(9000u * 60u);
    for (uint32_t i = 0; i < source.bytes.size(); ++i)
        source.bytes[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
    fx3_audio_init(hooks(source));
    play(4);
    fx3_audio_host_write(reg(fx3_audio::SpcLevel), 4);

    uint32_t source_offset = 0;
    uint8_t sequence = 0;
    for (unsigned frame = 0; frame < 4000; ++frame) {
        fx3_audio_host_write(reg(fx3_audio::SpcLevel), 4);
        fx3_audio_task();
        const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
        if (page < fx3_audio::HDMA_PAGE_COUNT) {
            uint32_t offset = static_cast<uint32_t>(page) * fx3_audio::HDMA_PAGE_SIZE;
            while (true) {
                const uint8_t lines = fx3_audio_hdma_read(
                    static_cast<uint16_t>(fx3_audio::HDMA_BASE + offset++));
                if (!lines)
                    break;
                for (uint8_t byte = 0; byte < 3; ++byte) {
                    test_require(source_offset < source.bytes.size() &&
                                     fx3_audio_hdma_read(static_cast<uint16_t>(
                                         fx3_audio::HDMA_BASE + offset++)) ==
                                         source.bytes[source_offset++],
                                 "60-second stream repeated or skipped BRR data");
                }
                test_require(fx3_audio_hdma_read(static_cast<uint16_t>(
                                 fx3_audio::HDMA_BASE + offset++)) == sequence++,
                             "60-second stream lost packet sequence continuity");
            }
            fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
            fx3_audio_host_write(reg(fx3_audio::HdmaPage), fx3_audio::NO_PAGE);
        }
        if (source_offset == source.bytes.size())
            fx3_audio_host_write(reg(fx3_audio::SpcLevel), 0);
        if (fx3_audio_host_read(reg(fx3_audio::Status)) ==
            static_cast<uint8_t>(Fx3AudioStatus::Eof))
            break;
    }

    fx3_audio_task();
    test_require(source_offset == source.bytes.size() &&
                     fx3_audio_host_read(reg(fx3_audio::Status)) ==
                         static_cast<uint8_t>(Fx3AudioStatus::Eof),
                 "60-second stream did not drain cleanly");
}

void test_commands_are_ordered_and_latch_assets() {
    TestSource source;
    source.bytes.resize(65536);
    fx3_audio_init(hooks(source));
    play(1);
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Stop));
    fx3_audio_host_write(reg(fx3_audio::AssetIdLow), 0x34);
    fx3_audio_host_write(reg(fx3_audio::AssetIdHigh), 0x12);
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Play));
    fx3_audio_host_write(reg(fx3_audio::AssetIdLow), 0xFF);
    fx3_audio_host_write(reg(fx3_audio::AssetIdHigh), 0xFF);
    fx3_audio_task();
    test_require(source.closes == 1 && source.opened_asset == 0x1234 &&
                     fx3_audio_host_read(reg(fx3_audio::Error)) == 0,
                 "queued STOP/PLAY lost order or did not latch the PLAY asset ID");

    for (unsigned i = 0; i < 16; ++i)
        fx3_audio_host_write(reg(fx3_audio::Command),
                            static_cast<uint8_t>(Fx3AudioCommand::Pause));
    test_require(fx3_audio_host_read(reg(fx3_audio::Error)) ==
                     static_cast<uint8_t>(Fx3AudioError::CommandQueueFull),
                 "command queue overflow was silently discarded");
    fx3_audio_request_reset();
    fx3_audio_task();
    test_require(source.closed && fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Idle),
                 "reset did not override a full command queue");
}

void test_backpressure_and_pause() {
    TestSource source;
    source.bytes.resize(65536);
    fx3_audio_init(hooks(source));
    play(1);
    const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
    for (uint8_t level : {uint8_t{7}, uint8_t{8}, uint8_t{255}}) {
        fx3_audio_host_write(reg(fx3_audio::SpcLevel), level);
        test_require(fx3_audio_host_read(reg(fx3_audio::HdmaPage)) == fx3_audio::NO_PAGE &&
                         fx3_audio_host_read(reg(fx3_audio::HdmaSequence)) == fx3_audio::NO_PAGE,
                     "a full SPC ring was offered more audio");
        fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
        test_require(counter(fx3_audio::StreamPacketsSent) == 0,
                     "stale page claim bypassed full-ring backpressure");
    }
    fx3_audio_host_write(reg(fx3_audio::SpcLevel), 6);
    fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
    fx3_audio_task();
    test_require(counter(fx3_audio::StreamPacketsSent) == 51 &&
                     fx3_audio_host_read(reg(fx3_audio::HdmaPage)) == fx3_audio::NO_PAGE,
                 "multiple page claims exceeded the reported SPC byte credit");

    fx3_audio_host_write(reg(fx3_audio::SpcLevel), 4);
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Pause));
    fx3_audio_task();
    const uint32_t reads = source.reads;
    fx3_audio_task();
    test_require(fx3_audio_host_read(reg(fx3_audio::HdmaPage)) == fx3_audio::NO_PAGE &&
                     source.reads == reads,
                 "PAUSE offered packets or kept reading the source");
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Resume));
    fx3_audio_task();
    test_require(fx3_audio_host_read(reg(fx3_audio::HdmaPage)) < fx3_audio::HDMA_PAGE_COUNT,
                 "RESUME did not restore the queued READY page");
}

void test_bounded_reads_and_failure_keep_active_page() {
    TestSource source;
    source.bytes.resize(65536);
    source.retries = 2;
    fx3_audio_init(hooks(source));
    play(1);
    test_require(source.reads == 2 && source.offset == 0 &&
                     fx3_audio_host_read(reg(fx3_audio::Status)) ==
                         static_cast<uint8_t>(Fx3AudioStatus::Priming),
                 "temporary SD latency became EOF or a source failure");
    fx3_audio_task();
    test_require(source.reads == 3 && source.offset == 4096,
                 "audio service did not limit itself to one bounded source burst");
    const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
    test_require(page < fx3_audio::HDMA_PAGE_COUNT, "retry never produced a READY page");
    fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
    const uint16_t address = static_cast<uint16_t>(
        fx3_audio::HDMA_BASE + page * fx3_audio::HDMA_PAGE_SIZE);
    std::vector<uint8_t> saved(256);
    for (uint16_t i = 0; i < saved.size(); ++i)
        saved[i] = fx3_audio_hdma_read(static_cast<uint16_t>(address + i));
    source.read_error = true;
    fx3_audio_task();
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Error),
                 "failed read was not contained");
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Pause));
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Resume));
    fx3_audio_task();
    source.read_error = false;
    play(2);
    for (uint16_t i = 0; i < saved.size(); ++i)
        test_require(saved[i] == fx3_audio_hdma_read(static_cast<uint16_t>(address + i)),
                     "failure/restart changed an ACTIVE page before its release");
}

uint32_t consume_hdma_frame(uint8_t page, const TestSource& source,
                            uint32_t& source_offset, uint8_t& sequence) {
    uint32_t address = fx3_audio::HDMA_BASE + page * fx3_audio::HDMA_PAGE_SIZE;
    uint32_t line = 0;
    uint32_t bytes = 0;
    while (true) {
        const uint8_t count = fx3_audio_hdma_read(static_cast<uint16_t>(address++));
        if (!count)
            break;
        test_require(count < 128 && line < 224,
                     "HDMA repeated a transfer or crossed the active frame boundary");
        for (unsigned i = 0; i < 3; ++i) {
            test_require(source_offset < source.bytes.size() &&
                             fx3_audio_hdma_read(static_cast<uint16_t>(address++)) ==
                                 source.bytes[source_offset++],
                         "clock-feedback stream skipped/repeated BRR data");
            ++bytes;
        }
        test_require(fx3_audio_hdma_read(static_cast<uint16_t>(address++)) == sequence++,
                     "HDMA did not write the new sequence after its three payload bytes");
        // Non-repeating direct entries transfer on their first line, then wait.
        line += count;
    }
    test_require(bytes >= 9 && bytes % 9 == 0 && line == 224,
                 "HDMA first/final transfer or frame terminator timing was incorrect");
    return bytes;
}

void test_short_eof_frame_and_partial_block() {
    for (uint32_t size : {9u, 18u, 20u}) {
        TestSource source;
        source.bytes.resize(size, 0xA5);
        fx3_audio_init(hooks(source));
        play(1);
        const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
        test_require(page < fx3_audio::HDMA_PAGE_COUNT, "short EOF lost complete BRR blocks");
        fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
        uint32_t source_offset = 0;
        uint8_t sequence = 0;
        test_require(consume_hdma_frame(page, source, source_offset, sequence) ==
                         size / 9u * 9u,
                     "short EOF transported an incomplete BRR block");
        test_require(fx3_audio_host_read(reg(fx3_audio::Error)) ==
                         static_cast<uint8_t>(size == 20 ? Fx3AudioError::PartialBlock :
                                                          Fx3AudioError::None),
                     "EOF partial-block diagnostic was incorrect");
        fx3_audio_host_write(reg(fx3_audio::HdmaPage), fx3_audio::NO_PAGE);
        fx3_audio_host_write(reg(fx3_audio::SpcLevel), 0);
        fx3_audio_task();
        test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                         static_cast<uint8_t>(Fx3AudioStatus::Eof),
                     "short EOF did not drain after host release");
    }
}

void test_clock_feedback_with_sd_latency() {
    for (double refresh : {50.0, 59.94}) for (double clock_scale : {0.998, 1.002}) {
        TestSource source;
        source.bytes.resize(1200000);
        for (uint32_t i = 0; i < source.bytes.size(); ++i)
            source.bytes[i] = static_cast<uint8_t>(i * 37u + 11u);
        fx3_audio_init(hooks(source));
        fx3_audio_host_write(reg(fx3_audio::Flags), refresh == 50.0 ? fx3_audio::PAL_RATE : 0);
        play(1);
        double queued = 0;
        bool playing = false;
        uint32_t source_offset = 0;
        uint8_t sequence = 0;
        for (unsigned frame = 0; frame < (refresh == 50.0 ? 6000u : 7200u); ++frame) {
            const uint8_t level = static_cast<uint8_t>(queued / 252.0);
            fx3_audio_host_write(reg(fx3_audio::SpcLevel), level);
            if (frame % 200 == 100)
                source.retries = 3;
            fx3_audio_task();
            const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
            fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
            if (page != fx3_audio::NO_PAGE)
                queued += consume_hdma_frame(page, source, source_offset, sequence);
            test_require(queued <= 2016.0, "SPC clock drift overflowed the eight-segment ring");
            if (queued >= 4.0 * 252.0)
                playing = true;
            if (playing) {
                // Regional frame cadence and an independent 16 kHz DSP clock.
                const double consumed = 9000.0 * clock_scale / refresh;
                test_require(queued >= consumed, "SPC clock drift or short SD latency underrun");
                queued -= consumed;
            }
        }
        test_require(playing && source_offset > 1000000,
                     "clock-feedback test did not sustain two minutes of streaming");
    }
}

void test_fifo_underrun_inserts_silence_and_recovers() {
    TestSource source;
    source.bytes.resize(65536, 0xA5);
    fx3_audio_init(hooks(source));
    fx3_audio_host_write(reg(fx3_audio::Command), static_cast<uint8_t>(Fx3AudioCommand::Play));
    fx3_audio_task();
    source.retries = 1000;
    uint32_t source_offset = 0;
    uint8_t sequence = 0;
    bool silence = false;
    for (unsigned frame = 0; frame < 40 && !silence; ++frame) {
        fx3_audio_host_write(reg(fx3_audio::SpcLevel), 4);
        fx3_audio_task();
        const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
        test_require(page < fx3_audio::HDMA_PAGE_COUNT, "FIFO underrun supplied no silence page");
        fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
        uint32_t address = fx3_audio::HDMA_BASE + page * fx3_audio::HDMA_PAGE_SIZE;
        silence = fx3_audio_hdma_read(static_cast<uint16_t>(address + 1u)) == 0;
        if (!silence) {
            consume_hdma_frame(page, source, source_offset, sequence);
            continue;
        }
        uint32_t lines = 0;
        uint32_t packets = 0;
        while (const uint8_t count = fx3_audio_hdma_read(static_cast<uint16_t>(address++))) {
            lines += count;
            for (unsigned i = 0; i < 3; ++i)
                test_require(fx3_audio_hdma_read(static_cast<uint16_t>(address++)) == 0,
                             "FIFO underrun did not insert valid zero BRR blocks");
            test_require(fx3_audio_hdma_read(static_cast<uint16_t>(address++)) == sequence++,
                         "silence broke packet sequence continuity");
            ++packets;
        }
        test_require(lines == 224 && packets % 3u == 0 &&
                         fx3_audio_host_read(reg(fx3_audio::Status)) ==
                             static_cast<uint8_t>(Fx3AudioStatus::Underrun) &&
                         fx3_audio_host_read(reg(fx3_audio::Error)) ==
                             static_cast<uint8_t>(Fx3AudioError::FifoUnderrun),
                     "FIFO starvation was not reported as a recoverable underrun");
    }
    test_require(silence, "starvation test did not exhaust the encoded FIFO");
    source.retries = 0;
    fx3_audio_host_write(reg(fx3_audio::SpcLevel), 4);
    fx3_audio_task();
    const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
    test_require(page < fx3_audio::HDMA_PAGE_COUNT, "source did not recover after starvation");
    fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
    consume_hdma_frame(page, source, source_offset, sequence);
    test_require(fx3_audio_host_read(reg(fx3_audio::Status)) ==
                     static_cast<uint8_t>(Fx3AudioStatus::Playing),
                 "recovered FIFO stayed in UNDERRUN");
}

#ifdef SDK_TEST_THREADED_CRITICAL_SECTIONS
void test_concurrent_stop_and_page_reads() {
    TestSource source;
    source.bytes.resize(65536);
    for (uint32_t i = 0; i < source.bytes.size(); ++i)
        source.bytes[i] = static_cast<uint8_t>(i * 37u + 11u);
    fx3_audio_init(hooks(source));
    play(1);
    std::atomic<bool> done {false};
    std::atomic<uint32_t> services {0};
    std::thread core1([&] {
        while (!done.load(std::memory_order_acquire)) {
            fx3_audio_task();
            services.fetch_add(1, std::memory_order_release);
        }
    });
    unsigned active_reads = 0;
    for (unsigned frame = 0; frame < 10000; ++frame) {
        fx3_audio_host_write(reg(fx3_audio::SpcLevel), 4);
        const uint8_t page = fx3_audio_host_read(reg(fx3_audio::HdmaPage));
        const uint32_t sent = counter(fx3_audio::StreamPacketsSent);
        fx3_audio_host_write(reg(fx3_audio::HdmaPage), page);
        if (page < fx3_audio::HDMA_PAGE_COUNT &&
            counter(fx3_audio::StreamPacketsSent) != sent) {
            const uint16_t address = static_cast<uint16_t>(
                fx3_audio::HDMA_BASE + page * fx3_audio::HDMA_PAGE_SIZE);
            uint8_t saved[16];
            for (uint16_t i = 0; i < sizeof(saved); ++i)
                saved[i] = fx3_audio_hdma_read(static_cast<uint16_t>(address + i));
            if (saved[0]) {
                ++active_reads;
                fx3_audio_host_write(reg(fx3_audio::Command),
                                    static_cast<uint8_t>(Fx3AudioCommand::Stop));
                fx3_audio_host_write(reg(fx3_audio::Command),
                                    static_cast<uint8_t>(Fx3AudioCommand::Play));
                const uint32_t previous = services.load(std::memory_order_acquire);
                while (services.load(std::memory_order_acquire) == previous)
                    std::this_thread::yield();
                for (uint16_t i = 0; i < sizeof(saved); ++i)
                    test_require(saved[i] == fx3_audio_hdma_read(
                                     static_cast<uint16_t>(address + i)),
                                 "cross-core STOP/PLAY mutated an ACTIVE HDMA table");
            }
        }
        const uint32_t previous = services.load(std::memory_order_acquire);
        while (services.load(std::memory_order_acquire) == previous)
            std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
    core1.join();
    test_require(active_reads > 10, "cross-core stress test did not exercise page reads");
}
#endif
}

int main() {
    test_unavailable_source_is_safe();
    test_hdma_table_and_page_ownership();
    test_eof_and_failures();
    test_sixty_second_stream_is_contiguous();
    test_commands_are_ordered_and_latch_assets();
    test_backpressure_and_pause();
    test_bounded_reads_and_failure_keep_active_page();
    test_clock_feedback_with_sd_latency();
    test_short_eof_frame_and_partial_block();
    test_fifo_underrun_inserts_silence_and_recovers();
#ifdef SDK_TEST_THREADED_CRITICAL_SECTIONS
    test_concurrent_stop_and_page_reads();
#endif
    std::puts("audio_stream_tests: PASS");
    return 0;
}
