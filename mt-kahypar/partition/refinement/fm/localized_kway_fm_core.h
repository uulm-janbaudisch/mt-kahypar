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

#include <mt-kahypar/definitions.h>
#include <mt-kahypar/partition/context.h>
#include <mt-kahypar/partition/metrics.h>

#include "mt-kahypar/datastructures/sparse_map.h"
#include "mt-kahypar/partition/refinement/fm/fm_commons.h"
#include "mt-kahypar/partition/refinement/fm/stop_rule.h"

namespace mt_kahypar {

class LocalizedKWayFM {

 struct FMLocalData {

   void clear() {
     seedVertices.clear();
     localMoves.clear();
     localMoveIDs.clear();
     runStats.clear();
   }

   // ! Contains all seed vertices of the current local search
   vec<HypernodeID> seedVertices;
   // ! Contains all moves performed during the current local search
   vec<Move> localMoves;
   // ! Contains all move IDs of all commited moves of the current local search
   vec<MoveID> localMoveIDs;
   // ! Stats of the current local search
   FMStats runStats;
 };

 public:
  explicit LocalizedKWayFM(const Context& context, HypernodeID numNodes, PosT* pq_handles) :
          context(context),
          thisSearch(0),
          k(context.partition.k),
          localData(),
          deltaPhg(context.partition.k),
          blockPQ(static_cast<size_t>(k)),
          vertexPQs(static_cast<size_t>(k), VertexPriorityQueue(pq_handles, numNodes)),
          updateDeduplicator(),
          validHyperedges() { }


  bool findMovesUsingFullBoundary(PartitionedHypergraph& phg, FMSharedData& sharedData) {
    localData.clear();
    validHyperedges.clear();
    thisSearch = ++sharedData.nodeTracker.highestActiveSearchID;

    for (HypernodeID u : sharedData.refinementNodes.safely_inserted_range()) {
      insertOrUpdatePQ(phg, u, sharedData.nodeTracker);
    }
    for (PartitionID i = 0; i < k; ++i) {
      updateBlock(i);
    }

    // this is boundary FM, so it's sequential. no need for delta hypergraph
    internalFindMovesOnGlobalHypergraph(phg, sharedData);
    return true;
  }

  bool findMovesLocalized(PartitionedHypergraph& phg, FMSharedData& sharedData, size_t taskID) {
    localData.clear();
    validHyperedges.clear();
    thisSearch = ++sharedData.nodeTracker.highestActiveSearchID;
    const size_t nSeeds = context.refinement.fm.num_seed_nodes;
    HypernodeID seedNode;
    while (localData.runStats.pushes < nSeeds && sharedData.refinementNodes.try_pop(seedNode, taskID)) {
      if (!updateDeduplicator.contains(seedNode) && insertOrUpdatePQ(phg, seedNode, sharedData.nodeTracker)) {
        localData.seedVertices.push_back(seedNode);
      }
    }
    updateBlocks(phg, kInvalidPartition);

    if (localData.runStats.pushes > 0) {
      if ( context.refinement.fm.perform_moves_global ) {
        internalFindMovesOnGlobalHypergraph(phg, sharedData);
      } else {
        deltaPhg.clear();
        deltaPhg.setPartitionedHypergraph(&phg);
        internalFindMovesOnDeltaHypergraph(phg, sharedData);
      }
      return true;
    } else {
      return false;
    }

  }

private:

