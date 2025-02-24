// Copyright 2023 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <mutex>

#include "absl/status/status.h"
#include "absl/strings/escaping.h"
#include "components/data/common/msg_svc.h"
#include "components/data/common/thread_manager.h"
#include "components/data/realtime/realtime_notifier.h"
#include "glog/logging.h"
#include "google/cloud/pubsub/message.h"
#include "google/cloud/pubsub/subscriber.h"
#include "src/cpp/telemetry/metrics_recorder.h"
#include "src/cpp/telemetry/telemetry.h"

namespace kv_server {
namespace {
namespace pubsub = ::google::cloud::pubsub;
namespace cloud = ::google::cloud;
using ::google::cloud::future;
using ::google::cloud::GrpcBackgroundThreadPoolSizeOption;
using ::google::cloud::Options;
using ::google::cloud::pubsub::Subscriber;
using ::privacy_sandbox::server_common::MetricsRecorder;
using privacy_sandbox::server_common::ScopeLatencyRecorder;

constexpr char* kReceivedLowLatencyNotifications =
    "ReceivedLowLatencyNotifications";
constexpr char* kReceivedLowLatencyNotificationsE2E =
    "ReceivedLowLatencyNotificationsE2E";
constexpr char* kReceivedLowLatencyNotificationsE2EGcpProvided =
    "ReceivedLowLatencyNotificationsE2EGcpProvided";
constexpr char* kRealtimeDecodeRealtimeMessageFailure =
    "RealtimeDecodeRealtimeMessageFailure";
constexpr char* kRealtimeRealtimeMessageApplicationFailure =
    "kRealtimeRealtimeMessageApplicationFailure";
constexpr char* kRealtimeTotalRowsUpdated = "RealtimeTotalRowsUpdated";

// The units below are microseconds.
const std::vector<double> kE2eBucketBoundaries = {
    160,     220,       280,       320,       640,       1'200,         2'500,
    5'000,   10'000,    20'000,    40'000,    80'000,    160'000,       320'000,
    640'000, 1'000'000, 1'300'000, 2'600'000, 5'000'000, 10'000'000'000};

class RealtimeNotifierGcp : public RealtimeNotifier {
 public:
  explicit RealtimeNotifierGcp(MetricsRecorder& metrics_recorder,
                               std::unique_ptr<Subscriber> gcp_subscriber,
                               std::unique_ptr<SleepFor> sleep_for)
      : thread_manager_(TheadManager::Create("Realtime notifier")),
        metrics_recorder_(metrics_recorder),
        sleep_for_(std::move(sleep_for)),
        gcp_subscriber_(std::move(gcp_subscriber)) {
    metrics_recorder.RegisterHistogram(kReceivedLowLatencyNotificationsE2E,
                                       "Low latency notifictionas E2E latency",
                                       "microsecond", kE2eBucketBoundaries);
    metrics_recorder.RegisterHistogram(
        kReceivedLowLatencyNotificationsE2EGcpProvided,
        "Low latency notifications E2E latency gcp supplied", "microsecond",
        kE2eBucketBoundaries);
  }

  ~RealtimeNotifierGcp() {
    if (const auto s = Stop(); !s.ok()) {
      LOG(ERROR) << "Realtime updater failed to stop: " << s;
    }
  }

  absl::Status Start(
      std::function<absl::StatusOr<DataLoadingStats>(const std::string& key)>
          callback) override {
    return thread_manager_->Start(
        [this, callback = std::move(callback)]() mutable {
          Watch(std::move(callback));
        });
  }

  absl::Status Stop() override {
    absl::Status status;
    LOG(INFO) << "Realtime updater received stop signal.";
    {
      absl::MutexLock lock(&mutex_);
      if (session_.valid()) {
        VLOG(8) << "Session valid.";
        session_.cancel();
        VLOG(8) << "Session cancelled.";
      }
      status = sleep_for_->Stop();
      VLOG(8) << "Sleep for just called stop.";
    }
    status.Update(thread_manager_->Stop());
    LOG(INFO) << "Thread manager just called stop.";
    return status;
  }

  bool IsRunning() const override { return thread_manager_->IsRunning(); }

 private:
  void RecordGcpSuppliedE2ELatency(pubsub::Message const& m) {
    // The time at which the message was published, populated by the server when
    // it receives the topics.publish call. It must not be populated by the
    // publisher in a topics.publish call.
    metrics_recorder_.RecordHistogramEvent(
        kReceivedLowLatencyNotificationsE2EGcpProvided,
        absl::ToInt64Microseconds(absl::Now() -
                                  absl::FromChrono(m.publish_time())));
  }

  void RecordProducerSuppliedE2ELatency(pubsub::Message const& m) {
    auto attributes = m.attributes();
    auto it = attributes.find("time_sent");
    if (it == attributes.end() || it->second.empty()) {
      return;
    }
    int64_t value_int64;
    if (!absl::SimpleAtoi(it->second, &value_int64)) {
      return;
    }
    auto e2eDuration = absl::Now() - absl::FromUnixNanos(value_int64);
    metrics_recorder_.RecordHistogramEvent(
        kReceivedLowLatencyNotificationsE2E,
        absl::ToInt64Microseconds(e2eDuration));
  }

