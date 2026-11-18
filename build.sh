#!/bin/bash
# Build the test firmware for one chip (firmware/<chip>, ESP-IDF) and publish the merged image to
# site/fw/<chip>/<chip>.bin (the manifest.json next to it is maintained by hand).
# Usage: ./build.sh <chip>
set -euo pipefail

CHIP=${1:?usage: ./build.sh <chip under firmware/>}
HERE=$(cd "$(dirname "$0")" && pwd)
case "$HERE" in /work/*) HOST="/srv/station/work/${HERE#/work/}" ;; *) HOST="$HERE" ;; esac
IMAGE=${IDF_IMAGE:-espressif/idf:v5.5.4}

OUT="site/fw/$CHIP"
mkdir -p "$HERE/$OUT"

docker run --rm --label ai-agent=true \
  -v "$HOST:$HOST" -w "$HOST/firmware/$CHIP" \
  -e HOME=/tmp -e IDF_COMPONENT_CACHE_PATH="$HOST/.tmp/idf-components" \
  -u "$(id -u):$(id -g)" \
  "$IMAGE" bash -c "
    idf.py build &&
    cd build && esptool.py --chip $CHIP merge_bin -o $HOST/$OUT/$CHIP.bin @flash_args"

ls -l "$HERE/$OUT/$CHIP.bin"
