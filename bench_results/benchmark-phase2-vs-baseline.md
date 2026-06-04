# Benchmark Report: vs a8f1d188 (pre-redirect)

- **Compiler**: clang (Release + LTO)
- **Duration**: 3.0s per benchmark
- **Iterations**: 5 runs, averaged
- **Backend**: epoll
- **Baseline (a8f1d188 (pre-redirect))**: 2026-06-03T12:19:44
- **Head**: 2026-06-03T12:53:43

## Chart 1: Corosio Regression Check

Rows with no baseline match (benchmarks new on head) are excluded.

| Benchmark | Unit | a8f1d188 (pre-redirect) Mean | a8f1d188 (pre-redirect) CV | head Mean | head CV | Diff |
|:----------|:----:|-----------:|-----------:|----------:|--------:|-----:|
| accept_churn:burst/10 | Kops/s | 4.11 | 0.5% | 4.14 | 0.7% | +0.7% |
| accept_churn:burst/100 | Kops/s | 0.395 | 0.6% | 0.398 | 0.4% | +0.8% |
| accept_churn:burst_lockless/10 | Kops/s | 4.11 | 1.2% | 4.16 | 0.2% | +1.2% |
| accept_churn:burst_lockless/100 | Kops/s | 0.398 | 0.2% | 0.401 | 0.6% | +0.8% |
| accept_churn:concurrent/1 | Kops/s | 33.8 | 0.8% | 34.0 | 0.6% | +0.6% |
| accept_churn:concurrent/4 | Kops/s | 33.9 | 1.0% | 34.1 | 0.5% | +0.6% |
| accept_churn:concurrent/16 | Kops/s | 33.6 | 0.8% | 33.9 | 0.6% | +0.8% |
| accept_churn:sequential | Kops/s | 33.6 | 2.5% | 34.1 | 0.7% | +1.6% |
| accept_churn:sequential_lockless | Kops/s | 34.0 | 1.0% | 34.2 | 0.2% | +0.7% |
| fan_out:concurrent_parents/1 | Kops/s | 7.80 | 0.5% | 7.86 | 0.2% | +0.8% |
| fan_out:concurrent_parents/4 | Kops/s | 7.61 | 0.6% | 7.63 | 0.4% | +0.3% |
| fan_out:concurrent_parents/16 | Kops/s | 7.44 | 0.6% | 7.48 | 0.3% | +0.5% |
| fan_out:concurrent_parents_lockless/1 | Kops/s | 7.85 | 0.6% | 7.89 | 0.5% | +0.5% |
| fan_out:concurrent_parents_lockless/4 | Kops/s | 7.63 | 0.6% | 7.67 | 0.3% | +0.5% |
| fan_out:concurrent_parents_lockless/16 | Kops/s | 7.47 | 0.5% | 7.52 | 0.2% | +0.6% |
| fan_out:fork_join/1 | Kops/s | 107.1 | 0.5% | 108.6 | 0.4% | +1.4% |
| fan_out:fork_join/4 | Kops/s | 30.1 | 0.4% | 30.4 | 0.2% | +1.0% |
| fan_out:fork_join/16 | Kops/s | 7.80 | 0.5% | 7.86 | 0.3% | +0.8% |
| fan_out:fork_join/64 | Kops/s | 1.90 | 0.5% | 1.91 | 0.3% | +0.6% |
| fan_out:fork_join_lockless/1 | Kops/s | 108.2 | 0.8% | 109.6 | 0.3% | +1.3% |
| fan_out:fork_join_lockless/4 | Kops/s | 30.4 | 0.6% | 30.7 | 0.3% | +0.8% |
| fan_out:fork_join_lockless/16 | Kops/s | 7.85 | 0.6% | 7.91 | 0.2% | +0.7% |
| fan_out:fork_join_lockless/64 | Kops/s | 1.91 | 0.8% | 1.92 | 0.5% | +0.5% |
| fan_out:nested/4 | Kops/s | 7.75 | 0.5% | 7.80 | 0.3% | +0.7% |
| fan_out:nested/16 | Kops/s | 1.89 | 0.6% | 1.90 | 0.4% | +0.5% |
| fan_out:nested_lockless/4 | Kops/s | 7.81 | 0.7% | 7.85 | 0.3% | +0.5% |
| fan_out:nested_lockless/16 | Kops/s | 1.90 | 0.7% | 1.91 | 0.3% | +0.4% |
| http_server:concurrent/1 | Kops/s | 111.6 | 0.8% | 112.5 | 0.2% | +0.9% |
| http_server:concurrent/4 | Kops/s | 119.7 | 0.9% | 120.1 | 0.3% | +0.3% |
| http_server:concurrent/16 | Kops/s | 121.5 | 0.6% | 122.1 | 0.4% | +0.5% |
| http_server:concurrent/32 | Kops/s | 120.2 | 0.7% | 120.9 | 0.3% | +0.6% |
| http_server:multithread/1 | Kops/s | 120.1 | 0.8% | 120.9 | 0.3% | +0.7% |
| http_server:multithread/2 | Kops/s | 226.0 | 0.1% | 226.9 | 0.3% | +0.4% |
| http_server:multithread/4 | Kops/s | 383.2 | 0.8% | 384.9 | 0.4% | +0.5% |
| http_server:multithread/8 | Kops/s | 594.3 | 1.2% | 599.9 | 0.3% | +0.9% |
| http_server:multithread/16 | Kops/s | 773.8 | 0.2% | 775.5 | 0.1% | +0.2% |
| http_server:single_conn | Kops/s | 111.8 | 0.7% | 112.5 | 0.3% | +0.6% |
| http_server:single_conn_lockless | Kops/s | 112.6 | 0.6% | 113.2 | 0.3% | +0.5% |
| io_context:concurrent/4 | Mops/s | 3.28 | 2.7% | 3.30 | 1.5% | +0.6% |
| io_context:high_inline_budget | Mops/s | 10.9 | 3.7% | 10.9 | 2.3% | -0.1% |
| io_context:interleaved | Mops/s | 11.5 | 3.7% | 11.6 | 2.9% | +0.6% |
| io_context:interleaved_lockless | Mops/s | 12.8 | 2.8% | 12.8 | 1.6% | -0.2% |
| io_context:large_event_buffer | Mops/s | 10.9 | 4.6% | 11.1 | 1.5% | +2.0% |
| io_context:multithreaded/8 | Mops/s | 0.973 | 3.5% | 1.01 | 2.1% | +3.4% |
| io_context:single_threaded | Mops/s | 5.94 | 0.9% | 5.85 | 2.0% | -1.5% |
| io_context:single_threaded_lockless | Mops/s | 12.3 | 3.3% | 12.2 | 2.7% | -0.4% |
| local_socket_latency:concurrent/1 | Kops/s | 297.8 | 2.6% | 305.5 | 0.4% | +2.6% |
| local_socket_latency:concurrent/4 | Kops/s | 323.0 | 1.7% | 329.0 | 0.5% | +1.9% |
| local_socket_latency:concurrent/16 | Kops/s | 329.3 | 0.9% | 336.2 | 0.3% | +2.1% |
| local_socket_latency:concurrent_lockless/1 | Kops/s | 294.4 | 3.4% | 306.7 | 1.6% | +4.2% |
| local_socket_latency:concurrent_lockless/4 | Kops/s | 324.4 | 1.3% | 330.0 | 0.8% | +1.7% |
| local_socket_latency:concurrent_lockless/16 | Kops/s | 331.1 | 1.1% | 337.6 | 0.4% | +2.0% |
| local_socket_latency:pingpong/1 | Kops/s | 300.6 | 1.3% | 305.1 | 0.3% | +1.5% |
| local_socket_latency:pingpong/64 | Kops/s | 300.1 | 1.1% | 305.7 | 0.4% | +1.9% |
| local_socket_latency:pingpong/1024 | Kops/s | 280.1 | 2.9% | 296.8 | 0.8% | +6.0% |
| local_socket_latency:pingpong_lockless/1 | Kops/s | 303.5 | 1.0% | 301.0 | 3.4% | -0.8% |
| local_socket_latency:pingpong_lockless/64 | Kops/s | 303.4 | 0.7% | 304.0 | 2.2% | +0.2% |
| local_socket_latency:pingpong_lockless/1024 | Kops/s | 286.9 | 3.9% | 288.5 | 3.4% | +0.6% |
| local_socket_throughput:bidirectional/1024 | MB/s | 687.9 | 0.3% | 694.9 | 0.3% | +1.0% |
| local_socket_throughput:bidirectional/4096 | MB/s | 2,249.9 | 0.4% | 2,277.0 | 0.1% | +1.2% |
| local_socket_throughput:bidirectional/16384 | MB/s | 7,173.7 | 1.0% | 7,258.7 | 0.4% | +1.2% |
| local_socket_throughput:bidirectional/65536 | MB/s | 9,202.5 | 9.1% | 9,679.1 | 3.9% | +5.2% |
| local_socket_throughput:bidirectional/262144 | MB/s | 11,154.4 | 0.8% | 11,267.0 | 0.5% | +1.0% |
| local_socket_throughput:bidirectional/1048576 | MB/s | 11,177.6 | 1.0% | 11,175.0 | 0.9% | -0.0% |
| local_socket_throughput:bidirectional_lockless/1024 | MB/s | 694.1 | 0.9% | 700.2 | 0.8% | +0.9% |
| local_socket_throughput:bidirectional_lockless/4096 | MB/s | 2,249.6 | 0.6% | 2,277.0 | 0.4% | +1.2% |
| local_socket_throughput:bidirectional_lockless/16384 | MB/s | 7,144.9 | 1.0% | 7,265.5 | 0.6% | +1.7% |
| local_socket_throughput:bidirectional_lockless/65536 | MB/s | 9,722.7 | 1.2% | 9,978.9 | 1.1% | +2.6% |
| local_socket_throughput:bidirectional_lockless/262144 | MB/s | 11,128.3 | 0.9% | 11,282.8 | 0.8% | +1.4% |
| local_socket_throughput:bidirectional_lockless/1048576 | MB/s | 11,070.2 | 1.4% | 11,294.3 | 0.5% | +2.0% |
| local_socket_throughput:unidirectional/1024 | MB/s | 667.7 | 0.9% | 671.5 | 0.4% | +0.6% |
| local_socket_throughput:unidirectional/4096 | MB/s | 2,192.1 | 0.7% | 2,212.3 | 0.5% | +0.9% |
| local_socket_throughput:unidirectional/16384 | MB/s | 7,032.4 | 1.2% | 7,040.1 | 0.4% | +0.1% |
| local_socket_throughput:unidirectional/65536 | MB/s | 9,815.6 | 2.3% | 10,032.7 | 0.7% | +2.2% |
| local_socket_throughput:unidirectional/262144 | MB/s | 10,751.0 | 11.9% | 10,426.8 | 6.4% | -3.0% |
| local_socket_throughput:unidirectional/1048576 | MB/s | 10,933.9 | 11.4% | 10,716.8 | 8.9% | -2.0% |
| local_socket_throughput:unidirectional_lockless/1024 | MB/s | 669.4 | 1.0% | 676.2 | 0.4% | +1.0% |
| local_socket_throughput:unidirectional_lockless/4096 | MB/s | 2,204.8 | 0.9% | 2,220.3 | 0.2% | +0.7% |
| local_socket_throughput:unidirectional_lockless/16384 | MB/s | 7,043.2 | 1.2% | 7,087.0 | 0.4% | +0.6% |
| local_socket_throughput:unidirectional_lockless/65536 | MB/s | 9,856.0 | 4.3% | 10,136.9 | 1.6% | +2.9% |
| local_socket_throughput:unidirectional_lockless/262144 | MB/s | 11,011.8 | 6.7% | 10,591.9 | 13.7% | -3.8% |
| local_socket_throughput:unidirectional_lockless/1048576 | MB/s | 11,441.0 | 7.3% | 10,054.0 | 13.7% | -12.1% |
| socket_latency:concurrent/1 | Kops/s | 136.8 | 0.8% | 137.3 | 0.4% | +0.4% |
| socket_latency:concurrent/4 | Kops/s | 141.8 | 0.9% | 142.7 | 0.6% | +0.6% |
| socket_latency:concurrent/16 | Kops/s | 143.1 | 0.9% | 143.8 | 0.5% | +0.5% |
| socket_latency:concurrent_lockless/1 | Kops/s | 137.0 | 0.8% | 137.9 | 0.3% | +0.7% |
| socket_latency:concurrent_lockless/4 | Kops/s | 142.1 | 0.8% | 143.1 | 0.4% | +0.7% |
| socket_latency:concurrent_lockless/16 | Kops/s | 143.2 | 0.9% | 143.9 | 0.4% | +0.5% |
| socket_latency:pingpong/1 | Kops/s | 136.2 | 0.8% | 137.6 | 0.2% | +1.0% |
| socket_latency:pingpong/64 | Kops/s | 135.9 | 0.9% | 137.4 | 0.4% | +1.1% |
| socket_latency:pingpong/1024 | Kops/s | 134.4 | 0.5% | 135.2 | 0.4% | +0.6% |
| socket_latency:pingpong_lockless/1 | Kops/s | 136.9 | 0.8% | 138.0 | 0.4% | +0.8% |
| socket_latency:pingpong_lockless/64 | Kops/s | 137.2 | 0.7% | 137.8 | 0.1% | +0.4% |
| socket_latency:pingpong_lockless/1024 | Kops/s | 135.8 | 0.8% | 135.7 | 0.4% | -0.1% |
| socket_throughput:bidirectional/1024 | MB/s | 278.5 | 0.8% | 280.2 | 0.5% | +0.6% |
| socket_throughput:bidirectional/4096 | MB/s | 1,058.9 | 0.8% | 1,069.3 | 0.6% | +1.0% |
| socket_throughput:bidirectional/16384 | MB/s | 3,727.7 | 0.7% | 3,753.9 | 0.5% | +0.7% |
| socket_throughput:bidirectional/65536 | MB/s | 8,941.4 | 5.2% | 9,049.4 | 5.8% | +1.2% |
| socket_throughput:bidirectional/262144 | MB/s | 10,470.8 | 0.6% | 10,490.2 | 2.6% | +0.2% |
| socket_throughput:bidirectional/1048576 | MB/s | 10,659.1 | 0.8% | 10,716.4 | 0.5% | +0.5% |
| socket_throughput:bidirectional_lockless/1024 | MB/s | 278.8 | 0.8% | 280.3 | 0.4% | +0.5% |
| socket_throughput:bidirectional_lockless/4096 | MB/s | 1,063.6 | 0.8% | 1,066.4 | 0.6% | +0.3% |
| socket_throughput:bidirectional_lockless/16384 | MB/s | 3,733.7 | 0.7% | 3,744.3 | 0.8% | +0.3% |
| socket_throughput:bidirectional_lockless/65536 | MB/s | 9,212.8 | 0.6% | 9,058.1 | 6.2% | -1.7% |
| socket_throughput:bidirectional_lockless/262144 | MB/s | 10,435.4 | 2.3% | 10,326.2 | 1.8% | -1.0% |
| socket_throughput:bidirectional_lockless/1048576 | MB/s | 10,670.0 | 1.2% | 10,773.6 | 1.2% | +1.0% |
| socket_throughput:multithread/2 | MB/s | 9,093.7 | 1.3% | 9,154.7 | 1.0% | +0.7% |
| socket_throughput:multithread/4 | MB/s | 13,323.4 | 1.9% | 13,463.8 | 1.1% | +1.1% |
| socket_throughput:multithread/8 | MB/s | 14,710.8 | 1.1% | 14,791.6 | 1.7% | +0.5% |
| socket_throughput:unidirectional/1024 | MB/s | 255.1 | 0.3% | 256.1 | 0.2% | +0.4% |
| socket_throughput:unidirectional/4096 | MB/s | 969.0 | 0.8% | 978.2 | 0.3% | +1.0% |
| socket_throughput:unidirectional/16384 | MB/s | 3,445.6 | 0.9% | 3,467.1 | 0.7% | +0.6% |
| socket_throughput:unidirectional/65536 | MB/s | 7,934.4 | 2.7% | 8,128.8 | 2.9% | +2.5% |
| socket_throughput:unidirectional/262144 | MB/s | 10,738.0 | 1.2% | 10,791.7 | 0.4% | +0.5% |
| socket_throughput:unidirectional/1048576 | MB/s | 10,743.1 | 1.0% | 10,820.4 | 1.0% | +0.7% |
| socket_throughput:unidirectional_lockless/1024 | MB/s | 254.8 | 0.7% | 256.1 | 0.9% | +0.5% |
| socket_throughput:unidirectional_lockless/4096 | MB/s | 969.1 | 0.8% | 974.5 | 1.4% | +0.6% |
| socket_throughput:unidirectional_lockless/16384 | MB/s | 3,449.8 | 0.6% | 3,469.1 | 0.9% | +0.6% |
| socket_throughput:unidirectional_lockless/65536 | MB/s | 8,119.9 | 3.3% | 7,881.7 | 1.1% | -2.9% |
| socket_throughput:unidirectional_lockless/262144 | MB/s | 10,650.5 | 0.6% | 10,783.0 | 0.9% | +1.2% |
| socket_throughput:unidirectional_lockless/1048576 | MB/s | 10,799.2 | 0.8% | 10,858.4 | 1.6% | +0.5% |
| timer:concurrent/10 | Mops/s | 0.028 | 0.0% | 0.028 | 0.0% | +0.0% |
| timer:concurrent/100 | Mops/s | 0.251 | 0.0% | 0.251 | 0.0% | +0.0% |
| timer:concurrent/1000 | Mops/s | 2.42 | 0.1% | 2.42 | 0.0% | +0.1% |
| timer:fire_rate | Mops/s | 3.49 | 0.7% | 3.52 | 0.6% | +1.1% |
| timer:fire_rate_lockless | Mops/s | 3.60 | 1.7% | 3.64 | 0.9% | +1.1% |
| timer:schedule_cancel | Mops/s | 51.7 | 0.2% | 51.9 | 0.2% | +0.3% |
| timer:schedule_cancel_lockless | Mops/s | 51.8 | 0.4% | 52.0 | 0.3% | +0.5% |
