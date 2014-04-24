// Copyright (c) 2014, Salesforce.com, Inc.  All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// - Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// - Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// - Neither the name of Salesforce.com nor the names of its contributors
//   may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
// OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
// TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
// USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include <distributions/common.hpp>
#include <distributions/special.hpp>
#include <distributions/random.hpp>
#include <distributions/sparse_counter.hpp>
#include <distributions/vector.hpp>
#include <distributions/vector_math.hpp>

namespace distributions {
namespace dirichlet_process_discrete {
typedef uint32_t count_t;
typedef uint32_t Value;
struct Group;
struct Scorer;
struct Sampler;
struct Mixture;


struct Shared
{
    float gamma;
    float alpha;
    float beta0;
    std::vector<float> betas;  // dense

    static constexpr Value OTHER () { return -1; }

    static Shared EXAMPLE () {
        Shared shared;
        size_t dim = 100;
        shared.gamma = 0.5;
        shared.alpha = 0.5;
        shared.beta0 = 0.0;  // must be zero for testing
        shared.betas.resize(dim, 1.0 / dim);
        return shared;
    }
};


struct Group
{
    SparseCounter<Value, count_t> counts;  // sparse

    void init (
            const Shared &,
            rng_t &)
    {
        counts.clear();
    }

    void add_value (
            const Shared &,
            const Value & value,
            rng_t &)
    {
       counts.add(value);
    }

    void remove_value (
            const Shared &,
            const Value & value,
            rng_t &)
    {
       counts.remove(value);
    }

    void merge (
            const Shared &,
            const Group & source,
            rng_t &)
    {
        counts.merge(source.counts);
    }
};

struct Sampler
{
    std::vector<float> probs;  // dense

    void init (
            const Shared & shared,
            const Group & group,
            rng_t & rng)
    {
        probs.clear();
        probs.reserve(shared.betas.size() + 1);
        for (float beta : shared.betas) {
            probs.push_back(beta * shared.alpha);
        }
        for (auto i : group.counts) {
            probs[i.first] += i.second;
        }
        probs.push_back(shared.beta0 * shared.alpha);

        sample_dirichlet(rng, probs.size(), probs.data(), probs.data());
    }

    Value eval (
            const Shared & shared,
            rng_t & rng) const
    {
        return sample_discrete(rng, probs.size(), probs.data());
    }
};

struct Scorer
{
    std::vector<float> scores;  // dense

    void init (
            const Shared & shared,
            const Group & group,
            rng_t &)
    {
        const size_t size = shared.betas.size();
        const size_t total = group.counts.get_total();
        scores.resize(size);

        const float betas_scale = shared.alpha / (shared.alpha + total);
        for (size_t i = 0; i < size; ++i) {
            scores[i] = betas_scale * shared.betas[i];
        }

        const float counts_scale = 1.0f / (shared.alpha + total);
        for (auto i : group.counts) {
            Value value = i.first;
            DIST_ASSERT(value < size,
                "unknown DPM value: " << value << " >= " << size);
            scores[value] += counts_scale * i.second;
        }
    }

    float eval (
            const Shared & shared,
            const Value & value,
            rng_t &) const
    {
        size_t size = scores.size();
        DIST_ASSERT(value < size,
            "unknown DPM value: " << value << " >= " << size);
        return fast_log(scores[value]);
    }
};

struct Mixture
{
    typedef dirichlet_process_discrete::Value Value;
    typedef dirichlet_process_discrete::Shared Shared;
    typedef dirichlet_process_discrete::Group Group;
    typedef dirichlet_process_discrete::Scorer Scorer;

    std::vector<Group> groups;
    std::vector<VectorFloat> scores;  // dense
    VectorFloat scores_shift;

    void init (
            const Shared & shared,
            rng_t &)
    {
        const Value dim = shared.betas.size();
        const size_t group_count = groups.size();
        scores_shift.resize(group_count);
        scores.resize(dim);
        for (Value value = 0; value < dim; ++value) {
            scores[value].resize(group_count);
        }
        for (size_t groupid = 0; groupid < group_count; ++groupid) {
            const Group & group = groups[groupid];
            for (Value value = 0; value < dim; ++value) {
                scores[value][groupid] =
                    shared.alpha * shared.betas[value] + group.counts.get_count(value);
            }
            scores_shift[groupid] = shared.alpha + group.counts.get_total();
        }
        vector_log(group_count, scores_shift.data());
        for (Value value = 0; value < dim; ++value) {
            vector_log(group_count, scores[value].data());
        }
    }

