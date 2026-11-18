#!/bin/bash
# Build mcu-test in the ESP-IDF container and publish the merged image to flasher/.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
case "$HERE" in /work/*) HOST="/srv/station/work/${HERE#/work/}" ;; *) HOST="$HERE" ;; esac
IMAGE=${IDF_IMAGE:-espressif/idf:v5.5.4}

docker run --rm --label ai-agent=true \
  -v "$HOST:$HOST" -w "$HOST/mcu-test" \
  -e HOME=/tmp -e IDF_COMPONENT_CACHE_PATH="$HOST/.tmp/idf-components" \
  -u "$(id -u):$(id -g)" \
  "$IMAGE" bash -c '
    idf.py build &&
    cd build && esptool.py --chip esp32c6 merge_bin -o ../../flasher/keybox-mcu-test.bin \
      @flash_args'

ls -l "$HERE/flasher/keybox-mcu-test.bin"
