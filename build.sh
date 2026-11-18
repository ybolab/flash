#!/bin/bash
# Build an ESP-IDF firmware project in firmware/<name> and publish its merged image to
# site/fw/<target>/<name>/<name>.bin (the manifest.json next to it is maintained by hand).
# Usage: ./build.sh <name>
set -euo pipefail

NAME=${1:?usage: ./build.sh <firmware name under firmware/>}
HERE=$(cd "$(dirname "$0")" && pwd)
case "$HERE" in /work/*) HOST="/srv/station/work/${HERE#/work/}" ;; *) HOST="$HERE" ;; esac
IMAGE=${IDF_IMAGE:-espressif/idf:v5.5.4}

SRC="$HERE/firmware/$NAME"
TARGET=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"/\1/p' "$SRC/sdkconfig.defaults")
[ -n "$TARGET" ] || { echo "CONFIG_IDF_TARGET missing in $SRC/sdkconfig.defaults" >&2; exit 1; }
OUT="site/fw/$TARGET/$NAME"
mkdir -p "$HERE/$OUT"

docker run --rm --label ai-agent=true \
  -v "$HOST:$HOST" -w "$HOST/firmware/$NAME" \
  -e HOME=/tmp -e IDF_COMPONENT_CACHE_PATH="$HOST/.tmp/idf-components" \
  -u "$(id -u):$(id -g)" \
  "$IMAGE" bash -c "
    idf.py build &&
    cd build && esptool.py --chip $TARGET merge_bin -o $HOST/$OUT/$NAME.bin @flash_args"

ls -l "$HERE/$OUT/$NAME.bin"
