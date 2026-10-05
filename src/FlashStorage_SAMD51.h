/******************************************************************************************************************************************
  FlashStorage_SAMD51.h
  For SAMD21/SAMD51 using Flash emulated-EEPROM

  The FlashStorage_SAMD library aims to provide a convenient way to store and retrieve user's data using the non-volatile flash memory
  of SAMD21/SAMD51. It now supports writing and reading the whole object, not just byte-and-byte.

  Based on and modified from Cristian Maglie's FlashStorage (https://github.com/cmaglie/FlashStorage)

  Built by Khoi Hoang https://github.com/khoih-prog/FlashStorage_SAMD
  Licensed under LGPLv3 license

  Orginally written by Cristian Maglie

  Copyright (c) 2015 Arduino LLC.  All right reserved.
  Copyright (c) 2020 Khoi Hoang.

  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License
  as published bythe Free Software Foundation, either version 3 of the License, or (at your option) any later version.
  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more details.
  You should have received a copy of the GNU Lesser General Public License along with this library.
  If not, see (https://www.gnu.org/licenses/)

  Version: 1.3.2

  Version Modified By   Date        Comments
  ------- -----------  ----------   -----------
  1.0.0   K Hoang      28/03/2020  Initial coding to add support to SAMD51 besides SAMD21
  1.1.0   K Hoang      26/01/2021  Add supports to put() and get() for writing and reading the whole object. Fix bug.
  1.2.0   K Hoang      18/08/2021  Optimize code. Add debug option
  1.2.1   K Hoang      10/10/2021  Update `platform.ini` and `library.json`
  1.3.0   K Hoang      25/01/2022  Fix `multiple-definitions` linker error. Add support to many more boards.
  1.3.1   K Hoang      25/01/2022  Reduce number of library files
  1.3.2   K Hoang      26/01/2022  Make compatible with old libraries and codes
 ******************************************************************************************************************************************/

// The .hpp contains only definitions, and can be included as many times as necessary, without `Multiple Definitions` Linker Error
// The .h contains implementations, and can be included only in main(), .ino with setup() to avoid `Multiple Definitions` Linker Error

#pragma once


#ifndef FlashStorage_SAMD51_h
#define FlashStorage_SAMD51_h

#ifndef BOARD_NAME
  #define BOARD_NAME    "Unknown SAMD51 board"
#endif

static const uint32_t pageSizes[] = { 8, 16, 32, 64, 128, 256, 512, 1024 };

/////////////////////////////////////////////////////

#if defined(NVMCTRL_REGS)
static inline uint32_t flash_nvmctrl_page_size_index()
{
  return (NVMCTRL_REGS->NVMCTRL_PARAM & NVMCTRL_PARAM_PSZ_Msk) >>
         NVMCTRL_PARAM_PSZ_Pos;
}

static inline uint32_t flash_nvmctrl_pages()
{
  return (NVMCTRL_REGS->NVMCTRL_PARAM & NVMCTRL_PARAM_NVMP_Msk) >>
         NVMCTRL_PARAM_NVMP_Pos;
}

static inline void flash_nvmctrl_set_manual_write()
{
  NVMCTRL_REGS->NVMCTRL_CTRLA =
      (NVMCTRL_REGS->NVMCTRL_CTRLA & ~NVMCTRL_CTRLA_WMODE_Msk) |
      NVMCTRL_CTRLA_WMODE_MAN;
}

static inline bool flash_nvmctrl_ready()
{
  return (NVMCTRL_REGS->NVMCTRL_STATUS & NVMCTRL_STATUS_READY_Msk) != 0;
}

static inline bool flash_nvmctrl_done()
{
  return (NVMCTRL_REGS->NVMCTRL_INTFLAG & NVMCTRL_INTFLAG_DONE_Msk) != 0;
}

static inline uint32_t flash_nvmctrl_disable_cache()
{
  const uint32_t control = NVMCTRL_REGS->NVMCTRL_CTRLA;
  NVMCTRL_REGS->NVMCTRL_CTRLA = control |
      NVMCTRL_CTRLA_CACHEDIS0_Msk | NVMCTRL_CTRLA_CACHEDIS1_Msk;
  return control;
}

static inline void flash_nvmctrl_restore_control(uint32_t control)
{
  NVMCTRL_REGS->NVMCTRL_CTRLA = control;
}

static inline void flash_nvmctrl_command(uint16_t command)
{
  NVMCTRL_REGS->NVMCTRL_INTFLAG = NVMCTRL_INTFLAG_DONE_Msk;
  NVMCTRL_REGS->NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMDEX_KEY | command;
}

static inline void flash_nvmctrl_set_address(const volatile void *flash_ptr)
{
  NVMCTRL_REGS->NVMCTRL_ADDR = (uint32_t)flash_ptr;
}
#else
static inline uint32_t flash_nvmctrl_page_size_index()
{
  return NVMCTRL->PARAM.bit.PSZ;
}

static inline uint32_t flash_nvmctrl_pages()
{
  return NVMCTRL->PARAM.bit.NVMP;
}

