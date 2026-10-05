#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
mkdir -p build/reserved
export TMPDIR="$(pwd)/build/reserved"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -Isrc tests/reserved/main.cpp -o build/reserved/tests
build/reserved/tests
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined -D__SAMD21__ -Isrc tests/reserved/main.cpp -o build/reserved/d21-tests
build/reserved/d21-tests
for family in __SAMD21__ __SAMD51__ __SAME53__ __SAME54__; do
  "${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined \
    -D"$family" -Isrc -Itests/reserved/fakes tests/reserved/registers.cpp -o "build/reserved/registers-$family"
  "build/reserved/registers-$family"
done
