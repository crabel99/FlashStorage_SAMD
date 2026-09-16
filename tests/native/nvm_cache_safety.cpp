#include "Arduino.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <vector>

SerialStub nativeSerial;
FakeNvm nativeNvm;
#if defined(TEST_LEGACY_SAMD51)
LegacyNvm legacyNvm;
#endif
constexpr uint32_t cache0 = 1u << 14;
constexpr uint32_t cache1 = 1u << 15;
constexpr uint32_t cacheMask = cache0 | cache1;

struct Entry {
    const char *kind;
    uint32_t value;
    uint32_t ctrla;
};
struct Trace {
    std::vector<Entry> entries;
    std::vector<std::vector<uint8_t>> committedBuffers;
    const uint8_t *destination = nullptr;
    size_t length = 0;
    bool open = false;
    bool unsafe = false;
    bool invalidOrder = false;
    bool ignoredAddress = false;
    unsigned pbcDelay = 0;
    unsigned wpDelay = 0;
    unsigned eraseDelay = 0;
    unsigned remaining = 0;
    uint32_t pending = 0;
    bool done = true;
    bool earlyBufferWrite = false;
    std::vector<uint8_t> beforeClear;

};
Trace *trace = nullptr;

void ControlA::operator=(uint32_t next) volatile {
    value = next;
    if (trace) {
        trace->entries.push_back({"CTRLA", next, next});
        if (trace->open && (next & cacheMask) != cacheMask)
            trace->unsafe = true;
    }
}
void checkPendingPageClear() {
    if (trace && trace->remaining && trace->pending == NVMCTRL_CTRLB_CMD_PBC)
        trace->earlyBufferWrite |= !std::equal(trace->beforeClear.begin(), trace->beforeClear.end(), trace->destination);
}
void advanceCommand() {
    if (!trace || !trace->remaining)
        return;
    checkPendingPageClear();
    if (--trace->remaining == 0) {
        trace->done = true;
        trace->entries.push_back({"COMPLETE", trace->pending, nativeNvm.NVMCTRL_CTRLA});
        if (trace->pending == NVMCTRL_CTRLB_CMD_WP)
            trace->open = false;
        trace->pending = 0;
    }
}
StatusRegister::operator uint32_t() const volatile {
    advanceCommand();
    return !trace || !trace->remaining ? NVMCTRL_STATUS_READY_Msk : 0u;
}
DoneRegister::operator uint32_t() const volatile {
    advanceCommand();
    return !trace || trace->done ? NVMCTRL_INTFLAG_DONE_Msk : 0u;
}
void DoneRegister::operator=(uint32_t value) volatile {
    if (trace && (value & NVMCTRL_INTFLAG_DONE_Msk))
        trace->done = false;
}
void AddressRegister::operator=(uint32_t next) volatile {
    if (trace) {
        trace->entries.push_back({"ADDR", next, nativeNvm.NVMCTRL_CTRLA});
        if (trace->remaining) {
            trace->ignoredAddress = true;
            return;
        }
    }
    value = next;
}
void ControlB::operator=(uint32_t next) volatile {
    value = next;
    if (!trace)
        return;
    const uint32_t ctrla = nativeNvm.NVMCTRL_CTRLA;
    trace->entries.push_back({"CTRLB", next, ctrla});
    checkPendingPageClear();
    if (trace->remaining) {
        trace->invalidOrder = true;
        trace->done = true;
        return;
    }
    const uint32_t command = next & 0x7fu;
    if (command == NVMCTRL_CTRLB_CMD_PBC) {
        trace->invalidOrder |= trace->open;
        trace->open = true;
        trace->unsafe |= (ctrla & cacheMask) != cacheMask;
        trace->remaining = trace->pbcDelay;
        trace->beforeClear.assign(trace->destination, trace->destination + trace->length);
    } else if (command == NVMCTRL_CTRLB_CMD_WP) {
        trace->invalidOrder |= !trace->open;
        trace->unsafe |= (ctrla & cacheMask) != cacheMask;
        trace->committedBuffers.emplace_back(trace->destination, trace->destination + trace->length);
        trace->remaining = trace->wpDelay;
        if (!trace->remaining)
            trace->open = false;
    }
    if (command == NVMCTRL_CTRLB_CMD_EB)
        trace->remaining = trace->eraseDelay;
    if (trace->remaining)
        trace->pending = command;
    else
        trace->done = true;
}

