#include "tiling/TilingAlgorithms.h"
#include "debug/ProgressReporter.h"

#include "containers/DestructuringIterator.h"
#include "terminal/stdout_helper.h"
#include "threading/Parallel.h"

#include <mutex>

/**
 * The maximum depth of an octree that is representable with a single
 * MortonIndex
 */
constexpr static uint32_t MAX_OCTREE_LEVELS = 21;
/**
 * The minimum number of points in a node needed in order for that node to be
 * processed asynchronously
 */
constexpr static size_t MIN_POINTS_FOR_ASYNC_PROCESSING = 100'000;

#pragma region helper_functions
/**
 * Reads the cached points for the given node from disk and returns them as
 * IndexedPoints
 */
static std::vector<IndexedPoint<MAX_OCTREE_LEVELS>>
read_pnts_from_disk(const std::string &node_name, const AABB &octree_bounds,
                    const AABB &node_bounds, PointsCache &points_cache,
                    PointsPersistence &persistence) {
  PointBuffer tmp_points;
  persistence.retrieve_points(node_name, tmp_points);
  if (!tmp_points.count())
    return {};

  auto &points = points_cache.emplace_points(std::move(tmp_points));

  std::vector<IndexedPoint<MortonIndex64Levels>> indexed_points;
  indexed_points.reserve(points.count());
  try {
    index_points<MortonIndex64Levels>(
        std::begin(points), std::end(points),
        std::back_inserter(indexed_points), octree_bounds,
        OutlierPointsBehaviour::ClampToBounds); // Clamp to fix floating point
                                                // errors
  } catch (const std::runtime_error &ex) {
    throw std::runtime_error{
        (boost::format("Indexing points of node %1% failed:\n%2%") % node_name %
         ex.what())
            .str()};
  }

  return indexed_points;
}

/**
 * Takes a sorted range of IndexedPoints and splits it up into up to eight
 * ranges, one for each child node. This method then returns the appropriate
 * NodeTilingData for tiling each of the child nodes
 */
static std::vector<NodeTilingData>
split_range_into_child_nodes(std::vector<IndexedPoint64>::iterator points_begin,
                             std::vector<IndexedPoint64>::iterator points_end,
                             octree::NodeStructure const &node,
                             octree::NodeStructure const &root_node) {
  const auto child_level = node.level + 1;
  const auto child_ranges = partition_points_into_child_octants(
      points_begin, points_end, static_cast<uint32_t>(child_level));

  std::vector<NodeTilingData> child_tiling_data;
  child_tiling_data.reserve(8);

  for (uint8_t octant = 0; octant < 8; ++octant) {
    const auto &child_range = child_ranges[octant];
    if (child_range.first == child_range.second)
      continue;

    auto child_node = node;
    child_node.morton_index.set_octant_at_level(child_level, octant);
    child_node.bounds = get_octant_bounds(octant, node.bounds);
    child_node.level = child_level;
    child_node.max_spacing /= 2;
    child_node.name = concat(node.name, static_cast<char>('0' + octant));

    // Copy the indices into a new vector and pass it on to the child processing
    child_tiling_data.emplace_back(
        octree::NodeData{child_range.first, child_range.second}, child_node,
        root_node);
  }

  return child_tiling_data;
}

template <typename Tree, typename GetTotalPoints>
static void dump_tree_to_graphviz(const Tree &tree, const std::string &path,
                                  GetTotalPoints get_total_points) {
  const auto tree_graphviz =
      tree.to_graphviz([get_total_points](const auto &node) {
        const auto total_points = get_total_points(node);

        std::stringstream ss;
        ss << OctreeNodeIndex64::to_string(node.index()) << " - "
           << total_points;
        return ss.str();
      });

  {
    std::ofstream fs{path};
    fs << tree_graphviz;
    fs.close();
  }
}

#pragma endregion

#pragma region TilingAlgorithmBase

TilingAlgorithmBase::TilingAlgorithmBase(SamplingStrategy &sampling_strategy,
                                         ProgressReporter *progress_reporter,
                                         PointsPersistence &persistence,
                                         TilerMetaParameters meta_parameters,
                                         size_t concurrency)
    : _sampling_strategy(sampling_strategy),
      _progress_reporter(progress_reporter), _persistence(persistence),
      _meta_parameters(meta_parameters), _concurrency(concurrency) {}

