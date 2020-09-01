/*******************************************************************************
 * This file is part of MT-KaHyPar.
 *
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

#include <mt-kahypar/partition/context.h>
#include <mt-kahypar/partition/metrics.h>

#include "mt-kahypar/datastructures/delta_partitioned_hypergraph.h"
#include "mt-kahypar/datastructures/sparse_map.h"
#include "mt-kahypar/partition/refinement/fm/fm_commons.h"
#include "mt-kahypar/partition/refinement/fm/stop_rule.h"

namespace mt_kahypar {

class FMInterface {
public:
  virtual bool findMoves(PartitionedHypergraph& phg, size_t taskID) = 0;
  virtual void memoryConsumption(utils::MemoryTreeNode* parent) const  = 0;

  FMStats stats;
};

template<typename FMDetails>
class LocalizedKWayFM : public FMInterface {
public:
  explicit LocalizedKWayFM(const Context& context, HypernodeID numNodes, FMSharedData& sharedData) :
          context(context),
          thisSearch(0),
          k(context.partition.k),
          deltaPhg(context.partition.k),
          updateDeduplicator(),
          fm_details(context, numNodes, sharedData, runStats),
          sharedData(sharedData)
          { }


  //bool findMovesUsingFullBoundary(PartitionedHypergraph& phg) ;

  bool findMoves(PartitionedHypergraph& phg, size_t taskID) override;

  void memoryConsumption(utils::MemoryTreeNode* parent) const override ;

private:

  // ! Performs localized FM local search on the delta partitioned hypergraph.
  // ! Moves made by this search are not immediately visible to other concurrent local searches.
  // ! The best prefix of moves is applied to the global partitioned hypergraph after the search finishes.
  //void internalFindMovesOnDeltaHypergraph(PartitionedHypergraph& phg, FMSharedData& sharedData);


  template<bool use_delta>
  void internalFindMoves(PartitionedHypergraph& phg);

  // TODO move to cpp file
  template<typename PHG>
  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE
  void acquireOrUpdateNeighbors(const PHG& phg, const Move& move) {

    // Note: In theory we should acquire/update all neighbors. It just turned out that this works fine
    for (HyperedgeID e : edgesWithGainChanges) {
      if (phg.edgeSize(e) < context.partition.ignore_hyperedge_size_threshold) {
        for (HypernodeID v : phg.pins(e)) {
          if (!updateDeduplicator.contains(v)) {
            SearchID searchOfV = sharedData.nodeTracker.searchOfNode[v].load(std::memory_order_acq_rel);
            if (searchOfV == thisSearch) {
              fm_details.updateGain(phg, v, move);
            } else if (sharedData.nodeTracker.tryAcquireNode(v, thisSearch)) {
              fm_details.insertIntoPQ(phg, v);
            }
            updateDeduplicator[v] = { };  // insert
          }
        }
      }
    }
    edgesWithGainChanges.clear();
    updateDeduplicator.clear();
  }


  // ! Makes moves applied on delta hypergraph visible on the global partitioned hypergraph.
  std::pair<Gain, size_t> applyMovesOnGlobalHypergraph(PartitionedHypergraph& phg,
                                                       size_t bestGainIndex,
                                                       Gain bestEstimatedImprovement);

  // ! Rollback to the best improvement found during local search in case we applied moves
  // ! directly on the global partitioned hypergraph.
  void revertToBestLocalPrefix(PartitionedHypergraph& phg, size_t bestGainIndex);

 private:

  const Context& context;

  // ! Unique search id associated with the current local search
  SearchID thisSearch;

  // ! Number of blocks
  PartitionID k;

  // ! Local data members required for one localized search run
  //FMLocalData localData;
  vec< std::pair<Move, MoveID> > localMoves;

  // ! Wrapper around the global partitioned hypergraph, that allows
  // ! to perform moves non-visible for other local searches
  ds::DeltaPartitionedHypergraph<PartitionedHypergraph> deltaPhg;

  // ! After a move it collects all neighbors of the moved vertex
  ds::DynamicSparseSet<HypernodeID> updateDeduplicator;   // TODO check again if this is the best data structure for the job.

  // ! Stores hyperedges whose pins's gains may have changed after vertex move
  vec<HyperedgeID> edgesWithGainChanges;

  FMStats runStats;

  FMDetails fm_details;   // TODO make generic in the end!

  FMSharedData& sharedData;

};

}