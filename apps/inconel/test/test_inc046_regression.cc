#include "apps/inconel/test/check.hh"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "pump/core/context.hh"
#include "pump/sender/just.hh"
#include "pump/sender/submit.hh"
#include "pump/sender/then.hh"

#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/registry.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/format/format_options.hh"
#include "apps/inconel/format/format_profile.hh"
#include "apps/inconel/format/formatted_storage.hh"
#include "apps/inconel/format/tree_page.hh"
#include "apps/inconel/mock_nvme/device.hh"
#include "apps/inconel/runtime/builder.hh"
#include "apps/inconel/runtime/facade.hh"
#include "apps/inconel/runtime/run.hh"
#include "apps/inconel/tree/lookup.hh"
#include "apps/inconel/tree/owner_scheduler.hh"
#include "apps/inconel/tree/page_reader.hh"
#include "apps/inconel/tree/sender.hh"
#include "apps/inconel/value/sender.hh"

using namespace apps::inconel;

using TreeCache  = core::clock_cache;
using ValueCache = core::clock_cache;

namespace {

const std::vector<uint32_t> kCores           = {0, 2, 4, 6, 8};
const std::vector<uint32_t> kReadDomainCores = {2, 4, 6};
constexpr int32_t           kValueCore       = 0;
constexpr int32_t           kOwnerCore       = 8;

// Follow the profile's configured Value Area top; sized at profile
// edit time, not here. build_runtime's validate_build_inputs gate
// requires device >= profile.value_data_area_end.lba * lba_size.
constexpr uint64_t kNamespaceBytes =
    static_cast<uint64_t>(format::kBootstrapFormatProfile.value_data_area_end.lba) *
    format::kBootstrapFormatProfile.lba_size;

enum class op_kind : uint8_t { put, tombstone };

struct round_op {
    op_kind      kind;
    std::string  key;
    std::string  value;
};

struct round_outcome {
    tree::tree_flush_result                       result;
    std::shared_ptr<const core::tree_manifest>    manifest;
    std::shared_ptr<const core::checkpoint_guard> next_base_guard;
    uint64_t                                      next_lsn = 0;
};

format::format_options
derive_format_options()
{
    const auto& p = format::kBootstrapFormatProfile;
    format::format_options opts{};
    opts.lba_size               = p.lba_size;
    opts.tree_page_size         = p.tree_page_size;
    opts.shadow_slots_per_range = p.shadow_slots_per_range;
    opts.value_class_count      = p.value_class_count;
    for (uint8_t i = 0; i < p.value_class_count; ++i) {
        opts.value_class_sizes[i] = p.value_class_sizes[i];
    }
    opts.wal_segment_size  = 1u << 20;
    opts.wal_segment_count = 8;
    return opts;
}

template <typename T, typename SenderBuilder>
T
submit_and_wait(SenderBuilder&& build_sender)
{
    auto ctx     = pump::core::make_root_context();
    auto promise = std::make_shared<std::promise<T>>();
    auto fut     = promise->get_future();

    std::forward<SenderBuilder>(build_sender)()
        >> pump::sender::then([promise](auto&& r) {
            promise->set_value(std::forward<decltype(r)>(r));
        })
        >> pump::sender::submit(ctx);

    return fut.get();
}

std::vector<format::value_ref>
persist_put_values(std::span<const round_op> ops)
{
    std::size_t put_count = 0;
    for (const auto& op : ops) {
        if (op.kind == op_kind::put) ++put_count;
    }

    std::vector<format::value_ref> durables(put_count);
    if (put_count == 0) return durables;

    std::vector<value::put_entry> entries;
    entries.reserve(put_count);
    std::size_t out_idx = 0;
    for (const auto& op : ops) {
        if (op.kind != op_kind::put) continue;
        entries.push_back(value::put_entry{
            .body   = op.value,
            .out_vr = &durables[out_idx++],
        });
    }

    const bool ok = submit_and_wait<bool>([&]() {
        return value::persist_values(std::span<value::put_entry>(entries));
    });
    CHECK(ok);
    return durables;
}

std::shared_ptr<core::memtable_gen>
build_sealed_gen(uint64_t                           gen_id,
                 uint64_t                           lsn_start,
                 std::span<const round_op>          ops,
                 std::span<const format::value_ref> durables)
{
    auto gen = std::make_shared<core::memtable_gen>();
    gen->gen_id            = gen_id;
    gen->front_owner_index = 0;
    gen->st                = core::memtable_gen::state::sealed;
    gen->min_lsn           = lsn_start;
    gen->max_lsn           = lsn_start + ops.size() - 1;

    std::size_t put_idx = 0;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const auto& op = ops[i];

        auto key_view = gen->kv_arena.allocate(op.key.data(), op.key.size());
        core::memtable_entry entry{};
        entry.data_ver = lsn_start + static_cast<uint64_t>(i);

        if (op.kind == op_kind::put) {
            auto val_view =
                gen->kv_arena.allocate(op.value.data(), op.value.size());
            entry.k  = core::memtable_entry::kind::value;
            entry.vh = core::value_handle{
                .durable = durables[put_idx++],
                .hot     = core::value_view{
                    .data = val_view.data(),
                    .len  = static_cast<uint32_t>(val_view.size()),
                },
            };
        } else {
            entry.k  = core::memtable_entry::kind::tombstone;
            entry.vh = core::value_handle{};
        }

        auto [it, inserted] = gen->table.try_emplace(key_view);
        CHECK(inserted);
        it->second.push_back(entry);
    }

