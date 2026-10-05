#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>
#include "../storage/snes_rom_installer.h"
#include "../platform/rp2350/qspi_rom.h"
#include "sdk_stubs/test_flash.h"
#include "test_support.h"

static bool qspi_usb = true;
bool snes_bus_usb_mode() { return qspi_usb; }

struct Nor {
    explicit Nor(uint32_t capacity = ParallelRomProgrammer::MAX_CAPACITY)
        : bytes(capacity, 0xFF), capacity(capacity) {}
    std::vector<uint8_t> bytes;
    uint32_t capacity;
    unsigned state = 0;
    bool id = false, cfi = false, active = true, busy = false;
    bool fail_final_program = false;
    SnesRomInstaller* cancel_target = nullptr;
    uint64_t now = 0;
    static uint8_t read(void* p, uint32_t a) {
        auto& n = *static_cast<Nor*>(p);
        if (n.cfi) {
            if (a == 0x20) return 'Q';
            if (a == 0x22) return 'R';
            if (a == 0x24) return 'Y';
            if (a == 0x26) return 2;
            if (a == 0x4E) {
                uint8_t power = 0;
                for (uint32_t size = n.capacity; size > 1u; size >>= 1) ++power;
                return power;
            }
            if (a == 0x58) return 1;
            if (a == 0x5A) return static_cast<uint8_t>(n.capacity /
                ParallelRomProgrammer::SECTOR_SIZE - 1u);
            if (a == 0x5C) return static_cast<uint8_t>((n.capacity /
                ParallelRomProgrammer::SECTOR_SIZE - 1u) >> 8);
            if (a == 0x60) return 2;
            return 0;
        }
        if (n.id) return a == 0 ? 0x9D : a == 2 ? 0x7E : a == 0x1C ? 0x21 : 0x01;
        return n.bytes.at(a);
    }
    static void write(void* p, uint32_t a, uint8_t b) {
        auto& n = *static_cast<Nor*>(p);
        if (n.state == 3) {
            test_require(n.busy && n.active, "NOR program without ownership");
            test_require((n.bytes[a] & b) == b, "NOR attempted zero-to-one program");
            if (!n.fail_final_program || sdk_flash::programs < 768u)
                n.bytes[a] &= b;
            n.state = 0;
        } else if (b == 0xF0) {
            n.state = 0;
            n.id = false;
            n.cfi = false;
        } else if (n.state == 0 && a == 0xAA && b == 0x98) {
            n.cfi = true;
        } else if (n.state == 0 && a == 0xAAA && b == 0xAA) n.state = 1;
        else if (n.state == 1 && a == 0x555 && b == 0x55) n.state = 2;
        else if (n.state == 2 && a == 0xAAA && b == 0x90) { n.id = true; n.state = 0; }
        else if (n.state == 2 && a == 0xAAA && b == 0xA0) n.state = 3;
        else if (n.state == 2 && a == 0xAAA && b == 0x80) n.state = 4;
        else if (n.state == 4 && a == 0xAAA && b == 0xAA) n.state = 5;
        else if (n.state == 5 && a == 0x555 && b == 0x55) n.state = 6;
        else if (n.state == 6 && b == 0x30) {
            test_require(n.busy && n.active, "NOR erase without ownership");
            const uint32_t base = a & ~(ParallelRomProgrammer::SECTOR_SIZE - 1u);
            std::fill_n(n.bytes.begin() + base, ParallelRomProgrammer::SECTOR_SIZE, 0xFF);
            n.state = 0;
        } else test_require(false, "invalid NOR command sequence");
    }
    static uint64_t time(void* p) { return ++static_cast<Nor*>(p)->now; }
    static bool usb(void* p) { return static_cast<Nor*>(p)->active; }
    static void irq(void* p, bool b) { static_cast<Nor*>(p)->busy = b; }
    static void service(void* p) {
        auto& n = *static_cast<Nor*>(p);
        if (n.cancel_target) n.cancel_target->abort();
    }
    ParallelRomBus bus() { return {this, read, write, time, nullptr}; }
    SnesRomInstallHooks hooks() { return {this, usb, irq, service}; }
};

