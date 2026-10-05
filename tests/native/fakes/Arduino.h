#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

using byte = uint8_t;
#define F(value) value
#define HEX 16
struct SerialStub {
    template<class... Args> void print(Args...) {}
    template<class... Args> void println(Args...) {}
    void flush() {}
};
extern SerialStub nativeSerial;
#define SERIAL_PORT_MONITOR nativeSerial

#define NVMCTRL_PARAM_NVMP_Msk 0xffffu
#define NVMCTRL_PARAM_NVMP_Pos 0u
#define NVMCTRL_PARAM_PSZ_Msk (7u << 16)
#define NVMCTRL_PARAM_PSZ_Pos 16u
#define NVMCTRL_CTRLA_WMODE_Msk (3u << 4)
#define NVMCTRL_CTRLA_WMODE_MAN 0u
#if defined(TEST_LEGACY_SAMD51)
#define NVMCTRL_CTRLA_CACHEDIS0 (1u << 14)
#define NVMCTRL_CTRLA_CACHEDIS1 (1u << 15)
#else
#define NVMCTRL_CTRLA_CACHEDIS0_Msk (1u << 14)
#define NVMCTRL_CTRLA_CACHEDIS1_Msk (1u << 15)
#endif
#define NVMCTRL_STATUS_READY_Msk 1u
#define NVMCTRL_INTFLAG_DONE_Msk 1u
#define NVMCTRL_CTRLB_CMDEX_KEY 0xa500u
#define NVMCTRL_CTRLB_CMD_EB 0x01u
#define NVMCTRL_CTRLB_CMD_WP 0x03u
#define NVMCTRL_CTRLB_CMD_PBC 0x15u

struct ControlA {
    uint32_t value = 4u;
    operator uint32_t() const volatile { return value; }
    void operator=(uint32_t next) volatile;
};
struct ControlB {
    uint32_t value = 0u;
    operator uint32_t() const volatile { return value; }
    void operator=(uint32_t next) volatile;
};
struct StatusRegister {
    operator uint32_t() const volatile;
};
struct DoneRegister {
    operator uint32_t() const volatile;
    void operator=(uint32_t value) volatile;
};
struct AddressRegister {
    uint32_t value = 0;
    operator uint32_t() const volatile { return value; }
    void operator=(uint32_t next) volatile;
};
struct FakeNvm {
    volatile ControlA NVMCTRL_CTRLA;
    volatile ControlB NVMCTRL_CTRLB;
    volatile uint32_t NVMCTRL_PARAM = (6u << 16) | 2048u;
    volatile StatusRegister NVMCTRL_STATUS;
    volatile DoneRegister NVMCTRL_INTFLAG;
    volatile AddressRegister NVMCTRL_ADDR;
};
extern FakeNvm nativeNvm;
#if !defined(TEST_LEGACY_SAMD51)
#define NVMCTRL_REGS (&nativeNvm)
#else
#define NVMCTRL_STATUS_READY NVMCTRL_STATUS_READY_Msk
struct ControlField {
    volatile ControlA *control;
    unsigned shift;
    unsigned mask;
    operator uint32_t() const { return (static_cast<uint32_t>(*control) >> shift) & mask; }
    void operator=(uint32_t next) { *control = (static_cast<uint32_t>(*control) & ~(mask << shift)) | ((next & mask) << shift); }
};
struct LegacyNvm {
    struct { struct { uint32_t PSZ = 6u; uint32_t NVMP = 2048u; } bit; } PARAM;
    struct {
        volatile ControlA &reg = nativeNvm.NVMCTRL_CTRLA;
        struct {
            ControlField WMODE{&nativeNvm.NVMCTRL_CTRLA, 4u, 3u};
            ControlField CACHEDIS0{&nativeNvm.NVMCTRL_CTRLA, 14u, 1u};
            ControlField CACHEDIS1{&nativeNvm.NVMCTRL_CTRLA, 15u, 1u};
        } bit;
    } CTRLA;
    struct { volatile ControlB &reg = nativeNvm.NVMCTRL_CTRLB; } CTRLB;
    struct {
        volatile StatusRegister &reg = nativeNvm.NVMCTRL_STATUS;
        struct { volatile StatusRegister &READY = nativeNvm.NVMCTRL_STATUS; } bit;
    } STATUS;
    struct {
        volatile DoneRegister &reg = nativeNvm.NVMCTRL_INTFLAG;
        struct { volatile DoneRegister &DONE = nativeNvm.NVMCTRL_INTFLAG; } bit;
    } INTFLAG;
    struct { volatile AddressRegister &reg = nativeNvm.NVMCTRL_ADDR; } ADDR;
};
extern LegacyNvm legacyNvm;
#define NVMCTRL (&legacyNvm)
#endif

