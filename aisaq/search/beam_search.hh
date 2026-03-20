#ifndef AISAQ_SEARCH_BEAM_SEARCH_HH
#define AISAQ_SEARCH_BEAM_SEARCH_HH

#include <immintrin.h>
#include "pump/sender/just.hh"
#include "pump/sender/then.hh"
#include "pump/sender/flat.hh"
#include "pump/sender/on.hh"
#include "pump/sender/push_context.hh"
#include "pump/sender/get_context.hh"
#include "pump/sender/pop_context.hh"
#include "pump/sender/for_each.hh"
#include "pump/sender/concurrent.hh"
#include "pump/sender/reduce.hh"
#include "pump/sender/generate.hh"
#include "pump/coro/coro.hh"

#include "env/scheduler/task/tasks_scheduler.hh"
#include "env/scheduler/nvme/scheduler.hh"
#include "env/scheduler/nvme/sender.hh"

#include "../pq/codebook.hh"
#include "../index/meta_data.hh"
#include "../index/node_accessor.hh"
#include "../index/index.hh"
#include "../nvme/sector_page.hh"
#include "../cache/node_cache.hh"
#include "../cache/scheduler.hh"
#include "state.hh"

namespace aisaq::search {
    using namespace pump::sender;
    using task_scheduler_t = pump::scheduler::task::scheduler;
    using preemptive_scheduler_t = pump::scheduler::task::preemptive_scheduler;

    // ── Per-core scheduler globals (set once at startup, no contention) ──
    inline preemptive_scheduler_t* g_preemptive = nullptr;
    inline thread_local task_scheduler_t* g_task = nullptr;
    inline task_scheduler_t* g_task_list[128] = {};
    inline uint32_t g_task_count = 0;
    inline task_scheduler_t* random_task() {
        return g_task_list[__rdtsc() % g_task_count];
    }
    static constexpr uint32_t MAX_NVME_DEVICES = 4;
    inline thread_local nvme::nvme_scheduler_t* g_nvme[MAX_NVME_DEVICES] = {};
    inline thread_local cache::cache_scheduler* g_cache = nullptr;
    inline const nvme::ssd_t* g_ssd[MAX_NVME_DEVICES] = {};
    inline uint32_t g_num_devices = 1;

    // ── Configuration ──

    struct
    search_config {
        uint32_t k = 10;
        uint32_t search_list = 100;
        uint32_t beam_width = 4;
        uint32_t num_probes = 1;     // independent beam searches from different entry points
        uint32_t max_iterations = 0;  // 0 = auto (search_list / beam_width * 2)
        uint32_t io_limit = UINT32_MAX;
    };

    struct
    search_result {
        struct item {
            uint32_t id;
            float distance;
        };
        std::vector<item> results;
        uint64_t io_ns = 0;
        uint64_t cpu_ns = 0;
        uint32_t io_count = 0;
        uint32_t iter_count = 0;
    };

    // ── NVMe beam read request ──

    struct
    beam_read {
        uint32_t node_id;
        uint64_t sector_id;
        uint32_t device;      // target NVMe device index
    };

    // ── Per-query search context (pushed to pipeline context stack) ──