static void small_fragmented_rom(uint32_t capacity = ParallelRomProgrammer::MAX_CAPACITY) {
    Nor nor(capacity);
    SnesRomInstaller installer(nor.bus(), nor.hooks());
    std::vector<uint8_t> file(32768u + 512u, 0xFF);
    std::fill_n(file.begin(), 512, 0xCA);
    const uint32_t h = 512u + 0x7FC0u;
    std::fill_n(file.begin() + h, 21, ' ');
    file[h + 0x15] = 0x20;
    file[h + 0x1C] = 0xCB; file[h + 0x1D] = 0xED;
    file[h + 0x1E] = 0x34; file[h + 0x1F] = 0x12;
    file[h + 0x3C] = 0; file[h + 0x3D] = 0x80;
    file[512] = 0x5A;
    const uint16_t pages[] = {33, 0, 1, 2, 3, 4, 5, 6, 7};
    test_require(installer.begin(UsbRomFileType::Raw), "begin failed");
    test_require(!installer.begin(UsbRomFileType::Raw), "reentrant begin accepted");
    for (uint32_t offset = static_cast<uint32_t>(file.size()); offset; offset -= 512u) {
        const uint32_t source = offset - 512u;
        const uint32_t physical = pages[source / 4096u] * 4096u + source % 4096u;
        test_require(installer.stage(physical, file.data() + source, 512), "fragmented stage failed");
    }
    // Repeat a sector after flush with a 0->1 change. NOR must use RMW, not
    // attempt to program over a previously cleared bit.
    uint8_t zero = 0;
    test_require(installer.stage(33u * 4096u + 512u, &zero, 1) && installer.flush(), "flush failed");
    test_require(installer.stage(33u * 4096u + 512u, &file[512], 1), "rewrite rejected");
    test_require(installer.finish(UsbRomFileType::Smc, static_cast<uint32_t>(file.size()), pages, 9),
                 "finish rejected FAT chain");
    test_require(installer.process() && !nor.busy, "mapped install failed");
    test_require(nor.bytes[0x8000] == 0x5A &&
                     installer.read(0) == 0xCA && installer.read(512) == 0x5A,
                 "copier header stripping or source reconstruction failed");
    if (capacity == ParallelRomProgrammer::MAX_CAPACITY)
        test_require(snes_rom_installed_map({&nor, Nor::read}) == SnesRomMap::LoRom,
                     "installed descriptor missing");
}

static void raw_full_capacity(uint32_t capacity) {
    Nor nor(capacity);
    SnesRomInstaller installer(nor.bus(), nor.hooks());
    test_require(installer.begin(UsbRomFileType::Raw), "raw begin failed");
    std::array<uint8_t, 4096> page{};
    std::vector<uint16_t> pages(capacity / page.size());
    for (uint32_t i = 0; i < pages.size(); ++i) {
        pages[i] = static_cast<uint16_t>(i);
        page.fill(0xFF);
        page[0] = static_cast<uint8_t>(i);
        page[1] = static_cast<uint8_t>(i >> 8);
        test_require(installer.stage(i * 4096u, page.data(), page.size()), "raw page failed");
    }
    test_require(installer.capacity() == capacity, "CFI capacity was not retained by installer");
    test_require(installer.finish(UsbRomFileType::Raw, capacity,
                                  pages.data(), static_cast<uint32_t>(pages.size())) &&
                     installer.process(), "full-capacity raw install failed");
    for (uint32_t i = 0; i < pages.size(); ++i)
        test_require(nor.bytes[i * 4096u] == static_cast<uint8_t>(i) &&
                         nor.bytes[i * 4096u + 1u] == static_cast<uint8_t>(i >> 8),
                     "full raw image lost data");
    test_require(!nor.busy, "raw completion left IRQ asserted");
}