    return gen;
}

std::shared_ptr<const core::checkpoint_guard>
make_empty_base_guard()
{
    auto manifest = std::make_shared<const core::tree_manifest>(
        core::tree_manifest::empty(&runtime::kBootstrapTreeGeometry));
    core::checkpoint_guard guard{};
    guard.manifest = std::move(manifest);
    return std::make_shared<const core::checkpoint_guard>(std::move(guard));
}

std::shared_ptr<const core::checkpoint_guard>
wrap_manifest_as_guard(std::shared_ptr<const core::tree_manifest> manifest)
{
    core::checkpoint_guard guard{};
    guard.manifest = std::move(manifest);
    return std::make_shared<const core::checkpoint_guard>(std::move(guard));
}

round_outcome
run_round(uint64_t                                   gen_id,
          uint64_t                                   lsn_start,
          uint64_t                                   recovery_safe_lsn,
          std::vector<round_op>                      ops,
          std::shared_ptr<const core::checkpoint_guard> base_guard)
{
    CHECK(!ops.empty());

    auto durables = persist_put_values(ops);
    auto gen = build_sealed_gen(gen_id, lsn_start, ops, durables);
    const uint64_t max_lsn = gen->max_lsn;

    tree::tree_flush_request req{
        .base_guard        = std::move(base_guard),
        .sealed_gens       = {},
        .recovery_safe_lsn = recovery_safe_lsn,
    };
    req.sealed_gens.push_back(std::move(gen));

    auto req_holder =
        std::make_shared<tree::tree_flush_request>(std::move(req));
    auto result = submit_and_wait<tree::tree_flush_result>([req_holder]() {
        return tree::tree_local_flush(std::move(*req_holder));
    });

    CHECK(result.st == tree::flush_stage_status::ok);
    CHECK(result.new_manifest != nullptr);
    CHECK(result.new_manifest->has_root());
    CHECK(result.new_manifest->leaf_order.size() == 1);

    auto manifest = result.new_manifest;
    return round_outcome{
        .result          = std::move(result),
        .manifest        = manifest,
        .next_base_guard = wrap_manifest_as_guard(std::move(manifest)),
        .next_lsn        = max_lsn + 1,
    };
}

tree::lookup_result
lookup_one(std::string_view key, const core::tree_manifest* manifest)
{
    std::vector<std::string_view> keys = {key};
    auto results =
        submit_and_wait<std::vector<tree::lookup_result>>([&]() {
            return pump::sender::just() >> tree::lookup(keys, manifest);
        });
    CHECK(results.size() == 1);
    return std::move(results[0]);
}