    struct
    search_ctx {
        search_state state;
        pq::dist_table dtable;
        const index::disk_index* idx;
        search_config cfg;
        const float* query;
        task_scheduler_t* base_task = nullptr;

        // Separate result set with exact L2 distances (not in candidates)
        std::vector<search_result::item> exact_results;

        // Timing (for profiling)
        uint64_t io_ns = 0;
        uint64_t cpu_ns = 0;
        uint32_t iter_count = 0;  // actual beam iterations (non-empty)

        // Pre-allocated DMA buffers for beam reads (beam_width slots, reused across iterations)
        nvme::node_read_buf read_bufs[4];

        search_ctx(const index::disk_index& idx, const search_config& cfg,
                   const float* q)
            : state(cfg.search_list, idx.meta.npts)
            , idx(&idx), cfg(cfg), query(q) {
            for (auto& rb : read_bufs)
                rb.acquire();
        }

        search_ctx(search_ctx&&) = default;
        search_ctx& operator=(search_ctx&&) = default;

        ~search_ctx() {
            for (auto& rb : read_bufs)
                rb.release();
        }

        // Per-iteration: track which bufs have valid data
        struct pending_node { uint32_t node_id; void* dma; };
        pending_node pending[4];
        uint32_t next_buf = 0;
        uint32_t pending_count = 0;

        nvme::node_read_buf& acquire_read_buf(uint32_t node_id) {
            auto& nb = read_bufs[next_buf % 4];
            pending[next_buf % 4] = {node_id, nb.dma_buf};
            next_buf++;
            return nb;
        }

        void reset_read_bufs() { next_buf = 0; pending_count = 0; }

        // Batch process all successfully read nodes after reduce
        void process_pending_nodes() {
            for (uint32_t i = 0; i < next_buf && i < 4; ++i)
                process_node(pending[i].node_id,
                             static_cast<const uint8_t*>(pending[i].dma));
        }

        // Fire-and-forget: enqueue NVMe-read nodes to cache scheduler.
        // Skip when cache is full — avoids alloc+memcpy churn on every miss.
        void cache_pending_nodes() {
            if (!g_cache) return;
            if (g_cache->cache->count >= g_cache->cache->max_nodes) return;
            uint32_t len = idx->locator.max_node_len;
            for (uint32_t i = 0; i < next_buf && i < 4; ++i)
                g_cache->schedule_insert(pending[i].node_id, pending[i].dma, len);
        }

        bool converged = false;

        // Select beam nodes for one iteration.
        // Cached nodes are processed immediately (sync).
        // Returns uncached nodes that need NVMe reads.
        // Sets converged=true when search should stop.
        std::vector<beam_read>
        prepare_beam_iteration() {
            converged = false;
            std::vector<beam_read> reads;
            if (!state.candidates.has_unexpanded()) { converged = true; return reads; }
            if (state.io_count >= cfg.io_limit) { converged = true; return reads; }

            // Early stop: if the closest unexpanded node is farther than
            // the worst candidate in the list, further expansion won't improve results
            if (state.candidates.size() >= state.candidates.capacity) {
                float worst = state.candidates.worst_distance();
                if (state.candidates.closest_unexpanded_distance() > worst) {
                    converged = true;
                    return reads;
                }
            }

            iter_count++;
            uint32_t positions[256];
            uint32_t count = state.candidates.select_beam(cfg.beam_width, positions);

            for (uint32_t b = 0; b < count; ++b) {
                uint32_t node_id = state.candidates[positions[b]].id;

                const uint8_t* cached = g_cache ? g_cache->cache->lookup(node_id) : nullptr;
                if (cached) {
                    g_cache->schedule_promote(node_id);
                    process_node(node_id, cached);
                } else {
                    reads.push_back({node_id, idx->locator.get_sector_id(node_id),
                                     idx->locator.get_device(node_id)});
                    state.io_count++;
                }
            }
            return reads;
        }

        // Process node from contiguous DMA buffer — zero copy
        void
        process_dma_node(uint32_t node_id, void* dma_buf) {
            process_node(node_id, static_cast<const uint8_t*>(dma_buf));
        }

        // Compute exact L2 distance between query and a full-precision vector (AVX2)
        float
        exact_l2(const float* vec) const {
            uint32_t ndims = idx->meta.ndims;
#ifdef __AVX2__
            __m256 sum8 = _mm256_setzero_ps();
            uint32_t j = 0;
            for (; j + 8 <= ndims; j += 8) {
                __m256 va = _mm256_loadu_ps(query + j);
                __m256 vb = _mm256_loadu_ps(vec + j);
                __m256 diff = _mm256_sub_ps(va, vb);
                sum8 = _mm256_fmadd_ps(diff, diff, sum8);
            }
            __m128 hi = _mm256_extractf128_ps(sum8, 1);
            __m128 lo = _mm256_castps256_ps128(sum8);
            __m128 sum4 = _mm_add_ps(lo, hi);
            sum4 = _mm_hadd_ps(sum4, sum4);
            sum4 = _mm_hadd_ps(sum4, sum4);
            float dist = _mm_cvtss_f32(sum4);
            for (; j < ndims; ++j) {
                float diff = query[j] - vec[j];
                dist += diff * diff;
            }
            return dist;
#else
            float dist = 0.0f;
            for (uint32_t j = 0; j < ndims; ++j) {
                float diff = query[j] - vec[j];
                dist += diff * diff;
            }
            return dist;
#endif
        }

        // Process a single node: exact distance to result set + PQ distance for neighbors
        // candidates uses PQ distances only (controls search direction & termination)
        // exact_results collects exact L2 distances (final output)
        //
        // Batch PQ: collect unvisited neighbor indices, then outer-chunk inner-neighbor
        // loop for L1-friendly distance table access. Zero copy from DMA buffer.
        void
        process_node(uint32_t node_id, const uint8_t* node_data) {
            index::node_accessor node(node_data, idx->meta.ndims,
                                       idx->codebook.n_chunks, idx->meta.inline_pq_width);

            // Exact L2 distance for this expanded node → separate result set
            exact_results.push_back({node_id, exact_l2(node.get_vector())});

            // Pass 1: collect unvisited neighbors with inline PQ (indices only, no copy)
            uint32_t degree = node.get_degree();
            uint32_t n_chunks = idx->codebook.n_chunks;
            uint32_t batch_idx[256];
            uint32_t batch_n = 0;

            for (uint32_t i = 0; i < degree; ++i) {
                uint32_t nbr = node.get_neighbor(i);
                if (nbr >= idx->meta.npts) continue;
                if (state.visited.test_and_set(nbr)) continue;
                if (node.has_inline_pq(i))
                    batch_idx[batch_n++] = i;
            }

            if (batch_n == 0) return;

            // Pass 2: batch PQ distance — outer chunk, inner neighbors → L1 friendly
            // Precompute PQ code pointers to eliminate inner-loop multiply.
            const uint8_t* pq_base = node.get_inline_pq(0);
            float batch_dists[256];
            std::memset(batch_dists, 0, batch_n * sizeof(float));

            const uint8_t* batch_pq[256];
            for (uint32_t j = 0; j < batch_n; ++j)
                batch_pq[j] = pq_base + batch_idx[j] * n_chunks;

            for (uint32_t c = 0; c < n_chunks; ++c) {
                const float* chunk_table = dtable.table.data() + c * NUM_PQ_CENTROIDS;
                for (uint32_t j = 0; j < batch_n; ++j)
                    batch_dists[j] += chunk_table[batch_pq[j][c]];
            }

            // Pass 3: insert into candidates
            for (uint32_t j = 0; j < batch_n; ++j)
                state.candidates.insert(node.get_neighbor(batch_idx[j]), batch_dists[j]);
        }
    };

