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

`begin` and `beginAtomicSnapshots` read or scan existing storage synchronously. It does not erase or program. If the peripheral is busy at entry, it returns `Busy`; call begin again later. Service advances one command, completion check, or bounded comparison step; it does not spin waiting for hardware. Snapshot verification and CRC calculation are bounded by the configured capacity. **D21 instruction fetches from flash can still stall while flash is erased/programmed.** An asynchronous API does not provide read-while-write hardware.

## D21 durability and capacity

The partition offset and size must be multiples of 256 bytes. Records contain a dedicated 64-byte metadata page followed by the complete logical payload, rounded up to a 256-byte row boundary. At least two records must fit. For example, 1024 logical bytes use 1280 bytes per record, so an 8 KiB partition holds six records. The 8 KiB reservation is not 8 KiB of logical EEPROM.

Each commit reuses the next record, erases its rows, writes the payload, then writes metadata last. The metadata includes a format marker, sequence, logical length, payload CRC-32, and header CRC-32. On startup the newest valid record wins. An interrupted first commit yields either an empty store or the new record. With an existing valid record, interrupted commits preserve the previous record until the replacement is complete. CRC detects accidental corruption; it is not authentication.

Each physical page is programmed once after erase, at most four page programs per row. This stays below the D21 limit of eight consecutive writes per row. Unchanged data does not erase or program. `valid()` means a matching, checksum-valid snapshot has been recovered or committed. With no valid snapshot, the working buffer starts at 0xFF. Keep partition layout, logical capacity, and record format stable across firmware versions; changing them is a data migration, not an automatic reinterpretation.

## Retiring older D21 snapshots

`retirePreviousAsync(callback, context)` verifies the active snapshot, then erases and verifies the header row of every other snapshot. It leaves the active record and other partitions untouched. Completion uses the same `service()`, `busy()`, status, and callback contract as `commitAsync`. Unopened, empty, busy, and default in-place SmartEEPROM stores reject the request. SmartEEPROM stores opened with `beginAtomicSnapshots` use logical marker retirement as described below. Already blank header rows do not incur another erase.

Use this operation when old metadata must never become authoritative again. For example, commit revocation of an application's trusted-image status, complete retirement, and only then permit that image to be overwritten. Repeat retirement before granting that permission after a reset, including when the revocation record itself is unchanged. A failed or interrupted retirement never grants permission to overwrite the image.

Retirement temporarily leaves one valid snapshot. If that record later becomes corrupt, reopening returns no valid snapshot instead of resurrecting older metadata. Ordinary commits still rotate through the partition and retain previous snapshots. Retirement does not erase an application image or define its boot policy.

## Default SmartEEPROM durability

Partition offset, partition size, and logical capacity must be multiples of four bytes. Logical capacity must fit within the configured virtual region. Service compares the working buffer with hardware and programs only changed 32-bit words. Completion requires a fresh write-completed flag, no hardware error/overflow, and the controller no longer busy. Hardware manages physical wear leveling.

**A multiword commit is not atomic.** Power loss can leave some words updated and others unchanged. Use `beginAtomicSnapshots` for boot metadata or other data that must change atomically. This library supplies EEPROM storage, not a boot policy. `valid()` on SmartEEPROM means the configured virtual storage opened successfully; erased 0xFF bytes are valid EEPROM contents. It is not a checksum assertion about application data.

Unbuffered mode avoids the restrictions in E5x errata section 2.14.2. Buffered mode requires strictly linear writes and no reads of the page being modified, including debugger reads. The library rejects it rather than silently changing a controller mode that might affect existing data.

## Atomic SmartEEPROM snapshots

`beginAtomicSnapshots(offset, partitionBytes, workingBuffer, capacity)` explicitly opens a snapshot partition. Its arguments, buffer ownership, and asynchronous commit contract match `begin`. On D21 it uses the existing record format and behavior. On SmartEEPROM it uses a separate layout from the default in-place store. Changing between these layouts requires an application data migration. `atomicSnapshotsEnabled()` is true after a successful open on D21 or in this opt-in SmartEEPROM mode. It is false before initialization, after a failed open, and for default in-place SmartEEPROM. Callers can use it to reject stores that do not provide atomic records.

SmartEEPROM snapshot offsets, partition sizes, and capacities are multiples of four bytes. Each record has a 20-byte header followed by the complete payload. At least two records must fit. A 640-byte payload uses 660 bytes per record, so two records require 1,320 logical bytes. A 4 KiB partition holds six such records and leaves 136 bytes unused. All operations stay inside the partition. No heap allocation or physical SmartEEPROM erase is used.

The five little-endian header words are the `REP1` format marker, wrapping sequence number, payload length, payload CRC-32, and CRC-32 of the first four header words. Startup checks both CRCs and the exact payload length, then selects the newest valid sequence. With no valid snapshot, `valid()` is false and the buffer starts at `0xFF`.

A changed commit rotates to the next record and completes these steps:

1. Write zero to the destination marker, wait for durable completion, and read back zero.
2. Write the payload and the remaining four header words through aligned 32-bit operations. Wait for each operation to complete.
3. Write the `REP1` marker last and wait for durable completion.
4. Read the complete record again. Require valid CRCs, the expected sequence, and an exact match to the requested payload before reporting `Ready`.

An unchanged commit performs no writes and revalidates the active record before reporting `Ready`. Submission errors, completion errors, and failed readback complete the callback exactly once with `HardwareError`. A failed completion does not authorize an application action, even if a fresh scan can recover the new record after reset.

