#include "apps/inconel/test/check.hh"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/core/batch_carrier.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/format/types.hh"
#include "apps/inconel/tree/memtable_fold.hh"

using namespace apps::inconel;

namespace {

template <typename T>
concept has_hot_member = requires(T t) {
    t.hot;
};

static_assert(!has_hot_member<core::value_handle>);
static_assert(std::same_as<
              typename decltype(std::declval<core::front_fragment>()
                                    .entry_indices)::value_type,
              uint32_t>);
static_assert(std::same_as<decltype(std::declval<core::memtable_value_hit>()
                                        .durable),
                           format::value_ref>);

format::value_ref
make_value_ref(uint64_t lba, uint32_t len = 128) {
    return format::value_ref{
        .base        = format::paddr{.device_id = 7, .lba = lba},
        .byte_offset = static_cast<uint16_t>(lba % 4096),
        .len         = len,
        .flags       = static_cast<uint16_t>(lba % 17),
    };
}

bool
same_value_ref(const format::value_ref& a,
               const format::value_ref& b) {
    return a.base.device_id == b.base.device_id &&
           a.base.lba == b.base.lba &&
           a.byte_offset == b.byte_offset &&
           a.len == b.len &&
           a.flags == b.flags;
}

std::span<const core::raw_batch_op>
op_span(const std::vector<core::raw_batch_op>& ops) {
    return {ops.data(), ops.size()};
}

bool
view_points_into(const core::batch_ctx& ctx, std::string_view view) {
    if (view.empty()) return true;
    const auto base = reinterpret_cast<std::uintptr_t>(ctx.input.bytes.data());
    const auto end  = base + ctx.input.bytes.size();
    const auto ptr  = reinterpret_cast<std::uintptr_t>(view.data());
    return ptr >= base && ptr + view.size() <= end;
}

void
append_bytes(std::vector<std::byte>& out,
             const void* data,
             std::size_t len) {
    if (len == 0) return;
    const auto* first = static_cast<const std::byte*>(data);
    out.insert(out.end(), first, first + len);
}

void
append_u8(std::vector<std::byte>& out, uint8_t value) {
    append_bytes(out, &value, sizeof(value));
}

void
append_u32(std::vector<std::byte>& out, uint32_t value) {
    append_bytes(out, &value, sizeof(value));
}

core::client_batch_buffer
make_client_bytes(std::span<const core::raw_batch_op> ops) {
    return core::encode_client_batch(ops);
}

void
expect_parse_rejects(const std::vector<std::byte>& bytes) {
    bool rejected = false;
    try {
        const core::client_batch_view view{
            std::span<const std::byte>{bytes.data(), bytes.size()}};
        (void)view;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void
canonicalization_keeps_last_op_per_key_in_original_order() {
    std::vector<core::raw_batch_op> ops{
        {.op = core::write_op_type::put, .key = "same", .value = "first"},
        {.op = core::write_op_type::put, .key = "route-b", .value = "old-b"},
        {.op = core::write_op_type::del, .key = "same", .value = ""},
        {.op = core::write_op_type::put, .key = "route-c", .value = "c"},
        {.op = core::write_op_type::put, .key = "route-b", .value = "new-b"},
    };

    auto ctx = core::build_batch_ctx(op_span(ops), 900, 4);

    CHECK(ctx.batch_lsn == 900);
    CHECK(ctx.entry_count == 3);
    CHECK(ctx.canonical_entries.size() == 3);
    CHECK(ctx.canonical_entries[0].op == core::write_op_type::del);
    CHECK(ctx.canonical_entries[0].key == "same");
    CHECK(ctx.canonical_entries[0].value.empty());
    CHECK(ctx.canonical_entries[1].op == core::write_op_type::put);
    CHECK(ctx.canonical_entries[1].key == "route-c");
    CHECK(ctx.canonical_entries[1].value == "c");
    CHECK(ctx.canonical_entries[2].op == core::write_op_type::put);
    CHECK(ctx.canonical_entries[2].key == "route-b");
    CHECK(ctx.canonical_entries[2].value == "new-b");

    CHECK(ctx.put_entry_indices.size() == 2);
    CHECK(ctx.put_entry_indices[0] == 1);
    CHECK(ctx.put_entry_indices[1] == 2);
}

void
routing_fragments_are_owner_sorted_and_index_based() {
    std::vector<core::raw_batch_op> ops{
        {.op = core::write_op_type::put, .key = "k0", .value = "v0"},
        {.op = core::write_op_type::put, .key = "k1", .value = "v1"},
        {.op = core::write_op_type::del, .key = "k2", .value = ""},
        {.op = core::write_op_type::put, .key = "k3", .value = "v3"},
    };

    auto ctx = core::build_batch_ctx(op_span(ops), 901, 3);
    CHECK(ctx.entry_count == ctx.canonical_entries.size());
    CHECK(!ctx.fragments.empty());

    uint32_t prev_owner = 0;
    bool first = true;
    std::vector<bool> seen(ctx.canonical_entries.size(), false);
    for (const auto& fragment : ctx.fragments) {
        CHECK(fragment.batch_lsn == 901);
        CHECK(fragment.entry_count == ctx.entry_count);
        if (!first) CHECK(prev_owner < fragment.owner);
        first = false;
        prev_owner = fragment.owner;

        uint32_t prev_index = 0;
        bool first_index = true;
        for (uint32_t idx : fragment.entry_indices) {
            CHECK(idx < ctx.canonical_entries.size());
            if (!first_index) CHECK(prev_index < idx);
            first_index = false;
            prev_index = idx;
            seen[idx] = true;

            const auto& entry = ctx.canonical_entries[idx];
            const auto expected_owner =
                static_cast<uint32_t>(core::key_hash(entry.key) % 3);
            CHECK(fragment.owner == expected_owner);
        }
    }

    for (bool present : seen) CHECK(present);
}

void
batch_views_are_owned_by_ctx_and_survive_move() {
    std::vector<core::raw_batch_op> ops{
        {.op = core::write_op_type::put, .key = "owned-a", .value = "value-a"},
        {.op = core::write_op_type::put, .key = "owned-b", .value = "value-b"},
        {.op = core::write_op_type::del, .key = "owned-a", .value = ""},
    };

    auto encoded = make_client_bytes(op_span(ops));
    const core::client_batch_view view = encoded.view();
    CHECK(view.op_count() == 3);
    CHECK(view.ops()[0].key == "owned-a");
    CHECK(view.ops()[0].value == "value-a");

    auto ctx = core::build_batch_ctx(std::move(encoded), 902, 2);
    ops[0].key = "mutated";
    ops[0].value = "mutated";
    ops.clear();

    CHECK(ctx.canonical_entries.size() == 2);
    for (const auto& entry : ctx.canonical_entries) {
        CHECK(view_points_into(ctx, entry.key));
        CHECK(view_points_into(ctx, entry.value));
    }

    auto moved = std::move(ctx);
    CHECK(moved.canonical_entries.size() == 2);
    CHECK(moved.canonical_entries[0].key == "owned-b");
    CHECK(moved.canonical_entries[0].value == "value-b");
    CHECK(moved.canonical_entries[1].key == "owned-a");
    CHECK(moved.canonical_entries[1].value.empty());

    for (const auto& fragment : moved.fragments) {
        for (uint32_t idx : fragment.entry_indices) {
            CHECK(idx < moved.canonical_entries.size());
            CHECK(view_points_into(moved, moved.canonical_entries[idx].key));
        }
    }
}

void
client_batch_parser_rejects_malformed_inputs() {
    expect_parse_rejects({});

    {
        std::vector<std::byte> bad;
        append_u32(bad, UINT32_MAX);
        expect_parse_rejects(bad);
    }

    {
        std::vector<std::byte> bad;
        append_u32(bad, 1);
        append_u8(bad, 0x7f);
        append_u32(bad, 1);
        append_u32(bad, 0);
        append_bytes(bad, "k", 1);
        expect_parse_rejects(bad);
    }

    {
        std::vector<std::byte> bad;
        append_u32(bad, 1);
        append_u8(bad, static_cast<uint8_t>(core::write_op_type::put));
        append_u32(bad, 4);
        append_u32(bad, 0);
        append_bytes(bad, "ab", 2);
        expect_parse_rejects(bad);
    }

    {
        std::vector<std::byte> bad;
        append_u32(bad, 1);
        append_u8(bad, static_cast<uint8_t>(core::write_op_type::del));
        append_u32(bad, 1);
        append_u32(bad, 1);
        append_bytes(bad, "k", 1);
        append_bytes(bad, "v", 1);
        expect_parse_rejects(bad);
    }

    {
        std::vector<core::raw_batch_op> ops{
            {.op = core::write_op_type::put, .key = "k", .value = "v"},
        };
        bool rejected = false;
        try {
            (void)core::build_batch_ctx(op_span(ops), 903, 0);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    {
        std::vector<core::raw_batch_op> ops{
            {.op = core::write_op_type::del, .key = "k", .value = "v"},
        };
        bool rejected = false;
        try {
            (void)core::build_batch_ctx(op_span(ops), 904, 1);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
    }

    {
        std::vector<core::raw_batch_op> empty;
        auto ctx = core::build_batch_ctx(op_span(empty), 905, 1);
        CHECK(ctx.entry_count == 0);
        CHECK(ctx.canonical_entries.empty());
        CHECK(ctx.fragments.empty());
        CHECK(ctx.put_entry_indices.empty());
    }
}

void
memtable_lookup_is_value_ref_only_and_max_visible_data_ver() {
    core::memtable_gen gen{
        .gen_id = 10,
        .st     = core::memtable_gen::state::active,
    };

    core::insert_value(gen, "lookup-key", 30, make_value_ref(30));
    core::insert_value(gen, "lookup-key", 10, make_value_ref(10));
    core::insert_tombstone(gen, "lookup-key", 20);

    auto miss = core::lookup_visible(gen, "lookup-key", 9);
    CHECK(std::holds_alternative<core::memtable_miss>(miss));

    auto tombstone = core::lookup_visible(gen, "lookup-key", 25);
    CHECK(std::holds_alternative<core::memtable_tombstone>(tombstone));

    auto hit = core::lookup_visible(gen, "lookup-key", 35);
    CHECK(std::holds_alternative<core::memtable_value_hit>(hit));
    CHECK(same_value_ref(
        std::get<core::memtable_value_hit>(hit).durable,
        make_value_ref(30)));

    const auto* visible = core::find_visible_entry(gen, "lookup-key", 35);
    CHECK(visible != nullptr);
    CHECK(visible->data_ver == 30);
}

void
memtable_arena_owns_keys_and_reuses_duplicate_key_storage() {
    core::memtable_gen gen{
        .gen_id = 11,
        .st     = core::memtable_gen::state::active,
    };

    std::string key = "arena-key";
    const char* caller_key_data = key.data();
    core::insert_value(gen, key, 100, make_value_ref(100));

    auto it = gen.table.find("arena-key");
    CHECK(it != gen.table.end());
    const char* arena_key_data = it->first.data();
    CHECK(arena_key_data != caller_key_data);
    CHECK(it->second.size() == 1);

    key[0] = 'z';
    CHECK(gen.table.find("arena-key") != gen.table.end());
    CHECK(gen.table.find("zrena-key") == gen.table.end());

    core::insert_tombstone(gen, "arena-key", 101);
    auto it2 = gen.table.find("arena-key");
    CHECK(it2 != gen.table.end());
    CHECK(gen.table.size() == 1);
    CHECK(it2->first.data() == arena_key_data);
    CHECK(it2->second.size() == 2);
}

void
fold_selects_max_data_ver_without_back_assumption() {
    auto gen0 = std::make_shared<core::memtable_gen>();
    gen0->gen_id = 20;
    gen0->st = core::memtable_gen::state::sealed;
    core::insert_value(*gen0, "fold-key", 30, make_value_ref(30));
    core::insert_value(*gen0, "fold-key", 10, make_value_ref(10));

    auto gen1 = std::make_shared<core::memtable_gen>();
    gen1->gen_id = 21;
    gen1->st = core::memtable_gen::state::sealed;
    core::insert_value(*gen1, "fold-key", 25, make_value_ref(25));

    tree::flush_round_state rs{};
    rs.pinned_gens.push_back(gen0);
    rs.pinned_gens.push_back(gen1);

    tree::fold_pinned_gens(rs);

    CHECK(rs.workset.size() == 1);
    const auto& group = rs.workset[0];
    CHECK(group.key == "fold-key");
    CHECK(group.winner_data_ver == 30);
    CHECK(group.winner_kind == core::memtable_entry::kind::value);
    CHECK(group.winner_pinned_gen_index == 0);
    CHECK(same_value_ref(group.winner_value.durable, make_value_ref(30)));

    std::vector<core::retired_value_ref> gen0_losers;
    gen0->loser_durable_refs.drain([&](core::retired_value_ref loser) {
        gen0_losers.push_back(loser);
    });
    CHECK(gen0_losers.size() == 1);
    CHECK(gen0_losers[0].data_ver == 10);
    CHECK(same_value_ref(gen0_losers[0].vr, make_value_ref(10)));

    std::vector<core::retired_value_ref> gen1_losers;
    gen1->loser_durable_refs.drain([&](core::retired_value_ref loser) {
        gen1_losers.push_back(loser);
    });
    CHECK(gen1_losers.size() == 1);
    CHECK(gen1_losers[0].data_ver == 25);
    CHECK(same_value_ref(gen1_losers[0].vr, make_value_ref(25)));
}

}  // namespace

int
main() {
    canonicalization_keeps_last_op_per_key_in_original_order();
    routing_fragments_are_owner_sorted_and_index_based();
    batch_views_are_owned_by_ctx_and_survive_move();
    client_batch_parser_rejects_malformed_inputs();
    memtable_lookup_is_value_ref_only_and_max_visible_data_ver();
    memtable_arena_owns_keys_and_reuses_duplicate_key_storage();
    fold_selects_max_data_ver_without_back_assumption();
    return 0;
}
