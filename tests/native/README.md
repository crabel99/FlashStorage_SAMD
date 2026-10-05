# SAME5x NVM cache safety regression

Run from the repository root with GNU C++ 15 installed:

```sh
CXX=g++-15 ./tests/native/run.sh
```

The test includes the repository's actual `FlashStorage_SAMD.h` and executes `FlashClass::write`. The fake Arduino header supplies only host types and observed NVMCTRL registers. It does not replace the driver or copy its write algorithm.

Microchip SAM D5x/E5x errata DS80000748 section 2.14.1 requires both NVM cache lines disabled while filling the page buffer when execution or data reads use NVM. Revisions A and D are affected. [Microchip errata](https://ww1.microchip.com/downloads/aemDocuments/documents/MCU32/ProductDocuments/Errata/SAM-D5x-E5x-Family-Silicon-Errata-and-Data-Sheet-Clarification-DS80000748.pdf).

The runner compiles both the SAME54 NVMCTRL_REGS API and the legacy SAMD51 bitfield API. Each uses its actual cache macro names.

The observer records every CTRLA write and every page-buffer-clear/write-page command. It rejects an enabled cache at the start of that interval or anywhere within it. Observer self-checks reject disabling the cache too late and temporarily enabling it before disabling it again. It also rejects cache restoration before delayed page programming completes. A check of the final command alone would miss both cases.

Cases cover both initial cache lines enabled, each line individually disabled, and both disabled. They also verify restoration of the initial cache state, preservation of other control bits, existing manual-write selection, unaligned source reads, destination guards, and zero/partial/exact/multiple-page lengths. Page sizes are 512 bytes, matching SAME54. Non-word-aligned lengths retain the driver's existing rounding to four-byte stores; source and destination allocations include that padding.

This is a regression for the documented unsafe command sequence. It does not emulate flash instruction corruption or reproduce a silicon HardFault on the host CPU. The fake peripheral models delayed command completion, READY becoming zero while busy, and sticky DONE cleared only by writing one. Register reads advance a deterministic command clock. Commands and ADDR writes while busy are ignored and recorded. Erase commands are observed, but physical erase, interrupt interleaving, silicon bus stalls, and flash endurance are not modeled. Original hardware reproduction is still required before claiming a device-fault fix.

The driver contains existing pointer-to-32-bit casts in its address helper and disabled debug expressions. GNU `-fpermissive` permits those on a 64-bit host without editing the source. Actual destination writes retain the full host pointer; the fake records erase/address commands without dereferencing the truncated address.

The completion cases follow Microchip DS60001507M sections 25.6.6 and 25.8.6. They cover sticky DONE with delayed page clear, page write, and erase; initially clear DONE becoming sticky after the first command; busy entry with an ignored ADDR write; and the real `FlashStorageClass::write` erase-then-write path. A combined cache-enabled/delayed-write case checks that cache restoration waits for programming completion.

Baseline commit `2a9fcf55549430a2425b1e361e5c89d0e9fad859` fails 20 of 23 cases in each API branch. The original cache-only matrix fails 13 cases while data, guards, cache-state preservation, manual-mode and page counts pass. Seven additional completion or combined cases expose premature return, ignored commands/address writes, or an unsafe cache interval. Both-caches-disabled and zero-length immediate-completion controls pass, as does the observer self-check.

Evidence is preserved in `evidence/baseline-failure.log` for the initial cache matrix and `evidence/both-apis-baseline-failure.log` for the final expanded baseline. No production source change preceded either recording.