    // ── Pipeline building blocks ──

    // Step 1: precompute PQ distance table for the query
    inline auto
    precompute_distances() {
        return get_context<search_ctx>()
            >> then([](search_ctx& ctx) {
                ctx.dtable.precompute(ctx.idx->codebook, ctx.query);
            });
    }

    // Step 2: seed candidates with entry point(s)
    // probe_id and num_probes determine which subset of entry points to use
    inline void
    init_candidates_for_probe(search_ctx& ctx, uint32_t probe_id, uint32_t num_probes) {
        ctx.state.reset();
        ctx.exact_results.clear();
        auto& idx = *ctx.idx;
        auto& dt = ctx.dtable;

        uint32_t n_entries = idx.all_entry_ids.size();
        for (uint32_t i = 0; i < n_entries; ++i) {
            // Distribute entry points across probes: round-robin
            if (num_probes > 1 && (i % num_probes) != probe_id) continue;
            uint32_t ep = idx.all_entry_ids[i];
            if (ctx.state.visited.test_and_set(ep)) continue;
            ctx.state.candidates.insert(ep, dt.lookup(idx.get_entry_pq_codes(i)));
        }
    }

    // Step 3a: single beam search iteration
    //
    // Pipeline: prepare_beam → for_each(uncached) >> concurrent >> nvme_read >> reduce
    // ctx from context stack, no closure capture needed.

    inline auto
    beam_search_iteration() {
        return get_context<search_ctx>()
            >> then([](search_ctx &ctx, auto &&) {
                ctx.reset_read_bufs();
                return ctx.prepare_beam_iteration();
            })
            >> for_each()
            >> concurrent(4) // bounded: max beam_width IOs per iteration
            >> get_context<search_ctx>()
            >> flat_map([](search_ctx &ctx, auto &&r) {
                auto &nb = ctx.acquire_read_buf(r.node_id);
                uint32_t dev = r.device;
                nb.setup(r.sector_id, g_ssd[dev]);

                return just()
                    >> pump::scheduler::nvme::get_pages(nb.page_range(), g_nvme[dev]);
            })
            >> reduce()
            >> get_context<search_ctx>()
            >> flat_map([](const search_ctx& ctx, ...) {
                return just() >> on(ctx.base_task->as_task());
            })
            // Per-core NVMe qpair: IO completion stays on same core, no bounce needed
            >> get_context<search_ctx>()
            >> then([](search_ctx &ctx) {
                ctx.process_pending_nodes();
                ctx.cache_pending_nodes();
            });
    }

