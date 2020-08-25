/*******************************************************************************
 * This file is part of KaHyPar.
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

#include "gmock/gmock.h"

#include "mt-kahypar/parallel/parallel_prefix_sum.h"

#include <random>
#include <algorithm>

using ::testing::Test;

namespace mt_kahypar {

  TEST(PrefixSumTest, AllZeroes) {
    size_t n = 1 << 19;
    vec<size_t> in(n, 0);
    vec<size_t> out(n, 420);

    for (size_t i = 0; i < n; ++i) {
      ASSERT_EQ(in[i], 0);
      ASSERT_EQ(out[i], 420);
    }

    parallel_prefix_sum(in.begin(), in.end(), out.begin(), std::plus(), 0);

    for (size_t i = 0; i < n; ++i) {
      if (in[i] != out[i]) {
        LOG << V(i) << V(in[i]) << V(out[i]) << V(n);
        FAIL();
      }
    }
  }

  TEST(PrefixSumTest, MatchesSequential) {
    size_t n = 1 << 19;
    vec<size_t> in(n, 0);

    std::mt19937 rng(420);
    std::generate(in.begin(), in.end(), rng);

    vec<size_t> out_parallel(n, 420);
    parallel_prefix_sum(in.begin(), in.end(), out_parallel.begin(), std::plus(), 0);

    vec<size_t> out_custom_seq;
    sequential_prefix_sum(in.begin(), in.end(), std::back_inserter(out_custom_seq), 0, std::plus());

    vec<size_t> out_stl;
    std::partial_sum(in.begin(), in.end(), std::back_inserter(out_stl), std::plus());

    ASSERT_EQ(out_custom_seq, out_parallel);
    ASSERT_EQ(out_parallel, out_stl);
  }

  TEST(PrefixSumTest, WorksInplace) {
    size_t n = 1 << 19;
    vec<size_t> in(n, 0);
    std::mt19937 rng(421);
    std::generate(in.begin(), in.end(), rng);
    vec<size_t> in_stl = in;

    parallel_prefix_sum(in.begin(), in.end(), in.begin(), std::plus(), 0);
    std::partial_sum(in_stl.begin(), in_stl.end(), in_stl.begin(), std::plus());

    ASSERT_EQ(in, in_stl);
  }

}