/**
 * Tile the given node as a terminal node, i.e. take up to 'max_points_per_node'
 * points and persist them without any sampling
 */
void TilingAlgorithmBase::tile_terminal_node(
    octree::NodeData const &all_points, octree::NodeStructure const &node,
    size_t previously_taken_points_count) {
  const auto points_to_take =
      std::min(all_points.size(), _meta_parameters.max_points_per_node);
  if (points_to_take < all_points.size()) {
    const auto dropped_points_count = all_points.size() - points_to_take;
    util::write_log((boost::format("Dropped %1% points at node %2%") %
                     dropped_points_count % node.name)
                        .str());

    if (_progress_reporter)
      _progress_reporter->increment_progress(progress::INDEXING,
                                             dropped_points_count);
  }

  _persistence.persist_points(
      member_iterator(std::begin(all_points), &IndexedPoint64::point_reference),
      member_iterator(std::begin(all_points) + points_to_take,
                      &IndexedPoint64::point_reference),
      node.bounds, node.name);

  if (_progress_reporter)
    _progress_reporter->increment_progress(
        progress::INDEXING, points_to_take - previously_taken_points_count);
}

/**
 * Tile the given node as an interior node, i.e. by using the given
 * SamplingStrategy
 */
std::vector<NodeTilingData>
TilingAlgorithmBase::tile_internal_node(octree::NodeData &all_points,
                                        octree::NodeStructure const &node,
                                        octree::NodeStructure const &root_node,
                                        size_t previously_taken_points_count) {
  const auto partition_point = filter_points_for_octree_node(
      std::begin(all_points), std::end(all_points), node.morton_index,
      node.level, root_node.bounds, root_node.max_spacing, _sampling_strategy);

  const auto points_taken = static_cast<size_t>(
      std::distance(std::begin(all_points), partition_point));

  _persistence.persist_points(
      member_iterator(std::begin(all_points), &IndexedPoint64::point_reference),
      member_iterator(partition_point, &IndexedPoint64::point_reference),
      node.bounds, node.name);

  if (_progress_reporter) {
    // To correctly increment progress, we have to know how many points were
    // cached when we last hit this node. In the 'worst' case, we take all the
    // same points as last time, so that would mean we made no progress on this
    // node. Hence this calculation here:
    const auto newly_taken_points =
        points_taken - previously_taken_points_count;
    _progress_reporter->increment_progress(progress::INDEXING,
                                           newly_taken_points);
  }

  return split_range_into_child_nodes(partition_point, std::end(all_points),
                                      node, root_node);
}

std::vector<NodeTilingData> TilingAlgorithmBase::tile_node(
    octree::NodeData &&node_data, const octree::NodeStructure &node_structure,
    const octree::NodeStructure &root_node_structure, tf::Subflow &subflow) {
  const auto current_node_name =
      concat(root_node_structure.name,
             to_string(node_structure.morton_index, node_structure.level + 1));

  auto cached_points =
      read_pnts_from_disk(current_node_name, root_node_structure.bounds,
                          node_structure.bounds, _points_cache, _persistence);

  const auto cached_points_count = cached_points.size();

  const auto node_level_to_sample_from = octree::get_node_level_to_sample_from(
      node_structure.level, root_node_structure);

  // Check whether this node is an interior node, terminal node, or a node that
  // is so deep that it is a new root node
  if (node_level_to_sample_from >=
      static_cast<int32_t>(node_structure.max_depth)) {
    const auto all_points_for_this_node = octree::merge_node_data_unsorted(
        std::move(node_data), std::move(cached_points));
    tile_terminal_node(all_points_for_this_node, node_structure,
                       cached_points_count);
    return {};
  }

  if (node_level_to_sample_from >=
      static_cast<int32_t>(MAX_OCTREE_LEVELS - 1)) {
    // If we are so deep that we exceed the capacity of the MortonIndex, we
    // have to index our points again with the current node as new root node. We
    // also have to carry the information that we have a new root over to the
    // children so that the paths of the nodes are correct.

    // Fun fact: We don't have to adjust the loaded indices because if we ever
    // get to a node this deep again, the indices have been calculated with the
    // new root the last time also, so everything is as it should be
    auto all_points_for_this_node = octree::merge_node_data_unsorted(
        std::move(node_data), std::move(cached_points));

    // Set this node as the new root node
    auto new_root_node = node_structure;
    new_root_node.max_depth = node_structure.max_depth - node_structure.level;

    // Compute new indices based upon this node as root node
    for (auto &indexed_point : all_points_for_this_node) {
      indexed_point.morton_index = calculate_morton_index<MAX_OCTREE_LEVELS>(
          indexed_point.point_reference.position(), new_root_node.bounds);
    }

    // Make sure everything is sorted again
    std::sort(all_points_for_this_node.begin(), all_points_for_this_node.end());

    return tile_internal_node(all_points_for_this_node, node_structure,
                              new_root_node, cached_points_count);
  }

  auto all_points_for_this_node = octree::merge_node_data_sorted(
      std::move(node_data), std::move(cached_points));
  return tile_internal_node(all_points_for_this_node, node_structure,
                            root_node_structure, cached_points_count);
}

