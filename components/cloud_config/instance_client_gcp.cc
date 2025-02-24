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

#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/synchronization/notification.h"
#include "components/cloud_config/instance_client.h"
#include "glog/logging.h"
#include "google/cloud/compute/instances/v1/instances_client.h"
#include "scp/cc/public/core/interface/execution_result.h"
#include "scp/cc/public/cpio/interface/instance_client/instance_client_interface.h"

ABSL_FLAG(std::string, shard_num, "0", "Shard number.");

namespace kv_server {
namespace {
using google::cloud::cpp::compute::instances::v1::
    AggregatedListInstancesRequest;
using google::cmrt::sdk::instance_service::v1::
    GetCurrentInstanceResourceNameRequest;
using google::cmrt::sdk::instance_service::v1::
    GetCurrentInstanceResourceNameResponse;
using google::cmrt::sdk::instance_service::v1::
    GetInstanceDetailsByResourceNameRequest;
using google::cmrt::sdk::instance_service::v1::
    GetInstanceDetailsByResourceNameResponse;
using google::cmrt::sdk::instance_service::v1::
    ListInstanceDetailsByEnvironmentRequest;
using google::cmrt::sdk::instance_service::v1::
    ListInstanceDetailsByEnvironmentResponse;
using ::google::scp::core::ExecutionResult;
using ::google::scp::core::errors::GetErrorMessage;
using google::scp::cpio::InstanceClientInterface;
using google::scp::cpio::InstanceClientOptions;
using privacy_sandbox::server_common::MetricsRecorder;

namespace compute = ::google::cloud::compute_instances_v1;

constexpr std::string_view kEnvironment = "environment";
constexpr std::string_view kShardNumberLabel = "shard-num";

// https://github.com/googleapis/google-cloud-cpp/blob/8234252ab1f661e210ec8773f68cd08d1cfc00d6/protos/google/cloud/compute/v1/internal/common_021.proto#L296
const absl::flat_hash_set<std::string> kInstancePreServiceStatuses = {
    "PROVISIONING",
    "STAGING",
};
const absl::flat_hash_set<std::string> kInstancePostServiceStatuses = {
    "STOPPING", "REPAIRING", "TERMINATED", "SUSPENDING", "SUSPENDED",
};

InstanceServiceStatus GetInstanceServiceStatus(const std::string& status) {
  if (status == "RUNNING") {
    return InstanceServiceStatus::kInService;
  }
  if (kInstancePreServiceStatuses.contains(status)) {
    return InstanceServiceStatus::kPreService;
  }
  if (kInstancePostServiceStatuses.contains(status)) {
    return InstanceServiceStatus::kPostService;
  }
  return InstanceServiceStatus::kUnknown;
}

class GcpInstanceClient : public InstanceClient {
 public:
  GcpInstanceClient()
      : instance_client_(google::scp::cpio::InstanceClientFactory::Create(
            InstanceClientOptions())) {
    instance_client_->Init();
  }

  absl::StatusOr<std::string> GetEnvironmentTag() override {
    if (environment_.empty()) {
      absl::Status result = GetInstanceDetails();
      if (!result.ok()) {
        return result;
      }
    }
    if (environment_.empty()) {
      return absl::UnavailableError("Environment label not found.");
    }
    return environment_;
  }

  absl::StatusOr<std::string> GetShardNumTag() override {
    if (shard_number_.empty()) {
      absl::Status result = GetInstanceDetails();
      if (!result.ok()) {
        return result;
      }
    }
    if (shard_number_.empty()) {
      return absl::UnavailableError("Shard number label not found.");
    }
    return shard_number_;
  }

  absl::Status RecordLifecycleHeartbeat(
      std::string_view lifecycle_hook_name) override {
    LOG(INFO) << "Record lifecycle heartbeat.";
    return absl::OkStatus();
  }

  absl::Status CompleteLifecycle(
      std::string_view lifecycle_hook_name) override {
    LOG(INFO) << "Complete lifecycle.";
    return absl::OkStatus();
  }

  absl::StatusOr<std::string> GetInstanceId() override {
    if (instance_id_.empty()) {
      absl::Status result = GetInstanceDetails();
      if (!result.ok()) {
        return result;
      }
    }
    if (instance_id_.empty()) {
      return absl::UnavailableError("Environment label not found.");
    }
    return instance_id_;
  }