#include "FlashStorage_SAMD.h"

bool runWrite(const char *name, uint32_t initial, size_t count, unsigned pbcDelay = 0, unsigned wpDelay = 0, bool initialDone = true) {
    alignas(512) std::array<uint8_t, 2560> destination;
    std::array<uint8_t, 1040> source;
    destination.fill(0xa7);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<uint8_t>(i * 37u + 11u);
    const size_t rounded = (count + 3u) & ~size_t(3u);
    nativeNvm.NVMCTRL_CTRLA = initial;
    FlashClass flash(destination.data() + 512, static_cast<uint32_t>(count));
    Trace captured;
    captured.destination = destination.data() + 512;
    captured.length = rounded;
    captured.pbcDelay = pbcDelay;
    captured.wpDelay = wpDelay;
    captured.done = initialDone;
    trace = &captured;
    flash.write(source.data() + 1);
    trace = nullptr;

    const size_t pages = (rounded + 511u) / 512u;
    bool data = std::equal(source.begin() + 1, source.begin() + 1 + rounded, destination.begin() + 512);
    const bool guards = std::all_of(destination.begin(), destination.begin() + 512,
                                   [](uint8_t v) { return v == 0xa7; }) &&
                        std::all_of(destination.begin() + 512 + rounded, destination.end(),
                                    [](uint8_t v) { return v == 0xa7; });
    for (size_t page = 0; page < captured.committedBuffers.size(); ++page) {
        const size_t written = std::min(rounded, (page + 1u) * 512u);
        const auto &snapshot = captured.committedBuffers[page];
        data &= std::equal(source.begin() + 1, source.begin() + 1 + written, snapshot.begin());
        data &= std::all_of(snapshot.begin() + written, snapshot.end(), [](uint8_t v) { return v == 0xa7; });
    }
    const bool preserved = (static_cast<uint32_t>(nativeNvm.NVMCTRL_CTRLA) & ~NVMCTRL_CTRLA_WMODE_Msk) ==
                           (initial & ~NVMCTRL_CTRLA_WMODE_Msk);
    const bool manual = (static_cast<uint32_t>(nativeNvm.NVMCTRL_CTRLA) & NVMCTRL_CTRLA_WMODE_Msk) == 0;
    const bool commandOrder = captured.committedBuffers.size() == pages && !captured.open && !captured.invalidOrder;
    const bool complete = !captured.remaining && !captured.earlyBufferWrite;
    const bool safe = !captured.unsafe;
    const bool pass = data && guards && preserved && manual && commandOrder && safe && complete;
    std::printf("%s %s bytes=%zu initial=0x%04x data=%d guards=%d preserved=%d manual=%d pages=%zu/%zu safe_window=%d ready_return=%d early_store=%d\n",
                pass ? "PASS" : "FAIL", name, count, initial, data, guards, preserved, manual,
                captured.committedBuffers.size(), pages, safe, !captured.remaining, captured.earlyBufferWrite);
    if (!pass) {
        for (const auto &entry : captured.entries)
            std::printf("  %s value=0x%04x ctrla=0x%04x\n", entry.kind, entry.value, entry.ctrla);
    }
    return pass;
}

bool verifyObserver() {
    uint8_t destination = 0;
    Trace late;
    late.destination = &destination;
    late.length = 1;
    nativeNvm.NVMCTRL_CTRLA = 4u;
    trace = &late;
    nativeNvm.NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_PBC;
    nativeNvm.NVMCTRL_CTRLA = cacheMask | 4u;
    nativeNvm.NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_WP;
    trace = nullptr;

    Trace transient;
    transient.destination = &destination;
    transient.length = 1;
    trace = &transient;
    nativeNvm.NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_PBC;
    nativeNvm.NVMCTRL_CTRLA = 4u;
    nativeNvm.NVMCTRL_CTRLA = cacheMask | 4u;
    nativeNvm.NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_WP;
    trace = nullptr;
    Trace earlyRestore;
    earlyRestore.destination = &destination;
    earlyRestore.length = 1;
    earlyRestore.wpDelay = 4;
    trace = &earlyRestore;
    nativeNvm.NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_PBC;
    nativeNvm.NVMCTRL_CTRLB = NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_WP;
    nativeNvm.NVMCTRL_CTRLA = 4u;
    trace = nullptr;
    const bool pass = late.unsafe && transient.unsafe && earlyRestore.unsafe;
    std::printf("%s observer_rejects_late_disable_transient_enable_early_restore\n", pass ? "PASS" : "FAIL");
    return pass;
}