/**
 * Perform tiling for the given node. This method does point selection for the
 * node and creates the execution graph for processing the child nodes of this
 * node
 */
void TilingAlgorithmBase::do_tiling_for_node(
    octree::NodeData &&node_data, const octree::NodeStructure &node_structure,
    const octree::NodeStructure &root_node_structure, tf::Subflow &subflow) {
  auto child_nodes = tile_node(std::move(node_data), node_structure,
                               root_node_structure, subflow);

  if (child_nodes.empty())
    return;

  // Create the execution graph for the child nodes. Only nodes with more than
  // MIN_POINTS_FOR_ASYNC_PROCESSING points are processed as asynchronous tasks,
  // the other nodes are processed synchronously right here. We sort the nodes
  // descending by point count so that the async tasks are created first, which
  // means that they can start processing while this method processes the
  // synchronous nodes
  std::sort(std::begin(child_nodes), std::end(child_nodes),
            [](const NodeTilingData &l, const NodeTilingData &r) {
              return r.points.size() < l.points.size();
            });

  const auto iter_to_first_sync_node = std::partition_point(
      std::begin(child_nodes), std::end(child_nodes),
      [](const NodeTilingData &node) {
        return node.points.size() >= MIN_POINTS_FOR_ASYNC_PROCESSING;
      });

  // Create async tasks for tiling child nodes that have many points
  std::for_each(std::begin(child_nodes), iter_to_first_sync_node,
                [this, &subflow](NodeTilingData &child_node) {
                  const auto child_node_name = child_node.node.name;
                  const auto child_points_count = child_node.points.size();
                  const auto child_task_name =
                      (boost::format("%1% [%2%]") % child_node_name %
                       child_points_count)
                          .str();
                  subflow
                      .emplace([this, _child_node = std::move(child_node)](
                                   tf::Subflow &sub_subflow) mutable {
                        do_tiling_for_node(std::move(_child_node.points),
                                           _child_node.node,
                                           _child_node.root_node, sub_subflow);
                      })
                      .name(child_task_name);
                });

  // Do tiling for child nodes that have few points
  std::for_each(iter_to_first_sync_node, std::end(child_nodes),
                [this, &subflow](NodeTilingData &child_node) {
                  do_tiling_for_node(std::move(child_node.points),
                                     child_node.node, child_node.root_node,
                                     subflow);
                });
}

#pragma endregion

#pragma region TilingAlgorithmV1

TilingAlgorithmV1::TilingAlgorithmV1(SamplingStrategy &sampling_strategy,
                                     ProgressReporter *progress_reporter,
                                     PointsPersistence &persistence,
                                     TilerMetaParameters meta_parameters,
                                     size_t concurrency)
    : TilingAlgorithmBase(sampling_strategy, progress_reporter, persistence,
                          meta_parameters, concurrency) {}

