#include "ReservedEEPROMCore.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include <type_traits>

static_assert(!std::is_copy_constructible<ReservedEEPROMCore>::value, "A store cannot copy its backend reference");
static_assert(!std::is_move_constructible<ReservedEEPROMCore>::value, "A store cannot move its backend reference");

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
using Status = ReservedEEPROMStatus;
struct Cut {};

class Memory : public ReservedEEPROMBackend {
public:
  explicit Memory(bool smart = false) : data(8192, 0xff), smart(smart), rowPrograms(32, 0) {}
  std::vector<uint8_t> data;
  bool smart;
  std::vector<unsigned> rowPrograms;
  unsigned programs = 0, erases = 0, operations = 0, delay = 0;
  int cutOperation = -1;
  unsigned prefix = 0;
  Status configuration = Status::Ready;
  bool error = false;
  bool readError = false;

  Status geometry(ReservedEEPROMGeometry& g) override {
    g = {uint32_t(data.size()), uint16_t(smart ? 4 : 64), uint16_t(smart ? 0 : 256), smart};
    return configuration;
  }
  bool readBytes(uint32_t offset, uint8_t* out, uint32_t count) override {
    CHECK(!delay);
    if (readError) return false;
    CHECK(offset <= data.size() && count <= data.size() - offset);
    std::copy(data.begin() + offset, data.begin() + offset + count, out);
    return true;
  }
  bool erase(uint32_t offset) override {
    CHECK(!smart && !delay && offset % 256 == 0 && offset + 256 <= data.size());
    ++erases;
    const bool cut = int(operations++) == cutOperation;
    std::fill(data.begin() + offset, data.begin() + offset + (cut ? std::min(prefix, 256u) : 256), 0xff);
    if (cut) throw Cut();
    rowPrograms[offset / 256] = 0;
    delay = 2;
    return true;
  }
  bool program(uint32_t offset, const uint8_t* src, uint32_t count) override {
    CHECK(!delay && offset + count <= data.size());
    CHECK(smart ? count == 4 && offset % 4 == 0 : count == 64 && offset % 64 == 0);
    ++programs;
    const bool cut = int(operations++) == cutOperation;
    const unsigned n = cut ? std::min(prefix, count) : count;
    for (unsigned i = 0; i < n; ++i) {
      if (!smart) CHECK((data[offset + i] & src[i]) == src[i]);
      data[offset + i] = src[i];
    }
    if (!smart) CHECK(++rowPrograms[offset / 256] <= 4);
    if (cut) throw Cut();
    delay = 2;
    return true;
  }
  Status poll() override {
    CHECK(delay);
    if (--delay) return Status::Busy;
    return error ? Status::HardwareError : Status::Ready;
  }
};

static void finish(ReservedEEPROMCore& store) {
  unsigned calls = 0;
  while (store.busy()) { CHECK(++calls < 100000); store.service(); }
  CHECK(store.status() == Status::Ready);
}
struct Callback { unsigned calls = 0; Status result = Status::Busy; };
static void completed(void* context, Status result) {
  Callback& state = *static_cast<Callback*>(context);
  ++state.calls;
  state.result = result;
}
static void fill(ReservedEEPROMCore& store, uint8_t seed) {
  for (uint32_t i = 0; i < store.length(); ++i) CHECK(store.update(i, uint8_t(seed + i)));
}
static bool pattern(ReservedEEPROMCore& store, uint8_t seed) {
  for (uint32_t i = 0; i < store.length(); ++i)
    if (store.read(i) != uint8_t(seed + i)) return false;
  return true;
}
static void commit(ReservedEEPROMCore& store) { CHECK(store.commitAsync()); finish(store); }

static void bounds() {
  Memory mem;
  uint8_t ram[128];
  ReservedEEPROMCore store(mem);
  CHECK(store.begin(1, 4096, ram, sizeof(ram)) == Status::InvalidConfiguration);
  CHECK(store.begin(0, 256, ram, sizeof(ram)) == Status::InvalidConfiguration);
  CHECK(store.begin(8193, 0, ram, sizeof(ram)) == Status::InvalidConfiguration);
  CHECK(store.begin(8192, 256, ram, sizeof(ram)) == Status::OutOfRange);
  CHECK(store.begin(0, 8192, ram, UINT32_MAX) == Status::OutOfRange);
  CHECK(store.begin(256, 4096, ram, sizeof(ram)) == Status::Ready);
  CHECK(!store.valid() && store.length() == sizeof(ram));
  CHECK(!store.update(128, 0));
  uint32_t value = 0x12345678, unchanged = value;
  CHECK(!store.put(UINT32_MAX, value));
  CHECK(!store.put(126, value));
  CHECK(!store.get(126, unchanged) && unchanged == value);
  CHECK(store.put(124, value));
  value = 0;
  CHECK(store.get(124, value) && value == 0x12345678);
  CHECK(store.commitAsync());
  CHECK(!store.put(0, value) && !store.write(0, 1));
  CHECK(!store.commitAsync());
  CHECK(store.begin(256, 4096, ram, sizeof(ram)) == Status::Busy);
  finish(store);
  CHECK(std::all_of(mem.data.begin(), mem.data.begin() + 256, [](uint8_t v) { return v == 0xff; }));
  CHECK(std::all_of(mem.data.begin() + 4352, mem.data.end(), [](uint8_t v) { return v == 0xff; }));
}

