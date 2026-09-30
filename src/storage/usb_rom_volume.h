/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class UsbRomFileType : uint8_t {
    Sfc,
    Smc,
    Raw,
};

struct UsbRomSink {
    void* context;
    // Begin block staging before directory metadata exists. Type is selected at eject.
    bool (*begin)(void* context, UsbRomFileType type, uint32_t start_cluster);
    bool (*write)(void* context, uint32_t file_offset, const uint8_t* data, size_t size);
    uint8_t (*read)(void* context, uint32_t file_offset);
    bool (*complete)(void* context, UsbRomFileType type, uint32_t file_size,
                     const uint16_t* pages, uint32_t page_count);
    bool (*flush)(void* context);
};

class UsbRomVolume {
public:
    static constexpr uint32_t BLOCK_SIZE = 512;
    static constexpr uint32_t MAX_FILE_SIZE = 16u * 1024u * 1024u + 512u;
    static constexpr uint32_t PAGE_SIZE = 4096;
    static constexpr uint32_t STAGING_PAGES = 16u * 1024u * 1024u / PAGE_SIZE + 4u;
    static constexpr uint32_t FIRST_UPLOAD_BLOCK = 89;
    // Four RAM spill pages also leave room for host-created filesystem metadata
    // alongside a full 16 MiB image. Never advertise unstorable data sectors.
    static constexpr uint32_t BLOCK_COUNT = FIRST_UPLOAD_BLOCK + STAGING_PAGES * 8u;

    explicit UsbRomVolume(const UsbRomSink& sink);
    void reset();
    bool read(uint32_t lba, uint8_t* data, size_t size) const;
    bool write(uint32_t lba, const uint8_t* data, size_t size);
    bool flush();
    bool eject();
    bool ejected() const { return completion_sent_; }

private:
    static constexpr uint32_t SECTORS_PER_CLUSTER = 8;
    static constexpr uint32_t FAT_SECTORS = 32;
    static constexpr uint32_t ROOT_SECTORS = 16;
    static constexpr uint32_t FAT1_START = 1;
    static constexpr uint32_t FAT2_START = FAT1_START + FAT_SECTORS;
    static constexpr uint32_t ROOT_START = FAT2_START + FAT_SECTORS;
    static constexpr uint32_t DATA_START = ROOT_START + ROOT_SECTORS;
    static constexpr uint32_t README_CLUSTER = 2;

    uint16_t fat_entry(uint16_t cluster) const;

    UsbRomSink sink_;
    uint8_t fat_[FAT_SECTORS * BLOCK_SIZE]{};
    uint8_t root_[ROOT_SECTORS * BLOCK_SIZE]{};
    uint8_t received_[STAGING_PAGES]{}; // Eight 512-byte sector-presence bits per page.
    uint16_t pages_[STAGING_PAGES]{};
    bool sink_started_ = false;
    bool completion_sent_ = false;
};
