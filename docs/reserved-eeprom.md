# Reserved EEPROM storage

`ReservedEEPROM.h` adds an explicit storage object for the D21 EEPROM fuse region and D51/E51/E53/E54 SmartEEPROM. It does not include the legacy `FlashStorage_SAMD` header, allocate storage inside the application image, change fuses, unlock memory, or implement boot selection. The existing library API remains unchanged.

```cpp
#include <ReservedEEPROM.h>
ReservedEEPROMClass settings;
uint8_t working[128];

void setup() {
  if (settings.begin(0, 0, working, sizeof(working)) != ReservedEEPROMStatus::Ready)
    return;
  uint32_t value = 42;
  settings.put(0, value);
  settings.commitAsync();
}
void loop() { settings.service(); }
```

## Provisioning and placement

Provision fuses separately, preserve unrelated User Row data, and reset before use. The library only reads the resulting configuration. An unconfigured device returns `Unconfigured` and never falls back to ordinary application flash.

- **D21:** the `EEPROM` fuse selects the upper portion of main flash. Fuse value 1 reserves 8 KiB. This is software emulation, not the separate RWWEE region found on some other D21 variants. The library derives the address from the configured size and actual flash geometry. It rejects overlap with the boot-protected region.
- **E5x/D51:** SmartEEPROM's SBLK and PSZ fields select physical backing and logical capacity. SBLK=2, PSZ=4 reserves 32 KiB of physical flash and exposes 8 KiB of logical storage. Access uses the SmartEEPROM virtual address, not its backing main-flash address. Disabled, locked, buffered, or manually reallocated configurations are rejected. Unbuffered mode and automatic reallocation are required.

**The linker, bootloader, debugger load procedure, and application updater must exclude the backing flash.** This library cannot enforce an application's linker layout. D21 fuses designate the region; they do not keep application sections or an unrestricted programming tool out of it. Full chip erase can destroy persistent data. SmartEEPROM configuration changes can also invalidate existing data.

The normal reset clock configuration enables the necessary NVM clocks. Callers that explicitly gate those clocks must restore them before opening the store.

## API and lifetime

`begin(offset, partitionBytes, workingBuffer, capacity)` opens a partition relative to the configured EEPROM region. `partitionBytes=0` selects the remainder after offset. The caller owns `workingBuffer`, which must remain alive and unmodified outside the API until the object is no longer used. `capacity` is the logical byte count and the required working-buffer size. `length()` returns it.

`read`, `write`, `update`, `get`, and `put` access the RAM shadow. `write` and `update` return bool. `get` and `put` return bool and reject an entire out-of-range object before copying anything. `read` returns 0xFF outside the range or before successful initialization. No method allocates heap memory. Storage objects cannot be copied or moved.

`commitAsync(callback, context)` starts a commit and returns false if unopened or already busy. The optional callback has signature `void(void*, ReservedEEPROMStatus)` and runs from `service()`. No-op commits also complete through service. Call `service()` until `busy()` is false; require `status()==Ready` before resetting or treating the change as durable. Hardware failures complete with `HardwareError`. Writes and additional commits are rejected while busy. Reads continue to return the RAM shadow, including pending values.

Only one caller may own and service an object, and only one NVM operation may run on the MCU at a time. Do not call from interrupts or concurrently use other NVM writers, including legacy FlashStorage. Multiple nonoverlapping partitions are permitted when their operations are serialized by the caller. No interrupts or framework ownership mechanisms are installed.

`begin` reads/scans existing storage synchronously. It does not erase or program. If the peripheral is busy at entry, it returns `Busy`; call begin again later. Service advances one command, completion check, or bounded comparison step; it does not spin waiting for hardware. D21 snapshot verification and CRC calculation are bounded by the configured capacity. **D21 instruction fetches from flash can still stall while flash is erased/programmed.** An asynchronous API does not provide read-while-write hardware.

## D21 durability and capacity

The partition offset and size must be multiples of 256 bytes. Records contain a dedicated 64-byte metadata page followed by the complete logical payload, rounded up to a 256-byte row boundary. At least two records must fit. For example, 1024 logical bytes use 1280 bytes per record, so an 8 KiB partition holds six records. The 8 KiB reservation is not 8 KiB of logical EEPROM.