static void d21Persistence() {
  Memory mem;
  uint8_t ram[1025];
  ReservedEEPROMCore store(mem);
  CHECK(store.begin(0, 0, ram, sizeof(ram)) == Status::Ready);
  for (unsigned generation = 0; generation < 18; ++generation) {
    fill(store, uint8_t(generation));
    Callback callback;
    CHECK(store.commitAsync(completed, &callback));
    CHECK(callback.calls == 0);
    finish(store);
    CHECK(callback.calls == 1 && callback.result == Status::Ready);
    CHECK(store.valid());
    uint8_t readback[1025];
    ReservedEEPROMCore reboot(mem);
    CHECK(reboot.begin(0, 0, readback, sizeof(readback)) == Status::Ready);
    CHECK(reboot.valid() && pattern(reboot, uint8_t(generation)));
    const unsigned writes = mem.operations;
    commit(store);
    CHECK(mem.operations == writes);
  }
}

static void d21PowerCuts() {
  unsigned cuts = 0;
  for (unsigned previous = 0; previous < 4; ++previous) {
    Memory base;
    uint8_t ram[129];
    ReservedEEPROMCore initial(base);
    CHECK(initial.begin(256, 512, ram, sizeof(ram)) == Status::Ready);
    for (unsigned generation = 0; generation < previous; ++generation) {
      fill(initial, uint8_t(generation + 1));
      commit(initial);
    }
    const unsigned start = base.operations;
    Memory complete = base;
    uint8_t working[129];
    ReservedEEPROMCore full(complete);
    CHECK(full.begin(256, 512, working, sizeof(working)) == Status::Ready);
    fill(full, 99);
    commit(full);
    const unsigned count = complete.operations - start;
    for (unsigned operation = 0; operation < count; ++operation) {
      for (unsigned prefix = 0; prefix <= 256; ++prefix) {
        Memory mem = base;
        mem.cutOperation = int(start + operation);
        mem.prefix = prefix;
        uint8_t stage[129];
        ReservedEEPROMCore interrupted(mem);
        CHECK(interrupted.begin(256, 512, stage, sizeof(stage)) == Status::Ready);
        fill(interrupted, 99);
        try { commit(interrupted); CHECK(false); } catch (const Cut&) {}
        mem.delay = 0;
        mem.cutOperation = -1;
        uint8_t recovered[129];
        ReservedEEPROMCore reboot(mem);
        CHECK(reboot.begin(256, 512, recovered, sizeof(recovered)) == Status::Ready);
        if (previous) CHECK(reboot.valid() && (pattern(reboot, uint8_t(previous)) || pattern(reboot, 99)));
        else CHECK(!reboot.valid() || pattern(reboot, 99));
        fill(reboot, 100);
        commit(reboot);
        CHECK(reboot.valid() && pattern(reboot, 100));
        ++cuts;
      }
    }
  }
  std::printf("D21 power-cut prefixes checked: %u\n", cuts);
}

static void corruption() {
  Memory mem;
  uint8_t ram[128];
  ReservedEEPROMCore store(mem);
  CHECK(store.begin(0, 512, ram, sizeof(ram)) == Status::Ready);
  fill(store, 1); commit(store);
  fill(store, 2); commit(store);
  const std::vector<uint8_t> good = mem.data;
  for (unsigned byte = 256; byte < 256 + 20; ++byte) {
    mem.data = good;
    mem.data[byte] ^= 1;
    ReservedEEPROMCore reboot(mem);
    CHECK(reboot.begin(0, 512, ram, sizeof(ram)) == Status::Ready);
    CHECK(reboot.valid() && pattern(reboot, 1));
  }
  mem.data = good;
  mem.data[256 + 64 + 127] ^= 1;
  ReservedEEPROMCore reboot(mem);
  CHECK(reboot.begin(0, 512, ram, sizeof(ram)) == Status::Ready);
  CHECK(reboot.valid() && pattern(reboot, 1));
}


