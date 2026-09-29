#pragma once
#include <stdint.h>
#include <stddef.h>

struct FlagRegister {
  uint32_t value = 0;
  uint32_t lastClear = 0;
  operator uint32_t() const { return value; }
  void operator=(uint32_t bits) { lastClear = bits; value &= ~bits; }
};

#if defined(__SAMD21__)
struct PlainRegister { uint32_t reg = 0; };
struct Parameter {
  struct Bits { uint32_t NVMP = 4096; uint32_t PSZ = 3; } bit;
};
struct FlagWrapper { FlagRegister reg; };
struct FakeD21 {
  Parameter PARAM;
  PlainRegister ADDR, CTRLA, CTRLB;
  FlagWrapper STATUS, INTFLAG;
};
extern FakeD21 fakeD21;
extern uint32_t fakeFuse;
#define NVMCTRL (&fakeD21)
#define NVMCTRL_FUSES_EEPROM_SIZE_ADDR uintptr_t(&fakeFuse)
#define NVMCTRL_FUSES_EEPROM_SIZE_Msk 0x70u
#define NVMCTRL_FUSES_EEPROM_SIZE_Pos 4
#define NVMCTRL_INTFLAG_READY 1u
#define NVMCTRL_INTFLAG_ERROR 2u
#define NVMCTRL_STATUS_PROGE 4u
#define NVMCTRL_STATUS_LOCKE 8u
#define NVMCTRL_STATUS_NVME 16u
#define NVMCTRL_CTRLB_MANW 128u
#define NVMCTRL_CTRLA_CMDEX_KEY 0xa500u
#define NVMCTRL_CTRLA_CMD_ER 2u
#define NVMCTRL_CTRLA_CMD_PBC 0x44u
#define NVMCTRL_CTRLA_CMD_WP 4u
#else
struct FakeNew {
  uint32_t NVMCTRL_SEESTAT = 0, NVMCTRL_PARAM = 0, NVMCTRL_SEECFG = 0, NVMCTRL_STATUS = 0;
  FlagRegister NVMCTRL_INTFLAG;
};
struct PlainRegister { uint32_t reg = 0; };
struct FlagWrapper { FlagRegister reg; };
struct FakeOld {
  PlainRegister SEESTAT, PARAM, SEECFG, STATUS;
  FlagWrapper INTFLAG;
};
extern FakeNew fakeNew;
extern FakeOld fakeOld;
alignas(4) extern uint8_t virtualEEPROM[65536];
#define SEEPROM_ADDR uintptr_t(virtualEEPROM)
#define NVMCTRL_SEESTAT_SBLK_Msk 0x00000f00u
#define NVMCTRL_SEESTAT_SBLK_Pos 8
#define NVMCTRL_SEESTAT_PSZ_Msk 0x00070000u
#define NVMCTRL_SEESTAT_PSZ_Pos 16
#define NVMCTRL_PARAM_SEE_Msk 0x80000000u
#define NVMCTRL_SEESTAT_LOCK_Msk 8u
#define NVMCTRL_SEESTAT_BUSY_Msk 4u
#define NVMCTRL_SEECFG_WMODE_Msk 1u
#define NVMCTRL_SEECFG_APRDIS_Msk 2u
#define NVMCTRL_STATUS_READY_Msk 1u
#define NVMCTRL_INTFLAG_ADDRE_Msk 2u
#define NVMCTRL_INTFLAG_PROGE_Msk 4u
#define NVMCTRL_INTFLAG_LOCKE_Msk 8u
#define NVMCTRL_INTFLAG_ECCDE_Msk 32u
#define NVMCTRL_INTFLAG_NVME_Msk 64u
#define NVMCTRL_INTFLAG_SEESOVF_Msk 512u
#define NVMCTRL_INTFLAG_SEEWRC_Msk 1024u
#if defined(__SAMD51__)
#define NVMCTRL (&fakeOld)
#define NVMCTRL_PARAM_SEE NVMCTRL_PARAM_SEE_Msk
#define NVMCTRL_SEESTAT_LOCK NVMCTRL_SEESTAT_LOCK_Msk
#define NVMCTRL_SEESTAT_BUSY NVMCTRL_SEESTAT_BUSY_Msk
#define NVMCTRL_SEECFG_WMODE NVMCTRL_SEECFG_WMODE_Msk
#define NVMCTRL_SEECFG_APRDIS NVMCTRL_SEECFG_APRDIS_Msk
#define NVMCTRL_STATUS_READY NVMCTRL_STATUS_READY_Msk
#define NVMCTRL_INTFLAG_ADDRE NVMCTRL_INTFLAG_ADDRE_Msk
#define NVMCTRL_INTFLAG_PROGE NVMCTRL_INTFLAG_PROGE_Msk
#define NVMCTRL_INTFLAG_LOCKE NVMCTRL_INTFLAG_LOCKE_Msk
#define NVMCTRL_INTFLAG_ECCDE NVMCTRL_INTFLAG_ECCDE_Msk
#define NVMCTRL_INTFLAG_NVME NVMCTRL_INTFLAG_NVME_Msk
#define NVMCTRL_INTFLAG_SEESOVF NVMCTRL_INTFLAG_SEESOVF_Msk
#define NVMCTRL_INTFLAG_SEEWRC NVMCTRL_INTFLAG_SEEWRC_Msk
#else
#define NVMCTRL_REGS (&fakeNew)
#endif
#endif
