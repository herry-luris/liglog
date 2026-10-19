#!/bin/bash
set -e

BUILD=build
OUT=results
mkdir -p $OUT

# 每个场景独立跑，避免全局状态污染
LIG_SCENES="BM_FrontendSuppressed|BM_FrontendEnabled_NoArgs|BM_FrontendEnabled_IntArg|BM_FrontendEnabled_MixedArgs|BM_SyncTransportFrom|BM_AsyncTransportFrom|BM_AsyncTransportFromFlush|BM_EndToEnd_Sync_Minimal|BM_EndToEnd_Async|BM_EndToEnd_Sync_FileFull|BM_MixedWorkload|BM_TextFormatter_Full|BM_TextFormatter_Minimal|BM_DataBufferPush|BM_FileSink_Write|BM_MultiThread_Async|BM_MultiThread_Sync"

SPD_SCENES="BM_Spdlog_FrontendSuppressed|BM_Spdlog_FrontendEnabled_NoArgs|BM_Spdlog_FrontendEnabled_IntArg|BM_Spdlog_FrontendEnabled_MixedArgs|BM_Spdlog_TransportFrom_Sync|BM_Spdlog_TransportFrom_Async|BM_Spdlog_TransportFromFlush|BM_Spdlog_EndToEnd_Sync_Minimal|BM_Spdlog_EndToEnd_Sync_FileFull|BM_Spdlog_MixedWorkload|BM_Spdlog_TextFormatter_Full|BM_Spdlog_TextFormatter_Minimal|BM_Spdlog_MultiThread_Async|BM_Spdlog_MultiThread_Sync"

rm -rf /tmp/lig_bench /tmp/spdlog_bench

./$BUILD/benchmarks/bench_liglog \
    --benchmark_filter="$LIG_SCENES" \
    --benchmark_out=$OUT/liglog.json \
    --benchmark_out_format=json

./$BUILD/benchmarks/bench_spdlog \
    --benchmark_filter="$SPD_SCENES" \
    --benchmark_out=$OUT/spdlog.json \
    --benchmark_out_format=json

python3 compare.py $OUT/liglog.json $OUT/spdlog.json
