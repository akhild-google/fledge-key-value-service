/*
 * Copyright 2023 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <array>
#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

#include "absl/container/flat_hash_map.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/time/time.h"
#include "benchmark/benchmark.h"
#include "components/data/blob_storage/blob_storage_client.h"
#include "components/data_server/cache/cache.h"
#include "components/data_server/cache/key_value_cache.h"
#include "components/telemetry/telemetry_provider.h"
#include "components/tools/benchmarks/benchmark_util.h"
#include "components/util/platform_initializer.h"
#include "glog/logging.h"
#include "public/data_loading/data_loading_generated.h"
#include "public/data_loading/readers/riegeli_stream_io.h"

ABSL_FLAG(std::string, data_directory, "",
          "Data directory or bucket to store benchmark input data files in.");
ABSL_FLAG(std::string, filename, "",
          "Data file (delta or snapshot) to read as part of the benchmarks.");
ABSL_FLAG(
    bool, create_input_file, false,
    "If true, the input data file used for benchmarking will be created.");
ABSL_FLAG(int64_t, num_records, 100'000,
          "Number of records in data file when '--create_input_file' "
          "is true.");
ABSL_FLAG(int64_t, record_size, 10 * 1024,
          "Size of reach record in data file when '--create_input_file' "
          "is true.");
ABSL_FLAG(std::vector<std::string>, args_reader_worker_threads,
          std::vector<std::string>({"16"}),
          "A list of num of worker threads to use for concurrent reading.");
ABSL_FLAG(std::vector<std::string>, args_client_max_connections,
          std::vector<std::string>({"32"}),
          "Maximum number of connections to use for reading blobs. Ignored for "
          "local platform.");
ABSL_FLAG(
    std::vector<std::string>, args_client_max_range_mb,
    std::vector<std::string>({"8"}),
    "Chunk size to use when reading blobs in mbs. Ignored for local platform.");
ABSL_FLAG(int64_t, args_benchmark_iterations, -1,
          "Number of iterations to run each benchmark.");

using kv_server::BlobReader;
using kv_server::BlobStorageClient;
using kv_server::Cache;
using kv_server::ConcurrentStreamRecordReader;
using kv_server::DeltaFileRecord;
using kv_server::DeltaMutationType;
using kv_server::KeyValueCache;
using kv_server::RecordStream;
using kv_server::TelemetryProvider;
using kv_server::benchmark::ParseInt64List;
using kv_server::benchmark::WriteRecords;

constexpr std::string_view kNoOpCacheNameFormat =
    "BM_DataLoading_NoOpCache/tds:%d/conns:%d/buf:%d";
constexpr std::string_view kMutexCacheNameFormat =
    "BM_DataLoading_MutexCache/tds:%d/conns:%d/buf:%d";

// Args config for benchmarks.
struct BenchmarkArgs {
  int64_t reader_worker_threads;
  int64_t client_max_connections;
  int64_t client_max_range_mb;
  std::function<std::unique_ptr<Cache>()> create_cache_fn;
};

// Wraps an io stream so that it can used as a blob reader.
class StreamBlobReader : public BlobReader {
 public:
  explicit StreamBlobReader(std::iostream& stream) : stream_(stream) {}
  std::istream& Stream() override { return stream_; }
  bool CanSeek() const override { return stream_.tellg() != -1; }

 private:
  std::iostream& stream_;
};

// Wraps a blob reader so that it can be used as record stream for the
// concurrent reader.
class BlobRecordStream : public RecordStream {
 public:
  explicit BlobRecordStream(std::unique_ptr<BlobReader> blob_reader)
      : blob_reader_(std::move(blob_reader)) {}
  std::istream& Stream() { return blob_reader_->Stream(); }

 private:
  std::unique_ptr<BlobReader> blob_reader_;
};

// Implements a noop cache.
class NoOpCache : public Cache {
 public:
  absl::flat_hash_map<std::string, std::string> GetKeyValuePairs(
      const std::vector<std::string_view>& key_list) const override {
    return {};
  };
  void UpdateKeyValue(std::string_view key, std::string_view value,
                      int64_t logical_commit_time) override {}
  void DeleteKey(std::string_view key, int64_t logical_commit_time) override {}
  void RemoveDeletedKeys(int64_t logical_commit_time) override {}
  static std::unique_ptr<Cache> Create() {
    return std::make_unique<NoOpCache>();
  }
};

BlobStorageClient::DataLocation GetBlobLocation() {
  return BlobStorageClient::DataLocation{
      .bucket = absl::GetFlag(FLAGS_data_directory),
      .key = absl::GetFlag(FLAGS_filename),
  };
}

int64_t GetBlobSize(BlobStorageClient& blob_client,
                    BlobStorageClient::DataLocation blob) {
  auto blob_reader = blob_client.GetBlobReader(blob);
  auto& stream = blob_reader->Stream();
  stream.seekg(0, std::ios_base::end);
  return stream.tellg();
}

void BM_LoadDataIntoCache(benchmark::State& state, BenchmarkArgs args);

void RegisterBenchmark(std::string_view benchmark_name, BenchmarkArgs args) {
  auto b = benchmark::RegisterBenchmark(benchmark_name.data(),
                                        BM_LoadDataIntoCache, args);
  b->MeasureProcessCPUTime();
  b->UseRealTime();
  if (absl::GetFlag(FLAGS_args_benchmark_iterations) > 0) {
    b->Iterations(absl::GetFlag(FLAGS_args_benchmark_iterations));
  }
}

// Registers benchmark
void RegisterBenchmarks() {
  auto num_worker_threads =
      ParseInt64List(absl::GetFlag(FLAGS_args_reader_worker_threads));
  auto client_max_conns =
      ParseInt64List(absl::GetFlag(FLAGS_args_client_max_connections));
  auto client_max_range_mb =
      ParseInt64List(absl::GetFlag(FLAGS_args_client_max_range_mb));
  for (const int64_t byte_range_mb : client_max_range_mb.value()) {
    for (const int64_t num_connections : client_max_conns.value()) {
      for (const int64_t num_threads : num_worker_threads.value()) {
        auto args = BenchmarkArgs{
            .reader_worker_threads = num_threads,
            .client_max_connections = num_connections,
            .client_max_range_mb = byte_range_mb,
            .create_cache_fn = []() { return NoOpCache::Create(); },
        };
        RegisterBenchmark(absl::StrFormat(kNoOpCacheNameFormat, num_threads,
                                          num_connections, byte_range_mb),
                          args);
        args.create_cache_fn = []() { return KeyValueCache::Create(); };
        RegisterBenchmark(absl::StrFormat(kMutexCacheNameFormat, num_threads,
                                          num_connections, byte_range_mb),
                          args);
      }
    }
  }
}

void BM_LoadDataIntoCache(benchmark::State& state, BenchmarkArgs args) {
  BlobStorageClient::ClientOptions options;
  options.max_range_bytes = args.client_max_range_mb * 1024 * 1024;
  options.max_connections = args.client_max_connections;
  auto noop_metrics_recorder =
      TelemetryProvider::GetInstance().CreateMetricsRecorder();
  auto blob_client = BlobStorageClient::Create(*noop_metrics_recorder, options);
  ConcurrentStreamRecordReader<std::string_view> record_reader(
      *noop_metrics_recorder,
      /*stream_factory=*/
      [blob_client = blob_client.get()]() {
        return std::make_unique<BlobRecordStream>(
            blob_client->GetBlobReader(GetBlobLocation()));
      },
      /*options=*/
      {
          .num_worker_threads = args.reader_worker_threads,
      });
  auto stream_size = GetBlobSize(*blob_client, GetBlobLocation());
  std::atomic<int64_t> num_records_read{0};
  for (auto _ : state) {
    state.PauseTiming();
    auto cache = args.create_cache_fn();
    state.ResumeTiming();
    auto status = record_reader.ReadStreamRecords(
        [&num_records_read, cache = cache.get()](std::string_view raw) {
          num_records_read++;
          auto record = flatbuffers::GetRoot<DeltaFileRecord>(raw.data());
          auto recordVerifier = flatbuffers::Verifier(
              reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
          if (!record->Verify(recordVerifier)) {
            return absl::InvalidArgumentError("Invalid flatbuffer format");
          }
          switch (record->mutation_type()) {
            case DeltaMutationType::Update: {
              cache->UpdateKeyValue(record->key()->string_view(),
                                    record->value()->string_view(),
                                    record->logical_commit_time());
              break;
            }
            case DeltaMutationType::Delete: {
              cache->DeleteKey(record->key()->string_view(),
                               record->logical_commit_time());
              break;
            }
            default:
              return absl::InvalidArgumentError(absl::StrCat(
                  "Invalid mutation type: ",
                  EnumNameDeltaMutationType(record->mutation_type())));
          }
          return absl::OkStatus();
        });
    benchmark::DoNotOptimize(status);
  }
  state.SetItemsProcessed(num_records_read);
  state.SetBytesProcessed(stream_size *
                          static_cast<int64_t>(state.iterations()));
}