  absl::StatusOr<std::vector<InstanceInfo>> DescribeInstanceGroupInstances(
      DescribeInstanceGroupInput& describe_instance_group_input) override {
    auto input = std::get_if<GcpDescribeInstanceGroupInput>(
        &describe_instance_group_input);
    std::vector<InstanceInfo> instance_infos{};
    CHECK(!environment_.empty())
        << "Environment must be set for the gcp instance client.";
    CHECK(input && !input->project_id.empty()) << "Project id must be set.";
    AggregatedListInstancesRequest request;
    *request.mutable_filter() =
        absl::StrFormat("labels.environment=%s", environment_);
    *request.mutable_project() = input->project_id;
    auto aggregated_instances = client_.AggregatedListInstances(request);
    for (auto& maybe_aggregated_instances_per_zone : aggregated_instances) {
      if (!maybe_aggregated_instances_per_zone.ok()) {
        continue;
      }
      auto aggregated_instances_per_zone =
          std::move(*maybe_aggregated_instances_per_zone);
      for (auto& instance :
           *aggregated_instances_per_zone.second.mutable_instances()) {
        absl::flat_hash_map<std::string, std::string> labels(
            instance.labels().begin(), instance.labels().end());
        InstanceInfo instance_info = {
            .id = std::move(instance.id()),
            .service_status =
                GetInstanceServiceStatus(std::move(instance.status())),
            .labels = std::move(labels),
        };
        for (auto& network : instance.network_interfaces()) {
          instance_info.private_ip_address = std::move(network.network_ip());
          break;
        }
        instance_infos.push_back(std::move(instance_info));
      }
    }
    return instance_infos;
  }

  absl::StatusOr<std::vector<InstanceInfo>> DescribeInstances(
      const absl::flat_hash_set<std::string>& instance_ids) override {
    auto id = GetInstanceId();
    if (!id.ok()) {
      return id.status();
    }
    return std::vector<InstanceInfo>{InstanceInfo{.id = *id}};
  }

 private:
  absl::Status GetInstanceDetails() {
    absl::StatusOr<std::string> resource_name =
        GetResourceName(instance_client_);
    if (!resource_name.ok()) {
      return resource_name.status();
    }

    absl::Notification done;
    GetInstanceDetailsByResourceNameRequest request;
    request.set_instance_resource_name(resource_name.value());

    const auto& result = instance_client_->GetInstanceDetailsByResourceName(
        std::move(request),
        [&done, this](
            const ExecutionResult& result,
            const GetInstanceDetailsByResourceNameResponse& response) {
          if (result.Successful()) {
            VLOG(2) << response.DebugString();
            instance_id_ =
                std::string{response.instance_details().instance_id()};
            environment_ =
                response.instance_details().labels().at(kEnvironment);
            shard_number_ =
                response.instance_details().labels().at(kShardNumberLabel);
          } else {
            LOG(ERROR) << "Failed to get instance details: "
                       << GetErrorMessage(result.status_code);
          }
          done.Notify();
        });
    done.WaitForNotification();
    return result.Successful()
               ? absl::OkStatus()
               : absl::InternalError(GetErrorMessage(result.status_code));
  }

  absl::StatusOr<std::string> GetResourceName(
      std::unique_ptr<InstanceClientInterface>& instance_client) {
    std::string resource_name;
    absl::Notification done;
    const auto& result = instance_client->GetCurrentInstanceResourceName(
        GetCurrentInstanceResourceNameRequest(),
        [&](const ExecutionResult& result,
            const GetCurrentInstanceResourceNameResponse& response) {
          if (result.Successful()) {
            resource_name = std::string{response.instance_resource_name()};
          } else {
            LOG(ERROR) << "Faild to get instance resource name: "
                       << GetErrorMessage(result.status_code);
          }

          done.Notify();
        });
    if (!result.Successful()) {
      return absl::InternalError(GetErrorMessage(result.status_code));
    }
    done.WaitForNotification();
    if (resource_name.empty()) {
      return absl::InternalError("Failed to fetch instance resource name.");
    }
    return resource_name;
  }

  std::string instance_id_;
  std::string environment_;
  std::string shard_number_;
  std::unique_ptr<InstanceClientInterface> instance_client_;
  compute::InstancesClient client_ =
      compute::InstancesClient(compute::MakeInstancesConnectionRest());
};
}  // namespace

std::unique_ptr<InstanceClient> InstanceClient::Create(
    MetricsRecorder& metrics_recorder) {
  return std::make_unique<GcpInstanceClient>();
}
}  // namespace kv_server
