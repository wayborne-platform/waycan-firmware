#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/host-tests
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I main main/waycan_buffer.c tests/waycan_buffer_test.c \
    -o build/host-tests/waycan_buffer_test
build/host-tests/waycan_buffer_test
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer -pthread -DHARDWARE_VER=2 -DWICAN_V300=2 -I tests/stubs -I main \
    main/waycan.c main/waycan_buffer.c tests/waycan_runtime_test.c \
    -o build/host-tests/waycan_runtime_test
build/host-tests/waycan_runtime_test
