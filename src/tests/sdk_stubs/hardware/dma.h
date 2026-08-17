#pragma once
#include <cstdint>
#include "../test_hardware.h"
using uint = unsigned int;
struct dma_channel_config {};
static constexpr int DMA_SIZE_32 = 2; ///< Stub value for 32-bit DMA transfers.
inline int dma_claim_unused_channel(bool) {
    if (sdk_test::next_dma >= sdk_test::DMA_COUNT)
        return -1;
    return static_cast<int>(sdk_test::next_dma++);
}
inline dma_channel_config dma_channel_get_default_config(uint) { return {}; }
inline void channel_config_set_transfer_data_size(dma_channel_config*, int) {}
inline void channel_config_set_read_increment(dma_channel_config*, bool) {}
inline void channel_config_set_write_increment(dma_channel_config*, bool) {}
inline void channel_config_set_high_priority(dma_channel_config*, bool) {}
inline void channel_config_set_dreq(dma_channel_config*, uint) {}
inline uint32_t dma_encode_endless_transfer_count() { return 0; }
inline void dma_channel_configure(uint channel, const dma_channel_config*, volatile void*, const volatile void*, uint32_t, bool start) {
    sdk_test::dma.at(channel).configured = true;
    sdk_test::dma.at(channel).running = start;
    sdk_test::dma.at(channel).aborted = false;
}
inline void dma_channel_abort(uint channel) {
    sdk_test::dma.at(channel).running = false;
    sdk_test::dma.at(channel).aborted = true;
}
