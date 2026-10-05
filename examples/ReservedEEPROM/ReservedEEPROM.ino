#include <ReservedEEPROM.h>

ReservedEEPROMClass settings;
uint8_t working[128];
bool opened = false;

void setup() {
  // Provision the EEPROM fuses and reserve its backing flash in the linker first.
  opened = settings.begin(0, 0, working, sizeof(working)) == ReservedEEPROMStatus::Ready;
  if (!opened) return;
  const uint32_t value = 42;
  settings.put(0, value);
  settings.commitAsync();
}

void loop() {
  if (opened) settings.service();
  // settings.busy() stays true until the last physical write completes.
  // Check settings.status() before relying on persistence or resetting.
}
