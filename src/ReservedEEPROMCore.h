#ifndef RESERVED_EEPROM_CORE_H
#define RESERVED_EEPROM_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum class ReservedEEPROMStatus : uint8_t {
  Ready, Unconfigured, InvalidConfiguration, OutOfRange, Busy, HardwareError
};

struct ReservedEEPROMGeometry {
  uint32_t bytes;
  uint16_t pageBytes;
  uint16_t rowBytes;
  bool smart;
};

// One owner services this backend; no other NVM writer may run concurrently.
class ReservedEEPROMBackend {
public:
  virtual ReservedEEPROMStatus geometry(ReservedEEPROMGeometry&) = 0;
  virtual bool readBytes(uint32_t offset, uint8_t* destination, uint32_t bytes) = 0;
  virtual bool erase(uint32_t offset) = 0;
  virtual bool program(uint32_t offset, const uint8_t* source, uint32_t bytes) = 0;
  virtual ReservedEEPROMStatus poll() = 0;
protected:
  ~ReservedEEPROMBackend() = default;
};

class ReservedEEPROMCore {
public:
  typedef void (*Completion)(void*, ReservedEEPROMStatus);

  explicit ReservedEEPROMCore(ReservedEEPROMBackend& backend)
      : backend_(backend), buffer_(nullptr), capacity_(0), offset_(0), bytes_(0),
        stride_(0), records_(0), active_(0), sequence_(0), target_(0), cursor_(0),
        valid_(false), initialized_(false), atomicSmart_(false), phase_(Phase::Idle),
        afterWait_(Phase::Idle), status_(ReservedEEPROMStatus::Unconfigured), readFailed_(false),
        callback_(nullptr), context_(nullptr) {}

  ReservedEEPROMCore(const ReservedEEPROMCore&) = delete;
  ReservedEEPROMCore& operator=(const ReservedEEPROMCore&) = delete;
  ReservedEEPROMCore(ReservedEEPROMCore&&) = delete;
  ReservedEEPROMCore& operator=(ReservedEEPROMCore&&) = delete;

  ReservedEEPROMStatus begin(uint32_t offset, uint32_t partitionBytes,
                            uint8_t* workingBuffer, uint32_t capacity) {
    return beginStore(offset, partitionBytes, workingBuffer, capacity, false);
  }

  ReservedEEPROMStatus beginAtomicSnapshots(uint32_t offset, uint32_t partitionBytes,
                                           uint8_t* workingBuffer, uint32_t capacity) {
    return beginStore(offset, partitionBytes, workingBuffer, capacity, true);
  }

private:
  ReservedEEPROMStatus beginStore(uint32_t offset, uint32_t partitionBytes,
                                 uint8_t* workingBuffer, uint32_t capacity,
                                 bool atomicSnapshots) {
    if (busy()) return ReservedEEPROMStatus::Busy;
    initialized_ = false;
    readFailed_ = false;
    valid_ = false;
    capacity_ = 0;
    status_ = backend_.geometry(geometry_);
    if (status_ != ReservedEEPROMStatus::Ready) return status_;
    if (!workingBuffer || !capacity || offset > geometry_.bytes)
      return status_ = ReservedEEPROMStatus::InvalidConfiguration;
    const uint32_t available = geometry_.bytes - offset;
    const uint32_t bytes = partitionBytes ? partitionBytes : available;
    if (bytes > available || !bytes || capacity > bytes)
      return status_ = ReservedEEPROMStatus::OutOfRange;
    atomicSmart_ = geometry_.smart && atomicSnapshots;
    if (geometry_.smart) {
      if ((offset & 3u) || (capacity & 3u) || (bytes & 3u))
        return status_ = ReservedEEPROMStatus::InvalidConfiguration;
      if (atomicSmart_) {
        if (capacity > UINT32_MAX - 20u)
          return status_ = ReservedEEPROMStatus::InvalidConfiguration;
        stride_ = capacity + 20u;
        records_ = bytes / stride_;
        if (records_ < 2) return status_ = ReservedEEPROMStatus::InvalidConfiguration;
      }
    } else {
      if (geometry_.pageBytes != 64 || geometry_.rowBytes != 256 ||
          (offset % geometry_.rowBytes) || (bytes % geometry_.rowBytes) ||
          capacity > UINT32_MAX - 319u)
        return status_ = ReservedEEPROMStatus::InvalidConfiguration;
      stride_ = (capacity + 64u + 255u) & ~255u;
      records_ = bytes / stride_;
      if (records_ < 2) return status_ = ReservedEEPROMStatus::InvalidConfiguration;
    }
    buffer_ = workingBuffer;
    capacity_ = capacity;
    offset_ = offset;
    bytes_ = bytes;
    memset(buffer_, 0xff, capacity_);
    if (!snapshots()) {
      if (!backend_.readBytes(offset_, buffer_, capacity_)) return failBegin();
      valid_ = true;
    } else {
      for (uint32_t record = 0; record < records_; ++record) {
        uint32_t candidate = 0;
        if (recordValid(record, candidate) &&
            (!valid_ || newer(candidate, sequence_))) {
          active_ = record;
          sequence_ = candidate;
          valid_ = true;
        }
      }
      if (readFailed_) return failBegin();
      if (valid_ && !backend_.readBytes(recordAddress(active_) + payloadOffset(), buffer_, capacity_))
        return failBegin();
    }
    initialized_ = true;
    return status_ = ReservedEEPROMStatus::Ready;
  }

public:
  uint32_t length() const { return capacity_; }
  bool busy() const { return phase_ != Phase::Idle; }
  bool valid() const { return valid_; }
  bool atomicSnapshotsEnabled() const { return initialized_ && snapshots(); }
  ReservedEEPROMStatus status() const { return status_; }

