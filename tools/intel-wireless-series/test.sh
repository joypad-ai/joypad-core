#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_dir=$(mktemp -d /tmp/joypad-intel-wireless-series-test.XXXXXX)
trap 'rm -f "$test_dir/test"; rmdir "$test_dir"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined \
    -DCFG_TUSB_CONFIG_FILE='"test_tusb_config.h"' \
    -Itools/intel-wireless-series -Isrc -Isrc/lib/tinyusb/src \
    tools/intel-wireless-series/test.c -o "$test_dir/test"
"$test_dir/test"
