#!/bin/bash
# Build an ESP-IDF firmware (firmware/<name>, named <chip>-<module>-<feature>, e.g. esp32-c6-wroom-test)
# and publish the merged image to site/fw/<name>/<name>.bin (manifest.json next to it is kept by hand).
# Usage: ./build.sh <name>
set -euo pipefail

NAME=${1:?usage: ./build.sh <firmware name under firmware/>}
HERE=$(cd "$(dirname "$0")" && pwd)
case "$HERE" in /work/*) HOST="/srv/station/work/${HERE#/work/}" ;; *) HOST="$HERE" ;; esac
IMAGE=${IDF_IMAGE:-espressif/idf:v5.5.4}

TARGET=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$HERE/firmware/$NAME/sdkconfig.defaults")
[ -n "$TARGET" ] || { echo "CONFIG_IDF_TARGET missing in firmware/$NAME/sdkconfig.defaults" >&2; exit 1; }
OUT="site/fw/$NAME"
mkdir -p "$HERE/$OUT"

docker run --rm --label ai-agent=true \
  -v "$HOST:$HOST" -w "$HOST/firmware/$NAME" \
  -e HOME=/tmp -e IDF_COMPONENT_CACHE_PATH="$HOST/.tmp/idf-components" \
  -u "$(id -u):$(id -g)" \
  "$IMAGE" bash -c "
    idf.py build &&
    cd build && esptool.py --chip $TARGET merge_bin -o $HOST/$OUT/$NAME.bin @flash_args"

ls -l "$HERE/$OUT/$NAME.bin"