static void cancellation() {
    Nor nor;
    SnesRomInstaller installer(nor.bus(), nor.hooks());
    test_require(installer.begin(UsbRomFileType::Raw) && nor.busy, "abort setup failed");
    nor.active = false;
    uint8_t data = 0;
    test_require(!installer.stage(0, &data, 1) && !nor.busy &&
                     installer.status() == SnesRomInstallStatus::Aborted,
                 "between-write cancellation leaked IRQ/state");
    nor.active = true;
    test_require(installer.begin(UsbRomFileType::Raw), "restart after abort failed");
    installer.abort();
    test_require(!nor.busy, "disconnect abort leaked IRQ");
    test_require(installer.begin(UsbRomFileType::Raw) && installer.stage(0, &data, 1),
                 "callback cancellation setup failed");
    nor.cancel_target = &installer;
    test_require(!installer.flush() && !nor.busy &&
                     installer.status() == SnesRomInstallStatus::Aborted,
                 "USB callback abort did not stop in-flight programming");
}

static void fx3_install(bool dump, uint32_t size = 0x400000u, unsigned failure = 0,
                        uint32_t capacity = ParallelRomProgrammer::MAX_CAPACITY,
                        bool copier_header = false) {
    Nor nor(capacity);
    auto hooks = nor.hooks();
    hooks.program_fx = qspi_rom_program;
    SnesRomInstaller installer(nor.bus(), hooks);
    std::vector<uint8_t> rom(size, 0xFF);
    for (uint32_t i = 0; i < rom.size(); i += 4096u) {
        rom[i] = static_cast<uint8_t>(i >> 12);
        rom[i + 1] = static_cast<uint8_t>(i >> 20);
    }
    std::fill_n(rom.begin() + 0x7FC0, 21, ' ');
    rom[0x7FBD] = 7;
    rom[0x7FD5] = 0x20; rom[0x7FD6] = 0x18;
    rom[0x7FD7] = size == 0x100000u ? 0x0A : size == 0x200000u ? 0x0B : 0x0C;
    rom[0x7FDC] = 0xCB; rom[0x7FDD] = 0xED;
    rom[0x7FDE] = 0x34; rom[0x7FDF] = 0x12;
    rom[0x7FFC] = 0; rom[0x7FFD] = 0x80;
    std::vector<uint8_t> file = rom;
    if (dump) {
        file.resize(0x800100u, 0xFF);
        for (uint32_t i = 0; i < 0x400000u; ++i) {
            file[i] = rom[(i >> 16) * 0x8000u + (i & 0x7FFFu)];
            file[0x400000u + i] = rom[i];
        }
    }
    if (copier_header)
        file.insert(file.begin(), 512u, 0xCA);
    sdk_flash::bytes.fill(0xA5);
    sdk_flash::programs = sdk_flash::erases = 0;
    test_require(installer.begin(UsbRomFileType::Sfc) &&
                     installer.stage(0, file.data(), file.size()), "FX3 staging failed");
    installer.finish(static_cast<uint32_t>(file.size()));
    if (failure) {
        sdk_flash::corrupt_program = failure == 1;
        nor.fail_final_program = failure == 2;
        test_require(!installer.process() && !nor.busy &&
                         installer.status() == SnesRomInstallStatus::FlashError,
                     "one-device failure reported a successful FX3 installation");
        test_require(snes_rom_installed_map({&nor, Nor::read}) != SnesRomMap::Fx3,
                     "FX3 descriptor committed before both flashes verified");
        sdk_flash::corrupt_program = false;
        return;
    }
    test_require(installer.process() && installer.installed_map() == SnesRomMap::Fx3 && !nor.busy,
                 "FX3 dual-storage install failed");
    test_require(sdk_flash::programs == 768 && sdk_flash::erases == 768 &&
                     !sdk_flash::other_core_parked && !sdk_flash::interrupts_disabled && sdk_flash::xip,
                 "FX3 QSPI programming count or flash lockout cleanup changed");
    for (uint32_t i = 0; i < 0x300000u; ++i)
        test_require(sdk_flash::bytes[0x100000u + i] == rom[size < 0x300000u ? i % size : i],
                     "FX3 QSPI payload/mirroring changed");
    test_require(std::all_of(sdk_flash::bytes.begin(), sdk_flash::bytes.begin() + 0x100000,
                            [](uint8_t value) { return value == 0xA5; }),
                 "FX3 installation modified firmware/save partitions");
    for (uint32_t i = 0; i < file.size(); ++i)
        test_require(installer.read(i) == file[i], "FX3 upload readback lost source data");
    for (uint32_t i = 0; i < nor.bytes.size(); ++i) {
        bool assigned = false;
        uint8_t expected = 0xFF;
        for (uint32_t alias = i; alias < ParallelRomProgrammer::MAX_CAPACITY;
             alias += capacity) {
            uint32_t source = 0;
            if (!snes_rom_source_offset({SnesRomMap::Fx3, size, 0}, alias, source))
                continue;
            test_require(!assigned || expected == rom[source],
                         "test selected an FX3 image that does not fit the device");
            expected = rom[source];
            assigned = true;
        }
        if (assigned)
            test_require(nor.bytes[i] == expected, "FX3 physical NOR mapping lost data");
    }
    if (capacity == ParallelRomProgrammer::MAX_CAPACITY)
        test_require(snes_rom_installed_map({&nor, Nor::read}) == SnesRomMap::Fx3,
                     "FX3 installed descriptor missing");
}