void
expect_value(std::string_view               key,
             const core::tree_manifest*     manifest,
             uint64_t                       expected_ver,
             std::string_view               expected_value)
{
    auto result = lookup_one(key, manifest);
    CHECK(std::holds_alternative<tree::lookup_value>(result));
    const auto& lv = std::get<tree::lookup_value>(result);
    CHECK(lv.data_ver == expected_ver);
    auto body = submit_and_wait<std::string>([vr = lv.vr]() {
        return value::read_value(vr);
    });
    CHECK(body == expected_value);
}

void
expect_absent(std::string_view key, const core::tree_manifest* manifest)
{
    auto result = lookup_one(key, manifest);
    CHECK(std::holds_alternative<tree::lookup_absent>(result));
}

void
expect_root_leaf_record_count(const mock_nvme::mock_device& device,
                              const core::tree_manifest*    manifest,
                              uint16_t                      expected_records)
{
    const void* raw = device.test_read_raw(manifest->root_slot.lba);
    CHECK(raw != nullptr);
    CHECK(format::inspect_tree_page(
              raw, runtime::kBootstrapTreeGeometry.tree_page_size)
          == format::tree_page_status::ok);
    auto* hdr = static_cast<const format::tree_slot_header*>(raw);
    CHECK(hdr->type == format::node_type::leaf);

    tree::leaf_page_reader reader;
    CHECK(reader.parse(raw, runtime::kBootstrapTreeGeometry.tree_page_size));
    CHECK(reader.record_count() == expected_records);
}

void
test_internal_child_base_uses_range_base()
{
    auto left = std::make_unique<tree::mem_tree_node>();
    left->type           = format::node_type::leaf;
    left->new_range_base = format::paddr{0, 5000};
    left->new_paddr      = format::paddr{0, 5001};

    auto right = std::make_unique<tree::mem_tree_node>();
    right->type           = format::node_type::leaf;
    right->new_range_base = format::paddr{0, 6000};
    right->new_paddr      = format::paddr{0, 6001};

    tree::mem_tree_node parent;
    parent.type = format::node_type::internal;
    parent.children.push_back(tree::child_ref{
        .target = std::move(left),
    });
    parent.children.push_back(tree::child_ref{
        .target = std::move(right),
    });
    parent.separators.push_back("m");
    parent.content.resize(runtime::kBootstrapTreeGeometry.tree_page_size);

    tree::_owner::reformat_internal_node(
        &parent, runtime::kBootstrapTreeGeometry.tree_page_size);

    tree::internal_page_reader reader;
    CHECK(reader.parse(
        parent.content.data(), runtime::kBootstrapTreeGeometry.tree_page_size));
    CHECK(reader.record_count() == 1);
    CHECK(reader.get(0).separator_key == "m");
    const auto expected_left  = format::paddr{0, 5000};
    const auto expected_right = format::paddr{0, 6000};
    CHECK(reader.get(0).child_base == expected_left);
    CHECK(reader.rightmost_child() == expected_right);
}

}  // namespace