    // Step 3b: iteration loop with early termination
    // reduce returns reduce_done when converged → loop stops immediately

    inline auto
    make_search_loop_coro(search_ctx& ctx, uint32_t max_iterations) -> pump::coro::return_yields<bool> {
        for (uint32_t i = 0; i < max_iterations; ++i) {
            if (ctx.converged)
                break;
            else
                co_yield true;
        }
        co_return false;
    }

    inline auto
    beam_search_loop(uint32_t max_iterations) {
        return get_context<search_ctx>()
            >> then([max_iterations](search_ctx &ctx, auto&&...) {
                return pump::coro::make_view_able(make_search_loop_coro(ctx, max_iterations));
            })
            >> for_each()
            >> beam_search_iteration()
            >> reduce()
            >> ignore_args();
    }

    // Step 4: extract top-K from exact results (sorted by exact L2 distance)
    inline auto
    extract_results() {
        return get_context<search_ctx>()
            >> then([](search_ctx& ctx) {
                // Sort exact results by distance and take top-K
                auto& er = ctx.exact_results;
                std::partial_sort(er.begin(),
                    er.begin() + std::min(static_cast<size_t>(ctx.cfg.k), er.size()),
                    er.end(),
                    [](const auto& a, const auto& b) { return a.distance < b.distance; });

                search_result result;
                uint32_t n = std::min(ctx.cfg.k, static_cast<uint32_t>(er.size()));
                result.results.assign(er.begin(), er.begin() + n);
                result.io_ns = ctx.io_ns;
                result.cpu_ns = ctx.cpu_ns;
                result.io_count = ctx.state.io_count;
                result.iter_count = ctx.iter_count;
                return result;
            });
    }

    // ── Full search pipeline ──
    //
    // Input: const float* query
    // Output: search_result
    //
    // num_probes=1: one beam search with all entry points
    // num_probes=N: N independent beam searches from different entry points, merge top-K
    // Both paths use the same for_each >> flat_map >> reduce structure.

    // Schedulers resolved from globals (g_nvme, g_cache, g_ssd, g_preemptive)
    inline auto
    search(const index::disk_index& idx, const search_config& cfg) {
        uint32_t np = std::max(1u, cfg.num_probes);
        uint32_t k = cfg.k;
        return then([p_idx = &idx, cfg](const float* query) {
                auto ctx = search_ctx(*p_idx, cfg, query);
                ctx.base_task = g_task;
                ctx.dtable.precompute(p_idx->codebook, query);
                return ctx;
            })
            >> push_result_to_context()
            >> for_each(std::views::iota(0u, np))
            >> get_context<search_ctx>()
            >> then([np](search_ctx& parent, uint32_t probe_id) {
                auto probe_ctx = search_ctx(*parent.idx, parent.cfg, parent.query);
                probe_ctx.dtable = parent.dtable;
                probe_ctx.base_task = parent.base_task;
                init_candidates_for_probe(probe_ctx, probe_id, np);
                return probe_ctx;
            })
            >> push_result_to_context()
            >> beam_search_loop(cfg.max_iterations > 0
                    ? cfg.max_iterations
                    : std::max(50u, cfg.search_list / std::max(1u, cfg.beam_width) * 2))
            >> extract_results()
            >> pop_context()
            >> reduce(search_result{}, [](search_result& merged, auto&& r) {
                if constexpr (std::is_same_v<std::decay_t<decltype(r)>, search_result>) {
                    for (auto& item : r.results) merged.results.push_back(item);
                    merged.io_ns += r.io_ns;
                    merged.cpu_ns += r.cpu_ns;
                    merged.io_count += r.io_count;
                    merged.iter_count += r.iter_count;
                }
            })
            >> then([k](search_result&& merged) {
                auto& r = merged.results;
                if (r.size() > k) {
                    std::partial_sort(r.begin(), r.begin() + k, r.end(),
                        [](const auto& a, const auto& b) { return a.distance < b.distance; });
                    r.resize(k);
                }
                return merged;
            })
            >> pop_context();
    }

}

#endif //AISAQ_SEARCH_BEAM_SEARCH_HH