void TilingAlgorithmV1::build_execution_graph(PointBuffer &points,
                                              const AABB &bounds,
                                              tf::Taskflow &tf) {
  _root_node_points.clear();
  _root_node_points.resize(points.count());
  _points_cache.clear();

  auto indexing_tasks = parallel::transform(
      std::begin(points), std::end(points), std::begin(_root_node_points),
      [this, &bounds](PointBuffer::PointReference point) {
        return index_point<MAX_OCTREE_LEVELS>(
            point, bounds, OutlierPointsBehaviour::ClampToBounds);
      },
      tf, _concurrency, "calc_morton_indices");

  auto sort_task =
      tf.emplace([this]() {
          std::sort(_root_node_points.begin(), _root_node_points.end());
        }).name("sort");

  octree::NodeStructure root_node;
  root_node.bounds = bounds;
  root_node.level = -1;
  root_node.max_depth = _meta_parameters.max_depth;
  root_node.max_spacing = _meta_parameters.spacing_at_root;
  root_node.morton_index = {};
  root_node.name = "r";

  auto process_task =
      tf.emplace([this, root_node](tf::Subflow &subflow) mutable {
          do_tiling_for_node(std::move(_root_node_points), root_node, root_node,
                             subflow);
          _root_node_points = {};
        }).name(concat(root_node.name, " [", _root_node_points.size(), "]"));

  indexing_tasks.second.precede(sort_task);
  sort_task.precede(process_task);
}

#pragma endregion

#pragma region TilingAlgorithmV2

TilingAlgorithmV2::TilingAlgorithmV2(SamplingStrategy &sampling_strategy,
                                     ProgressReporter *progress_reporter,
                                     PointsPersistence &persistence,
                                     TilerMetaParameters meta_parameters,
                                     size_t concurrency)
    : TilingAlgorithmBase(sampling_strategy, progress_reporter, persistence,
                          meta_parameters, concurrency) {}

void TilingAlgorithmV2::build_execution_graph(PointBuffer &points,
                                              const AABB &bounds,
                                              tf::Taskflow &tf) {
  /**
   * #### Revised algorithm for better concurrency ####
   *
   * Instead of starting from the root node (which has to be processed by a
   * single thread), select a number of deeper nodes that can all be processed
   * in parallel. This will skip some nodes, their content has to be
   * reconstructed from the deeper nodes at the end, either by subsampling or
   * averaging
   */

  _root_node_points.clear();
  _root_node_points.resize(points.count());
  _points_cache.clear();
  _indexed_points_ranges.clear();
  _indexed_points_ranges.resize(_concurrency);

  const auto chunk_size = _root_node_points.size() / _concurrency;

  auto scatter_task = parallel::scatter(
      std::begin(points), std::end(points),
      [this, chunk_size, bounds](PointsIter points_begin, PointsIter points_end,
                                 size_t task_index) {
        // A bit of logic to access the slots of pre-allocated memory for this
        // task
        const auto point_data_offset = (task_index * chunk_size);
        const auto indexed_points_begin =
            std::begin(_root_node_points) + point_data_offset;
        const auto indexed_points_end =
            indexed_points_begin + std::distance(points_begin, points_end);
        auto &task_output = _indexed_points_ranges[task_index];

        index_and_sort_points({points_begin, points_end},
                              {indexed_points_begin, indexed_points_end},
                              bounds);
        task_output = split_indexed_points_into_subranges(
            {indexed_points_begin, indexed_points_end}, _concurrency);
      },
      tf, _concurrency);

  auto transpose_task = tf.emplace([this, bounds](tf::Subflow &subflow) {
    auto ranges_per_node =
        merge_selected_start_nodes(_indexed_points_ranges, _concurrency);

    // ranges_per_node is an Octree where some of the nodes contain the starting
    // data for tiling

    std::unordered_set<OctreeNodeIndex64> left_out_nodes;
    std::vector<tf::Task> new_tiling_tasks;

    for (auto node : ranges_per_node.traverse_level_order()) {
      if (node->empty())
        continue;

      auto parent = node.index().parent();
      while (true) {
        // Insert parent node and its parents etc. to left_out_nodes
        left_out_nodes.insert(parent);
        if (parent.levels() == 0)
          break;
        parent = parent.parent();
      }

      new_tiling_tasks.push_back(
          subflow.emplace([this, bounds, index = node.index(),
                           _data = std::move(*node)](tf::Subflow &subsubflow) {
            auto process_data = prepare_range_for_tiling(_data, index, bounds);

            do_tiling_for_node(std::move(process_data.points),
                               process_data.node, process_data.root_node,
                               subsubflow);
          }));
    }

    const auto reconstruct_task = subflow.emplace(
        [this, bounds, _left_out_nodes = std::move(left_out_nodes)]() {
          reconstruct_left_out_nodes(_left_out_nodes, bounds);
        });

    for (auto tiling_task : new_tiling_tasks) {
      tiling_task.precede(reconstruct_task);
    }
  });

  for (auto &scatter_subtask : scatter_task.scattered_tasks) {
    scatter_subtask.precede(transpose_task);
  }
}