bool runErase(unsigned initialBusy = 0) {
    uint32_t destination = 0;
    FlashClass flash(&destination, sizeof(destination));
    Trace captured;
    captured.destination = reinterpret_cast<uint8_t *>(&destination);
    captured.eraseDelay = 4;
    captured.remaining = initialBusy;
    trace = &captured;
    flash.erase();
    trace = nullptr;
    const bool pass = !captured.remaining && !captured.invalidOrder && !captured.ignoredAddress && captured.entries.size() >= 2;
    std::printf("%s sticky_done_delayed_erase initial_busy=%u ready_return=%d ignored_address=%d\n", pass ? "PASS" : "FAIL", initialBusy, !captured.remaining, captured.ignoredAddress);
    return pass;
}

bool runStorageWrite() {
    using Settings = std::array<uint32_t, 4>;
    Settings source{0x31564953u, 0x00100003u, 0x0000003fu, 0xd096bbcbu};
    alignas(512) Settings destination{};
    FlashStorageClass<Settings> storage(destination.data());
    nativeNvm.NVMCTRL_CTRLA = 4u;
    Trace captured;
    captured.destination = reinterpret_cast<uint8_t *>(destination.data());
    captured.length = sizeof(destination);
    captured.eraseDelay = 4;
    captured.pbcDelay = 4;
    captured.wpDelay = 4;
    trace = &captured;
    storage.write(source);
    trace = nullptr;
    std::vector<uint32_t> commands;
    for (const auto &entry : captured.entries)
        if (std::strcmp(entry.kind, "CTRLB") == 0)
            commands.push_back(entry.value);
    const bool order = commands == std::vector<uint32_t>{
        NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_EB,
        NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_PBC,
        NVMCTRL_CTRLB_CMDEX_KEY | NVMCTRL_CTRLB_CMD_WP};
    const bool restored = static_cast<uint32_t>(nativeNvm.NVMCTRL_CTRLA) == 4u;
    const bool complete = !captured.remaining && !captured.open && !captured.invalidOrder &&
                          !captured.ignoredAddress && !captured.earlyBufferWrite;
    const bool pass = source == destination && order && restored && complete && !captured.unsafe;
    std::printf("%s storage_erase_then_write data=%d order=%d restored=%d complete=%d safe_window=%d\n",
                pass ? "PASS" : "FAIL", source == destination, order, restored, complete, !captured.unsafe);
    if (!pass)
        for (const auto &entry : captured.entries)
            std::printf("  %s value=0x%04x ctrla=0x%04x\n", entry.kind, entry.value, entry.ctrla);
    return pass;
}

int main() {
    unsigned failed = !verifyObserver();
    for (uint32_t disabled : {0u, cache0, cache1, cacheMask})
        failed += !runWrite("initial_cache_state", disabled | 4u, 16u);
    for (size_t count : {0u, 1u, 3u, 4u, 16u, 511u, 512u, 513u, 516u, 1024u, 1028u})
        failed += !runWrite("page_boundary", 0x34u, count);
    failed += !runWrite("sticky_done_delayed_page_write", cacheMask | 4u, 16u, 0u, 4u);
    failed += !runWrite("sticky_done_delayed_page_clear", cacheMask | 4u, 16u, 4u, 4u);
    failed += !runWrite("initially_clear_done_then_sticky", cacheMask | 4u, 516u, 4u, 4u, false);
    failed += !runErase();
    failed += !runErase(4u);
    failed += !runStorageWrite();
    failed += !runWrite("cache_enabled_delayed_page_write", 4u, 16u, 0u, 4u);
    std::printf("%u failed cases. This tests the documented unsafe NVM command sequence, not silicon HardFault execution.\n", failed);
    return failed ? 1 : 0;
}
