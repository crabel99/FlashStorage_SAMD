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
  int failProgramOperation = -1;
  int incompleteProgramOperation = -1;
  int pollErrorOperation = -1;
  unsigned prefix = 0;
  Status configuration = Status::Ready;
  bool error = false;
  bool readError = false;
  bool eraseError = false;
  bool incompleteErase = false;

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
    if (eraseError) return false;
    ++erases;
    const bool cut = int(operations++) == cutOperation;
    std::fill(data.begin() + offset, data.begin() + offset + (cut ? std::min(prefix, 256u) : 256), 0xff);
    if (cut) throw Cut();
    if (incompleteErase) data[offset + 255] = 0;
    rowPrograms[offset / 256] = 0;
    delay = 2;
    return true;
  }
  bool program(uint32_t offset, const uint8_t* src, uint32_t count) override {
    CHECK(!delay && offset + count <= data.size());
    CHECK(smart ? count == 4 && offset % 4 == 0 : count == 64 && offset % 64 == 0);
    if (int(operations) == failProgramOperation) return false;
    const bool incomplete = int(operations) == incompleteProgramOperation;
    ++programs;
    const bool cut = int(operations++) == cutOperation;
    const unsigned n = incomplete ? 0 : cut ? std::min(prefix, count) : count;
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
    return error || int(operations - 1) == pollErrorOperation ? Status::HardwareError : Status::Ready;
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

static void ringBoundsAndWrap() {
  for (bool smart : {false, true}) {
#if defined(__SAMD21__)
    if (smart) continue;
#endif
    for (unsigned records : {2u, 3u, 4u, 6u}) {
      Memory mem(smart);
      const unsigned start = 256, capacity = 64;
      const unsigned stride = smart ? capacity + 20 : 256;
      const unsigned length = records * stride;
      uint8_t ram[capacity];
      ReservedEEPROMCore store(mem);
      CHECK(store.beginAtomicSnapshots(start, length, ram, sizeof ram) == Status::Ready);
      for (unsigned generation = 1; generation <= records * 3; ++generation) {
        fill(store, uint8_t(generation)); commit(store);
        const unsigned target = start + ((generation - 1) % records) * stride;
        CHECK(mem.data[target] == 0x52 && mem.data[target + 4] == generation - 1);
        uint8_t reopenedRam[capacity]; ReservedEEPROMCore reopened(mem);
        CHECK(reopened.beginAtomicSnapshots(start, length, reopenedRam, sizeof reopenedRam) == Status::Ready);
        CHECK(reopened.valid() && pattern(reopened, uint8_t(generation)));
      }
      CHECK(std::all_of(mem.data.begin(), mem.data.begin() + start, [](uint8_t v) { return v == 0xff; }));
      CHECK(std::all_of(mem.data.begin() + start + length, mem.data.end(), [](uint8_t v) { return v == 0xff; }));
    }
  }
  struct WrongRow : Memory {
    uint16_t row = 128;
    Status geometry(ReservedEEPROMGeometry& g) override {
      Memory::geometry(g); g.rowBytes = row; return Status::Ready;
    }
  } wrong;
  uint8_t ram[64]; ReservedEEPROMCore bad(wrong);
  for (uint16_t row : {128, 512}) {
    wrong.row = row;
    CHECK(bad.begin(256, 1024, ram, sizeof ram) == Status::InvalidConfiguration);
  }
  Memory mem; ReservedEEPROMCore aligned(mem);
  for (unsigned offset : {255u, 257u})
    CHECK(aligned.begin(offset, 1024, ram, sizeof ram) == Status::InvalidConfiguration);
  for (unsigned length : {1023u, 1025u})
    CHECK(aligned.begin(256, length, ram, sizeof ram) == Status::InvalidConfiguration);
  CHECK(!mem.operations && !wrong.operations);
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
  CHECK(store.valid() && !store.atomicSnapshotsEnabled());
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

static Memory retirementSeed() {
  Memory mem;
  uint8_t ram[640];
  ReservedEEPROMCore store(mem);
  CHECK(store.begin(256, 3072, ram, sizeof(ram)) == Status::Ready);
  for (unsigned generation = 1; generation <= 10; ++generation) {
    fill(store, uint8_t(generation));
    commit(store);
  }
  return mem;
}

static void retirement() {
  Memory mem = retirementSeed();
  const std::vector<uint8_t> before = mem.data;
  const unsigned start = mem.operations;
  uint8_t ram[640];
  ReservedEEPROMCore store(mem);
  CHECK(store.begin(256, 3072, ram, sizeof(ram)) == Status::Ready);
  Callback callback;
  CHECK(store.retirePreviousAsync(completed, &callback));
  CHECK(callback.calls == 0 && store.status() == Status::Busy);
  CHECK(!store.retirePreviousAsync() && !store.commitAsync() && !store.update(0, 1));
  while (store.busy()) {
    CHECK(callback.calls == 0);
    store.service();
  }
  CHECK(callback.calls == 1 && callback.result == Status::Ready);
  CHECK(mem.operations == start + 3);
  for (unsigned record = 0; record < 4; ++record) {
    const unsigned address = 256 + record * 768;
    if (record == 1) {
      CHECK(std::equal(before.begin() + address, before.begin() + address + 768,
                       mem.data.begin() + address));
    } else {
      CHECK(std::all_of(mem.data.begin() + address, mem.data.begin() + address + 256,
                        [](uint8_t value) { return value == 0xff; }));
      CHECK(std::equal(before.begin() + address + 256, before.begin() + address + 768,
                       mem.data.begin() + address + 256));
    }
  }
  CHECK(std::equal(before.begin(), before.begin() + 256, mem.data.begin()));
  CHECK(std::equal(before.begin() + 3328, before.end(), mem.data.begin() + 3328));
  const unsigned retiredOperations = mem.operations;
  CHECK(store.retirePreviousAsync(completed, &callback));
  finish(store);
  CHECK(callback.calls == 2 && callback.result == Status::Ready);
  CHECK(mem.operations == retiredOperations);
  store.service();
  CHECK(callback.calls == 2);
  fill(store, 11); commit(store);
  ReservedEEPROMCore reboot(mem);
  CHECK(reboot.begin(256, 3072, ram, sizeof(ram)) == Status::Ready);
  CHECK(reboot.valid() && pattern(reboot, 11));
}

static void retirementPowerCuts() {
  const Memory base = retirementSeed();
  unsigned cuts = 0;
  for (unsigned operation = 0; operation < 3; ++operation) {
    for (unsigned prefix = 0; prefix <= 256; ++prefix) {
      Memory mem = base;
      mem.cutOperation = int(base.operations + operation);
      mem.prefix = prefix;
      uint8_t ram[640];
      ReservedEEPROMCore store(mem);
      CHECK(store.begin(256, 3072, ram, sizeof(ram)) == Status::Ready);
      Callback callback;
      CHECK(store.retirePreviousAsync(completed, &callback));
      try { finish(store); CHECK(false); } catch (const Cut&) {}
      CHECK(callback.calls == 0);
      mem.delay = 0;
      mem.cutOperation = -1;
      ReservedEEPROMCore reboot(mem);
      CHECK(reboot.begin(256, 3072, ram, sizeof(ram)) == Status::Ready);
      CHECK(reboot.valid() && pattern(reboot, 10));
      CHECK(reboot.retirePreviousAsync());
      finish(reboot);
      mem.data[1024] ^= 1;
      ReservedEEPROMCore corrupted(mem);
      CHECK(corrupted.begin(256, 3072, ram, sizeof(ram)) == Status::Ready);
      CHECK(!corrupted.valid());
      ++cuts;
    }
  }
  std::printf("D21 retirement erase prefixes checked: %u\n", cuts);
}

static void retirementRejectionsAndErrors() {
  Memory fresh;
  uint8_t ram[640];
  ReservedEEPROMCore uninitialized(fresh);
  CHECK(!uninitialized.retirePreviousAsync());
  CHECK(uninitialized.begin(0, 3072, ram, sizeof(ram)) == Status::Ready);
  CHECK(!uninitialized.retirePreviousAsync());
  Memory smart(true);
  ReservedEEPROMCore smartStore(smart);
  CHECK(smartStore.begin(0, 3072, ram, sizeof(ram)) == Status::Ready);
  CHECK(!smartStore.retirePreviousAsync());
  CHECK(smart.operations == 0);

  for (unsigned failure = 0; failure < 6; ++failure) {
    Memory mem = retirementSeed();
    ReservedEEPROMCore store(mem);
    CHECK(store.begin(256, 3072, ram, sizeof(ram)) == Status::Ready);
    Callback callback;
    if (failure == 0) mem.readError = true;
    if (failure == 1) mem.eraseError = true;
    if (failure == 2) mem.error = true;
    if (failure == 3) mem.incompleteErase = true;
    if (failure == 4) mem.data[1024] ^= 1;
    CHECK(store.retirePreviousAsync(completed, &callback));
    unsigned services = 0;
    const unsigned originalErases = mem.erases;
    while (store.busy()) {
      CHECK(++services < 1000);
      store.service();
      if (failure == 5 && mem.erases != originalErases) mem.readError = true;
    }
    CHECK(store.status() == Status::HardwareError);
    CHECK(callback.calls == 1 && callback.result == Status::HardwareError);
    store.service();
    CHECK(callback.calls == 1);
    if (failure == 0 || failure == 4) CHECK(mem.operations == retirementSeed().operations);
  }
}


#if !defined(__SAMD21__)
static const unsigned smartOffset = 256;
static const unsigned smartCapacity = 640;
static const unsigned smartStride = smartCapacity + 20;

static void openSmart(ReservedEEPROMCore& store, uint8_t* ram, unsigned records = 2) {
  CHECK(store.beginAtomicSnapshots(smartOffset, records * smartStride, ram, smartCapacity) == Status::Ready);
}

static Memory smartSeed(unsigned generations, unsigned records = 2) {
  Memory mem(true);
  uint8_t ram[smartCapacity];
  ReservedEEPROMCore store(mem);
  openSmart(store, ram, records);
  CHECK(!store.valid() && !store.retirePreviousAsync());
  for (unsigned generation = 1; generation <= generations; ++generation) {
    fill(store, uint8_t(generation));
    commit(store);
  }
  return mem;
}

static void smartSnapshotBoundsAndPersistence() {
  Memory mem(true);
  uint8_t ram[smartCapacity];
  ReservedEEPROMCore store(mem);
  CHECK(store.beginAtomicSnapshots(1, 2048, ram, sizeof(ram)) == Status::InvalidConfiguration);
  CHECK(store.beginAtomicSnapshots(0, 2048, ram, 639) == Status::InvalidConfiguration);
  CHECK(store.beginAtomicSnapshots(0, 1320, ram, sizeof(ram)) == Status::Ready);
  CHECK(store.beginAtomicSnapshots(0, 1316, ram, sizeof(ram)) == Status::InvalidConfiguration);
  CHECK(!store.commitAsync() && !store.atomicSnapshotsEnabled());
  openSmart(store, ram);
  CHECK(!store.valid() && store.length() == sizeof(ram) && store.atomicSnapshotsEnabled());
  for (unsigned generation = 1; generation <= 12; ++generation) {
    fill(store, uint8_t(generation));
    Callback callback;
    CHECK(store.commitAsync(completed, &callback));
    CHECK(!store.commitAsync() && !store.retirePreviousAsync() && !store.update(0, 0));
    CHECK(store.beginAtomicSnapshots(0, 0, ram, sizeof(ram)) == Status::Busy);
    while (store.busy()) {
      CHECK(callback.calls == 0);
      store.service();
    }
    CHECK(callback.calls == 1 && callback.result == Status::Ready);
    store.service();
    CHECK(callback.calls == 1);
    uint8_t recovered[smartCapacity];
    ReservedEEPROMCore reboot(mem);
    openSmart(reboot, recovered);
    CHECK(reboot.valid() && pattern(reboot, uint8_t(generation)));
    const unsigned operations = mem.operations;
    CHECK(reboot.commitAsync(completed, &callback));
    CHECK(callback.calls == 1);
    finish(reboot);
    CHECK(callback.calls == 2 && callback.result == Status::Ready);
    CHECK(mem.operations == operations && mem.erases == 0);
  }
  CHECK(std::all_of(mem.data.begin(), mem.data.begin() + smartOffset, [](uint8_t b) { return b == 0xff; }));
  CHECK(std::all_of(mem.data.begin() + smartOffset + 2 * smartStride, mem.data.end(), [](uint8_t b) { return b == 0xff; }));

  Memory d21;
  ReservedEEPROMCore existing(d21);
  CHECK(existing.beginAtomicSnapshots(0, 1536, ram, sizeof(ram)) == Status::Ready);
  fill(existing, 6); commit(existing);
  ReservedEEPROMCore defaultAPI(d21);
  CHECK(defaultAPI.begin(0, 1536, ram, sizeof(ram)) == Status::Ready);
  CHECK(defaultAPI.valid() && pattern(defaultAPI, 6) && defaultAPI.atomicSnapshotsEnabled());
}

static void smartSnapshotPowerCuts() {
  unsigned cuts = 0;
  for (unsigned previous = 0; previous < 4; ++previous) {
    const Memory base = smartSeed(previous);
    const unsigned start = base.operations;
    Memory complete = base;
    uint8_t ram[smartCapacity];
    ReservedEEPROMCore full(complete);
    openSmart(full, ram);
    fill(full, 99); commit(full);
    CHECK(complete.operations - start == smartCapacity / 4 + 6);
    for (unsigned operation = start; operation < complete.operations; ++operation) {
      for (unsigned prefix = 0; prefix <= 4; ++prefix) {
        Memory mem = base;
        mem.cutOperation = int(operation);
        mem.prefix = prefix;
        ReservedEEPROMCore interrupted(mem);
        openSmart(interrupted, ram);
        fill(interrupted, 99);
        Callback callback;
        CHECK(interrupted.commitAsync(completed, &callback));
        try { finish(interrupted); CHECK(false); } catch (const Cut&) {}
        CHECK(callback.calls == 0);
        mem.delay = 0;
        mem.cutOperation = -1;
        ReservedEEPROMCore reboot(mem);
        openSmart(reboot, ram);
        if (previous) CHECK(reboot.valid() && (pattern(reboot, uint8_t(previous)) || pattern(reboot, 99)));
        else CHECK(!reboot.valid() || pattern(reboot, 99));
        fill(reboot, 100); commit(reboot);
        ReservedEEPROMCore retry(mem);
        openSmart(retry, ram);
        CHECK(retry.valid() && pattern(retry, 100) && mem.erases == 0);
        ++cuts;
      }
    }
  }
  std::printf("SmartEEPROM snapshot word-write prefixes checked: %u\n", cuts);
}

static void smartSnapshotCorruption() {
  const Memory base = smartSeed(2);
  uint8_t ram[smartCapacity];
  for (unsigned byte = 0; byte < smartStride; ++byte) {
    Memory mem = base;
    mem.data[smartOffset + smartStride + byte] ^= 1;
    ReservedEEPROMCore reboot(mem);
    openSmart(reboot, ram);
    CHECK(reboot.valid() && pattern(reboot, 1));
  }
  Memory mem = base;
  ReservedEEPROMCore live(mem);
  openSmart(live, ram);
  mem.data[smartOffset + smartStride] ^= 1;
  Callback callback;
  CHECK(live.commitAsync(completed, &callback));
  while (live.busy()) live.service();
  CHECK(callback.calls == 1 && callback.result == Status::HardwareError);
  CHECK(mem.operations == base.operations);
}

static void smartSnapshotFailures() {
  const Memory base = smartSeed(2);
  const unsigned count = smartCapacity / 4 + 6;
  uint8_t ram[smartCapacity];
  for (unsigned failure = 0; failure < 3; ++failure) {
    for (unsigned operation = base.operations; operation < base.operations + count; ++operation) {
      Memory mem = base;
      if (failure == 0) mem.failProgramOperation = int(operation);
      if (failure == 1) mem.pollErrorOperation = int(operation);
      if (failure == 2) mem.incompleteProgramOperation = int(operation);
      ReservedEEPROMCore store(mem);
      openSmart(store, ram);
      fill(store, 99);
      Callback callback;
      CHECK(store.commitAsync(completed, &callback));
      while (store.busy()) store.service();
      const bool unchangedLength = failure == 2 && operation == base.operations + smartCapacity / 4 + 2;
      CHECK(callback.calls == 1 && callback.result == (unchangedLength ? Status::Ready : Status::HardwareError));
      store.service();
      CHECK(callback.calls == 1);
      mem.delay = 0;
      mem.failProgramOperation = mem.pollErrorOperation = mem.incompleteProgramOperation = -1;
      ReservedEEPROMCore reboot(mem);
      openSmart(reboot, ram);
      CHECK(reboot.valid() && (pattern(reboot, 2) || pattern(reboot, 99)));
      if (failure == 1 && operation == base.operations + count - 1) CHECK(pattern(reboot, 99));
    }
  }
  for (unsigned operation = base.operations; operation <= base.operations + count; ++operation) {
    Memory mem = base;
    ReservedEEPROMCore store(mem);
    openSmart(store, ram);
    fill(store, 99);
    Callback callback;
    CHECK(store.commitAsync(completed, &callback));
    while (store.busy()) {
      if (mem.operations == operation) mem.readError = true;
      store.service();
    }
    CHECK(callback.calls == 1 && callback.result == Status::HardwareError);
    store.service();
    CHECK(callback.calls == 1);
  }
  Memory failed = base;
  failed.readError = true;
  ReservedEEPROMCore unopened(failed);
  CHECK(unopened.beginAtomicSnapshots(smartOffset, 2 * smartStride, ram, sizeof(ram)) == Status::HardwareError);
  CHECK(!unopened.valid() && !unopened.atomicSnapshotsEnabled() && !unopened.commitAsync() && !unopened.retirePreviousAsync());
}

static void smartSequenceWrap() {
  Memory mem = smartSeed(2);
  for (unsigned i = 4; i < 8; ++i) mem.data[smartOffset + i] = 0xff;
  uint32_t crc = 0xffffffffu;
  for (unsigned i = 0; i < 16; ++i) {
    crc ^= mem.data[smartOffset + i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
  }
  crc ^= 0xffffffffu;
  for (unsigned i = 0; i < 4; ++i) mem.data[smartOffset + 16 + i] = uint8_t(crc >> (8 * i));
  std::fill(mem.data.begin() + smartOffset + smartStride,
            mem.data.begin() + smartOffset + 2 * smartStride, 0xff);
  uint8_t ram[smartCapacity];
  ReservedEEPROMCore before(mem);
  openSmart(before, ram);
  CHECK(before.valid() && pattern(before, 1));
  fill(before, 3); commit(before);
  ReservedEEPROMCore after(mem);
  openSmart(after, ram);
  CHECK(after.valid() && pattern(after, 3));
  CHECK(after.retirePreviousAsync()); finish(after);
  mem.data[smartOffset + smartStride] ^= 1;
  ReservedEEPROMCore corrupted(mem);
  openSmart(corrupted, ram);
  CHECK(!corrupted.valid());
}

static void smartRetirement() {
  const Memory base = smartSeed(10, 4);
  uint8_t ram[smartCapacity];
  unsigned cuts = 0;
  for (unsigned operation = 0; operation < 3; ++operation) {
    for (unsigned prefix = 0; prefix <= 4; ++prefix) {
      Memory mem = base;
      mem.cutOperation = int(base.operations + operation);
      mem.prefix = prefix;
      ReservedEEPROMCore store(mem);
      openSmart(store, ram, 4);
      Callback callback;
      CHECK(store.retirePreviousAsync(completed, &callback));
      CHECK(!store.retirePreviousAsync() && !store.commitAsync() && !store.update(0, 0));
      try { finish(store); CHECK(false); } catch (const Cut&) {}
      CHECK(callback.calls == 0);
      mem.delay = 0;
      mem.cutOperation = -1;
      ReservedEEPROMCore reboot(mem);
      openSmart(reboot, ram, 4);
      CHECK(reboot.valid() && pattern(reboot, 10));
      const unsigned beforeReplay = mem.operations;
      commit(reboot);
      CHECK(mem.operations == beforeReplay);
      CHECK(reboot.retirePreviousAsync(completed, &callback));
      finish(reboot);
      CHECK(callback.calls == 1 && callback.result == Status::Ready);
      const unsigned retired = mem.operations;
      CHECK(reboot.retirePreviousAsync(completed, &callback));
      finish(reboot);
      CHECK(callback.calls == 2 && mem.operations == retired && mem.erases == 0);
      for (unsigned record = 0; record < 4; ++record) {
        const unsigned address = smartOffset + record * smartStride;
        if (record != 1) {
          CHECK(std::all_of(mem.data.begin() + address, mem.data.begin() + address + 4, [](uint8_t b) { return b == 0; }));
        }
        CHECK(std::equal(base.data.begin() + address + 4, base.data.begin() + address + smartStride, mem.data.begin() + address + 4));
      }
      CHECK(std::equal(base.data.begin(), base.data.begin() + smartOffset, mem.data.begin()));
      CHECK(std::equal(base.data.begin() + smartOffset + 4 * smartStride, base.data.end(), mem.data.begin() + smartOffset + 4 * smartStride));
      mem.data[smartOffset + smartStride + 20] ^= 1;
      ReservedEEPROMCore corrupted(mem);
      openSmart(corrupted, ram, 4);
      CHECK(!corrupted.valid());
      ++cuts;
    }
  }
  std::printf("SmartEEPROM retirement word-write prefixes checked: %u\n", cuts);

  for (unsigned failure = 0; failure < 5; ++failure) {
    for (unsigned operation = 0; operation < 3; ++operation) {
      Memory mem = base;
      ReservedEEPROMCore store(mem);
      openSmart(store, ram, 4);
      const int failAt = int(base.operations + operation);
      if (failure == 0) mem.failProgramOperation = failAt;
      if (failure == 1) mem.pollErrorOperation = failAt;
      if (failure == 2) mem.incompleteProgramOperation = failAt;
      if (failure == 3) mem.readError = true;
      Callback callback;
      CHECK(store.retirePreviousAsync(completed, &callback));
      while (store.busy()) {
        store.service();
        if (failure == 4 && mem.operations > unsigned(failAt)) mem.readError = true;
      }
      CHECK(callback.calls == 1 && callback.result == Status::HardwareError);
      store.service();
      CHECK(callback.calls == 1);
      mem.readError = false;
      mem.failProgramOperation = mem.pollErrorOperation = mem.incompleteProgramOperation = -1;
      ReservedEEPROMCore resumed(mem);
      openSmart(resumed, ram, 4);
      CHECK(resumed.retirePreviousAsync()); finish(resumed);
      mem.data[smartOffset + smartStride] ^= 1;
      ReservedEEPROMCore corrupt(mem);
      openSmart(corrupt, ram, 4);
      CHECK(!corrupt.valid());
    }
  }
}

#else
static void rejectsUnsupportedSmartSnapshots() {
  Memory smart(true);
  uint8_t ram[640];
  ReservedEEPROMCore store(smart);
  CHECK(store.beginAtomicSnapshots(0, 2048, ram, sizeof(ram)) == Status::InvalidConfiguration);
  CHECK(!store.atomicSnapshotsEnabled() && !store.commitAsync() && smart.operations == 0);
  Memory d21;
  ReservedEEPROMCore snapshots(d21);
  CHECK(snapshots.beginAtomicSnapshots(0, 2048, ram, sizeof(ram)) == Status::Ready);
  CHECK(snapshots.atomicSnapshotsEnabled());
  fill(snapshots, 42); commit(snapshots);
  CHECK(snapshots.valid() && pattern(snapshots, 42));
}
#endif

int main() {
#if !defined(__SAMD21__)
  smartSnapshotBoundsAndPersistence();
  smartSnapshotPowerCuts();
  smartSnapshotCorruption();
  smartSnapshotFailures();
  smartRetirement();
  smartSequenceWrap();
#else
  rejectsUnsupportedSmartSnapshots();
#endif
  retirement();
  retirementPowerCuts();
  retirementRejectionsAndErrors();
  bounds();
  ringBoundsAndWrap();
  d21Persistence();
  d21PowerCuts();
  corruption();
  sequenceWrapAndReadFailure();
  smartEEPROM();
  std::puts("Reserved EEPROM native tests passed");
}
