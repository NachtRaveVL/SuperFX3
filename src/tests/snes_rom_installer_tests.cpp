#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>
#include "../storage/snes_rom_installer.h"
#include "test_support.h"

struct Nor {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(ParallelRomProgrammer::CAPACITY, 0xFF);
    unsigned state = 0;
    bool id = false, active = true, busy = false;
    SnesRomInstaller* cancel_target = nullptr;
    uint64_t now = 0;
    static uint8_t read(void* p, uint32_t a) {
        auto& n = *static_cast<Nor*>(p);
        if (n.id) return a == 0 ? 0x9D : a == 2 ? 0x7E : a == 0x1C ? 0x21 : 0x01;
        return n.bytes.at(a);
    }
    static void write(void* p, uint32_t a, uint8_t b) {
        auto& n = *static_cast<Nor*>(p);
        if (n.state == 3) {
            test_require(n.busy && n.active, "NOR program without ownership");
            test_require((n.bytes[a] & b) == b, "NOR attempted zero-to-one program");
            n.bytes[a] &= b;
            n.state = 0;
        } else if (b == 0xF0) {
            n.state = 0;
            n.id = false;
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

static void small_fragmented_rom() {
    Nor nor;
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
    test_require(nor.bytes[0x8000] == 0x5A && nor.bytes[0x808000] == 0x5A &&
                     installer.read(0) == 0xCA && installer.read(512) == 0x5A,
                 "copier header stripping or source reconstruction failed");
    test_require(snes_rom_installed_map({&nor, Nor::read}) == SnesRomMap::LoRom,
                 "installed descriptor missing");
}

static void raw_full_capacity() {
    Nor nor;
    SnesRomInstaller installer(nor.bus(), nor.hooks());
    test_require(installer.begin(UsbRomFileType::Raw), "raw begin failed");
    std::array<uint8_t, 4096> page{};
    std::array<uint16_t, 4096> pages{};
    for (uint32_t i = 0; i < pages.size(); ++i) {
        pages[i] = static_cast<uint16_t>(i);
        page.fill(0xFF);
        page[0] = static_cast<uint8_t>(i);
        page[1] = static_cast<uint8_t>(i >> 8);
        test_require(installer.stage(i * 4096u, page.data(), page.size()), "raw page failed");
    }
    test_require(installer.finish(UsbRomFileType::Raw, ParallelRomProgrammer::CAPACITY,
                                  pages.data(), static_cast<uint32_t>(pages.size())) &&
                     installer.process(), "full 128-Mbit raw install failed");
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

int main() {
    small_fragmented_rom();
    raw_full_capacity();
    cancellation();
    std::puts("snes_rom_installer_tests: PASS");
}