void TilingAlgorithmV2::index_and_sort_points(
    util::Range<PointsIter> points,
    util::Range<IndexedPointsIter> indexed_points, const AABB &bounds) const {
  assert(points.size() == indexed_points.size());

  points.transform(
      std::begin(indexed_points),
      [&bounds](PointBuffer::PointReference point_ref) -> IndexedPoint64 {
        return index_point<MAX_OCTREE_LEVELS>(
            point_ref, bounds, OutlierPointsBehaviour::ClampToBounds);
      });

  indexed_points.sort();
}

/**
 * This method takes a range of IndexedPoints that span the root node of the
 * dataset and break it up into approximately* 'min_number_of_ranges' connected
 * ranges, where each of the new ranges spans exactly one node in the octree.
 * Ranges are split by size, with the largest range being split first. When
 * split, the range is replaced by ALL ranges for its immediate child nodes (up
 * to 8 ranges).
 *
 * To understand this, here is a simplified case in 2D, with node indices
 * written as 'ABC' where A is the nodes index at level 0, B at level 1 etc.
 *
 * Input range: [000, 002, 003, 011, 013, 030, 102, 103, 120, 300, 310, 313]
 * Desired ranges: 4
 *
 * Output: [[000, 002, 003], [011, 013], [030], [102, 103, 120], [300, 310,
 * 313]]
 *
 * This result is achieved in two steps:
 *
 * 1) Split up the root range. Yields:
 * [[000, 002, 003, 011, 013, 030], [102, 103, 120], [300, 310, 313]]
 *  |----------------------------|  |-------------|  |-------------|
 *      all points for node 0          ...node 1        ...node 3
 *
 * 2) Current range count is 3, which is less than desired ranges. Split up the
 * largest range, which is the range for node 0 (contains 6 points). This yields
 * the final result:
 * [[000, 002, 003], [011, 013], [030], [102, 103, 120], [300, 310, 313]]
 *  |-------------|  |--------|  |---|  |-------------|  |-------------|
 *      node 00        node 01  node 03     node 1            node 3
 */
