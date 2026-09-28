#ifndef RESERVED_EEPROM_H
#define RESERVED_EEPROM_H

#include <Arduino.h>
#include "ReservedEEPROMCore.h"

#if defined(__SAMD51__) || defined(__SAME51__) || defined(__SAME53__) || defined(__SAME54__)
#if defined(NVMCTRL_REGS)
#define RESERVED_NVM_REG(name) (NVMCTRL_REGS->NVMCTRL_##name)
#define RESERVED_NVM_MASK(name) NVMCTRL_##name##_Msk
#else
#define RESERVED_NVM_REG(name) (NVMCTRL->name.reg)
#define RESERVED_NVM_MASK(name) NVMCTRL_##name
#endif

class ReservedEEPROMHardware : public ReservedEEPROMBackend {
public:
  ReservedEEPROMHardware() : bytes_(0), pending_(false) {}
  ReservedEEPROMStatus geometry(ReservedEEPROMGeometry& result) override {
    bytes_ = 0;
    if (pending_ || hardwareBusy()) return ReservedEEPROMStatus::Busy;
    const uint32_t state = RESERVED_NVM_REG(SEESTAT);
    const uint32_t blocks = (state & NVMCTRL_SEESTAT_SBLK_Msk) >> NVMCTRL_SEESTAT_SBLK_Pos;
    const uint32_t size = (state & NVMCTRL_SEESTAT_PSZ_Msk) >> NVMCTRL_SEESTAT_PSZ_Pos;
    if (!(RESERVED_NVM_REG(PARAM) & RESERVED_NVM_MASK(PARAM_SEE)) || !blocks)
      return ReservedEEPROMStatus::Unconfigured;
    if (blocks > 10 || (state & RESERVED_NVM_MASK(SEESTAT_LOCK)) ||
        (RESERVED_NVM_REG(SEECFG) & (RESERVED_NVM_MASK(SEECFG_WMODE) |
                                   RESERVED_NVM_MASK(SEECFG_APRDIS))))
      return ReservedEEPROMStatus::InvalidConfiguration;
    const uint32_t maximum = blocks == 1 ? 4096 : blocks == 2 ? 8192 :
                             blocks <= 4 ? 16384 : blocks <= 8 ? 32768 : 65536;
    bytes_ = 512u << size;
    if (bytes_ > maximum) bytes_ = maximum;
    result = {bytes_, 4, 0, true};
    return ReservedEEPROMStatus::Ready;
  }
  bool readBytes(uint32_t offset, uint8_t* destination, uint32_t bytes) override {
    if (pending_ || hardwareBusy() || offset > bytes_ || bytes > bytes_ - offset)
      return false;
    const volatile uint8_t* source = reinterpret_cast<const volatile uint8_t*>(SEEPROM_ADDR + offset);
    for (uint32_t i = 0; i < bytes; ++i) destination[i] = source[i];
    return true;
  }
  bool erase(uint32_t) override { return false; }
  bool program(uint32_t offset, const uint8_t* source, uint32_t bytes) override {
    if (pending_ || hardwareBusy() || bytes != 4 || (offset & 3u) ||
        offset > bytes_ || bytes > bytes_ - offset ||
        (RESERVED_NVM_REG(SEESTAT) & RESERVED_NVM_MASK(SEESTAT_LOCK)) ||
        (RESERVED_NVM_REG(SEECFG) & (RESERVED_NVM_MASK(SEECFG_WMODE) |
                                   RESERVED_NVM_MASK(SEECFG_APRDIS)))) return false;
    RESERVED_NVM_REG(INTFLAG) = errors() | RESERVED_NVM_MASK(INTFLAG_SEEWRC);
    uint32_t value;
    memcpy(&value, source, sizeof(value));
    pending_ = true;
    *reinterpret_cast<volatile uint32_t*>(SEEPROM_ADDR + offset) = value;
    return true;
  }
  ReservedEEPROMStatus poll() override {
    if (hardwareBusy()) return ReservedEEPROMStatus::Busy;
    const uint32_t flags = RESERVED_NVM_REG(INTFLAG);
    if (flags & errors()) { pending_ = false; return ReservedEEPROMStatus::HardwareError; }
    if (pending_ && !(flags & RESERVED_NVM_MASK(INTFLAG_SEEWRC)))
      return ReservedEEPROMStatus::Busy;
    pending_ = false;
    return ReservedEEPROMStatus::Ready;
  }
private:
  static bool hardwareBusy() {
    return (RESERVED_NVM_REG(SEESTAT) & RESERVED_NVM_MASK(SEESTAT_BUSY)) ||
           !(RESERVED_NVM_REG(STATUS) & RESERVED_NVM_MASK(STATUS_READY));
  }
  static uint32_t errors() {
    return RESERVED_NVM_MASK(INTFLAG_ADDRE) | RESERVED_NVM_MASK(INTFLAG_PROGE) |
           RESERVED_NVM_MASK(INTFLAG_LOCKE) | RESERVED_NVM_MASK(INTFLAG_ECCDE) |
           RESERVED_NVM_MASK(INTFLAG_NVME) | RESERVED_NVM_MASK(INTFLAG_SEESOVF);
  }
  uint32_t bytes_;
  bool pending_;
};
#undef RESERVED_NVM_REG
#undef RESERVED_NVM_MASK

