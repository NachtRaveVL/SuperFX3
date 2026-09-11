#pragma once
#include <cstdint>
#include "../test_hardware.h"
using uint = unsigned int;
struct dma_channel_config { uint dreq = 0; uint chain_to = 0; };
static constexpr int DMA_SIZE_32 = 2; ///< Stub value for 32-bit DMA transfers.
inline int dma_claim_unused_channel(bool) {
    if (sdk_test::next_dma >= sdk_test::DMA_COUNT)
        return -1;
    return static_cast<int>(sdk_test::next_dma++);
}
inline dma_channel_config dma_channel_get_default_config(uint channel) { return {0, channel}; }
inline void channel_config_set_transfer_data_size(dma_channel_config*, int) {}
inline void channel_config_set_read_increment(dma_channel_config*, bool) {}
inline void channel_config_set_write_increment(dma_channel_config*, bool) {}
inline void channel_config_set_high_priority(dma_channel_config*, bool) {}
inline void channel_config_set_dreq(dma_channel_config* config, uint dreq) { config->dreq = dreq; }
inline void channel_config_set_chain_to(dma_channel_config* config, uint channel) { config->chain_to = channel; }
inline void channel_config_set_enable(dma_channel_config*, bool) {}
inline uint32_t dma_encode_transfer_count(uint count) { return count; }
inline uint32_t dma_encode_endless_transfer_count() { return 0xF0000000u; }
inline void dma_channel_set_config(uint, const dma_channel_config*, bool) {}
inline void dma_channel_configure(uint channel, const dma_channel_config* config, volatile void* write_addr,
                                  const volatile void* read_addr, uint32_t count, bool start) {
    sdk_test::dma.at(channel).configured = true;
    sdk_test::dma.at(channel).running = start;
    sdk_test::dma.at(channel).aborted = false;
    sdk_test::dma.at(channel).dreq = config->dreq;
    sdk_test::dma.at(channel).chain_to = config->chain_to;
    sdk_test::dma.at(channel).transfer_count = count;
    sdk_test::dma.at(channel).read_addr = read_addr;
    sdk_test::dma.at(channel).write_addr = write_addr;
}
inline void dma_channel_abort(uint channel) {
    sdk_test::dma.at(channel).running = false;
    sdk_test::dma.at(channel).aborted = true;
}
