#pragma once

#include <chrono>
#include <cstdint>

struct QmiCsr {
    uint32_t value = 0;
    bool busy = false;
    operator uint32_t() const { return value | (busy && (value & 1u) ? 2u : 0u); }
    QmiCsr& operator=(uint32_t next) { value = next; return *this; }
};
struct qmi_hw_t { QmiCsr direct_csr; };
inline qmi_hw_t qmi_instance;
inline auto* qmi_hw = &qmi_instance;
#define QMI_DIRECT_CSR_EN_BITS 1u
#define QMI_DIRECT_CSR_BUSY_BITS 2u
#define QMI_DIRECT_CSR_ASSERT_CS0N_BITS 4u
#define QMI_DIRECT_CSR_ASSERT_CS1N_BITS 8u

struct io_qspi_hw_t { struct { uint32_t status = 0, ctrl = 0; } io[6]; };
inline io_qspi_hw_t io_qspi_instance;
inline auto* io_qspi_hw = &io_qspi_instance;
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_BITS 0x3000u
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_VALUE_HIGH 3u
#define IO_QSPI_GPIO_QSPI_SS_CTRL_OUTOVER_LSB 12u
#define GPIO_FUNC1_SIO 5u

struct pads_qspi_hw_t { uint32_t io[6] {}; };
inline pads_qspi_hw_t pads_qspi_instance;
inline auto* pads_qspi_hw = &pads_qspi_instance;
#define PADS_QSPI_GPIO_QSPI_SCLK_ISO_BITS 0x100u
#define PADS_QSPI_GPIO_QSPI_SCLK_OD_BITS 0x80u
#define PADS_QSPI_GPIO_QSPI_SD0_ISO_BITS 0x100u
#define PADS_QSPI_GPIO_QSPI_SD0_OD_BITS 0x80u
#define PADS_QSPI_GPIO_QSPI_SD1_ISO_BITS 0x100u
#define PADS_QSPI_GPIO_QSPI_SD1_IE_BITS 0x40u

struct SioAlias {
    uint32_t* target;
    bool set;
    void operator=(uint32_t value) const { if (set) *target |= value; else *target &= ~value; }
};
struct sio_hw_t {
    uint32_t cpuid = 1;
    uint32_t gpio_hi_in = 0, gpio_hi_out = 0, gpio_hi_oe = 0;
    SioAlias gpio_hi_set {&gpio_hi_out, true};
    SioAlias gpio_hi_clr {&gpio_hi_out, false};
    SioAlias gpio_hi_oe_set {&gpio_hi_oe, true};
    SioAlias gpio_hi_oe_clr {&gpio_hi_oe, false};
};
inline sio_hw_t sio_instance;
inline auto* sio_hw = &sio_instance;

struct TimerRead {
    uint32_t offset = 0;
    operator uint32_t() const {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) + offset;
    }
};
struct timer_hw_t { TimerRead timerawl; };
inline timer_hw_t timer_instance;
inline auto* timer0_hw = &timer_instance;
#define M33_ICSR_PENDSVSET_BITS 0x10000000u
#define M33_ICSR_PENDSVCLR_BITS 0x08000000u
#define M33_ICSR_PENDSTSET_BITS 0x04000000u
#define M33_ICSR_PENDSTCLR_BITS 0x02000000u
struct ExceptionPending {
    uint32_t value = 0;
    operator uint32_t() const { return value; }
    void operator=(uint32_t next) {
        if (next & M33_ICSR_PENDSVCLR_BITS) value &= ~M33_ICSR_PENDSVSET_BITS;
        if (next & M33_ICSR_PENDSTCLR_BITS) value &= ~M33_ICSR_PENDSTSET_BITS;
        value |= next & (M33_ICSR_PENDSVSET_BITS | M33_ICSR_PENDSTSET_BITS);
    }
};
struct scb_hw_t { uint32_t vtor = 0x20000000u; ExceptionPending icsr; };
inline scb_hw_t scb_instance;
inline auto* scb_hw = &scb_instance;
struct systick_hw_t { uint32_t csr = 0; };
inline systick_hw_t systick_instance;
inline auto* systick_hw = &systick_instance;
struct m33_eppb_hw_t { uint32_t nmi_mask[2] {}; };
inline m33_eppb_hw_t eppb_instance;
inline auto* m33_eppb_hw = &eppb_instance;