  // ! Starts a localized FM search on the delta partitioned hypergraph. Moves
  // ! that are made by this local search are not immediatly visible to other
  // ! concurrent running local searches. Moves are applied to global hypergraph,
  // ! if search yield to an improvement.
  void internalFindMovesOnDeltaHypergraph(PartitionedHypergraph& phg,
                                          FMSharedData& sharedData) {
    StopRule stopRule(phg.initialNumNodes());
    Move m;

    auto hes_to_update_func = [&](const HyperedgeID he,
                                  const HyperedgeWeight,
                                  const HypernodeID,
                                  const HypernodeID pin_count_in_from_part_after,
                                  const HypernodeID pin_count_in_to_part_after) {
      // Gains of the pins of a hyperedge can only change in the following situation.
      // In such cases, we mark the hyperedge as invalid and update the gain of all
      // pins afterwards.
      if ( pin_count_in_from_part_after == 0 || pin_count_in_from_part_after == 1 ||
           pin_count_in_to_part_after == 1 || pin_count_in_to_part_after == 2 ) {
        validHyperedges[he] = false;
      }
    };

    size_t bestImprovementIndex = 0;
    Gain estimatedImprovement = 0;
    Gain bestImprovement = 0;

    HypernodeWeight heaviestPartWeight = 0;
    HypernodeWeight toWeight = 0;

    while (!stopRule.searchShouldStop() && findNextMove(deltaPhg, m)) {
      sharedData.nodeTracker.deactivateNode(m.node, thisSearch);

      bool moved = false;
      if (m.to != kInvalidPartition) {
        heaviestPartWeight = metrics::heaviestPartAndWeight(deltaPhg).second;
        const HypernodeWeight fromWeight = deltaPhg.partWeight(m.from);
        toWeight = deltaPhg.partWeight(m.to);
        moved = deltaPhg.changeNodePart(m.node, m.from, m.to, std::max(
          context.partition.max_part_weights[m.to], fromWeight), hes_to_update_func);
      }

      if (moved) {
        localData.runStats.moves++;
        estimatedImprovement += m.gain;
        localData.localMoves.push_back(m);
        stopRule.update(m.gain);

        // Check if move improves current best solution
        bool move_improved_quality = false;
        if ( context.refinement.fm.allow_zero_gain_moves ) {
          move_improved_quality = estimatedImprovement >= bestImprovement;
        } else {
          const bool improved_km1 = estimatedImprovement > bestImprovement;
          const bool improved_balance_less_equal_km1 = estimatedImprovement >= bestImprovement &&
                                                       toWeight + phg.nodeWeight(m.node) < heaviestPartWeight;
          move_improved_quality = improved_km1 || improved_balance_less_equal_km1;
        }

        if (move_improved_quality) {
          stopRule.reset();
          bestImprovement = estimatedImprovement;
          bestImprovementIndex = localData.localMoves.size();
        }

        insertOrUpdateNeighbors(deltaPhg, sharedData, m.node);
      }
      updateBlocks(deltaPhg, m.from);
    }

    std::tie(bestImprovement, bestImprovementIndex) =
      applyMovesOnGlobalHypergraph(phg, sharedData, bestImprovementIndex, bestImprovement);
    localData.runStats.estimated_improvement = bestImprovement;
    clearPQs(sharedData, bestImprovementIndex);
    localData.runStats.merge(stats);
  }

  // ! Starts a localized FM search on the global partitioned hypergraph. Moves
  // ! that are made by this local search are immediatly visible to other concurrent
  // ! running local searches. Moves are rollbacked to best improvement found during
  // ! that search.
  void internalFindMovesOnGlobalHypergraph(PartitionedHypergraph& phg,
                                           FMSharedData& sharedData) {
    StopRule stopRule(phg.initialNumNodes());
    Move m;


    auto hes_to_update_func = [&](const HyperedgeID he,
                                  const HyperedgeWeight,
                                  const HypernodeID,
                                  const HypernodeID pin_count_in_from_part_after,
                                  const HypernodeID pin_count_in_to_part_after) {
      // Gains of the pins of a hyperedge can only change in the following situations.
      // In such cases, we mark the hyperedge as invalid and update the gain of all
      // pins afterwards.
      if ( pin_count_in_from_part_after == 0 || pin_count_in_from_part_after == 1 ||
           pin_count_in_to_part_after == 1 || pin_count_in_to_part_after == 2 ) {
        validHyperedges[he] = false;
      }
    };

    size_t bestImprovementIndex = 0;
    Gain estimatedImprovement = 0;
    Gain bestImprovement = 0;

    HypernodeWeight heaviestPartWeight = 0;
    HypernodeWeight toWeight = 0;

    while (!stopRule.searchShouldStop() && findNextMove(phg, m)) {
      sharedData.nodeTracker.deactivateNode(m.node, thisSearch);
      MoveID move_id = std::numeric_limits<MoveID>::max();
      auto report_success = [&] { move_id = sharedData.moveTracker.insertMove(m); };

      bool moved = false;
      if (m.to != kInvalidPartition) {
        heaviestPartWeight = metrics::heaviestPartAndWeight(phg).second;
        const HypernodeWeight fromWeight = phg.partWeight(m.from);
        toWeight = phg.partWeight(m.to);
        moved = phg.changeNodePartFullUpdate(m.node, m.from, m.to, std::max(
          context.partition.max_part_weights[m.to], fromWeight),
          report_success, hes_to_update_func);
      }

      if (moved) {
        localData.runStats.moves++;
        estimatedImprovement += m.gain;
        localData.localMoveIDs.push_back(move_id);
        stopRule.update(m.gain);

        // Check if move improves current best solution
        bool move_improved_quality = false;
        if ( context.refinement.fm.allow_zero_gain_moves ) {
          move_improved_quality = estimatedImprovement >= bestImprovement;
        } else {
          const bool improved_km1 = estimatedImprovement > bestImprovement;
          const bool improved_balance_less_equal_km1 = estimatedImprovement >= bestImprovement &&
                                                      toWeight + phg.nodeWeight(m.node) < heaviestPartWeight;
          move_improved_quality = improved_km1 || improved_balance_less_equal_km1;
        }

        if (move_improved_quality) {
          stopRule.reset();
          bestImprovement = estimatedImprovement;
          bestImprovementIndex = localData.localMoveIDs.size();
        }

        insertOrUpdateNeighbors(phg, sharedData, m.node);
      }
      updateBlocks(phg, m.from);
    }

    revertToBestLocalPrefix(phg, sharedData, bestImprovementIndex);
    localData.runStats.estimated_improvement = bestImprovement;
    clearPQs(sharedData, bestImprovementIndex);
    localData.runStats.merge(stats);
  }

