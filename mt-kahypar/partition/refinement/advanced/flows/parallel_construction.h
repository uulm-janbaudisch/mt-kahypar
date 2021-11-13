/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2014 Sebastian Schlag <sebastian.schlag@kit.edu>
 *
 * KaHyPar is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * KaHyPar is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with KaHyPar.  If not, see <http://www.gnu.org/licenses/>.
 *
 ******************************************************************************/

#pragma once

#include <tbb/concurrent_vector.h>
#include "tbb/enumerable_thread_specific.h"

#include "algorithm/hyperflowcutter.h"
#include "algorithm/dinic.h"

#include "mt-kahypar/partition/context.h"
#include "mt-kahypar/datastructures/sparse_map.h"
#include "mt-kahypar/datastructures/concurrent_flat_map.h"
#include "mt-kahypar/datastructures/thread_safe_fast_reset_flag_array.h"
#include "mt-kahypar/datastructures/concurrent_bucket_map.h"
#include "mt-kahypar/partition/refinement/advanced/i_advanced_refiner.h"
#include "mt-kahypar/partition/refinement/advanced/flows/flow_hypergraph_builder.h"

namespace mt_kahypar {

struct FlowProblem;

class ParallelConstruction {

  static constexpr bool debug = false;

  static constexpr size_t NUM_CSR_BUCKETS = 1024;

  struct TmpPin {
    HyperedgeID e;
    whfc::Node pin;
    PartitionID block;
  };

  struct TmpHyperedge {
    const size_t hash;
    const size_t bucket;
    const whfc::Hyperedge e;
  };

  class DynamicIdenticalNetDetection {

    using IdenticalNetVector = tbb::concurrent_vector<TmpHyperedge>;

    struct HashBucket {
      HashBucket() :
        identical_nets(),
        threshold(0) { }

      IdenticalNetVector identical_nets;
      uint32_t threshold;
    };

   public:
    explicit DynamicIdenticalNetDetection(const Hypergraph& hg,
                                          FlowHypergraphBuilder& flow_hg,
                                          const Context& context) :
      _flow_hg(flow_hg),
      _hash_buckets(),
      _threshold(2) {
      const size_t num_parallel_refiners =
        context.shared_memory.num_threads / context.refinement.advanced.num_threads_per_search
        + (context.shared_memory.num_threads % context.refinement.advanced.num_threads_per_search != 0);
      _hash_buckets.resize(std::max(1024UL, hg.initialNumEdges() / num_parallel_refiners));
    }

    TmpHyperedge get(const size_t he_hash,
                     const vec<whfc::Node>& pins);

    void add(const TmpHyperedge& tmp_he);

    void reset() {
      _threshold += 2;
    }

   private:
    FlowHypergraphBuilder& _flow_hg;
    vec<HashBucket> _hash_buckets;
    uint32_t _threshold;
  };

 public:
  explicit ParallelConstruction(const Hypergraph& hg,
                                FlowHypergraphBuilder& flow_hg,
                                whfc::HyperFlowCutter<whfc::Dinic>& hfc,
                                SharedMap& node_to_whfc,
                                const Context& context) :
    _context(context),
    _flow_hg(flow_hg),
    _hfc(hfc),
    _node_to_whfc(node_to_whfc),
    _visited_hns(),
    _tmp_pins(),
    _cut_hes(),
    _pins(),
    _he_to_whfc(),
    _identical_nets(hg, flow_hg, context) { }

  ParallelConstruction(const ParallelConstruction&) = delete;
  ParallelConstruction(ParallelConstruction&&) = delete;
  ParallelConstruction & operator= (const ParallelConstruction &) = delete;
  ParallelConstruction & operator= (ParallelConstruction &&) = delete;

  virtual ~ParallelConstruction() = default;


  FlowProblem constructFlowHypergraph(const SearchID search_id,
                                      const PartitionedHypergraph& phg,
                                      const Subhypergraph& sub_hg,
                                      const PartitionID block_0,
                                      const PartitionID block_1,
                                      vec<HypernodeID>& whfc_to_node);

  // ! Only for testing
  FlowProblem constructFlowHypergraph(const SearchID search_id,
                                      const PartitionedHypergraph& phg,
                                      const Subhypergraph& sub_hg,
                                      const PartitionID block_0,
                                      const PartitionID block_1,
                                      vec<HypernodeID>& whfc_to_node,
                                      const bool default_construction);

 private:
  FlowProblem constructDefault(const SearchID search_id,
                               const PartitionedHypergraph& phg,
                               const Subhypergraph& sub_hg,
                               const PartitionID block_0,
                               const PartitionID block_1,
                               vec<HypernodeID>& whfc_to_node);

  FlowProblem constructOptimizedForLargeHEs(const PartitionedHypergraph& phg,
                                            const Subhypergraph& sub_hg,
                                            const PartitionID block_0,
                                            const PartitionID block_1,
                                            vec<HypernodeID>& whfc_to_node);

  void determineDistanceFromCut(const PartitionedHypergraph& phg,
                                const whfc::Node source,
                                const whfc::Node sink,
                                const PartitionID block_0,
                                const PartitionID block_1,
                                const vec<HypernodeID>& whfc_to_node);

  bool canHyperedgeBeDropped(const PartitionedHypergraph& phg,
                             const HyperedgeID he,
                             const PartitionID block_0,
                             const PartitionID block_1) {
    return _context.partition.objective == kahypar::Objective::cut &&
      phg.pinCountInPart(he, block_0) + phg.pinCountInPart(he, block_1) < phg.edgeSize(he);
  }

  const Context& _context;

  FlowHypergraphBuilder& _flow_hg;
  whfc::HyperFlowCutter<whfc::Dinic>& _hfc;

  SharedMap& _node_to_whfc;
  ds::ThreadSafeFastResetFlagArray<> _visited_hns;
  tbb::enumerable_thread_specific<vec<whfc::Node>> _tmp_pins;
  tbb::concurrent_vector<TmpHyperedge> _cut_hes;

  ds::ConcurrentBucketMap<TmpPin> _pins;
  ds::ConcurrentFlatMap<HyperedgeID, HyperedgeID> _he_to_whfc;

  DynamicIdenticalNetDetection _identical_nets;
};
}  // namespace mt_kahypar