static void fx3_program_failures() {
    std::array<uint8_t, 4096> data{};
    const uint32_t erases = sdk_flash::erases;
    for (uint32_t offset : {1u, 0x300000u, 0xFFFFFFFFu})
        test_require(!qspi_rom_program(nullptr, offset, data.data(), 4096u), "invalid FX offset accepted");
    test_require(!qspi_rom_program(nullptr, 0, nullptr, 4096u) &&
                     !qspi_rom_program(nullptr, 0, data.data(), 256u), "invalid FX sector accepted");
    qspi_usb = false;
    test_require(!qspi_rom_program(nullptr, 0, data.data(), 4096u), "FX write outside USB mode accepted");
    qspi_usb = true;
    sdk_flash::fail_enter = true;
    test_require(!qspi_rom_program(nullptr, 0, data.data(), 4096u), "FX lockout failure ignored");
    sdk_flash::fail_enter = false;
    test_require(sdk_flash::erases == erases, "rejected FX write erased flash");
    sdk_flash::corrupt_program = true;
    test_require(!qspi_rom_program(nullptr, 0, data.data(), 4096u), "FX verification failure ignored");
    sdk_flash::corrupt_program = false;
}

int main() {
    small_fragmented_rom();
    small_fragmented_rom(ParallelRomProgrammer::MIN_CAPACITY);
    for (uint32_t capacity = ParallelRomProgrammer::MIN_CAPACITY;
         capacity <= ParallelRomProgrammer::MAX_CAPACITY; capacity <<= 1)
        raw_full_capacity(capacity);
    cancellation();
    test_require(!sdk_flash::erases && !sdk_flash::programs, "ordinary/raw upload wrote QSPI");
    fx3_install(false);
    fx3_install(false, 0x200000u);
    fx3_install(false, 0x300000u);
    fx3_install(true);
    fx3_install(false, 0x400000u, 0, 8u * 1024u * 1024u);
    fx3_install(true, 0x400000u, 0, 8u * 1024u * 1024u, true);
    fx3_install(false, 0x200000u, 1);
    fx3_install(false, 0x200000u, 2);
    fx3_program_failures();
    std::puts("snes_rom_installer_tests: PASS");
}