  void clearPQs(FMSharedData& sharedData,
                const size_t bestImprovementIndex) {
    // release all nodes that were not moved
    // reinsert into task queue only if we're doing multitry and at least one node was moved
    // unless a node was moved, only seed nodes are in the pqs
    const bool release = context.refinement.fm.algorithm == FMAlgorithm::fm_multitry && localData.runStats.moves > 0;
    const bool reinsert_seeds = bestImprovementIndex > 0;

    if (release) {
      if (!reinsert_seeds) {
        for (HypernodeID u : localData.seedVertices) {
          sharedData.fruitlessSeed.set(u, true);
        }
      }

      for (PartitionID i = 0; i < k; ++i) {
        for (PosT j = 0; j < vertexPQs[i].size(); ++j) {
          const HypernodeID node = vertexPQs[i].at(j);
          sharedData.nodeTracker.releaseNode(node);
          if (!sharedData.fruitlessSeed[node] && sharedData.refinementNodes.was_pushed_and_removed(node)) {
            sharedData.refinementNodes.concurrent_push(node);
          }
        }
      }
    }

    for (PartitionID i = 0; i < k; ++i) {
      vertexPQs[i].clear();
    }
    blockPQ.clear();
  }

  void updateBlock(PartitionID i) {
    if (!vertexPQs[i].empty()) {
      blockPQ.insertOrAdjustKey(i, vertexPQs[i].topKey());
    } else if (blockPQ.contains(i)) {
      blockPQ.remove(i);
    }
  }

  template<typename PHG>
  void updateBlocks(const PHG& phg, const PartitionID moved_from) {
    if (moved_from == kInvalidPartition || updateDeduplicator.size() >= size_t(k)) {
      for (PartitionID i = 0; i < k; ++i) {
        updateBlock(i);
      }
    } else {
      updateBlock(moved_from);
      for (const auto& sparse_map_element : updateDeduplicator) {
        updateBlock(phg.partID(sparse_map_element.key));
      }
    }
    updateDeduplicator.clear();
  }

  template<typename PHG>
  void insertOrUpdateNeighbors(const PHG& phg,
                               FMSharedData& sharedData,
                               const HypernodeID u) {
    for (HyperedgeID e : phg.incidentEdges(u)) {
      if (phg.edgeSize(e) < context.partition.hyperedge_size_threshold && !validHyperedges[e]) {
        for (HypernodeID v : phg.pins(e)) {
          if (!updateDeduplicator.contains(v)) {
            updateDeduplicator[v] = { };  // insert
            insertOrUpdatePQ(phg, v, sharedData.nodeTracker);
          }
        }
        validHyperedges[e] = true;
      }
    }
  }

