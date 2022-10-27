/* -----------------------------------------------------------------------------
 * Copyright 2022 Massachusetts Institute of Technology.
 * All Rights Reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Research was sponsored by the United States Air Force Research Laboratory and
 * the United States Air Force Artificial Intelligence Accelerator and was
 * accomplished under Cooperative Agreement Number FA8750-19-2-1000. The views
 * and conclusions contained in this document are those of the authors and should
 * not be interpreted as representing the official policies, either expressed or
 * implied, of the United States Air Force or the U.S. Government. The U.S.
 * Government is authorized to reproduce and distribute reprints for Government
 * purposes notwithstanding any copyright notation herein.
 * -------------------------------------------------------------------------- */
#include "hydra_dsg_builder/incremental_mesh_segmenter.h"

#include <pcl/segmentation/extract_clusters.h>
#include <spark_dsg/bounding_box_extraction.h>

#include <glog/logging.h>

namespace hydra {
namespace incremental {

using kimera::HashableColor;
using kimera::SemanticLabel2Color;
using Clusters = MeshSegmenter::Clusters;
using LabelClusters = MeshSegmenter::LabelClusters;
using LabelIndices = MeshSegmenter::LabelIndices;
using OptPosition = std::optional<Eigen::Vector3d>;

std::ostream& operator<<(std::ostream& out, const std::set<uint8_t>& labels) {
  out << "[";
  auto iter = labels.begin();
  while (iter != labels.end()) {
    out << static_cast<int>(*iter);
    ++iter;
    if (iter != labels.end()) {
      out << ", ";
    }
  }
  out << "]";
  return out;
}

std::ostream& operator<<(std::ostream& out, const HashableColor& color) {
  return out << "[" << static_cast<int>(color.r) << ", " << static_cast<int>(color.g)
             << ", " << static_cast<int>(color.b) << ", " << static_cast<int>(color.a)
             << "]";
}

inline bool objectsMatch(const Cluster& cluster, const SceneGraphNode& node) {
  pcl::PointXYZ centroid;
  cluster.centroid.get(centroid);

  Eigen::Vector3f point;
  point << centroid.x, centroid.y, centroid.z;

  return node.attributes<ObjectNodeAttributes>().bounding_box.isInside(point);
}

MeshSegmenter::MeshSegmenter(const MeshSegmenterConfig& config,
                             const MeshVertexCloud::Ptr& vertices)
    : full_mesh_vertices_(vertices), config_(config), next_node_id_(config.prefix, 0) {
  VLOG(1) << "[Hydra Frontend] Detecting objects for labels: " << config.labels;
  for (const auto& label : config.labels) {
    active_objects_[label] = std::set<NodeId>();
  }
}

using KdTreeT = pcl::search::KdTree<pcl::PointXYZRGBA>;

Clusters MeshSegmenter::findClusters(const MeshVertexCloud::Ptr& cloud,
                                     const std::vector<size_t>& indices) const {
  // in general, this is unsafe, but PCL doesn't offer us any alternative
  pcl::IndicesPtr cloud_indices(new std::vector<int>(indices.begin(), indices.end()));

  KdTreeT::Ptr tree(new KdTreeT());
  tree->setInputCloud(cloud, cloud_indices);

  pcl::EuclideanClusterExtraction<Cluster::PointT> estimator;
  estimator.setClusterTolerance(config_.cluster_tolerance);
  estimator.setMinClusterSize(config_.min_cluster_size);
  estimator.setMaxClusterSize(config_.max_cluster_size);
  estimator.setSearchMethod(tree);
  estimator.setInputCloud(cloud);
  estimator.setIndices(cloud_indices);

  std::vector<pcl::PointIndices> cluster_indices;
  estimator.extract(cluster_indices);

  Clusters clusters;
  clusters.resize(cluster_indices.size());
  for (size_t k = 0; k < clusters.size(); ++k) {
    clusters.at(k).indices = cluster_indices.at(k);
    clusters.at(k).cloud.reset(new MeshVertexCloud());

    const auto& object_indices = cluster_indices.at(k).indices;
    clusters.at(k).cloud->resize(object_indices.size());

    for (size_t i = 0; i < object_indices.size(); ++i) {
      const auto& cp = cloud->at(object_indices.at(i));
      clusters.at(k).cloud->at(i) = cp;
      clusters.at(k).centroid.add(pcl::PointXYZ(cp.x, cp.y, cp.z));
    }
  }

  return clusters;
}

std::vector<size_t> MeshSegmenter::getActiveIndices(const std::vector<size_t>& indices,
                                                    const OptPosition& pos) const {
  std::vector<size_t> active_indices;
  if (!pos) {
    active_indices = indices;
  } else {
    active_indices.reserve(indices.size());
    const Eigen::Vector3d root_pos = *pos;
    for (const size_t idx : indices) {
      const auto& p = full_mesh_vertices_->at(idx);
      const Eigen::Vector3d vertex_pos(p.x, p.y, p.z);
      if ((vertex_pos - root_pos).norm() < config_.active_index_horizon_m) {
        active_indices.push_back(idx);
      }
    }
  }
  VLOG(1) << "active indices: " << indices.size() << " used: " << active_indices.size();
  return active_indices;
}

LabelClusters MeshSegmenter::detect(const SemanticLabel2Color& label_map,
                                    const std::vector<size_t>& frontend_indices,
                                    const std::optional<Eigen::Vector3d>& pos) {
  const auto active_indices = getActiveIndices(frontend_indices, pos);

  LabelClusters label_clusters;

  if (active_indices.empty()) {
    VLOG(3) << "[Mesh Segmenter] No active indices in mesh";
    return label_clusters;
  }

  LabelIndices label_indices = getLabelIndices(label_map, active_indices);
  if (label_indices.empty()) {
    VLOG(3) << "[Mesh Segmenter] No vertices found matching desired labels";
    for (const auto& callback_func : callback_funcs_) {
      callback_func(*full_mesh_vertices_, active_indices, label_indices);
    }
    return label_clusters;
  }

  VLOG(3) << "[Mesh Segmenter] Detecting clusters for labels";
  for (const auto label : config_.labels) {
    if (!label_indices.count(label)) {
      continue;
    }

    if (label_indices.at(label).size() < config_.min_cluster_size) {
      continue;
    }

    const auto clusters = findClusters(full_mesh_vertices_, label_indices.at(label));

    VLOG(3) << "[Mesh Segmenter]  - Found " << clusters.size() << " clusters of label "
            << static_cast<int>(label);
    label_clusters.insert({label, clusters});
  }

  for (const auto& callback_func : callback_funcs_) {
    callback_func(*full_mesh_vertices_, active_indices, label_indices);
  }

  return label_clusters;
}

void MeshSegmenter::pruneObjectsToCheckForPlaces(const DynamicSceneGraph& graph) {
  std::list<NodeId> to_remove;
  for (const auto& object_id : objects_to_check_for_places_) {
    if (!graph.hasNode(object_id)) {
      LOG(ERROR) << "Missing node " << NodeSymbol(object_id).getLabel();
      to_remove.push_back(object_id);
      continue;
    }

    if (graph.getNode(object_id).value().get().hasParent()) {
      to_remove.push_back(object_id);
    }
  }

  for (const auto& node_id : to_remove) {
    objects_to_check_for_places_.erase(node_id);
  }
}

std::set<NodeId> MeshSegmenter::archiveOldObjects(const DynamicSceneGraph& graph,
                                                  uint64_t latest_timestamp) {
  std::set<NodeId> archived = {};
  for (const auto& label : config_.labels) {
    std::list<NodeId> removed_nodes;
    for (const auto& object_node : active_objects_.at(label)) {
      if (!graph.hasNode(object_node)) {
        removed_nodes.push_back(object_node);
      }

      if (latest_timestamp - active_object_timestamps_.at(object_node) >
          static_cast<uint64_t>(config_.active_horizon_s * 1e9)) {
        removed_nodes.push_back(object_node);
        archived.insert(object_node);
      }
    }

    for (const auto& node_id : removed_nodes) {
      active_objects_[label].erase(node_id);
      active_object_timestamps_.erase(node_id);
    }
  }
  return archived;
}

std::optional<uint8_t> MeshSegmenter::getVertexLabel(
    const SemanticLabel2Color& label_map,
    size_t index) const {
  if (index >= full_mesh_vertices_->size()) {
    return std::nullopt;
  }

  const auto& point = full_mesh_vertices_->at(index);
  const HashableColor color(point.r, point.g, point.b, 255);
  return label_map.getSemanticLabelFromColor(color);
}

LabelIndices MeshSegmenter::getLabelIndices(const SemanticLabel2Color& label_map,
                                            const std::vector<size_t>& indices) const {
  LabelIndices label_indices;

  std::set<uint8_t> seen_labels;
  for (const auto idx : indices) {
    const auto label_opt = getVertexLabel(label_map, idx);
    if (!label_opt) {
      LOG(ERROR) << "bad index " << idx << "(of " << full_mesh_vertices_->size() << ")";
      continue;
    }

    const auto label = *label_opt;
    seen_labels.insert(label);

    if (!config_.labels.count(label)) {
      continue;
    }

    if (!label_indices.count(label)) {
      label_indices[label] = std::vector<size_t>();
    }

    label_indices[label].push_back(idx);
  }

  VLOG(3) << "[Mesh Segmenter] Seen labels: " << seen_labels;

  return label_indices;
}

std::set<NodeId> MeshSegmenter::updateGraph(DynamicSceneGraph& graph,
                                            const LabelClusters& clusters,
                                            uint64_t timestamp) {
  std::set<NodeId> archived = archiveOldObjects(graph, timestamp);

  for (const auto& label_clusters : clusters) {
    for (const auto& cluster : label_clusters.second) {
      bool matches_prev_object = false;
      std::vector<NodeId> nodes_not_in_graph;
      for (const auto& prev_node_id : active_objects_.at(label_clusters.first)) {
        const SceneGraphNode& prev_node = graph.getNode(prev_node_id).value();
        if (objectsMatch(cluster, prev_node)) {
          updateObjectInGraph(graph, cluster, prev_node, timestamp);
          matches_prev_object = true;
          break;
        }
      }

      if (!matches_prev_object) {
        addObjectToGraph(graph, cluster, label_clusters.first, timestamp);
      }
    }

    auto to_check = active_objects_[label_clusters.first];
    for (const auto& node_id : to_check) {
      if (!graph.hasNode(node_id)) {
        continue;
      }

      const auto& node =
          graph.getNode(node_id).value().get().attributes<SemanticNodeAttributes>();

      for (const auto& other_id : to_check) {
        if (node_id == other_id) {
          continue;
        }

        if (!graph.hasNode(other_id)) {
          continue;
        }

        const auto& other =
            graph.getNode(other_id).value().get().attributes<SemanticNodeAttributes>();

        if (node.bounding_box.isInside(other.position) ||
            other.bounding_box.isInside(node.position)) {
          if (node.bounding_box.volume() >= other.bounding_box.volume()) {
            graph.removeNode(other_id);
            active_objects_[label_clusters.first].erase(other_id);
            active_object_timestamps_.erase(other_id);
            objects_to_check_for_places_.erase(other_id);
          } else {
            graph.removeNode(node_id);
            active_objects_[label_clusters.first].erase(node_id);
            active_object_timestamps_.erase(node_id);
            objects_to_check_for_places_.erase(node_id);
          }
        }
      }
    }
  }

  return archived;
}

void MeshSegmenter::updateObjectInGraph(DynamicSceneGraph& graph,
                                        const Cluster& cluster,
                                        const SceneGraphNode& node,
                                        uint64_t timestamp) {
  active_object_timestamps_.at(node.id) = timestamp;

  for (const auto& idx : cluster.indices.indices) {
    graph.insertMeshEdge(node.id, idx, true);
  }

  auto new_box = bounding_box::extract(cluster.cloud, config_.bounding_box_type);
  ObjectNodeAttributes& attrs = node.attributes<ObjectNodeAttributes>();
  if (attrs.bounding_box.volume() >= new_box.volume()) {
    // TODO(nathan) merge object vertices
    return;  // prefer the largest detection
  }

  objects_to_check_for_places_.insert(node.id);

  // if we have a more complete detection, update centroid and box
  pcl::PointXYZ centroid;
  cluster.centroid.get(centroid);
  attrs.position << centroid.x, centroid.y, centroid.z;
  attrs.bounding_box = new_box;
}

void MeshSegmenter::addObjectToGraph(DynamicSceneGraph& graph,
                                     const Cluster& cluster,
                                     uint8_t label,
                                     uint64_t timestamp) {
  if (cluster.cloud->empty()) {
    LOG(ERROR) << "Encountered empty cluster with label" << static_cast<int>(label)
               << " @ " << timestamp << "[ns]";
    return;
  }

  ObjectNodeAttributes::Ptr attrs = std::make_unique<ObjectNodeAttributes>();
  attrs->semantic_label = label;
  attrs->name = NodeSymbol(next_node_id_).getLabel();
  attrs->bounding_box = bounding_box::extract(cluster.cloud, config_.bounding_box_type);

  const pcl::PointXYZRGBA& point = cluster.cloud->at(0);
  attrs->color << point.r, point.g, point.b;

  pcl::PointXYZ centroid;
  cluster.centroid.get(centroid);
  attrs->position << centroid.x, centroid.y, centroid.z;

  graph.emplaceNode(DsgLayers::OBJECTS, next_node_id_, std::move(attrs));

  active_objects_.at(label).insert(next_node_id_);
  active_object_timestamps_[next_node_id_] = timestamp;
  objects_to_check_for_places_.insert(next_node_id_);

  for (const auto& idx : cluster.indices.indices) {
    graph.insertMeshEdge(next_node_id_, idx, true);
  }

  ++next_node_id_;
}

}  // namespace incremental
}  // namespace hydra