    void add_group (
            const Shared & shared,
            rng_t & rng)
    {
        const Value dim = shared.betas.size();
        const size_t group_count = groups.size() + 1;
        groups.resize(group_count);
        groups.back().init(shared, rng);
        scores_shift.resize(group_count, 0);
        for (Value value = 0; value < dim; ++value) {
            scores[value].resize(group_count, 0);
        }
    }

    void remove_group (
            const Shared & shared,
            size_t groupid)
    {
        DIST_ASSERT1(groupid < groups.size(), "bad groupid: " << groupid);
        const Value dim = shared.betas.size();
        const size_t group_count = groups.size() - 1;
        if (groupid != group_count) {
            std::swap(groups[groupid], groups.back());
            scores_shift[groupid] = scores_shift.back();
            for (Value value = 0; value < dim; ++value) {
                VectorFloat & vscores = scores[value];
                vscores[groupid] = vscores.back();
            }
        }
        groups.resize(group_count);
        scores_shift.resize(group_count);
        for (Value value = 0; value < dim; ++value) {
            scores[value].resize(group_count);
        }
    }

    void add_value (
            const Shared & shared,
            size_t groupid,
            const Value & value,
            rng_t &)
    {
        DIST_ASSERT1(groupid < groups.size(), "bad groupid: " << groupid);
        DIST_ASSERT1(value < shared.betas.size(), "value out of bounds");
        Group & group = groups[groupid];
        count_t count = group.counts.add(value);
        count_t count_sum = group.counts.get_total();
        scores[value][groupid] = fast_log(shared.alpha * shared.betas[value] + count);
        scores_shift[groupid] = fast_log(shared.alpha + count_sum);
    }

    void remove_value (
            const Shared & shared,
            size_t groupid,
            const Value & value,
            rng_t &)
    {
        DIST_ASSERT2(groupid < groups.size(), "bad groupid: " << groupid);
        DIST_ASSERT1(value < shared.betas.size(), "value out of bounds");
        Group & group = groups[groupid];
        count_t count = group.counts.remove(value);
        count_t count_sum = group.counts.get_total();
        scores[value][groupid] = fast_log(shared.alpha * shared.betas[value] + count);
        scores_shift[groupid] = fast_log(shared.alpha + count_sum);
    }

    void score_value (
            const Shared & shared,
            const Value & value,
            VectorFloat & scores_accum,
            rng_t &) const
    {
        DIST_ASSERT1(value < shared.betas.size(), "value out of bounds");
        if (DIST_DEBUG_LEVEL >= 2) {
            DIST_ASSERT_EQ(scores_accum.size(), groups.size());
        }
        const size_t group_count = groups.size();
        vector_add_subtract(
            group_count,
            scores_accum.data(),
            scores[value].data(),
            scores_shift.data());
    }
};

} // namespace dirichlet_process_discrete

inline dirichlet_process_discrete::Value sample_value (
        const dirichlet_process_discrete::Shared & shared,
        const dirichlet_process_discrete::Group & group,
        rng_t & rng)
{
    dirichlet_process_discrete::Sampler sampler;
    sampler.init(shared, group, rng);
    return sampler.eval(shared, rng);
}

inline float score_value (
        const dirichlet_process_discrete::Shared & shared,
        const dirichlet_process_discrete::Group & group,
        const dirichlet_process_discrete::Value & value,
        rng_t & rng)
{
    dirichlet_process_discrete::Scorer scorer;
    scorer.init(shared, group, rng);
    return scorer.eval(shared, value, rng);
}

inline float score_group (
        const dirichlet_process_discrete::Shared & shared,
        const dirichlet_process_discrete::Group & group,
        rng_t &)
{
    const size_t size = shared.betas.size();
    const size_t total = group.counts.get_total();

    float score = 0;
    for (auto i : group.counts) {
        dirichlet_process_discrete::Value value = i.first;
        DIST_ASSERT(value < size,
            "unknown DPM value: " << value << " >= " << size);
        float prior_i = shared.betas[value] * shared.alpha;
        score += fast_lgamma(prior_i + i.second)
               - fast_lgamma(prior_i);
    }
    score += fast_lgamma(shared.alpha)
           - fast_lgamma(shared.alpha + total);

    return score;
}

} // namespace distributions