Octree<util::Range<TilingAlgorithmV2::IndexedPointsIter>>
TilingAlgorithmV2::split_indexed_points_into_subranges(
    util::Range<IndexedPointsIter> indexed_points,
    size_t min_number_of_ranges) const {
  Octree<util::Range<IndexedPointsIter>> point_ranges{{{}, indexed_points}};

  if (indexed_points.size() <= min_number_of_ranges) {
    return point_ranges;
  }

  // Recursively split the node with the largest range into up to 8 child nodes

  auto get_node_with_most_points =
      [](Octree<util::Range<IndexedPointsIter>> &octree) {
        const auto iter_to_max_range =
            std::max_element(std::begin(octree.traverse_level_order()),
                             std::end(octree.traverse_level_order()),
                             [](const auto &left_node, const auto &right_node) {
                               return left_node->size() < right_node->size();
                             });
        if (iter_to_max_range == std::end(octree.traverse_level_order()))
          return iter_to_max_range;

        // To ensure that this whole method always terminates, we have to make
        // SURE that we only return a range from 'get_largest_range' if this
        // range can be split up into its child nodes. This can be done with a
        // simple check for the Morton index difference between the first and
        // last points in the range
        const auto &first_point = iter_to_max_range->first();
        const auto &last_point = iter_to_max_range->last();
        if (first_point.morton_index == last_point.morton_index)
          return std::end(octree.traverse_level_order());

        return iter_to_max_range;
      };

  size_t num_non_empty_nodes = 1;

  // // As long as we have less than 'min_number_of_ranges', keep splitting up
  // the largest range
  while (num_non_empty_nodes < min_number_of_ranges) {
    auto iter_to_max_range = get_node_with_most_points(point_ranges);
    if (iter_to_max_range == std::end(point_ranges.traverse_level_order()))
      break;

    // Split node up into child ranges
    auto node_with_max_range = *iter_to_max_range;
    const auto child_level = node_with_max_range.index().levels();
    const auto child_ranges = partition_points_into_child_octants(
        node_with_max_range->begin(), node_with_max_range->end(), child_level);

    // Insert each child range into the octree
    for (uint8_t octant = 0; octant < 8; ++octant) {
      const auto &child_range = child_ranges[octant];
      if (child_range.first == child_range.second)
        continue;

      point_ranges.insert(node_with_max_range.index().child(octant),
                          {child_range.first, child_range.second});

      ++num_non_empty_nodes;
    }

    // Clear the range of this node
    point_ranges.insert(node_with_max_range.index(), {});
    --num_non_empty_nodes;
  }

  size_t points_in_octree = 0;
  for (auto node : point_ranges.traverse_level_order()) {
    points_in_octree += node->size();
  }

  assert(points_in_octree == indexed_points.size());

  return point_ranges;
}