#else

class ReservedEEPROMHardware : public ReservedEEPROMBackend {
public:
  ReservedEEPROMHardware() : base_(0), bytes_(0), target_(0), manual_(0), phase_(Idle) {}
  ReservedEEPROMStatus geometry(ReservedEEPROMGeometry& result) override {
    bytes_ = 0;
    if (phase_ != Idle || !ready()) return ReservedEEPROMStatus::Busy;
    const uint32_t fuse = *reinterpret_cast<const volatile uint32_t*>(NVMCTRL_FUSES_EEPROM_SIZE_ADDR);
    const uint32_t selector = (fuse & NVMCTRL_FUSES_EEPROM_SIZE_Msk) >> NVMCTRL_FUSES_EEPROM_SIZE_Pos;
    if (selector == 7) return ReservedEEPROMStatus::Unconfigured;
    const uint32_t page = 8u << NVMCTRL->PARAM.bit.PSZ;
    const uint32_t total = page * NVMCTRL->PARAM.bit.NVMP;
    const uint32_t bytes = 16384u >> selector;
    const uint32_t boot = (fuse & 7u) == 7u ? 0 : (32768u >> (fuse & 7u));
    if (page != 64 || bytes > total || boot > total - bytes)
      return ReservedEEPROMStatus::InvalidConfiguration;
    bytes_ = bytes;
    base_ = total - bytes;
    result = {bytes_, 64, 256, false};
    return ReservedEEPROMStatus::Ready;
  }
  bool readBytes(uint32_t offset, uint8_t* destination, uint32_t bytes) override {
    if (phase_ != Idle || !ready() || offset > bytes_ || bytes > bytes_ - offset) return false;
    const volatile uint8_t* source = reinterpret_cast<const volatile uint8_t*>(base_ + offset);
    for (uint32_t i = 0; i < bytes; ++i) destination[i] = source[i];
    return true;
  }
  bool erase(uint32_t offset) override {
    if (phase_ != Idle || !ready() || (offset & 255u) || offset > bytes_ || 256 > bytes_ - offset)
      return false;
    clearErrors();
    NVMCTRL->ADDR.reg = (base_ + offset) / 2;
    phase_ = Erasing;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_ER;
    return true;
  }
  bool program(uint32_t offset, const uint8_t* source, uint32_t bytes) override {
    if (phase_ != Idle || !ready() || bytes != 64 || (offset & 63u) ||
        offset > bytes_ || bytes > bytes_ - offset) return false;
    memcpy(page_, source, sizeof(page_));
    target_ = base_ + offset;
    manual_ = NVMCTRL->CTRLB.reg & NVMCTRL_CTRLB_MANW;
    NVMCTRL->CTRLB.reg |= NVMCTRL_CTRLB_MANW;
    clearErrors();
    phase_ = Clearing;
    NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_PBC;
    return true;
  }
  ReservedEEPROMStatus poll() override {
    if (!ready()) return ReservedEEPROMStatus::Busy;
    if ((NVMCTRL->STATUS.reg & errors()) || (NVMCTRL->INTFLAG.reg & NVMCTRL_INTFLAG_ERROR)) {
      restore();
      return ReservedEEPROMStatus::HardwareError;
    }
    if (phase_ == Clearing) {
      volatile uint32_t* destination = reinterpret_cast<volatile uint32_t*>(target_);
      for (unsigned i = 0; i < 16; ++i) {
        uint32_t value;
        memcpy(&value, page_ + i * 4, sizeof(value));
        destination[i] = value;
      }
      NVMCTRL->ADDR.reg = target_ / 2;
      phase_ = Programming;
      NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_WP;
      return ReservedEEPROMStatus::Busy;
    }
    restore();
    return ReservedEEPROMStatus::Ready;
  }
private:
  enum Phase : uint8_t { Idle, Erasing, Clearing, Programming };
  static bool ready() { return (NVMCTRL->INTFLAG.reg & NVMCTRL_INTFLAG_READY) != 0; }
  static uint16_t errors() { return NVMCTRL_STATUS_PROGE | NVMCTRL_STATUS_LOCKE | NVMCTRL_STATUS_NVME; }
  static void clearErrors() {
    NVMCTRL->STATUS.reg = errors();
    NVMCTRL->INTFLAG.reg = NVMCTRL_INTFLAG_ERROR;
  }
  void restore() {
    if (phase_ == Clearing || phase_ == Programming)
      NVMCTRL->CTRLB.reg = (NVMCTRL->CTRLB.reg & ~NVMCTRL_CTRLB_MANW) | manual_;
    phase_ = Idle;
  }
  uint32_t base_, bytes_, target_, manual_;
  uint8_t page_[64];
  Phase phase_;
};
#endif

class ReservedEEPROMClass : private ReservedEEPROMHardware, public ReservedEEPROMCore {
public:
  ReservedEEPROMClass() : ReservedEEPROMHardware(), ReservedEEPROMCore(static_cast<ReservedEEPROMHardware&>(*this)) {}
};
#endif