Each commit reuses the next record, erases its rows, writes the payload, then writes metadata last. The metadata includes a format marker, sequence, logical length, payload CRC-32, and header CRC-32. On startup the newest valid record wins. An interrupted first commit yields either an empty store or the new record. With an existing valid record, interrupted commits preserve the previous record until the replacement is complete. CRC detects accidental corruption; it is not authentication.

Each physical page is programmed once after erase, at most four page programs per row. This stays below the D21 limit of eight consecutive writes per row. Unchanged data does not erase or program. `valid()` means a matching, checksum-valid snapshot has been recovered or committed. With no valid snapshot, the working buffer starts at 0xFF. Keep partition layout, logical capacity, and record format stable across firmware versions; changing them is a data migration, not an automatic reinterpretation.

## SmartEEPROM durability

Partition offset, partition size, and logical capacity must be multiples of four bytes. Logical capacity must fit within the configured virtual region. Service compares the working buffer with hardware and programs only changed 32-bit words. Completion requires a fresh write-completed flag, no hardware error/overflow, and the controller no longer busy. Hardware manages physical wear leveling.

**A multiword commit is not atomic.** Power loss can leave some words updated and others unchanged. Use redundant application records with integrity checks for boot metadata or other data that must change atomically. This library supplies EEPROM storage, not a boot policy. `valid()` on SmartEEPROM means the configured virtual storage opened successfully; erased 0xFF bytes are valid EEPROM contents. It is not a checksum assertion about application data.

Unbuffered mode avoids the restrictions in E5x errata section 2.14.2. Buffered mode requires strictly linear writes and no reads of the page being modified, including debugger reads. The library rejects it rather than silently changing a controller mode that might affect existing data.

## Verification

```sh
sh tests/reserved/run.sh
sh tests/reserved/compile.sh
```

The native runner uses a compiler with address/undefined-behavior sanitizers (`c++` by default, `CXX` override). It exercises the actual portable engine, including 5,140 D21 interrupted erase/program prefixes, circular reuse, checksum corruption, bounds, no-change commits, callback completion, and SmartEEPROM partial updates. Register-adapter tests cover D21 configuration/erase/error restoration and both E5x register APIs, including fresh completion flags, busy states, capacity tables, configuration rejection, and overflow. The D21 native register test does not execute page-buffer stores to a real flash address.

The compile runner uses installed PlatformIO compiler/CMSIS packages, with `PLATFORMIO_PACKAGES_DIR` override. It compiles D21, D51, E53, and E54 against actual vendor headers. These checks do not prove silicon power-loss behavior, debugger reset behavior, or endurance. Hardware acceptance evidence belongs with the specific fixture and board configuration.

## E54 hardware acceptance

On September 28, 2026, an ATSAME54P20A connected through J-Link passed these checks:

- Disabled SmartEEPROM returned `Unconfigured` without writes.
- SBLK=2 and PSZ=4 opened the expected virtual region.
- A 128-byte asynchronous commit survived a reset and a fresh read.
- Out-of-range access and mutation during a pending commit were rejected.
- The legacy flash API wrote and read back 16 bytes with both NVM caches initially enabled.
- Thirty-two completed commits crossed seven observed active-sector changes. The final pattern survived reset.

The complete original 1 MiB flash and 512-byte User Row were restored and verified byte for byte. These were debugger reset tests, not physical power-cut tests. D21 has native interruption tests and a compiled hardware fixture; trim-wheel hardware acceptance remains pending. E53 and D51 have compilation and register-model coverage, not separate silicon acceptance.

The local fixture and evidence are under `SimIODevice/build/reserved-eeprom-hardware/`. The tested E54 firmware SHA-256 is `48888108c6a769c40022a5be897bc5ae90d978a98663a5302d2982c0a9ed32fe`.

## References

- [Microchip D21/DA1 datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/DataSheets/SAM-D21-DA1-Family-Data-Sheet-DS40001882.pdf): sections 22.6.4–22.6.7, EEPROM fuse table, and flash endurance note limiting consecutive row writes.
- [Microchip D5x/E5x datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/DataSheets/SAM-D5x-E5x-Family-Data-Sheet-DS60001507.pdf): sections 25.6.8–25.6.9, SmartEEPROM geometry, busy/write-completed/overflow flags, and fuse configuration.
- [Microchip D5x/E5x errata](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/Errata/SAM-D5x-E5x-Family-Silicon-Errata-and-Data-Sheet-Clarification-DS80000748.pdf): section 2.14.2, SmartEEPROM buffered mode restrictions.