Octree<std::vector<util::Range<TilingAlgorithmV2::IndexedPointsIter>>>
TilingAlgorithmV2::merge_selected_start_nodes(
    const std::vector<Octree<util::Range<IndexedPointsIter>>> &selected_nodes,
    size_t min_number_of_ranges) {
  using Octree_t =
      Octree<std::vector<util::Range<TilingAlgorithmV2::IndexedPointsIter>>>;
  // Merge all trees into one tree

  auto merged_tree = std::accumulate(
      std::begin(selected_nodes), std::end(selected_nodes), Octree_t{},
      [](const auto &accum, const auto &val) {
        return Octree_t::transform_merge<util::Range<IndexedPointsIter>>(
            accum, val,
            [](util::Range<IndexedPointsIter> range)
                -> std::vector<util::Range<IndexedPointsIter>> {
              if (range.size() == 0)
                return {};
              return {range};
            },
            [](const std::vector<util::Range<IndexedPointsIter>> &l,
               const std::vector<util::Range<IndexedPointsIter>> &r) {
              std::vector<util::Range<IndexedPointsIter>> merged_ranges;
              for (auto &range : l) {
                if (range.size() == 0)
                  continue;
                merged_ranges.push_back(range);
              }
              for (auto &range : r) {
                if (range.size() == 0)
                  continue;
                merged_ranges.push_back(range);
              }
              return merged_ranges;
            });
      });

  // This tree can now have nodes that are in a parent/child relationship that
  // both have a non-empty range assigned to them. To break this pattern, we
  // first split the ranges in the parent nodes and push them to the child
  // nodes. Then, we keep merging child nodes until we have an acceptable number
  // of nodes

  std::vector<Octree_t::MutableNode> interior_nodes;
  for (auto node : merged_tree.traverse_level_order()) {
    // Look for nodes that have at least one non-empty range
    if (node->empty())
      continue;
    if (std::all_of(std::begin(*node), std::end(*node),
                    [](const auto &range) { return range.size() == 0; })) {
      continue;
    }

    // If the node is a leaf node, we skip it
    const auto children = node.children();
    if (children.empty())
      continue;

    // For each range, split the range and distribute all splitted ranges to the
    // appropriate child nodes
    for (auto &range : *node) {
      if (range.size() == 0) {
        __builtin_trap();
      }
      const auto split_ranges = partition_points_into_child_octants(
          std::begin(range), std::end(range), node.index().levels());
      for (uint8_t octant = 0; octant < 8; ++octant) {
        const auto &subrange = split_ranges[octant];
        if (subrange.first == subrange.second) {
          continue;
        }

        const auto child_node_index = node.index().child(octant);
        merged_tree.at(child_node_index)
            ->push_back({subrange.first, subrange.second});
      }
    }

    // Clear this node (but keep it in the tree)
    merged_tree.at(node.index())->clear();
  }

  // Keep merging leaf-nodes until we have ~min_number_of_ranges leaf nodes
  std::unordered_map<OctreeNodeIndex64, Octree_t::MutableNode>
      penultimate_nodes;
  const auto is_penultimate_node = [](const auto &node) {
    const auto children = node.children();
    return !node.is_leaf() &&
           std::all_of(std::begin(children), std::end(children),
                       [](const auto &node) { return node.is_leaf(); });
  };

  // Look for all nodes that have only leaf nodes as children
  for (auto node : merged_tree.traverse_level_order()) {
    if (!is_penultimate_node(node))
      continue;
    penultimate_nodes[node.index()] = node;
  }

  auto num_start_nodes = static_cast<size_t>(std::count_if(
      std::begin(merged_tree.traverse_level_order()),
      std::end(merged_tree.traverse_level_order()),
      [](const auto &node) { return node.is_leaf() && (!node->empty()); }));

  // Counts the total number of points in all direct child nodes of 'node'
  const auto num_points_in_children = [](const Octree_t::MutableNode &node) {
    const auto children = node.children();
    return std::accumulate(
        std::begin(children), std::end(children), size_t{0},
        [](size_t accum, const auto &node) {
          return accum + std::accumulate(std::begin(*node), std::end(*node),
                                         size_t{0},
                                         [](size_t accum, const auto &range) {
                                           return accum + range.size();
                                         });
        });
  };

  const auto merge_leaves =
      [is_penultimate_node](
          const Octree_t::MutableNode &leaf_parent,
          std::unordered_map<OctreeNodeIndex64, Octree_t::MutableNode>
              &penultimate_nodes) -> size_t {
    size_t merged_nodes = 0;
    std::vector<util::Range<TilingAlgorithmV2::IndexedPointsIter>>
        merged_ranges;
    for (auto leaf : leaf_parent.children()) {
      if (leaf->empty())
        continue;

      merged_ranges.insert(std::end(merged_ranges), std::begin(*leaf),
                           std::end(*leaf));
      ++merged_nodes;
    }

    leaf_parent.erase(); // Clears all child nodes that we just merged
    *leaf_parent =
        std::move(merged_ranges); // Puts all the merged ranges into this node

    // 'leaf_parent' is no longer a penultimate node, but its parent might be
    // now!
    penultimate_nodes.erase(leaf_parent.index());

    if (leaf_parent.index() == OctreeNodeIndex64{})
      return merged_nodes;
    const auto parent = leaf_parent.parent();
    if (!is_penultimate_node(parent))
      return merged_nodes;

    penultimate_nodes[parent.index()] = parent;
    --merged_nodes;
    return merged_nodes;
  };

  // Finds the best node to merge, which is the node with the least number of
  // points in its direct children
  const auto find_best_node_to_merge =
      [num_points_in_children](const auto &penultimate_nodes) {
        return std::min_element(
            std::begin(penultimate_nodes), std::end(penultimate_nodes),
            [num_points_in_children](const auto &l, const auto &r) {
              return num_points_in_children(l.second) <
                     num_points_in_children(r.second);
            });
      };

  // uint32_t steps = 0;

  while (num_start_nodes > min_number_of_ranges) {
    const auto node_to_merge_iter = find_best_node_to_merge(penultimate_nodes);
    if (node_to_merge_iter == std::end(penultimate_nodes))
      break;

    const auto children_of_node_to_merge =
        node_to_merge_iter->second.children();
    auto num_nodes_to_merge =
        std::count_if(std::begin(children_of_node_to_merge),
                      std::end(children_of_node_to_merge),
                      [](const auto &node) { return !node->empty(); });

    // Don't merge anymore if we drop below the minimum number of ranges
    if ((num_start_nodes - num_nodes_to_merge) < min_number_of_ranges) {
      break;
    }

    // Merge the leaves
    num_start_nodes -=
        merge_leaves(node_to_merge_iter->second, penultimate_nodes);
  }

  return merged_tree;
}