static inline void flash_nvmctrl_set_manual_write()
{
  NVMCTRL->CTRLA.bit.WMODE = 0;
}

static inline bool flash_nvmctrl_ready()
{
  return NVMCTRL->STATUS.bit.READY == NVMCTRL_STATUS_READY;
}

static inline bool flash_nvmctrl_done()
{
  return NVMCTRL->INTFLAG.bit.DONE != 0;
}

static inline uint32_t flash_nvmctrl_disable_cache()
{
  const uint32_t control = NVMCTRL->CTRLA.reg;
  NVMCTRL->CTRLA.reg = control |
      NVMCTRL_CTRLA_CACHEDIS0 | NVMCTRL_CTRLA_CACHEDIS1;
  return control;
}

static inline void flash_nvmctrl_restore_control(uint32_t control)
{
  NVMCTRL->CTRLA.reg = control;
}

static inline void flash_nvmctrl_command(uint16_t command)
{
  NVMCTRL->INTFLAG.reg = 1u; // DONE is write-one-to-clear; preserve other flags.
  NVMCTRL->CTRLB.reg = NVMCTRL_CTRLB_CMDEX_KEY | command;
}

static inline void flash_nvmctrl_set_address(const volatile void *flash_ptr)
{
  NVMCTRL->ADDR.reg = (uint32_t)flash_ptr;
}
#endif

/////////////////////////////////////////////////////

FlashClass::FlashClass(const void *flash_addr, uint32_t size) :
  PAGE_SIZE(pageSizes[flash_nvmctrl_page_size_index()]),
  PAGES(flash_nvmctrl_pages()),
  MAX_FLASH(PAGE_SIZE * PAGES),
  ROW_SIZE(8192),
  flash_address((volatile void *)flash_addr),
  flash_size(size)
{
}

/////////////////////////////////////////////////////

static inline uint32_t read_unaligned_uint32(const void *data)
{
  union
  {
    uint32_t u32;
    uint8_t u8[4];
  } res;
  const uint8_t *d = (const uint8_t *)data;
  res.u8[0] = d[0];
  res.u8[1] = d[1];
  res.u8[2] = d[2];
  res.u8[3] = d[3];
  return res.u32;
}

/////////////////////////////////////////////////////

void FlashClass::write(const volatile void *flash_ptr, const void *data)
{
  uint32_t size = (flash_size + 3) / 4;
  volatile uint32_t *dst_addr = (volatile uint32_t *)flash_ptr;
  const uint8_t *src_addr = (const uint8_t *)data;

  while (!flash_nvmctrl_ready()) { }
  flash_nvmctrl_set_manual_write();

  // DS80000748 section 2.14.1: disable both caches before filling the page buffer.
  const uint32_t control = flash_nvmctrl_disable_cache();
  while (size)
  {
    flash_nvmctrl_command(NVMCTRL_CTRLB_CMD_PBC);
    while (!flash_nvmctrl_ready() || !flash_nvmctrl_done()) { }

    for (uint32_t i = 0; i < (PAGE_SIZE / 4) && size; i++)
    {
      *dst_addr++ = read_unaligned_uint32(src_addr);
      src_addr += 4;
      size--;
    }

    flash_nvmctrl_command(NVMCTRL_CTRLB_CMD_WP);
    while (!flash_nvmctrl_ready() || !flash_nvmctrl_done()) { }
  }
  flash_nvmctrl_restore_control(control);
}

/////////////////////////////////////////////////////

void FlashClass::read(const volatile void *flash_ptr, void *data)
{
  FLASH_LOGERROR3(F("MAX_FLASH (KB) = "), MAX_FLASH / 1024, F(", ROW_SIZE ="), ROW_SIZE);
  FLASH_LOGERROR1(F("FlashStorage size = "), flash_size);
  FLASH_LOGERROR0(F("FlashStorage Start Address: 0x"));
  FLASH_HEXLOGERROR0((uint32_t ) flash_address);

  FLASH_LOGDEBUG0(F("Read: flash_ptr = 0x"));
  FLASH_HEXLOGDEBUG0((uint32_t ) flash_ptr);
  FLASH_LOGDEBUG0(F("data = 0x"));
  FLASH_HEXLOGDEBUG0(* (uint32_t *) data);

  memcpy(data, (const void *)flash_ptr, flash_size);
}

/////////////////////////////////////////////////////

void FlashClass::erase(const volatile void *flash_ptr, uint32_t size)
{
  const uint8_t *ptr = (const uint8_t *)flash_ptr;

  while (size)
  {
    erase(ptr);
    ptr += ROW_SIZE;
    size = size > ROW_SIZE ? size - ROW_SIZE : 0;
  }
}

/////////////////////////////////////////////////////

void FlashClass::erase(const volatile void *flash_ptr)
{
  while (!flash_nvmctrl_ready()) { }
  flash_nvmctrl_set_address(flash_ptr);
  flash_nvmctrl_command(NVMCTRL_CTRLB_CMD_EB);
  while (!flash_nvmctrl_ready() || !flash_nvmctrl_done()) { }
}

/////////////////////////////////////////////////////

#endif      //#ifndef FlashStorage_SAMD51_h
