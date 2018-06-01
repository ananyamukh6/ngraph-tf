/*******************************************************************************
 * Copyright 2017-2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *******************************************************************************/
#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "tensorflow/core/common_runtime/optimization_registry.h"
#include "tensorflow/core/framework/attr_value_util.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/graph/graph.h"
#include "tensorflow/core/platform/default/logging.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/util/device_name_utils.h"

#include "ngraph_cluster.h"
#include "ngraph_cluster_manager.h"
#include "ngraph_utils.h"
#include "tf_graphcycles.h"

using namespace std;
namespace ngraph_bridge {

extern const char* const DEVICE_NGRAPH_CPU;

#define MINIMUM_CLUSTER_NODES 2

tf::Status NGraphClusterPass::Run(const tf::GraphOptimizationPassOptions& options) {
  // TODO(amprocte): Remove this when we have proper support for graphs with
  // cycles.
  if (std::getenv("NGRAPH_TF_SKIP_CLUSTERING") != nullptr) {
    return tf::Status::OK();
  }

  if (std::getenv("NGRAPH_TF_CLUSTER_BY_OP_NAME") != nullptr) {
    VLOG(0) << "NGRAPH_TF_CLUSTER_BY_OP_NAME is set. This mode is "
               "experimental and unlikely to work.";
  }

  tf::Graph* graph = options.graph->get();

  TF_RETURN_IF_ERROR(IdentifyClusters(graph));

  return tf::Status::OK();
}

// TODO(amprocte): do we need to look at job name, replica, task?
bool NGraphClusterPass::IsNGraphNode(const tf::Node* node) {
  if (std::getenv("NGRAPH_TF_CLUSTER_BY_OP_NAME") != nullptr) {
    if (tf::str_util::StartsWith(node->name(), "accuracy/")) {
      return false;
    }

    // clang-format off
    static std::set<std::string> supported_ops{
        "Add",
        "AddN",
        "ArgMax",
        "AvgPool",
        "AvgPoolGrad",
        "BiasAdd",
        "BiasAddGrad",
        "BroadcastGradientArgs",
        "Cast",
        "ConcatV2",
        "Const",
        "Conv2D",
        "Conv2DBackpropFilter",
        "Conv2DBackpropInput",
        "Equal",
        "ExpandDims",
        "Fill",
        "FusedBatchNorm",
        "FusedBatchNormGrad",
        "Greater",
        "Identity",
        "L2Loss",
        "LessEqual",
        "LogicalAnd",
        "MatMul",
        "MaxPool",
        "MaxPoolGrad",
        "Mean",
        "Mul",
        "NoOp",
        "Pad",
        "Prod",
        "RealDiv",
        "Relu",
        "ReluGrad",
        "ReluGrad",
        "Reshape",
        "Select",
        "Shape",
        "ShapeN",
        "Slice",
        "Snapshot",
        "SoftmaxCrossEntropyWithLogits",
        "Sub",
        "Sum",
        "Tile",
    };
    // clang-format on

    return (supported_ops.count(node->type_string()) > 0);
  } else {
    tf::DeviceNameUtils::ParsedName parsed;

    if (!tf::DeviceNameUtils::ParseFullName(node->assigned_device_name(),
                                            &parsed)) {
      return false;
    }

    return (parsed.has_type && parsed.type == DEVICE_NGRAPH_CPU);
  }
}

bool NGraphClusterPass::IsClusterable(const tf::Node* node) {
  return (s_unclusterable_ops.count(node->type_string()) == 0);
}

bool NGraphClusterPass::CanBeOutsideCluster(const tf::Node* node) {
  return (!IsClusterable(node) || s_can_be_outside_cluster_ops.count(node->type_string()) > 0);
}

tf::Status NGraphClusterPass::IdentifyClusters(tf::Graph* graph) {
  std::map<tf::Node*, std::shared_ptr<Cluster>> cluster_map;

  tf::GraphCycles gc;

  for (auto node : graph->op_nodes()) {
    int new_index = gc.NewNode();
    cluster_map[node] = std::make_shared<Cluster>();
    cluster_map[node]->index = new_index;
    cluster_map[node]->nodes.insert(node);
  }

  for (auto edge : graph->edges()) {
    tf::Node* src = edge->src();
    tf::Node* dst = edge->dst();

    // Skip source/sink
    if (!src->IsOp() || !dst->IsOp()) {
      continue;
    }

    if (!gc.InsertEdge(cluster_map[src]->index, cluster_map[dst]->index)) {
      return tf::errors::InvalidArgument(
          "Input graph has a cycle (inserting an edge from ",
          src->DebugString(), " to ", dst->DebugString(),
          " would create a cycle)");
    }
  }

  bool changed;

  do {
    changed = false;

    for (auto edge : graph->edges()) {
      tf::Node* src = edge->src();
      tf::Node* dst = edge->dst();

      if (!src->IsOp() || !dst->IsOp()) {
        continue;
      }

      if (!IsNGraphNode(src) || !IsNGraphNode(dst) || !IsClusterable(src) || !IsClusterable(dst)) {
        continue;
      }

      int src_index = cluster_map[src]->index;
      int dst_index = cluster_map[dst]->index;

      if (gc.HasEdge(src_index, dst_index) &&
          gc.ContractEdge(src_index, dst_index)) {
        for (auto node : cluster_map[dst]->nodes) {
          cluster_map[src]->nodes.insert(node);
          cluster_map[node] = cluster_map[src];
        }
        changed = true;
      }
    }
  } while (changed);

  std::set<Cluster*> seen;

  for (auto kv : cluster_map) {
    auto cluster = kv.second.get();
    bool has_clusterable_ngraph_ops = false;
    bool all_can_be_outside_cluster = true;

    for (auto node : cluster->nodes) {
      if (IsNGraphNode(node) && IsClusterable(node)) {
        has_clusterable_ngraph_ops = true;
        break;
      }
    }

    for (auto node : cluster->nodes) {
      if (IsNGraphNode(node) && !CanBeOutsideCluster(node)) {
        all_can_be_outside_cluster = false;
        break;
      }
    }

    if (!has_clusterable_ngraph_ops || all_can_be_outside_cluster) {
      continue;
    }

    if (seen.count(cluster) == 0) {
      int cluster_idx = NGraphClusterManager::NewCluster();

      bool is_trivial = cluster->nodes.size() < MINIMUM_CLUSTER_NODES;

      seen.insert(cluster);
      VLOG(0) << "cluster " << cluster_idx << ": " << cluster->nodes.size()
              << " nodes" << (is_trivial ? " (trivial)" : "");

      for (auto node : cluster->nodes) {
        if (!IsNGraphNode(node)) {
          return tf::errors::InvalidArgument(
              "Node ", node->DebugString(),
              " is not an nGraph node but was placed in an nGraph cluster.");
        }

        if (!IsClusterable(node)) {
          return tf::errors::InvalidArgument(
              "Node ", node->DebugString(),
              " is not a clusterable node but was placed in an nGraph cluster.");
        }

        VLOG(0) << ">> cluster " << cluster_idx << ": " << node
                << " :: " << node->name() << " [" << node->type_string()
                << "]";

        node->AddAttr("_ngraph_cluster", cluster_idx);
        if (is_trivial) {
          node->AddAttr("_ngraph_cluster_is_trivial", is_trivial);
        }
      }
    }
  }

  return tf::Status::OK();
}

// clang-format off
const std::set<std::string> NGraphClusterPass::s_unclusterable_ops{
    "Assign",
    "IsVariableInitialized",
    "VariableV2",
};
// clang-format on

// clang-format off
const std::set<std::string> NGraphClusterPass::s_can_be_outside_cluster_ops{
    "Const",
    "Identity",
    "NoOp",
};
// clang-format on

}  // namespace ngraph_bridge

namespace tensorflow {
REGISTER_OPTIMIZATION(OptimizationPassRegistry::POST_REWRITE_FOR_EXEC, 105,
                      ngraph_bridge::NGraphClusterPass);
}  // namespace tensorflow