  void OnMessageReceived(
      pubsub::Message const& m, pubsub::AckHandler h,
      std::function<absl::StatusOr<DataLoadingStats>(const std::string& key)>&
          callback) {
    ScopeLatencyRecorder latency_recorder(
        std::string(kReceivedLowLatencyNotifications), metrics_recorder_);
    std::string string_decoded;
    if (!absl::Base64Unescape(m.data(), &string_decoded)) {
      metrics_recorder_.IncrementEventCounter(
          kRealtimeDecodeRealtimeMessageFailure);
      LOG(ERROR) << "The body of the message is not a base64 encoded string.";
      std::move(h).ack();
      return;
    }
    auto count = callback(string_decoded);
    if (count.ok()) {
      metrics_recorder_.IncrementEventStatus(
          kRealtimeTotalRowsUpdated, count.status(),
          (count->total_updated_records + count->total_deleted_records));
    } else {
      metrics_recorder_.IncrementEventCounter(
          kRealtimeRealtimeMessageApplicationFailure);
    }
    RecordGcpSuppliedE2ELatency(m);
    RecordProducerSuppliedE2ELatency(m);
    std::move(h).ack();
  }

  void Watch(
      std::function<absl::StatusOr<DataLoadingStats>(const std::string& key)>
          callback) {
    {
      absl::MutexLock lock(&mutex_);
      session_ = gcp_subscriber_->Subscribe(
          [&](pubsub::Message const& m, pubsub::AckHandler h) {
            OnMessageReceived(m, std::move(h), callback);
          });
    }
    LOG(INFO) << "Realtime updater initialized.";
    sleep_for_->Duration(absl::InfiniteDuration());
    LOG(INFO) << "Realtime updater stopped watching.";
  }

  std::unique_ptr<TheadManager> thread_manager_;
  MetricsRecorder& metrics_recorder_;
  mutable absl::Mutex mutex_;
  future<cloud::Status> session_ ABSL_GUARDED_BY(mutex_);
  std::unique_ptr<SleepFor> sleep_for_;
  std::unique_ptr<Subscriber> gcp_subscriber_;
};

absl::StatusOr<std::unique_ptr<Subscriber>> CreateSubscriber(
    NotifierMetadata metadata) {
  GcpNotifierMetadata notifier_metadata =
      std::get<GcpNotifierMetadata>(metadata);
  auto realtime_message_service_status =
      MessageService::Create(notifier_metadata);
  if (!realtime_message_service_status.ok()) {
    return realtime_message_service_status.status();
  }
  auto realtime_message_service = std::move(*realtime_message_service_status);
  auto queue_setup_result = realtime_message_service->SetupQueue();
  if (!queue_setup_result.ok()) {
    return queue_setup_result;
  }
  auto queue_metadata =
      std::get<GcpQueueMetadata>(realtime_message_service->GetQueueMetadata());
  LOG(INFO) << "Listening to queue_id " << queue_metadata.queue_id
            << " project id " << notifier_metadata.project_id << " with "
            << notifier_metadata.num_threads << " threads.";
  return std::make_unique<Subscriber>(pubsub::MakeSubscriberConnection(
      pubsub::Subscription(notifier_metadata.project_id,
                           queue_metadata.queue_id),
      Options{}
          .set<pubsub::MaxConcurrencyOption>(notifier_metadata.num_threads)
          .set<GrpcBackgroundThreadPoolSizeOption>(
              notifier_metadata.num_threads)));
}
}  // namespace

absl::StatusOr<std::unique_ptr<RealtimeNotifier>> RealtimeNotifier::Create(
    MetricsRecorder& metrics_recorder, NotifierMetadata metadata,
    RealtimeNotifierMetadata realtime_metadata) {
  auto realtime_notifier_metadata =
      std::get_if<GcpRealtimeNotifierMetadata>(&realtime_metadata);
  std::unique_ptr<SleepFor> sleep_for;
  if (realtime_notifier_metadata &&
      realtime_notifier_metadata->maybe_sleep_for) {
    sleep_for = std::move(realtime_notifier_metadata->maybe_sleep_for);
  } else {
    sleep_for = std::make_unique<SleepFor>();
  }
  std::unique_ptr<Subscriber> gcp_subscriber;
  if (realtime_notifier_metadata &&
      realtime_notifier_metadata->gcp_subscriber_for_unit_testing) {
    gcp_subscriber.reset(
        realtime_notifier_metadata->gcp_subscriber_for_unit_testing);
  } else {
    auto maybe_gcp_subscriber = CreateSubscriber(metadata);
    if (!maybe_gcp_subscriber.ok()) {
      return maybe_gcp_subscriber.status();
    }
    gcp_subscriber = std::move(*maybe_gcp_subscriber);
  }
  return std::make_unique<RealtimeNotifierGcp>(
      metrics_recorder, std::move(gcp_subscriber), std::move(sleep_for));
}

}  // namespace kv_server