int
main()
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::printf("inc046 regression: same-leaf multi-round + compact-to-empty "
                "(shadow_slots_per_range=%u)\n",
                runtime::kBootstrapTreeGeometry.shadow_slots_per_range);

    test_internal_child_base_uses_range_base();
    std::printf("  internal child_base contract OK\n");

    auto fmt_opts = derive_format_options();
    auto storage  = format::make_formatted_storage(fmt_opts, kNamespaceBytes);
    mock_nvme::mock_device dev(
        std::move(storage), kNamespaceBytes, fmt_opts.lba_size);
    std::mutex dev_mtx;
    dev.enable_thread_safety(&dev_mtx);

    runtime::build_options bopts{
        .cores             = kCores,
        .device            = &dev,
        .read_domain_cores = kReadDomainCores,
        .value_core        = kValueCore,
        .owner_core        = kOwnerCore,
    };
    auto* rt = runtime::build_runtime<TreeCache, ValueCache>(bopts);
    CHECK(core::registry::current_shard_partitions() != nullptr);
    CHECK(core::registry::current_shard_partitions()->shard_count() == 1);

    std::atomic<uint32_t>     cores_started{0};
    std::vector<std::jthread> workers;
    workers.reserve(kCores.size());
    for (uint32_t core : kCores) {
        workers.emplace_back([rt, core, &cores_started]() {
            rt::run(rt, core, [&cores_started](auto*, uint32_t) {
                cores_started.fetch_add(1, std::memory_order_release);
            });
        });
    }
    while (cores_started.load(std::memory_order_acquire)
           < static_cast<uint32_t>(kCores.size()))
    {
        std::this_thread::yield();
    }

    pump::core::this_core_id = 0;

    const std::vector<std::string> keys = {"k0", "k1", "k2"};

    auto round1 = run_round(
        /*gen_id=*/1,
        /*lsn_start=*/1,
        /*recovery_safe_lsn=*/0,
        std::vector<round_op>{
            {.kind = op_kind::put, .key = keys[0], .value = "v1_k0"},
            {.kind = op_kind::put, .key = keys[1], .value = "v1_k1"},
            {.kind = op_kind::put, .key = keys[2], .value = "v1_k2"},
        },
        make_empty_base_guard());

    const auto root_rb1   = round1.manifest->root_range_base;
    const auto root_slot1 = round1.manifest->root_slot;
    expect_root_leaf_record_count(dev, round1.manifest.get(), 3);
    expect_value(keys[0], round1.manifest.get(), 1, "v1_k0");
    expect_value(keys[1], round1.manifest.get(), 2, "v1_k1");
    expect_value(keys[2], round1.manifest.get(), 3, "v1_k2");
    std::printf("  round 1 OK: bootstrap leaf root at lba=%lu\n",
                static_cast<unsigned long>(root_slot1.lba));

    auto round2 = run_round(
        /*gen_id=*/2,
        /*lsn_start=*/round1.next_lsn,
        /*recovery_safe_lsn=*/0,
        std::vector<round_op>{
            {.kind = op_kind::put, .key = keys[0], .value = "v2_k0"},
            {.kind = op_kind::put, .key = keys[1], .value = "v2_k1"},
            {.kind = op_kind::put, .key = keys[2], .value = "v2_k2"},
        },
        round1.next_base_guard);

    CHECK(round2.manifest->root_slot != root_slot1);
    if (runtime::kBootstrapTreeGeometry.shadow_slots_per_range > 1) {
        CHECK(round2.manifest->root_range_base == root_rb1);
    }
    expect_root_leaf_record_count(dev, round2.manifest.get(), 3);
    expect_value(keys[0], round2.manifest.get(), 4, "v2_k0");
    expect_value(keys[1], round2.manifest.get(), 5, "v2_k1");
    expect_value(keys[2], round2.manifest.get(), 6, "v2_k2");
    std::printf("  round 2 OK: cross-round rewrite %lu -> %lu\n",
                static_cast<unsigned long>(root_slot1.lba),
                static_cast<unsigned long>(round2.manifest->root_slot.lba));

    const uint64_t round3_safe_lsn = round2.next_lsn + keys.size() - 1;
    auto round3 = run_round(
        /*gen_id=*/3,
        /*lsn_start=*/round2.next_lsn,
        /*recovery_safe_lsn=*/round3_safe_lsn,
        std::vector<round_op>{
            {.kind = op_kind::tombstone, .key = keys[0], .value = {}},
            {.kind = op_kind::tombstone, .key = keys[1], .value = {}},
            {.kind = op_kind::tombstone, .key = keys[2], .value = {}},
        },
        round2.next_base_guard);

    CHECK(round3.manifest->root_slot != round2.manifest->root_slot);
    CHECK(round3.manifest->leaf_order.size() == 1);
    expect_root_leaf_record_count(dev, round3.manifest.get(), 0);
    expect_absent(keys[0], round3.manifest.get());
    expect_absent(keys[1], round3.manifest.get());
    expect_absent(keys[2], round3.manifest.get());
    expect_absent("k_missing", round3.manifest.get());
    std::printf("  round 3 OK: compact-to-empty kept empty root leaf at lba=%lu\n",
                static_cast<unsigned long>(round3.manifest->root_slot.lba));

    for (uint32_t core : kCores) {
        rt->is_running_by_core[core].store(false);
    }
    workers.clear();
    runtime::destroy_runtime<TreeCache, ValueCache>(rt);

    std::printf("all passed\n");
    return 0;
}
