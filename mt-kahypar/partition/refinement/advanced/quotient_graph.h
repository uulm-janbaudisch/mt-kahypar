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

#include "tbb/concurrent_vector.h"

#include "kahypar/datastructure/binary_heap.h"

#include "mt-kahypar/definitions.h"
#include "mt-kahypar/partition/context.h"
#include "mt-kahypar/parallel/atomic_wrapper.h"

namespace mt_kahypar {

struct BlockPairStats {
  BlockPairStats() :
    num_active_searches(0),
    is_one_block_underloaded(false),
    cut_he_weight(0) { }

  BlockPairStats(bool /* sentinel */) :
    num_active_searches(std::numeric_limits<int>::min()),
    is_one_block_underloaded(false),
    cut_he_weight(0) { }


  CAtomic<int> num_active_searches;
  bool is_one_block_underloaded;
  CAtomic<HyperedgeWeight> cut_he_weight;
};

struct BlockPairStatsComparator {
  bool operator()(const BlockPairStats& lhs, const BlockPairStats& rhs) const {
    return lhs.num_active_searches > rhs.num_active_searches ||
      ( lhs.num_active_searches == rhs.num_active_searches &&
        lhs.is_one_block_underloaded < rhs.is_one_block_underloaded ) ||
      ( lhs.num_active_searches == rhs.num_active_searches &&
        lhs.is_one_block_underloaded == rhs.is_one_block_underloaded &&
        lhs.cut_he_weight < rhs.cut_he_weight );
  }
};

bool operator==(const BlockPairStats& lhs, const BlockPairStats& rhs);

std::ostream& operator<<(std::ostream& out, const BlockPairStats& stats);

} // namespace mt_kahypar

namespace kahypar::ds {

class BlockPairStatsHeap;

// Traits specialization for block pair stats heap:
template<>
class BinaryHeapTraits<BlockPairStatsHeap>{
 public:
  using IDType = size_t;
  using KeyType = mt_kahypar::BlockPairStats;
  using Comparator = mt_kahypar::BlockPairStatsComparator;

  static KeyType sentinel() {
    return mt_kahypar::BlockPairStats(true);
  }
};

class BlockPairStatsHeap final : public BinaryHeapBase<BlockPairStatsHeap>{
  using Base = BinaryHeapBase<BlockPairStatsHeap>;
  friend Base;

 public:
  using IDType = typename BinaryHeapTraits<BlockPairStatsHeap>::IDType;
  using KeyType = typename BinaryHeapTraits<BlockPairStatsHeap>::KeyType;

  // Second parameter is used to satisfy EnhancedBucketPQ interface
  explicit BlockPairStatsHeap(const IDType& storage_initializer) :
    Base(storage_initializer) { }

  friend void swap(BlockPairStatsHeap& a, BlockPairStatsHeap& b) {
    using std::swap;
    swap(static_cast<Base&>(a), static_cast<Base&>(b));
  }

 protected:
  inline void decreaseKeyImpl(const size_t handle) {
    Base::upHeap(handle);
  }

  inline void increaseKeyImpl(const size_t handle) {
    Base::downHeap(handle);
  }
};

} // namespace kahypar::ds

namespace mt_kahypar {

class QuotientGraph {

  static constexpr bool debug = false;
  static constexpr bool enable_heavy_assert = false;

  struct BlockPair {
    PartitionID i = kInvalidPartition;
    PartitionID j = kInvalidPartition;
  };

  // ! Represents an edge of the quotient graph
  struct QuotientGraphEdge {
    QuotientGraphEdge() :
      blocks(),
      first_valid_entry(0),
      cut_hes(),
      stats() { }

    void add_hyperedge(const HyperedgeID he,
                       const HyperedgeWeight weight);

    HyperedgeID pop_hyperedge();

    void reset(const bool is_one_block_underloaded);

    bool is_active() {
      return ( cut_hes.size() - first_valid_entry ) > 0;
    }

    BlockPair blocks;
    size_t first_valid_entry;
    tbb::concurrent_vector<HyperedgeID> cut_hes;
    BlockPairStats stats;
  };

  // Contains information required by a local search
  struct Search {
    explicit Search(const BlockPair blocks) :
      blocks(blocks),
      used_cut_hes(),
      is_finalized(false) { }

