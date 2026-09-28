#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
packages="${PLATFORMIO_PACKAGES_DIR:-$HOME/.platformio/packages}"
compiler="$packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-g++"
mkdir -p build/reserved
export TMPDIR="$(pwd)/build/reserved"
for variant in samd21 samd51 same53 same54; do
  case "$variant" in
    samd21) device=__SAMD21E18A__; family=__SAMD21__; cpu=cortex-m0plus;;
    samd51) device=__SAMD51P20A__; family=__SAMD51__; cpu=cortex-m4;;
    same53) device=__SAME53J19A__; family=__SAME53__; cpu=cortex-m4;;
    same54) device=__SAME54P20A__; family=__SAME54__; cpu=cortex-m4;;
  esac
  "$compiler" -std=c++11 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -mcpu="$cpu" -mthumb \
    -D"$device" -D"$family" -Isrc -Itests/reserved/compile \
    -I"$packages/framework-cmsis-atmel/CMSIS/Device/ATMEL/$variant/include" \
    -I"$packages/framework-cmsis/CMSIS/Core/Include" \
    -c tests/reserved/compile/main.cpp -o "build/reserved/$variant.o"
  echo "Compiled actual $variant device headers"
done
