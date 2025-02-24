// Copyright 2022 Google LLC
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

// TODO(b/296901861): Modify the implementation with GCP specific logic (the
// current implementation is copied from local).

#include "components/util/platform_initializer.h"
#include "glog/logging.h"
#include "public/cpio/interface/cpio.h"
#include "scp/cc/public/core/interface/errors.h"
#include "scp/cc/public/core/interface/execution_result.h"

namespace kv_server {
namespace {
using google::scp::core::GetErrorMessage;
using google::scp::cpio::Cpio;
using google::scp::cpio::CpioOptions;
using google::scp::cpio::LogOption;
google::scp::cpio::CpioOptions cpio_options_;
}  // namespace

PlatformInitializer::PlatformInitializer() {
  cpio_options_.log_option = LogOption::kConsoleLog;
  auto execution_result = Cpio::InitCpio(cpio_options_);
  CHECK(execution_result.Successful())
      << "Failed to initialize CPIO: "
      << GetErrorMessage(execution_result.status_code);
}

PlatformInitializer::~PlatformInitializer() {
  auto execution_result = Cpio::ShutdownCpio(cpio_options_);
  if (!execution_result.Successful()) {
    LOG(ERROR) << "Failed to shutdown CPIO: "
               << GetErrorMessage(execution_result.status_code);
  }
}
}  // namespace kv_server