  template<typename PHG>
  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE
  bool insertOrUpdatePQ(const PHG& phg,
                        const HypernodeID v,
                        NodeTracker& nt) {
    SearchID searchOfV = nt.searchOfNode[v].load(std::memory_order_acq_rel);
    // Note. Deactivated nodes have a special active search ID so that neither branch is executed
    if (nt.isSearchInactive(searchOfV)) {
      if (nt.searchOfNode[v].compare_exchange_strong(searchOfV, thisSearch, std::memory_order_acq_rel)) {
        const PartitionID pv = phg.partID(v);
        const Gain gain = bestDestinationBlock(phg, v).second;
        vertexPQs[pv].insert(v, gain);  // blockPQ updates are done later, collectively.
        localData.runStats.pushes++;
        return true;
      }
    } else if (searchOfV == thisSearch) {
      const PartitionID pv = phg.partID(v);
      assert(vertexPQs[pv].contains(v));
      const Gain gain = bestDestinationBlock(phg, v).second;
      vertexPQs[pv].adjustKey(v, gain);
      // if pv == move.from or pv == move.to only the gains of move.from and move.to could change
      // if these gains are better than vertexPQ[pv].keyOf(v), we could increase the key
      // however, this is incorrect if this entry is for the target move.from or move.to.
      // since we don't store this information (on purpose), we can't easily figure that out
      return true;
    }
    return false;
  }

  template<typename PHG>
  MT_KAHYPAR_ATTRIBUTE_ALWAYS_INLINE
  std::pair<PartitionID, HyperedgeWeight> bestDestinationBlock(const PHG& phg,
                                                               const HypernodeID u) {
    const HypernodeWeight wu = phg.nodeWeight(u);
    const PartitionID from = phg.partID(u);
    const HypernodeWeight from_weight = phg.partWeight(from);
    PartitionID to = kInvalidPartition;
    HyperedgeWeight to_penalty = std::numeric_limits<HyperedgeWeight>::max();
    HypernodeWeight best_to_weight = from_weight - wu;
    for (PartitionID i = 0; i < k; ++i) {
      if (i != from) {
        const HypernodeWeight to_weight = phg.partWeight(i);
        const HyperedgeWeight penalty = phg.moveToPenalty(u, i);
        if ( ( penalty < to_penalty || ( penalty == to_penalty && to_weight < best_to_weight ) ) &&
              ( to_weight + wu <= context.partition.max_part_weights[i] || to_weight < best_to_weight ) ) {
          to_penalty = penalty;
          to = i;
          best_to_weight = to_weight;
        }
      }
    }
    return std::make_pair(to, phg.moveFromBenefit(u) - to_penalty);
  }

  template<typename PHG>
  bool findNextMove(const PHG& phg, Move& m) {
    if (blockPQ.empty()) {
      return false;
    }
    while (true) {
      const PartitionID from = blockPQ.top();
      const HypernodeID u = vertexPQs[from].top();
      const Gain estimated_gain = vertexPQs[from].topKey();
      assert(estimated_gain == blockPQ.topKey());
      auto [to, gain] = bestDestinationBlock(phg, u);
      if (gain >= estimated_gain) { // accept any gain that is at least as good
        m.node = u; m.to = to; m.from = from;
        m.gain = gain;
        localData.runStats.extractions++;
        vertexPQs[from].deleteTop();  // blockPQ updates are done later, collectively.
        return true;
      } else {
        localData.runStats.retries++;
        vertexPQs[from].adjustKey(u, gain);
        if (vertexPQs[from].topKey() != blockPQ.keyOf(from)) {
          blockPQ.adjustKey(from, vertexPQs[from].topKey());
        }
      }
    }
  }

