# Benchmarks

Build and run `bridge_frame_benchmark` from a release build. It measures the
production frame parser using fixed bounded input; it is not a transfer-throughput
benchmark. Record compiler, dependency versions, hardware and repeated-run timings
when comparing results. Future LAN measurements need disk baselines and an
`iperf3` run over the same connection.

`bridge_transfer_benchmark` (BUILD_TESTING and session enabled) exercises actual
local source hashing, mutual TLS pairing, checkpoint flushing and final integrity
verification. It accepts MiB (1..1024) and trial count (1..10), defaults 128/5:

```sh
cmake --preset release
cmake --build --preset release --target bridge_transfer_benchmark
../bridge-build/release/bridge_transfer_benchmark 500 5
```

Only loopback peers are created. The harness compares both ephemeral fingerprints
before consent; there is no auto-confirm option in the desktop. Output reports
payload bytes, total/data seconds and decimal MB/s. Generation is not timed;
data time includes final verification. Fixtures are streamed through a reusable
1 MiB block and deleted by scoped temporary-directory owners. This executable
is excluded from install/release payloads. One-MiB CTest/Memcheck smoke validates
the harness. Record environment/cache/disk load and at least five trials for
comparisons; local results do not establish LAN/Wi-Fi speed.
