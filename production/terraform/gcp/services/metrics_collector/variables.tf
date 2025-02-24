/**
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

variable "environment" {
  description = "Assigned environment name to group related resources."
  type        = string
}

variable "collector_ip_address" {
  description = "Collector IP address"
  type        = string
}

variable "collector_instance_groups" {
  description = "OpenTelemetry collector instance group URLs created by instance group managers."
  type        = set(string)
}

variable "collector_service_name" {
  type = string
}

variable "collector_service_port" {
  description = "The grpc port that receives traffic destined for the OpenTelemetry collector."
  type        = number
}

variable "collector_domain_name" {
  description = "The dns domain name for OpenTelemetry collector"
  type        = string
}

variable "dns_zone" {
  description = "Google Cloud DNS zone name for the Key Value domain"
  type        = string
}
