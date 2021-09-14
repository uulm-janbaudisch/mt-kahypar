/*******************************************************************************
 * This file is part of KaHyPar.
 *
 * Copyright (C) 2019 Tobias Heuer <tobias.heuer@kit.edu>
 * Copyright (C) 2020 Lars Gottesbüren <lars.gottesbueren@kit.edu>
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

#include "tbb/concurrent_queue.h"
#include "tbb/concurrent_vector.h"
#include "tbb/enumerable_thread_specific.h"

#include "mt-kahypar/definitions.h"
#include "mt-kahypar/partition/context.h"
#include "mt-kahypar/partition/refinement/advanced/refiner_adapter.h"
#include "mt-kahypar/parallel/atomic_wrapper.h"

namespace mt_kahypar {

struct BlockPair {
  PartitionID i = kInvalidPartition;
  PartitionID j = kInvalidPartition;
};

struct BlockPairCutHyperedges {
  BlockPairCutHyperedges() :
    blocks(),
    cut_hes() { }

  BlockPair blocks;
  vec<HyperedgeID> cut_hes;
};

class QuotientGraph {

  static constexpr bool debug = false;
  static constexpr bool enable_heavy_assert = false;

  // ! Represents an edge of the quotient graph
  struct QuotientGraphEdge {
    QuotientGraphEdge() :
      blocks(),
      ownership(INVALID_SEARCH_ID),
      is_in_queue(false),
      cut_hes(),
      first_valid_entry(0),
      initial_num_cut_hes(0),
      initial_cut_he_weight(0),
      cut_he_weight(0),
      num_improvements_found(0),
      total_improvement(0) { }

    // ! Adds a cut hyperedge to this quotient graph edge
    void add_hyperedge(const HyperedgeID he,
                       const HyperedgeWeight weight);

    // ! Pops a cut hyperedge from this quotient graph edge
    HyperedgeID pop_hyperedge();

    void reset();

    // ! Returns true, if quotient graph edge is acquired by a search
    bool isAcquired() const {
      return ownership.load() != INVALID_SEARCH_ID;
    }

    // ! Tries to acquire quotient graph edge with corresponding search id
    bool acquire(const SearchID search_id) {
      SearchID expected = INVALID_SEARCH_ID;
      SearchID desired = search_id;
      return ownership.compare_exchange_strong(expected, desired);
    }

    // ! Releases quotient graph edge
    void release(const SearchID search_id) {
      unused(search_id);
      ASSERT(ownership.load() == search_id);
      ownership.store(INVALID_SEARCH_ID);
    }

    // ! Marks quotient graph edge as in queue. Queued edges are scheduled
    // ! for refinement.
    bool markAsInQueue() {
      bool expected = false;
      bool desired = true;
      return is_in_queue.compare_exchange_strong(expected, desired);
    }

    // ! Marks quotient graph edge as nnot in queue
    bool markAsNotInQueue() {
      bool expected = true;
      bool desired = false;
      return is_in_queue.compare_exchange_strong(expected, desired);
    }

    // ! Block pair this quotient graph edge represents
    BlockPair blocks;
    // ! Atomic that contains the search currently constructing
    // ! a problem on this block pair
    CAtomic<SearchID> ownership;
    // ! True, if block is contained in block scheduler queue
    CAtomic<bool> is_in_queue;
    // ! Cut hyperedges of block pair
    tbb::concurrent_vector<HyperedgeID> cut_hes;
    // ! Position of the first valid cut hyperedge in cut_hes
    size_t first_valid_entry;
    // ! Initial number of cut hyperedges
    size_t initial_num_cut_hes;
    // ! Initial weight of all cut hyperedges
    HyperedgeWeight initial_cut_he_weight;
    // ! Current weight of all cut hyperedges
    CAtomic<HyperedgeWeight> cut_he_weight;
    // ! Number of improvements found on this block pair
    CAtomic<size_t> num_improvements_found;
    // ! Total improvement found on this block pair
    CAtomic<HyperedgeWeight> total_improvement;
  };

  class ActiveBlockSchedulingRound {

   public:
    explicit ActiveBlockSchedulingRound(const Context& context,
                                        vec<vec<QuotientGraphEdge>>& quotient_graph,
                                        const vec<CAtomic<size_t>>& num_active_searches_on_blocks) :
      _context(context),
      _quotient_graph(quotient_graph),
      _num_active_searches_on_blocks(num_active_searches_on_blocks),
      _unscheduled_blocks(),
      _round_improvement(0),
      _active_blocks_lock(),
      _active_blocks(context.partition.k, false),
      _remaining_blocks(0) { }

    bool popBlockPairFromQueue(BlockPair& blocks);

    bool pushBlockPairIntoQueue(const BlockPair& blocks);

    void finalizeSearch(const BlockPair& blocks,
                        const HyperedgeWeight improvement,
                        bool& block_0_becomes_active,
                        bool& block_1_becomes_active);

    HyperedgeWeight roundImprovement() const {
      return _round_improvement.load(std::memory_order_relaxed);
    }

    size_t numRemainingBlocks() const {
      return _remaining_blocks;
    }

   const Context& _context;
   // ! Quotient graph
    vec<vec<QuotientGraphEdge>>& _quotient_graph;
    // ! Number of active searches on each block
    const vec<CAtomic<size_t>>& _num_active_searches_on_blocks;
    // ! Queue that contains all unscheduled block pairs of the current round
    tbb::concurrent_queue<BlockPair> _unscheduled_blocks;
    // ! Current improvement made in this round
    CAtomic<HyperedgeWeight> _round_improvement;
    // Active blocks for next round
    SpinLock _active_blocks_lock;
    vec<uint8_t> _active_blocks;
    CAtomic<size_t> _remaining_blocks;
  };

  class ActiveBlockScheduler {

   public:
    explicit ActiveBlockScheduler(const Context& context,
                                  vec<vec<QuotientGraphEdge>>& quotient_graph) :
      _context(context),
      _quotient_graph(quotient_graph),
      _rounds(),
      _num_active_searches_on_blocks(
        context.partition.k, CAtomic<size_t>(0)),
      _min_improvement_per_round(0),
      _terminate(false),
      _round_lock(),
      _first_active_round(0),
      _is_input_hypergraph(false) { }

    size_t currentRound() const {
      return _rounds.size();
    }

    void initialize(const bool is_input_hypergraph);

    bool popBlockPairFromQueue(BlockPair& blocks, size_t& round);

    void startSearch(const BlockPair& blocks) {
      ++_num_active_searches_on_blocks[blocks.i];
      ++_num_active_searches_on_blocks[blocks.j];
    }

    void finalizeSearch(const BlockPair& blocks,
                        const size_t round,
                        const HyperedgeWeight improvement);

    void setObjective(const HyperedgeWeight objective) {
      _min_improvement_per_round =
        _context.refinement.advanced.min_relative_improvement_per_round * objective;
    }

   private:

    void reset() {
      _rounds.clear();
      _num_active_searches_on_blocks.assign(
        _context.partition.k, CAtomic<size_t>(0));
      _first_active_round = 0;
      _terminate = false;
    }

    bool isActiveBlockPair(const PartitionID i,
                           const PartitionID j,
                           const size_t round) const;

    const Context& _context;
    // ! Quotient graph
    vec<vec<QuotientGraphEdge>>& _quotient_graph;
    // Contains all active block scheduling rounds
    tbb::concurrent_vector<ActiveBlockSchedulingRound> _rounds;
    // ! Number of active searches on each block
    vec<CAtomic<size_t>> _num_active_searches_on_blocks;
    // ! Minimum improvement per round to continue with next round
    HyperedgeWeight _min_improvement_per_round;
    // ! If true, then search is immediatly terminated
    bool _terminate;
    // ! First Active Round
    SpinLock _round_lock;
    size_t _first_active_round;
    // ! Indicate if the current hypergraph represents the input hypergraph
    bool _is_input_hypergraph;
  };

  // Contains information required by a local search
  struct Search {
    explicit Search(const BlockPair& blocks, const size_t round) :
      blocks(blocks),
      round(round),
      used_cut_hes(),
      is_finalized(false) { }

    // ! Block pair on which this search operates on
    BlockPair blocks;
    // ! Round of active block scheduling
    size_t round;
    // ! Used cut hyperedges
    vec<HyperedgeID> used_cut_hes;
    // ! Flag indicating if construction of the corresponding search
    // ! is finalized
    bool is_finalized;
  };

  struct BFSData {
    BFSData(const HypernodeID num_nodes,
            const HyperedgeID num_edges) :
      visited_hns(num_nodes, false),
      distance(num_edges, -1) {}

    void reset() {
      std::fill(visited_hns.begin(), visited_hns.end(), false);
      std::fill(distance.begin(), distance.end(), -1);
    }

    vec<bool> visited_hns;
    vec<int> distance;
  };

public:
  static constexpr SearchID INVALID_SEARCH_ID = std::numeric_limits<SearchID>::max();

  explicit QuotientGraph(const Hypergraph& hg,
                         const Context& context) :
    _phg(nullptr),
    _context(context),
    _initial_num_edges(hg.initialNumEdges()),
    _current_num_edges(kInvalidHyperedge),
    _quotient_graph(context.partition.k,
      vec<QuotientGraphEdge>(context.partition.k)),
    _register_search_lock(),
    _block_scheduler(),
    _active_block_scheduler(context, _quotient_graph),
    _num_active_searches(0),
    _searches(),
    _local_bfs(hg.initialNumNodes(), hg.initialNumEdges()) {
    for ( PartitionID i = 0; i < _context.partition.k; ++i ) {
      for ( PartitionID j = i + 1; j < _context.partition.k; ++j ) {
        _quotient_graph[i][j].blocks.i = i;
        _quotient_graph[i][j].blocks.j = j;
      }
    }
  }

  QuotientGraph(const QuotientGraph&) = delete;
  QuotientGraph(QuotientGraph&&) = delete;

  QuotientGraph & operator= (const QuotientGraph &) = delete;
  QuotientGraph & operator= (QuotientGraph &&) = delete;

  /**
   * Returns a new search id which is associated with a certain number
   * of block pairs. The corresponding search can request hyperedges
   * with the search id that are cut between the corresponding blocks
   * associated with the search. If there are currently no block pairs
   * available then INVALID_SEARCH_ID is returned.
   */
  SearchID requestNewSearch(AdvancedRefinerAdapter& refiner);

  // ! Returns the block pair on which the corresponding search operates on
  BlockPair getBlockPair(const SearchID search_id) const {
    ASSERT(search_id < _searches.size());
    return _searches[search_id].blocks;
  }

  // ! Number of block pairs used by the corresponding search
  size_t numBlockPairs(const SearchID) const {
    return 1;
  }

  /**
   * Requests cut hyperedges that contains the blocks
   * associated with the corresponding search.
   */
  BlockPairCutHyperedges requestCutHyperedges(const SearchID search_id,
                                              const size_t max_num_edges);

  /**
   * During problem construction we might acquire additional cut hyperedges
   * not requested by the search. This function associates those hyperedges
   * with the search and flags them as used.
   */
  size_t acquireUsedCutHyperedges(const SearchID& search_id, vec<bool>& used_hes);


  /**
   * Notifies the quotient graph that hyperedge he contains
   * a new block, which was previously not contained. The thread
   * that increases the pin count of hyperedge he in the corresponding
   * block to 1 is responsible to call this function.
   */
  void addNewCutHyperedge(const HyperedgeID he,
                          const PartitionID block);

  /**
   * Notify the quotient graph that the construction of the corresponding
   * search is completed. The corresponding block pairs associated with the
   * search are made available again for other searches.
   */
  void finalizeConstruction(const SearchID search_id);

  /**
   * Notify the quotient graph that the corrseponding search terminated.
   * If the search improves the quality of the partition (success == true),
   * we reinsert all hyperedges that were used throughout the construction
   * and are still cut between the corresponding block.
   */
  void finalizeSearch(const SearchID search_id,
                      const HyperedgeWeight total_improvement);

  // ! Initializes the quotient graph. This includes to find
  // ! all cut hyperedges between all block pairs
  void initialize(const PartitionedHypergraph& phg);

  void setObjective(const HyperedgeWeight objective) {
    _active_block_scheduler.setObjective(objective);
  }

  size_t maximumRequiredRefiners() const;

  // ! Only for testing
  HyperedgeWeight getCutHyperedgeWeightOfBlockPair(const PartitionID i, const PartitionID j) const {
    ASSERT(i < j);
    ASSERT(0 <= i && i < _context.partition.k);
    ASSERT(0 <= j && j < _context.partition.k);
    return _quotient_graph[i][j].cut_he_weight;
  }

 private:

  void resetQuotientGraphEdges();

  /**
   * The idea is to sort the cut hyperedges of a block pair (i,j)
   * in increasing order of their distance to each other. Meaning that
   * we perform a BFS from a randomly selected start cut hyperedge and
   * expand along cut hyperedges that contains pins of both blocks.
   * The BFS distance determines the order of the cut hyperedges.
   */
  void sortCutHyperedges(const PartitionID i,
                         const PartitionID j,
                         BFSData& bfs_data);

  bool isInputHypergraph() const {
    return _current_num_edges == _initial_num_edges;
  }

  const PartitionedHypergraph* _phg;
  const Context& _context;
  const HypernodeID _initial_num_edges;
  HypernodeID _current_num_edges;

  // ! Each edge contains stats and the cut hyperedges
  // ! of the block pair which its represents.
  vec<vec<QuotientGraphEdge>> _quotient_graph;

  SpinLock _register_search_lock;
  // ! Queue that contains all block pairs.
  tbb::concurrent_queue<BlockPair> _block_scheduler;
  ActiveBlockScheduler _active_block_scheduler;

  // ! Number of active searches
  CAtomic<size_t> _num_active_searches;
  // ! Information about searches that are currently running
  tbb::concurrent_vector<Search> _searches;

  // ! BFS data required to sort cut hyperedges
  tbb::enumerable_thread_specific<BFSData> _local_bfs;
};

}  // namespace kahypar
