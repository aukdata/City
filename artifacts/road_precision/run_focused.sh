#!/bin/bash
set -u
ROOT=/workspace/scratch/67b01c4a1273/City-latest
cd "$ROOT"
source ../build-tools/env.sh
OUT="$ROOT/artifacts/road_precision"
cd Test/App
result=0
for filter in DevelopmentSnapshot Construction SharedTransport; do
  mkdir -p "$OUT/$filter"
  ./CityTests --filter "$filter" > "$OUT/$filter/runtime.log" 2>&1
  code=$?
  printf '%s\n' "$code" > "$OUT/$filter/exit.txt"
  if [ -f TestResults/results.json ]; then cp TestResults/results.json "$OUT/$filter/results.json"; fi
  if [ -f TestResults/results.xml ]; then cp TestResults/results.xml "$OUT/$filter/results.xml"; fi
  if [ "$code" -ne 0 ]; then result=1; fi
  printf 'SAVE_FILTER_DONE filter=%s exit=%s\n' "$filter" "$code"
done
printf '%s\n' "$result" > "$OUT/focused.exit"
printf 'SAVE_FOCUSED_DONE exit=%s\n' "$result"
exit "$result"