static void sequenceWrapAndReadFailure() {
  Memory mem;
  uint8_t ram[128];
  ReservedEEPROMCore store(mem);
  CHECK(store.begin(0, 512, ram, sizeof(ram)) == Status::Ready);
  fill(store, 1); commit(store);
  fill(store, 2); commit(store);
  // Change the first valid record's sequence to UINT32_MAX and reseal its header.
  for (unsigned i = 4; i < 8; ++i) mem.data[i] = 0xff;
  uint32_t crc = 0xffffffffu;
  for (unsigned i = 0; i < 16; ++i) {
    crc ^= mem.data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  crc ^= 0xffffffffu;
  for (unsigned i = 0; i < 4; ++i) mem.data[16 + i] = uint8_t(crc >> (8 * i));
  // Remove the second record. The next real commit must wrap the sequence to zero.
  std::fill(mem.data.begin() + 256, mem.data.begin() + 512, 0xff);
  mem.rowPrograms[1] = 0;
  ReservedEEPROMCore beforeWrap(mem);
  CHECK(beforeWrap.begin(0, 512, ram, sizeof(ram)) == Status::Ready);
  CHECK(beforeWrap.valid() && pattern(beforeWrap, 1));
  fill(beforeWrap, 3); commit(beforeWrap);
  ReservedEEPROMCore afterWrap(mem);
  CHECK(afterWrap.begin(0, 512, ram, sizeof(ram)) == Status::Ready);
  CHECK(afterWrap.valid() && pattern(afterWrap, 3));
  mem.readError = true;
  ReservedEEPROMCore failed(mem);
  CHECK(failed.begin(0, 512, ram, sizeof(ram)) == Status::HardwareError);
  CHECK(!failed.valid() && failed.length() == 0 && !failed.commitAsync());
  mem.readError = false;
  CHECK(failed.begin(0, 512, ram, sizeof(ram)) == Status::Ready);
  mem.readError = true;
  CHECK(failed.commitAsync());
  failed.service();
  CHECK(!failed.busy() && failed.status() == Status::HardwareError);
}

static void smartEEPROM() {
  Memory mem(true);
  uint8_t ram[128];
  ReservedEEPROMCore store(mem);
  mem.configuration = Status::Unconfigured;
  CHECK(store.begin(0, 0, ram, sizeof(ram)) == Status::Unconfigured);
  CHECK(!store.commitAsync());
  mem.configuration = Status::Ready;
  CHECK(store.begin(0, 0, ram, 127) == Status::InvalidConfiguration);
  CHECK(store.begin(4096, 4096, ram, sizeof(ram)) == Status::Ready);
  CHECK(store.valid());
  commit(store);
  CHECK(mem.programs == 0);
  CHECK(store.write(0, 1) && store.write(1, 2) && store.write(127, 3));
  Callback callback;
  CHECK(store.commitAsync(completed, &callback));
  while (store.busy()) {
    if (mem.delay) CHECK(callback.calls == 0);
    store.service();
  }
  CHECK(callback.calls == 1 && callback.result == Status::Ready);
  CHECK(mem.programs == 2 && mem.erases == 0);
  CHECK(mem.data[4096] == 1 && mem.data[4097] == 2 && mem.data[4223] == 3);
  CHECK(mem.data[4095] == 0xff && mem.data[4224] == 0xff);
  CHECK(store.write(4, 12));
  mem.error = true;
  CHECK(store.commitAsync(completed, &callback));
  while (store.busy()) store.service();
  CHECK(store.status() == Status::HardwareError && callback.calls == 2 && callback.result == Status::HardwareError);
  mem.error = false;
  commit(store);

  Memory partial(true);
  ReservedEEPROMCore first(partial);
  CHECK(first.begin(0, 0, ram, sizeof(ram)) == Status::Ready);
  fill(first, 8);
  partial.cutOperation = 1;
  partial.prefix = 1;
  try { commit(first); CHECK(false); } catch (const Cut&) {}
  partial.delay = 0;
  ReservedEEPROMCore restart(partial);
  CHECK(restart.begin(0, 0, ram, sizeof(ram)) == Status::Ready);
  CHECK(restart.read(0) == 8 && restart.read(4) == 12 && restart.read(5) == 0xff);
}

int main() {
  bounds();
  d21Persistence();
  d21PowerCuts();
  corruption();
  sequenceWrapAndReadFailure();
  smartEEPROM();
  std::puts("Reserved EEPROM native tests passed");
}
