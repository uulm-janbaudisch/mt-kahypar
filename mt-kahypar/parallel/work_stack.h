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

#include <tbb/concurrent_queue.h>
#include "../definitions.h"

namespace mt_kahypar {

// scheduling the same refinement node directly in the next round is no bueno since it is unlikely to have a positive gain --> make it an actual queue not a dequeue

// SingleProducerMultipleConsumerQueue (actually Deque but we don't want to use that functionality)

template<typename T>
struct SPMCQueue {
  static constexpr size_t in_reallocation = std::numeric_limits<size_t>::max() / 2;
  static constexpr bool move_to_front_after_reallocation = true;


  SPMCQueue() {
    clear();
    elements.reserve(1 << 13);
  }

  vec<T> elements;
  CAtomic<size_t> front;

  void clear() {
    elements.clear();
    front.store(0, std::memory_order_relaxed);
  }

  template<bool unchecked_push>
  void push_back(const T& el) {
    if constexpr (unchecked_push) {
      elements.push_back(el);
    } else {

      // another counter-measure against incrementing beyond size
      // it's not terribly bad to lose some of these elements since it means we're at the end of the move phase
      // still try to counter act.
      if (load_front() > elements.size()) {
        front.store(elements.size(), std::memory_order_acq_rel);
      }

      if (elements.size() < elements.capacity()) {
        elements.push_back(el);
      } else {
        size_t old_front = load_front();
        size_t front_spin_variable = std::min(elements.size(), old_front);
        while (front.compare_exchange_weak(front_spin_variable, in_reallocation, std::memory_order_acq_rel)) { /* spin */ }
        elements.push_back(el); // causes reallocation
        if constexpr (move_to_front_after_reallocation) {
          old_front = std::min(elements.size() - 1, old_front);
          size_t elements_in_queue = elements.size() - old_front;
          for (size_t i = 0, j = old_front; i < elements_in_queue; ++i, ++j) {
            elements[i] = elements[j];
          }
          elements.erase(elements.begin() + elements_in_queue, elements.end());
          front.store(0, std::memory_order_acq_rel);          // release
        } else {
          front.store(old_front, std::memory_order_acq_rel);  // release
        }
      }

    }
  }

  bool try_pop_front(T& el) {
    size_t f = load_front();
    // this extra check still allows #threads fetch_add beyond size()
    // but it should reduce that amount.
    if (f < in_reallocation && f < elements.size()) {
      size_t slot = front.fetch_add(1, std::memory_order_acq_rel);
      if (slot < in_reallocation) {
        if (slot < elements.size()) { // elements.size() can be completely broken during reallocation
          el = elements[slot];
          return true;
        }
      }
    }
    return false;
  }

  bool currently_blocked() const {
    return front.load(std::memory_order_acq_rel) >= std::numeric_limits<size_t>::max() / 2;
  }

  size_t load_front() const {
    return front.load(std::memory_order_acq_rel);
  }

  size_t unsafe_size() const {
    const size_t f = front.load(std::memory_order_acq_rel), b = elements.size();
    return b >= f ? b - f : 0;
  }

  bool empty() const {
    return unsafe_size() > 0;
  }

  bool next_push_causes_reallocation() {
    return elements.size() == elements.capacity();
  }
};

template<typename T>
struct WorkContainer {

  using TimestampT = uint32_t;
  using Queue = SPMCQueue<T>;

  WorkContainer(size_t maxNumElements) : timestamps(maxNumElements, 0) { }

  size_t unsafe_size() const {
    size_t sz = 0;
    for (const Queue& x : tls_queues) {
      sz += x.unsafe_size();
    }
    return sz;
  }

  template<bool unchecked_push>
  void push_back(const T el) {
    tls_queues.local().template push_back<unchecked_push>(el);
    timestamps[el] = current;
  }

  bool try_pop(T& dest) {
    // use pop_front even on the thread local queue to avoid immediately reusing a just released node
    if (tls_queues.local().try_pop_front(dest)) {
      timestamps[dest] = current+1;
      return true;
    } else {

      // try stealing
      bool some_are_blocked = false;
      for (Queue& other_queue : tls_queues) {
        if (other_queue.try_pop_front(dest)) {
          timestamps[dest] = current+1;
          return true;
        } else {
          some_are_blocked |= other_queue.currently_blocked();
        }
      }

      if (some_are_blocked && steal_failures.fetch_add(1, std::memory_order_relaxed) < 1024) {
        // stealing failed --> check if any of them are currently blocked. if so spin. otherwise return false
        for (Queue& other : tls_queues) {
          if (other.currently_blocked()) {
            while (other.currently_blocked()) { /* spin */ }
            if (other.try_pop_front(dest)) {
              timestamps[dest] = current+1;
              return true;
            }
          }
        }
      }
    }
    return false;
  }

  bool was_pushed_and_removed(const T el) const {
    return timestamps[el] == current+1;
  }

  void shuffle() {
    tbb::parallel_for_each(tls_queues, [&](Queue& tlq) {
      assert(tlq.front == 0);
      std::shuffle(tlq.elements.begin(), tlq.elements.end(), utils::Randomize::instance().getGenerator());
    });

  }

  void clear() {
    if (current >= std::numeric_limits<TimestampT>::max() - 2) {
      tbb::parallel_for_each(timestamps, [](TimestampT& x) { x = 0; });
      current = 0;
    }
    for (SPMCQueue<T>& tlq : tls_queues) {
      tlq.clear();
    }
    current += 2;
    steal_failures.store(0, std::memory_order_relaxed);
  }

  TimestampT current = 2;
  vec<TimestampT> timestamps;
  CAtomic<size_t> steal_failures { 0 };
  tls_enumerable_thread_specific<Queue> tls_queues;

};


}