`retirePreviousAsync` verifies the active record, then writes zero to every other nonblank, nonzero record marker. Each write completes durably and reads back as zero before retirement advances. Other header words, payloads, and partitions remain unchanged. Blank or already retired markers incur no write. An interrupted retirement can be repeated after a fresh scan.

For boot metadata, the caller commits its Writing or trust-revocation state, completes retirement, and only then grants permission to erase or reuse the application slot. Every resumed erase grant repeats retirement, including when committing the same state required no write. If the newest record later becomes corrupt, a completed retirement prevents fallback to an older trusted identity. The library does not supply the boot-state policy or grant erase permission itself.

The fault model assumes each completed unbuffered write remains durable and an interrupted write affects only the addressed logical word. Native tests cover partial words, including partial commit and retirement markers. They do not establish behavior under corruption of the SmartEEPROM controller's physical remapping metadata. Physical power-cut validation on E54 remains required. The existing hardware backend owns unbuffered completion and automatic reallocation. This mode neither erases backing flash nor changes controller configuration.

## Verification

```sh
sh tests/reserved/run.sh
sh tests/reserved/compile.sh
bash tests/native/run.sh
```

The native runner uses a compiler with address/undefined-behavior sanitizers (`c++` by default, `CXX` override). It exercises the actual portable engine, including 5,140 D21 interrupted erase/program prefixes and 771 retirement erase prefixes, circular reuse, checksum corruption, bounds, no-change commits, callback completion, and default SmartEEPROM partial updates. Atomic SmartEEPROM tests use 640-byte payloads and cover 3,320 interrupted write prefixes, 15 retirement prefixes, ring reuse, every header and payload byte corrupted in the newest record, no-change commits, stale-record retirement, and submission, poll, and readback failures with exactly-once callback delivery. Register-adapter tests cover D21 configuration/erase/error restoration and both E5x register APIs, including fresh completion flags, busy states, capacity tables, configuration rejection, and overflow. The D21 native register test does not execute page-buffer stores to a real flash address.

The compile runner uses installed PlatformIO compiler/CMSIS packages, with `PLATFORMIO_PACKAGES_DIR` override. It compiles D21, D51, E53, and E54 against actual vendor headers. These checks do not prove silicon power-loss behavior, debugger reset behavior, or endurance. Hardware acceptance evidence belongs with the specific fixture and board configuration.

## E54 hardware acceptance

On September 28, 2026, an ATSAME54P20A connected through J-Link passed these checks:

- Disabled SmartEEPROM returned `Unconfigured` without writes.
- SBLK=2 and PSZ=4 opened the expected virtual region.
- A 128-byte asynchronous commit survived a reset and a fresh read.
- Out-of-range access and mutation during a pending commit were rejected.
- The legacy flash API wrote and read back 16 bytes with both NVM caches initially enabled.
- Thirty-two completed commits crossed seven observed active-sector changes. The final pattern survived reset.

The complete original 1 MiB flash and 512-byte User Row were restored and verified byte for byte. These were debugger reset tests, not physical power-cut tests. E53 and D51 have compilation and register-model coverage, not separate silicon acceptance.

The local fixture and evidence are under `SimIODevice/build/reserved-eeprom-hardware/`. The tested E54 firmware SHA-256 is `48888108c6a769c40022a5be897bc5ae90d978a98663a5302d2982c0a9ed32fe`.

## D21 hardware acceptance

On September 28, 2026, the SAMD21E18 trim wheel passed these checks through J-Link:

- Disabled EEPROM returned `Unconfigured`. An 8 KiB reservation opened after a masked User Row update that preserved every other bit.
- A 128-byte asynchronous commit survived reset and a fresh read. Bounds checks and rejection of mutations during a pending commit passed.
- A hardware breakpoint stopped a commit after both payload pages were written but before its validity header. Flash readback confirmed the new payload and erased header. Reset recovered the previous complete record.
- Forty completed commits wrapped the 32-record ring. All 32 retained records had valid header and payload CRCs and the expected payload. Reset selected the newest record.
- Flash between `0x2000` and `0x3E000` remained identical to the backup. The diagnostic image occupied the lower region; the EEPROM reservation occupied the upper region.

The complete original 256 KiB flash and 64-byte User Row were restored and verified byte for byte. The interruption test used a debugger reset between NVM commands, not a physical power cut during programming. Native tests cover interrupted erase/program prefixes.

The local fixture and evidence are under `SimIODevice/build/reserved-eeprom-hardware/d21/`. The tested D21 firmware SHA-256 is `edc218e20690d4129bed33007a02e2aadd8d0c31ded76317d509af7e79a405cd`.

## References

- [Microchip D21/DA1 datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/DataSheets/SAM-D21-DA1-Family-Data-Sheet-DS40001882.pdf): sections 22.6.4–22.6.7, EEPROM fuse table, and flash endurance note limiting consecutive row writes.
- [Microchip D5x/E5x datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/DataSheets/SAM-D5x-E5x-Family-Data-Sheet-DS60001507.pdf): sections 25.6.8–25.6.9, SmartEEPROM geometry, busy/write-completed/overflow flags, and fuse configuration.
- [Microchip D5x/E5x errata](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/Errata/SAM-D5x-E5x-Family-Silicon-Errata-and-Data-Sheet-Clarification-DS80000748.pdf): section 2.14.2, SmartEEPROM buffered mode restrictions.