  // ! Makes moves applied on delta hypergraph visible on the global partitioned hypergraph.
  std::pair<Gain, size_t> applyMovesOnGlobalHypergraph(PartitionedHypergraph& phg,
                                                       FMSharedData& sharedData,
                                                       const size_t bestGainIndex,
                                                       const Gain bestEstimatedImprovement) {
    ASSERT(localData.localMoveIDs.empty());
    Gain estimatedImprovement = 0;
    Gain lastGain = 0;

    auto delta_gain_func = [&](const HyperedgeID he,
                               const HyperedgeWeight edge_weight,
                               const HypernodeID edge_size,
                               const HypernodeID pin_count_in_from_part_after,
                               const HypernodeID pin_count_in_to_part_after) {
      lastGain += km1Delta(he, edge_weight, edge_size,
        pin_count_in_from_part_after, pin_count_in_to_part_after);
    };

    // Apply move sequence to original hypergraph and update gain values
    Gain bestImprovement = 0;
    size_t bestIndex = 0;
    for ( size_t i = 0; i < bestGainIndex; ++i ) {
      Move& m = localData.localMoves[i];
      MoveID m_id = std::numeric_limits<MoveID>::max();
      lastGain = 0;
      phg.changeNodePartFullUpdate(m.node, m.from, m.to, std::numeric_limits<HypernodeWeight>::max(),
        [&] { m_id = sharedData.moveTracker.insertMove(m); }, delta_gain_func);
      lastGain = -lastGain; // delta func yields negative sum of improvements, i.e. negative values mean improvements
      estimatedImprovement += lastGain;
      ASSERT(m_id != std::numeric_limits<MoveID>::max());
      Move& move = sharedData.moveTracker.getMove(m_id);
      move.gain = lastGain; // Update gain value based on hypergraph delta
      localData.localMoveIDs.push_back(m_id);
      if ( estimatedImprovement >= bestImprovement ) {
        bestImprovement = estimatedImprovement;
        bestIndex = i;
      }
    }

    // Kind of double rollback, if gain values are not correct
    ASSERT(localData.localMoveIDs.size() == bestGainIndex);
    if ( estimatedImprovement < 0 ) {
      localData.runStats.local_reverts += localData.localMoves.size() - bestIndex;
      for ( size_t i = bestIndex + 1; i < bestGainIndex; ++i ) {
        Move& m = sharedData.moveTracker.getMove(localData.localMoveIDs[i]);
        phg.changeNodePartFullUpdate(m.node, m.to, m.from);
        sharedData.moveTracker.invalidateMove(m);
      }
      localData.runStats.local_reverts += localData.localMoves.size() - bestGainIndex;
      return std::make_pair(bestImprovement, bestIndex);
    } else {
      return std::make_pair(bestEstimatedImprovement, bestGainIndex);
    }
  }

  // ! Rollback to the best improvement found during local search in case we applied moves
  // ! directly on the global partitioned hypergraph.
  void revertToBestLocalPrefix(PartitionedHypergraph& phg, FMSharedData& sharedData, size_t bestGainIndex) {
    localData.runStats.local_reverts += localData.localMoveIDs.size() - bestGainIndex;
    while (localData.localMoveIDs.size() > bestGainIndex) {
      Move& m = sharedData.moveTracker.getMove(localData.localMoveIDs.back());
      phg.changeNodePartFullUpdate(m.node, m.to, m.from);
      sharedData.moveTracker.invalidateMove(m);
      localData.localMoveIDs.pop_back();
    }
  }

 public:
  FMStats stats;

 private:

  const Context& context;

  // ! Unique search id associated with the current local search
  SearchID thisSearch;

  // ! Number of blocks
  PartitionID k;

  // ! Local data members required for one localized search run
  FMLocalData localData;

  // ! Wrapper around the global partitioned hypergraph, that allows
  // ! to perform moves non-visible for other local searches
  DeltaPartitionedHypergraph deltaPhg;

  // ! Priority Queue that contains for each block of the partition
  // ! the vertex with the best gain value
  BlockPriorityQueue blockPQ;

  // ! From PQs -> For each block it contains the vertices (contained
  // ! in that block) touched by the current local search associated
  // ! with their gain values
  vec<VertexPriorityQueue> vertexPQs;

  // ! After a move it collects all neighbors of the moved vertex
  ds::DynamicSparseSet<HypernodeID> updateDeduplicator;

  // ! Marks all hyperedges that are visited during the local search
  // ! and where the gain of its pin is expected to be equal to gain value
  // ! inside the PQs. A hyperedge can become invalid if a move changes the
  // ! gain values of its pins.
  ds::DynamicSparseMap<HyperedgeID, bool> validHyperedges;
};

}