// Sample usage:
//
// GLOG_logtostderr=1 bazel run \
//  components/tools/benchmarks:data_loading_benchmark \
//    --//:instance=local --//:platform=local -- \
//    --benchmark_time_unit=ms \
//    --benchmark_counters_tabular=true \
//    --data_directory=/tmp/data \
//    --filename=DELTA_10000000000001 \
//    --create_input_file \
//    --num_records=1000000 \
//    --record_size=1000 \
//    --args_client_max_range_mb=8 \
//    --args_client_max_connections=64 \
//    --args_reader_worker_threads=16,32,64
int main(int argc, char** argv) {
  ::kv_server::PlatformInitializer platform_initializer;
  google::InitGoogleLogging(argv[0]);
  ::benchmark::Initialize(&argc, argv);
  absl::ParseCommandLine(argc, argv);
  if (absl::GetFlag(FLAGS_data_directory).empty()) {
    LOG(ERROR) << "Flag '--data_directory' must be set.";
    return -1;
  }
  if (absl::GetFlag(FLAGS_filename).empty()) {
    LOG(ERROR) << "Flag '--filename' must be not empty.";
    return -1;
  }
  auto noop_metrics_recorder =
      TelemetryProvider::GetInstance().CreateMetricsRecorder();
  auto blob_client = BlobStorageClient::Create(*noop_metrics_recorder);
  if (absl::GetFlag(FLAGS_create_input_file)) {
    LOG(INFO) << "Creating input file: " << GetBlobLocation();
    std::stringstream data_stream;
    if (auto status =
            WriteRecords(absl::GetFlag(FLAGS_num_records),
                         absl::GetFlag(FLAGS_record_size), data_stream);
        !status.ok()) {
      LOG(ERROR) << "Failed to write records for data file. " << status;
      return -1;
    }
    StreamBlobReader blob_reader(data_stream);
    if (auto status = blob_client->PutBlob(blob_reader, GetBlobLocation());
        !status.ok()) {
      LOG(ERROR) << "Failed to write data file. " << status;
      return -1;
    }
    LOG(INFO) << "Done creating input file: " << GetBlobLocation();
  }
  RegisterBenchmarks();
  ::benchmark::RunSpecifiedBenchmarks();
  ::benchmark::Shutdown();
  if (absl::GetFlag(FLAGS_create_input_file)) {
    LOG(INFO) << "Deleting input file: " << GetBlobLocation();
    if (auto status = blob_client->DeleteBlob(GetBlobLocation());
        !status.ok()) {
      LOG(ERROR) << "Failed to write data file. " << status;
      return -1;
    }
    LOG(INFO) << "Done deleting input file: " << GetBlobLocation();
  }
  return 0;
}
