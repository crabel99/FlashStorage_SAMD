#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
packages="${PLATFORMIO_PACKAGES_DIR:-$HOME/.platformio/packages}"
compiler="$packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-g++"
framework="${ARDUINO_FRAMEWORK_DIR:-$packages/framework-arduino-samd-simio}"
mkdir -p build/reserved
export TMPDIR="$(pwd)/build/reserved"
for variant in samd21 samd51 same53 same54; do
  case "$variant" in
    samd21) device=__SAMD21E18A__; family=__SAMD21__; cpu=cortex-m0plus; board=SimIO_Device_M0;;
    samd51) device=__SAMD51P20A__; family=__SAMD51__; cpu=cortex-m4; board=SimIO_Device_M4;;
    same53) device=__SAME53J19A__; family=__SAME53__; cpu=cortex-m4; board=SimIO_Device_M4;;
    same54) device=__SAME54P20A__; family=__SAME54__; cpu=cortex-m4; board=SimIO_Device_M4;;
  esac
  "$compiler" -std=c++11 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -mcpu="$cpu" -mthumb \
    -D"$device" -D"$family" -Isrc -Itests/reserved/compile \
    -isystem "$packages/framework-cmsis-atmel/CMSIS/Device/ATMEL/$variant/include" \
    -isystem "$packages/framework-cmsis-atmel/CMSIS/Device/ATMEL" \
    -isystem "$packages/framework-cmsis/CMSIS/Core/Include" \
    -c tests/reserved/compile/main.cpp -o "build/reserved/$variant.o"
  echo "Compiled standalone $variant device headers without Arduino"
  "$compiler" -std=c++11 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -mcpu="$cpu" -mthumb \
    -D"$device" -D"$family" -DARDUINO=10819 -DARDUINO_ARCH_SAMD -DF_CPU=48000000L \
    -Isrc -isystem "$framework/cores/arduino" -isystem "$framework/variants/$board" \
    -isystem "$packages/framework-cmsis-atmel/CMSIS/Device/ATMEL/$variant/include" \
    -isystem "$packages/framework-cmsis-atmel/CMSIS/Device/ATMEL" \
    -isystem "$packages/framework-cmsis/CMSIS/Core/Include" \
    -include Arduino.h -x c++ -c examples/ReservedEEPROM/ReservedEEPROM.ino -o "build/reserved/$variant-arduino.o"
  echo "Compiled Arduino ReservedEEPROM example for $variant"
done