  uint8_t read(uint32_t address) const {
    return initialized_ && address < capacity_ ? buffer_[address] : 0xff;
  }
  bool update(uint32_t address, uint8_t value) {
    if (!writable(address, 1)) return false;
    buffer_[address] = value;
    return true;
  }
  bool write(uint32_t address, uint8_t value) { return update(address, value); }
  template<class T> bool get(uint32_t address, T& value) const {
    if (!initialized_ || address > capacity_ || sizeof(T) > capacity_ - address)
      return false;
    memcpy(&value, buffer_ + address, sizeof(T));
    return true;
  }
  template<class T> bool put(uint32_t address, const T& value) {
    if (!writable(address, sizeof(T))) return false;
    memcpy(buffer_ + address, &value, sizeof(T));
    return true;
  }

  bool retirePreviousAsync(Completion completion = nullptr, void* context = nullptr) {
    if (!initialized_ || !valid_ || !snapshots() || busy()) return false;
    callback_ = completion;
    context_ = context;
    status_ = ReservedEEPROMStatus::Busy;
    target_ = 0;
    cursor_ = 0;
    phase_ = Phase::RetireActive;
    return true;
  }

  bool commitAsync(Completion completion = nullptr, void* context = nullptr) {
    if (!initialized_ || busy()) return false;
    callback_ = completion;
    context_ = context;
    status_ = ReservedEEPROMStatus::Busy;
    cursor_ = 0;
    phase_ = Phase::Compare;
    return true;
  }

