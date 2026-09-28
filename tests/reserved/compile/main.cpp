#include "ReservedEEPROM.h"
#include <type_traits>
ReservedEEPROMClass storage;
uint8_t buffer[128];
void exercise() {
  storage.begin(0, 4096, buffer, sizeof(buffer));
  storage.write(0, storage.read(0));
  uint32_t value = 0;
  storage.get(4, value);
  storage.put(4, value);
  storage.commitAsync();
  storage.service();
}

static_assert(!std::is_copy_constructible<ReservedEEPROMClass>::value, "A hardware store cannot be copied");
static_assert(!std::is_move_constructible<ReservedEEPROMClass>::value, "A hardware store cannot be moved");