    // ! Blocks on which this search operates on
    BlockPair blocks;
    // ! Used cut hyperedges
    vec<HyperedgeID> used_cut_hes;
    // ! Flag indicating if construction of the corresponding search
    // ! is finalized
    bool is_finalized;
  };

  using BlockSchedulingHeap = kahypar::ds::BlockPairStatsHeap;

public:
  static constexpr SearchID INVALID_SEARCH_ID = std::numeric_limits<SearchID>::max();

  explicit QuotientGraph(const Context& context) :
    _phg(nullptr),
    _context(context),
    _quotient_graph(context.partition.k,
      vec<QuotientGraphEdge>(context.partition.k)),
    _heap_lock(),
    _block_scheduler(_context.partition.k * _context.partition.k),
    _num_active_searches(0),
    _searches() {
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
  SearchID requestNewSearch();

  // ! Returns the block pair on which the corresponding search operates on
  BlockPair getBlockPair(const SearchID search_id) const {
    ASSERT(search_id < _searches.size());
    return _searches[search_id].blocks;
  }

  /**
   * Requests cut hyperedges that contains the blocks
   * associated with the corresponding search.
   */
  vec<HyperedgeID> requestCutHyperedges(const SearchID search_id,
                                        const size_t max_num_edges);

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
                      const bool success);

  // ! Initializes the quotient graph. This includes to find
  // ! all cut hyperedges between all block pairs
  void initialize(const PartitionedHypergraph& phg);

  bool terminate() const {
    return _block_scheduler.empty() && _num_active_searches == 0;
  }

  // ! Only for testing
  HyperedgeWeight getCutHyperedgeWeightOfBlockPair(const PartitionID i, const PartitionID j) const {
    ASSERT(i < j);
    ASSERT(0 <= i && i < _context.partition.k);
    ASSERT(0 <= j && j < _context.partition.k);
    return _quotient_graph[i][j].stats.cut_he_weight;
  }

 private:
  void fullHeapUpdate();

  void resetQuotientGraphEdges();

  /**
   * The idea is to sort the cut hyperedges of a block pair (i,j)
   * in increasing order of their distance to each other. Meaning that
   * we perform a BFS from a randomly selected start cut hyperedge and
   * expand along cut hyperedges that contains pins of both blocks.
   * The BFS distance determines the order of the cut hyperedges.
   */
  void sortCutHyperedges(const PartitionID i, const PartitionID j);

  size_t index(const QuotientGraphEdge& edge) {
    return edge.blocks.i + _context.partition.k * edge.blocks.j;
  }

  BlockPair blockPairFromIndex(const size_t index) {
    const PartitionID block_1 = index % _context.partition.k;
    ASSERT((index - block_1) % _context.partition.k == 0);
    const PartitionID block_2 = (index - block_1) / _context.partition.k;
    ASSERT(block_2 < _context.partition.k);
    ASSERT(block_1 < block_2);
    return BlockPair { block_1, block_2 };
  }

  bool isOneBlockUnderloaded(const PartitionID i, const PartitionID j) {
    ASSERT(_phg);
    ASSERT(i >= 0 && i < _context.partition.k);
    ASSERT(j >= 0 && j < _context.partition.k);
    ASSERT(i != j);
    return ( _phg->partWeight(i) < _context.partition.perfect_balance_part_weights[i] &&
             _phg->partWeight(j) >= _context.partition.perfect_balance_part_weights[j] ) ||
           ( _phg->partWeight(i) >= _context.partition.perfect_balance_part_weights[i] &&
             _phg->partWeight(j) < _context.partition.perfect_balance_part_weights[j] );
  }

  const PartitionedHypergraph* _phg;
  const Context& _context;

  // ! Each edge contains stats and the cut hyperedges
  // ! of the block pair which its represents.
  vec<vec<QuotientGraphEdge>> _quotient_graph;

  SpinLock _heap_lock;
  // ! Heap that contains all block pairs.
  // ! Block pair on top of the heap is scheduled next.
  BlockSchedulingHeap _block_scheduler;

  // ! Number of active searches
  CAtomic<size_t> _num_active_searches;
  // ! Information about searches that are currently running
  tbb::concurrent_vector<Search> _searches;
};

}  // namespace kahypar