NodeTilingData TilingAlgorithmV2::prepare_range_for_tiling(
    const std::vector<util::Range<IndexedPointsIter>> &start_node_data,
    OctreeNodeIndex64 node_index, const AABB &bounds) {
  const auto start_node_point_count = std::accumulate(
      std::begin(start_node_data), std::end(start_node_data), size_t{0},
      [](size_t accum, const auto &range) { return accum + range.size(); });

  octree::NodeData merged_data{start_node_point_count};

  merge_ranges(util::range(start_node_data), util::range(merged_data),
               [](const IndexedPoint64 &l, const IndexedPoint64 &r) {
                 return l.morton_index.get() < r.morton_index.get();
               });

  octree::NodeStructure root_node;
  root_node.bounds = bounds;
  root_node.level = -1;
  root_node.max_depth = _meta_parameters.max_depth;
  root_node.max_spacing = _meta_parameters.spacing_at_root;
  root_node.morton_index = {};
  root_node.name = "r";

  octree::NodeStructure this_node;
  this_node.bounds = get_bounds_from_node_index(node_index, bounds);
  this_node.level = static_cast<int32_t>(node_index.levels()) - 1;
  this_node.max_depth = root_node.max_depth;
  this_node.max_spacing =
      root_node.max_spacing / std::pow(2, node_index.levels());
  this_node.morton_index = node_index.to_static_morton_index();
  this_node.name = std::string{"r"} + OctreeNodeIndex64::to_string(node_index);

  return {std::move(merged_data), this_node, root_node};
}

void TilingAlgorithmV2::reconstruct_single_node(
    const OctreeNodeIndex64 &node_index, const AABB &root_bounds) {

  // TODO If this node already exists (from a previous iteration), we will lose
  // points!
  // const auto this_node_name = concat("r",
  // OctreeNodeIndex64::to_string(node_index));

  // 1) Read data of direct child nodes
  PointBuffer data;
  for (uint8_t octant = 0; octant < 8; ++octant) {
    const auto child_index = node_index.child(octant);
    const auto node_name =
        concat("r", OctreeNodeIndex64::to_string(child_index));

    PointBuffer tmp;
    _persistence.retrieve_points(node_name, tmp);

    if (tmp.empty())
      continue;

    data.append_buffer(tmp);
  }

  // 2) Calculate morton indices for child data
  std::vector<IndexedPoint64> indexed_points;
  indexed_points.reserve(data.count());
  index_points<MAX_OCTREE_LEVELS>(
      std::begin(data), std::end(data), std::back_inserter(indexed_points),
      root_bounds, OutlierPointsBehaviour::ClampToBounds);

  // 3) Data is sorted, so we can sample directly
  const auto morton_index_for_node = node_index.to_static_morton_index();
  const auto selected_points_end = sample_points(
      _sampling_strategy, std::begin(indexed_points), std::end(indexed_points),
      morton_index_for_node, static_cast<int32_t>(node_index.levels()) - 1,
      root_bounds, _meta_parameters.spacing_at_root);

  // 3) Write to disk
  const auto node_bounds = get_bounds_from_node_index(node_index, root_bounds);
  const auto node_name = concat("r", OctreeNodeIndex64::to_string(node_index));

  // TOOD For 3D Tiles, reconstructed nodes should have their children be
  // 'REPLACE' instead of 'ADD'

  _persistence.persist_points(
      member_iterator(std::begin(indexed_points),
                      &IndexedPoint64::point_reference),
      member_iterator(selected_points_end, &IndexedPoint64::point_reference),
      node_bounds, node_name);
}

void TilingAlgorithmV2::reconstruct_left_out_nodes(
    const std::unordered_set<OctreeNodeIndex64> &left_out_nodes,
    const AABB &root_bounds) {
  std::list<OctreeNodeIndex64> nodes_by_level{std::begin(left_out_nodes),
                                              std::end(left_out_nodes)};
  nodes_by_level.sort(
      [](const OctreeNodeIndex64 &l, const OctreeNodeIndex64 &r) {
        return l.levels() > r.levels();
      });

  for (auto &node_index : nodes_by_level) {
    reconstruct_single_node(node_index, root_bounds);
  }
}

#pragma endregion