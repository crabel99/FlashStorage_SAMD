#include "ReservedEEPROM.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); std::abort(); } } while (0)
using Status = ReservedEEPROMStatus;
#if defined(__SAMD21__)
FakeD21 fakeD21;
uint32_t fakeFuse = 0x17;
int main() {
  ReservedEEPROMHardware hardware;
  ReservedEEPROMGeometry g;
  fakeD21.INTFLAG.reg.value = 1;
  CHECK(hardware.geometry(g) == Status::Ready && g.bytes == 8192 && !g.smart);
  CHECK(fakeFuse == 0x17);
  fakeFuse = 0x77;
  CHECK(hardware.geometry(g) == Status::Unconfigured);
  fakeFuse = 0x17;
  fakeD21.INTFLAG.reg.value = 0;
  CHECK(hardware.geometry(g) == Status::Busy);
  fakeD21.INTFLAG.reg.value = 1;
  CHECK(hardware.geometry(g) == Status::Ready);
  CHECK(!hardware.erase(1) && !hardware.erase(8192));
  CHECK(hardware.erase(256));
  CHECK(fakeD21.ADDR.reg == (256 * 1024 - 8192 + 256) / 2);
  fakeD21.INTFLAG.reg.value = 0;
  CHECK(hardware.poll() == Status::Busy);
  fakeD21.INTFLAG.reg.value = 1;
  CHECK(hardware.poll() == Status::Ready);
  uint8_t page[64] = {};
  CHECK(!hardware.program(0, page, 63));
  CHECK(!hardware.program(1, page, 64));
  for (uint32_t manual : {0u, uint32_t(NVMCTRL_CTRLB_MANW)}) {
    fakeD21.CTRLB.reg = manual | 0x800u;
    CHECK(hardware.program(0, page, 64));
    CHECK(fakeD21.CTRLB.reg == (manual | 0x800u | NVMCTRL_CTRLB_MANW));
    CHECK(fakeD21.CTRLA.reg == (NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_PBC));
    fakeD21.STATUS.reg.value = NVMCTRL_STATUS_PROGE;
    CHECK(hardware.poll() == Status::HardwareError);
    CHECK(fakeD21.CTRLB.reg == (manual | 0x800u));
    fakeD21.STATUS.reg.value = 0;
  }
  std::puts("D21 register configuration/erase/error tests passed");
}
#else
FakeNew fakeNew;
FakeOld fakeOld;
alignas(4) uint8_t virtualEEPROM[65536];
#if defined(__SAMD51__)
#define REG(name) fakeOld.name.reg
#else
#define REG(name) fakeNew.NVMCTRL_##name
#endif
int main() {
  ReservedEEPROMHardware hardware;
  ReservedEEPROMGeometry g;
  REG(STATUS) = 1;
  REG(PARAM) = NVMCTRL_PARAM_SEE_Msk;
  CHECK(hardware.geometry(g) == Status::Unconfigured);
  for (unsigned blocks = 1; blocks <= 10; ++blocks) {
    for (unsigned psz = 0; psz < 8; ++psz) {
      REG(SEESTAT) = (blocks << 8) | (psz << 16);
      CHECK(hardware.geometry(g) == Status::Ready);
      const uint32_t maximum = blocks == 1 ? 4096 : blocks == 2 ? 8192 : blocks <= 4 ? 16384 : blocks <= 8 ? 32768 : 65536;
      const uint32_t desired = 512u << psz;
      CHECK(g.bytes == (desired < maximum ? desired : maximum));
    }
  }
  REG(SEESTAT) = 2u << 8 | 4u << 16;
  REG(SEECFG) = 1;
  CHECK(hardware.geometry(g) == Status::InvalidConfiguration);
  REG(SEECFG) = 2;
  CHECK(hardware.geometry(g) == Status::InvalidConfiguration);
  REG(SEECFG) = 0;
  REG(SEESTAT) |= 8;
  CHECK(hardware.geometry(g) == Status::InvalidConfiguration);
  REG(SEESTAT) &= ~8u;
  REG(SEESTAT) |= 4;
  CHECK(hardware.geometry(g) == Status::Busy);
  REG(SEESTAT) &= ~4u;
  CHECK(hardware.geometry(g) == Status::Ready && g.bytes == 8192);
  uint8_t bytes[4] = {1, 2, 3, 4}, readback[4];
  CHECK(!hardware.program(8192, bytes, 4));
  CHECK(!hardware.program(1, bytes, 4));
  CHECK(!hardware.program(0, bytes, 3));
  REG(INTFLAG).value = NVMCTRL_INTFLAG_SEEWRC_Msk;
  CHECK(hardware.program(8188, bytes, 4));
  CHECK(REG(INTFLAG).value == 0);
  CHECK(std::memcmp(virtualEEPROM + 8188, bytes, 4) == 0);
  CHECK(hardware.poll() == Status::Busy);
  CHECK(!hardware.readBytes(8188, readback, 4));
  REG(SEESTAT) |= 4;
  REG(INTFLAG).value = NVMCTRL_INTFLAG_SEEWRC_Msk;
  CHECK(hardware.poll() == Status::Busy);
  REG(SEESTAT) &= ~4u;
  CHECK(hardware.poll() == Status::Ready);
  CHECK(hardware.readBytes(8188, readback, 4));
  CHECK(std::memcmp(bytes, readback, 4) == 0);
  CHECK(hardware.program(0, bytes, 4));
  REG(INTFLAG).value = NVMCTRL_INTFLAG_SEESOVF_Msk;
  CHECK(hardware.poll() == Status::HardwareError);
  REG(INTFLAG).value = 0;
  CHECK(!hardware.readBytes(8192, readback, 4));
  std::puts("SmartEEPROM register geometry/fresh completion/error tests passed");
}
#endif