  void service() {
    if (!busy()) return;
    uint8_t data[64];
    switch (phase_) {
      case Phase::Wait: {
        const ReservedEEPROMStatus result = backend_.poll();
        if (result == ReservedEEPROMStatus::Busy) return;
        if (result != ReservedEEPROMStatus::Ready) { finish(result); return; }
        phase_ = afterWait_;
        return;
      }
      case Phase::Compare: {
        if (cursor_ == capacity_) { finishComparison(); return; }
        if (!valid_ && snapshots()) { startSnapshot(); return; }
        const uint32_t count = minimum(4, capacity_ - cursor_);
        const uint32_t address = snapshots() ? recordAddress(active_) + payloadOffset() : offset_;
        if (!backend_.readBytes(address + cursor_, data, count)) { hardwareError(); return; }
        if (memcmp(data, buffer_ + cursor_, count)) {
          if (snapshots()) { startSnapshot(); return; }
          if (!backend_.program(offset_ + cursor_, buffer_ + cursor_, count)) {
            hardwareError(); return;
          }
          cursor_ += count;
          waitFor(Phase::Compare);
          return;
        }
        cursor_ += count;
        if (cursor_ == capacity_) finishComparison();
        return;
      }
      case Phase::Invalidate:
        store32(data, 0);
        if (!backend_.program(recordAddress(target_), data, 4)) { hardwareError(); return; }
        waitFor(Phase::InvalidateVerify);
        return;
      case Phase::InvalidateVerify:
        if (!backend_.readBytes(recordAddress(target_), data, 4) || load32(data) != 0) {
          hardwareError(); return;
        }
        phase_ = Phase::Payload;
        return;
      case Phase::Erase:
        if (!backend_.erase(recordAddress(target_) + cursor_)) { hardwareError(); return; }
        cursor_ += geometry_.rowBytes;
        if (cursor_ == stride_) {
          cursor_ = 0;
          waitFor(Phase::Payload);
        } else waitFor(Phase::Erase);
        return;
      case Phase::Payload: {
        const uint32_t unit = atomicSmart_ ? 4 : 64;
        const uint32_t count = minimum(unit, capacity_ - cursor_);
        memset(data, 0xff, sizeof(data));
        memcpy(data, buffer_ + cursor_, count);
        if (!backend_.program(recordAddress(target_) + payloadOffset() + cursor_, data, unit)) {
          hardwareError(); return;
        }
        cursor_ += count;
        if (cursor_ == capacity_) {
          cursor_ = atomicSmart_ ? 4 : 0;
          waitFor(Phase::Header);
        } else waitFor(Phase::Payload);
        return;
      }
      case Phase::Header:
        memset(data, 0xff, sizeof(data));
        store32(data, marker());
        store32(data + 4, valid_ ? sequence_ + 1u : 0u);
        store32(data + 8, capacity_);
        store32(data + 12, crc(buffer_, capacity_));
        store32(data + 16, crc(data, 16));
        if (atomicSmart_) {
          if (!backend_.program(recordAddress(target_) + cursor_, data + cursor_, 4)) {
            hardwareError(); return;
          }
          cursor_ += 4;
          waitFor(cursor_ == 20 ? Phase::Marker : Phase::Header);
        } else {
          if (!backend_.program(recordAddress(target_), data, 64)) { hardwareError(); return; }
          waitFor(Phase::Verify);
        }
        return;
      case Phase::Marker:
        store32(data, marker());
        if (!backend_.program(recordAddress(target_), data, 4)) { hardwareError(); return; }
        waitFor(Phase::Verify);
        return;
      case Phase::Verify: {
        uint32_t sequence = 0;
        if (!recordValid(target_, sequence, atomicSmart_) ||
            (atomicSmart_ && sequence != (valid_ ? sequence_ + 1u : 0u))) {
          hardwareError(); return;
        }
        active_ = target_;
        sequence_ = sequence;
        valid_ = true;
        finish(ReservedEEPROMStatus::Ready);
        return;
      }
      case Phase::RetireActive: {
        uint32_t sequence = 0;
        if (!recordValid(active_, sequence) || sequence != sequence_) { hardwareError(); return; }
        phase_ = Phase::RetireScan;
        return;
      }
      case Phase::RetireScan:
        if (target_ == records_) { finish(ReservedEEPROMStatus::Ready); return; }
        if (target_ == active_) { ++target_; return; }
        if (atomicSmart_) {
          if (!backend_.readBytes(recordAddress(target_), data, 4)) { hardwareError(); return; }
          if (load32(data) == 0 || load32(data) == UINT32_MAX) ++target_;
          else phase_ = Phase::RetireErase;
          return;
        }
        if (!backend_.readBytes(recordAddress(target_) + cursor_, data, sizeof(data))) {
          hardwareError(); return;
        }
        for (unsigned i = 0; i < sizeof(data); ++i) {
          if (data[i] != 0xff) { phase_ = Phase::RetireErase; return; }
        }
        cursor_ += sizeof(data);
        if (cursor_ == geometry_.rowBytes) { ++target_; cursor_ = 0; }
        return;
      case Phase::RetireErase:
        if (atomicSmart_) {
          store32(data, 0);
          if (!backend_.program(recordAddress(target_), data, 4)) { hardwareError(); return; }
        } else if (!backend_.erase(recordAddress(target_))) { hardwareError(); return; }
        cursor_ = 0;
        waitFor(Phase::RetireVerify);
        return;
      case Phase::RetireVerify:
        if (atomicSmart_) {
          if (!backend_.readBytes(recordAddress(target_), data, 4) || load32(data) != 0) {
            hardwareError(); return;
          }
          ++target_;
          phase_ = Phase::RetireScan;
          return;
        }
        if (!backend_.readBytes(recordAddress(target_) + cursor_, data, sizeof(data))) {
          hardwareError(); return;
        }
        for (unsigned i = 0; i < sizeof(data); ++i) {
          if (data[i] != 0xff) { hardwareError(); return; }
        }
        cursor_ += sizeof(data);
        if (cursor_ == geometry_.rowBytes) {
          ++target_;
          cursor_ = 0;
          phase_ = Phase::RetireScan;
        }
        return;
      case Phase::Idle: return;
    }
  }

private:
  enum class Phase : uint8_t { Idle, Compare, Invalidate, InvalidateVerify, Erase, Payload, Header, Marker, Verify, Wait,
                               RetireActive, RetireScan, RetireErase, RetireVerify };
  static uint32_t minimum(uint32_t a, uint32_t b) { return a < b ? a : b; }
  static uint32_t marker() { return 0x31504552u; } // REP1, little endian.
  static bool newer(uint32_t a, uint32_t b) { return a != b && (a - b) < 0x80000000u; }
  static uint32_t load32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
  }
  static void store32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (i * 8));
  }
  static uint32_t extendCRC(uint32_t value, const uint8_t* p, uint32_t bytes) {
    while (bytes--) {
      value ^= *p++;
      for (unsigned bit = 0; bit < 8; ++bit)
        value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0);
    }
    return value;
  }
  static uint32_t crc(const uint8_t* p, uint32_t bytes) {
    return extendCRC(0xffffffffu, p, bytes) ^ 0xffffffffu;
  }
  uint32_t recordAddress(uint32_t index) const { return offset_ + index * stride_; }
  bool snapshots() const { return !geometry_.smart || atomicSmart_; }
  uint32_t payloadOffset() const { return atomicSmart_ ? 20 : 64; }
  bool recordValid(uint32_t index, uint32_t& sequence, bool matchBuffer = false) {
    uint8_t header[20];
    if (!backend_.readBytes(recordAddress(index), header, sizeof(header))) { readFailed_ = true; return false; }
    if (load32(header) != marker() || load32(header + 8) != capacity_ ||
        load32(header + 16) != crc(header, 16)) return false;
    uint32_t value = 0xffffffffu;
    uint8_t data[64];
    for (uint32_t pos = 0; pos < capacity_; pos += sizeof(data)) {
      const uint32_t count = minimum(sizeof(data), capacity_ - pos);
      if (!backend_.readBytes(recordAddress(index) + payloadOffset() + pos, data, count)) { readFailed_ = true; return false; }
      if (matchBuffer && memcmp(data, buffer_ + pos, count)) return false;
      value = extendCRC(value, data, count);
    }
    if ((value ^ 0xffffffffu) != load32(header + 12)) return false;
    sequence = load32(header + 4);
    return true;
  }
  bool writable(uint32_t address, size_t bytes) {
    if (!initialized_ || busy()) return false;
    if (address > capacity_ || bytes > capacity_ - address) {
      status_ = ReservedEEPROMStatus::OutOfRange;
      return false;
    }
    status_ = ReservedEEPROMStatus::Ready;
    return true;
  }
  ReservedEEPROMStatus failBegin() {
    capacity_ = 0;
    valid_ = false;
    return status_ = ReservedEEPROMStatus::HardwareError;
  }
  void finishComparison() {
    if (atomicSmart_) {
      uint32_t sequence = 0;
      if (!recordValid(active_, sequence, true) || sequence != sequence_) {
        hardwareError(); return;
      }
    }
    finish(ReservedEEPROMStatus::Ready);
  }
  void startSnapshot() {
    target_ = valid_ ? (active_ + 1) % records_ : 0;
    cursor_ = 0;
    phase_ = atomicSmart_ ? Phase::Invalidate : Phase::Erase;
  }
  void waitFor(Phase next) { afterWait_ = next; phase_ = Phase::Wait; }
  void hardwareError() { finish(ReservedEEPROMStatus::HardwareError); }
  void finish(ReservedEEPROMStatus result) {
    phase_ = Phase::Idle;
    status_ = result;
    Completion callback = callback_;
    void* context = context_;
    callback_ = nullptr;
    context_ = nullptr;
    if (callback) callback(context, result);
  }

  ReservedEEPROMBackend& backend_;
  ReservedEEPROMGeometry geometry_;
  uint8_t* buffer_;
  uint32_t capacity_, offset_, bytes_, stride_, records_, active_, sequence_, target_, cursor_;
  bool valid_, initialized_, atomicSmart_;
  Phase phase_, afterWait_;
  ReservedEEPROMStatus status_;
  bool readFailed_;
  Completion callback_;
  void* context_;
};

#endif
