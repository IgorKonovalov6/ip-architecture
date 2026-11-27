#!/usr/bin/env bash
set -euo pipefail

interface_ip="${1:-127.0.0.1}"
payload_size="${2:-1024}"
cycle_count="${3:-100}"
output_directory="${4:-results}"

mkdir -p "$output_directory"

pids=()
for node_id in $(seq 2 10); do
    ./build/udp_node "$node_id" "$interface_ip" "$payload_size" \
        "$cycle_count" "$output_directory" &
    pids+=("$!")
done

sleep 1
./build/udp_node 1 "$interface_ip" "$payload_size" \
    "$cycle_count" "$output_directory"

for pid in "${pids[@]}"; do
    wait "$pid"
done

python3 analyze.py "$output_directory" --output analysis
