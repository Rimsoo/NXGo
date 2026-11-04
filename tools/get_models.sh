#!/bin/sh
# Downloads the KataGo networks bundled in the app (romfs/models) and the test bench.
# Networks come from https://katagotraining.org/networks/
set -e
cd "$(dirname "$0")/.."
BASE=https://media.katagotraining.org/uploaded/networks/models/kata1
mkdir -p romfs/models kgbench/romfs/models
curl -fL -o romfs/models/b6.txt.gz  "$BASE/kata1-b6c96-s175395328-d26788732.txt.gz"
curl -fL -o romfs/models/b10.txt.gz "$BASE/kata1-b10c128-s1141046784-d204142634.txt.gz"
cp romfs/models/b6.txt.gz romfs/models/b10.txt.gz kgbench/romfs/models/
echo "Optional, stronger but slow on Switch: copy a b15 net as /switch/nxgo/models/b15.txt.gz on the SD card:"
echo "  $BASE/kata1-b15c192-s1672170752-d466197061.txt.gz"
