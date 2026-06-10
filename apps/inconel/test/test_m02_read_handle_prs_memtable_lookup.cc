#include "apps/inconel/test/check.hh"

#include <concepts>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "apps/inconel/core/checkpoint_guard.hh"
#include "apps/inconel/core/memtable.hh"
#include "apps/inconel/core/memtable_lookup.hh"
#include "apps/inconel/core/read_catalog.hh"
#include "apps/inconel/core/tree_geometry.hh"
#include "apps/inconel/core/tree_manifest.hh"
#include "apps/inconel/format/types.hh"

using namespace apps::inconel;

namespace {

template <typename T>
concept has_hot_member = requires(T t) {
    t.hot;
};

static_assert(!has_hot_member<core::value_handle>);
static_assert(std::same_as<core::memtable_lookup_result,
                           std::variant<core::memtable_value_hit,
                                        core::memtable_tombstone,
                                        core::memtable_miss>>);
static_assert(std::same_as<decltype(std::declval<core::memtable_scan_item>().vh),
                           core::value_handle>);

format::value_ref
make_value_ref(uint64_t lba, uint32_t len = 128) {
    return format::value_ref{
        .base        = format::paddr{.device_id = 3, .lba = lba},
        .byte_offset = static_cast<uint16_t>(lba % 4096),
        .len         = len,
        .flags       = static_cast<uint16_t>(lba % 13),
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

std::shared_ptr<core::memtable_gen>
make_gen(uint64_t gen_id,
         core::memtable_gen::state st = core::memtable_gen::state::sealed) {
    auto gen = std::make_shared<core::memtable_gen>();
    gen->gen_id = gen_id;
    gen->st = st;
    gen->front_owner_index = 0;
    return gen;
}

std::shared_ptr<const std::vector<core::front_read_set>>
make_fronts(std::shared_ptr<core::memtable_gen> active = {},
            std::vector<std::shared_ptr<core::memtable_gen>> imms = {}) {
    auto fronts = std::make_shared<std::vector<core::front_read_set>>();
    fronts->push_back(core::front_read_set{
        .active = std::move(active),
        .imms   = std::move(imms),
    });
    return fronts;
}

std::shared_ptr<core::checkpoint_guard>
make_guard() {
    static const core::tree_geometry kGeom{
        .lba_size               = 4096,
        .tree_page_size         = 4096,
        .shadow_slots_per_range = 1,
    };

    auto guard = std::make_shared<core::checkpoint_guard>();
    guard->manifest =
        std::make_shared<const core::tree_manifest>(core::tree_manifest::empty(&kGeom));
    return guard;
}

std::shared_ptr<const core::published_read_set>
make_prs(std::shared_ptr<core::checkpoint_guard> guard,
         std::shared_ptr<const std::vector<core::front_read_set>> fronts,
         uint64_t epoch) {
    return std::make_shared<core::published_read_set>(
        core::published_read_set{
            .tree_guard = std::move(guard),
            .fronts     = std::move(fronts),
            .epoch      = epoch,
        });
}

std::shared_ptr<const core::publish_catalog>
make_cat(std::shared_ptr<const core::published_read_set> prs,
         uint64_t durable_lsn,
         uint64_t epoch) {
    return std::make_shared<core::publish_catalog>(
        std::move(prs), durable_lsn, epoch);
}

const core::memtable_value_hit&
expect_value(const core::memtable_lookup_result& result) {
    CHECK(std::holds_alternative<core::memtable_value_hit>(result));
    return std::get<core::memtable_value_hit>(result);
}

void
lookup_memtable_prefers_highest_visible_version_across_active_and_imms() {
    auto active = make_gen(100, core::memtable_gen::state::active);
    auto imm = make_gen(90);

    core::insert_value(*active, "key", 10, make_value_ref(1000));
    core::insert_value(*imm, "key", 12, make_value_ref(2000));

    const core::front_read_set frs{
        .active = active,
        .imms   = {imm},
    };

    auto before = core::lookup_memtable("key", 11, frs);
    CHECK(same_value_ref(expect_value(before).durable, make_value_ref(1000)));

    auto after = core::lookup_memtable("key", 12, frs);
    CHECK(same_value_ref(expect_value(after).durable, make_value_ref(2000)));
}

void
lookup_memtable_respects_read_lsn() {
    auto active = make_gen(101, core::memtable_gen::state::active);
    core::insert_value(*active, "key", 30, make_value_ref(3000));
    core::insert_value(*active, "key", 10, make_value_ref(1000));

    const core::front_read_set frs{.active = active};

    auto miss = core::lookup_memtable("key", 9, frs);
    CHECK(std::holds_alternative<core::memtable_miss>(miss));

    auto old = core::lookup_memtable("key", 20, frs);
    CHECK(same_value_ref(expect_value(old).durable, make_value_ref(1000)));

    auto latest = core::lookup_memtable("key", 30, frs);
    CHECK(same_value_ref(expect_value(latest).durable, make_value_ref(3000)));
}

void
lookup_memtable_tombstone_masks_older_value() {
    auto active = make_gen(102, core::memtable_gen::state::active);
    auto imm = make_gen(92);

    core::insert_tombstone(*active, "key", 20);
    core::insert_value(*imm, "key", 10, make_value_ref(1000));

    const core::front_read_set frs{
        .active = active,
        .imms   = {imm},
    };

    auto before_tombstone = core::lookup_memtable("key", 15, frs);
    CHECK(same_value_ref(expect_value(before_tombstone).durable,
                         make_value_ref(1000)));

    auto tombstone = core::lookup_memtable("key", 20, frs);
    CHECK(std::holds_alternative<core::memtable_tombstone>(tombstone));
}

void
empty_front_read_set_returns_miss_and_empty_scan() {
    const core::front_read_set frs{};

    auto point = core::lookup_memtable("key", 100, frs);
    CHECK(std::holds_alternative<core::memtable_miss>(point));

    auto rows = core::scan_memtable("a", "z", 100, frs);
    CHECK(rows.empty());

    auto empty_range = core::scan_memtable("z", "a", 100, frs);
    CHECK(empty_range.empty());
}

void
scan_memtable_merges_winners_sorted_by_key() {
    auto active = make_gen(200, core::memtable_gen::state::active);
    auto imm_new = make_gen(190);
    auto imm_old = make_gen(180);

    core::insert_value(*active, "b", 10, make_value_ref(1010));
    core::insert_tombstone(*active, "c", 11);

    core::insert_value(*imm_new, "a", 9, make_value_ref(2009));
    core::insert_value(*imm_new, "b", 8, make_value_ref(2008));

    core::insert_value(*imm_old, "c", 7, make_value_ref(3007));
    core::insert_value(*imm_old, "d", 6, make_value_ref(3006));

    const core::front_read_set frs{
        .active = active,
        .imms   = {imm_new, imm_old},
    };

    auto rows = core::scan_memtable("a", "z", 100, frs);
    CHECK(rows.size() == 4);

    CHECK(rows[0].key == "a");
    CHECK(rows[0].data_ver == 9);
    CHECK(rows[0].kind == core::memtable_entry::kind::value);
    CHECK(same_value_ref(rows[0].vh.durable, make_value_ref(2009)));

    CHECK(rows[1].key == "b");
    CHECK(rows[1].data_ver == 10);
    CHECK(rows[1].kind == core::memtable_entry::kind::value);
    CHECK(same_value_ref(rows[1].vh.durable, make_value_ref(1010)));

    CHECK(rows[2].key == "c");
    CHECK(rows[2].data_ver == 11);
    CHECK(rows[2].kind == core::memtable_entry::kind::tombstone);

    CHECK(rows[3].key == "d");
    CHECK(rows[3].data_ver == 6);
    CHECK(rows[3].kind == core::memtable_entry::kind::value);
    CHECK(same_value_ref(rows[3].vh.durable, make_value_ref(3006)));
}

void
scan_memtable_respects_range_bounds_and_keeps_tombstones() {
    auto active = make_gen(300, core::memtable_gen::state::active);
    auto imm = make_gen(290);

    core::insert_value(*active, "a", 10, make_value_ref(4010));
    core::insert_tombstone(*active, "c", 12);
    core::insert_value(*imm, "b", 11, make_value_ref(5011));
    core::insert_value(*imm, "d", 9, make_value_ref(5009));

    const core::front_read_set frs{
        .active = active,
        .imms   = {imm},
    };

    auto rows = core::scan_memtable("b", "d", 100, frs);
    CHECK(rows.size() == 2);

    CHECK(rows[0].key == "b");
    CHECK(rows[0].data_ver == 11);
    CHECK(rows[0].kind == core::memtable_entry::kind::value);
    CHECK(same_value_ref(rows[0].vh.durable, make_value_ref(5011)));

    CHECK(rows[1].key == "c");
    CHECK(rows[1].data_ver == 12);
    CHECK(rows[1].kind == core::memtable_entry::kind::tombstone);
}

void
scan_memtable_respects_read_lsn() {
    auto active = make_gen(302, core::memtable_gen::state::active);
    auto imm = make_gen(292);

    core::insert_value(*active, "x", 30, make_value_ref(3030));
    core::insert_value(*imm, "x", 10, make_value_ref(3010));

    const core::front_read_set frs{
        .active = active,
        .imms   = {imm},
    };

    auto low = core::scan_memtable("x", "y", 9, frs);
    CHECK(low.empty());

    auto old_visible = core::scan_memtable("x", "y", 20, frs);
    CHECK(old_visible.size() == 1);
    CHECK(old_visible[0].key == "x");
    CHECK(old_visible[0].data_ver == 10);
    CHECK(same_value_ref(old_visible[0].vh.durable, make_value_ref(3010)));

    auto latest_visible = core::scan_memtable("x", "y", 30, frs);
    CHECK(latest_visible.size() == 1);
    CHECK(latest_visible[0].data_ver == 30);
    CHECK(same_value_ref(latest_visible[0].vh.durable, make_value_ref(3030)));
}

void
scan_memtable_does_not_copy_value_body() {
    auto active = make_gen(301, core::memtable_gen::state::active);
    const auto vr = make_value_ref(6100, 777);
    core::insert_value(*active, "bodyless", 44, vr);

    const core::front_read_set frs{.active = active};
    auto rows = core::scan_memtable("body", "bodym", 44, frs);

    CHECK(rows.size() == 1);
    CHECK(rows[0].key == "bodyless");
    CHECK(rows[0].kind == core::memtable_entry::kind::value);
    CHECK(same_value_ref(rows[0].vh.durable, vr));
}

void
acquire_read_handle_snapshots_cat_and_read_lsn() {
    auto prs0 = make_prs(make_guard(), make_fronts(make_gen(1)), 7);
    auto cat0 = make_cat(prs0, 41, 7);

    core::catalog_store store(cat0);
    core::read_handle rh = store.acquire_read_handle();

    CHECK(rh.cat == cat0);
    CHECK(rh.read_lsn == 41);

    cat0->durable_lsn.store(55, std::memory_order_release);
    CHECK(rh.read_lsn == 41);

    auto newer = store.acquire_read_handle();
    CHECK(newer.cat == cat0);
    CHECK(newer.read_lsn == 55);
}

void
install_cat_switches_only_new_readers() {
    auto cat0 = make_cat(make_prs(make_guard(), make_fronts(make_gen(1)), 10),
                         50,
                         10);
    auto cat1 = make_cat(make_prs(make_guard(), make_fronts(make_gen(2)), 11),
                         80,
                         11);

    core::catalog_store store(cat0);
    core::read_handle old_rh = store.acquire_read_handle();
    store.install_cat(cat1);
    core::read_handle new_rh = store.acquire_read_handle();

    CHECK(old_rh.cat == cat0);
    CHECK(old_rh.read_lsn == 50);
    CHECK(new_rh.cat == cat1);
    CHECK(new_rh.read_lsn == 80);
    CHECK(old_rh.cat != new_rh.cat);
}

void
old_handle_pins_old_prs_guard_and_front_gens() {
    auto old_active = make_gen(400, core::memtable_gen::state::active);
    auto old_imm = make_gen(390);
    auto old_guard = make_guard();
    auto old_fronts = make_fronts(old_active, {old_imm});
    auto old_prs = make_prs(old_guard, old_fronts, 20);
    auto old_cat = make_cat(old_prs, 61, 20);

    auto new_cat = make_cat(make_prs(make_guard(), make_fronts(make_gen(401)), 21),
                            62,
                            21);

    std::weak_ptr<const core::publish_catalog> weak_cat = old_cat;
    std::weak_ptr<const core::published_read_set> weak_prs = old_prs;
    std::weak_ptr<core::checkpoint_guard> weak_guard = old_guard;
    std::weak_ptr<const core::tree_manifest> weak_manifest = old_guard->manifest;
    std::weak_ptr<const std::vector<core::front_read_set>> weak_fronts = old_fronts;
    std::weak_ptr<core::memtable_gen> weak_active = old_active;
    std::weak_ptr<core::memtable_gen> weak_imm = old_imm;

    core::catalog_store store(old_cat);
    core::read_handle old_rh = store.acquire_read_handle();
    store.install_cat(new_cat);

    old_cat.reset();
    old_prs.reset();
    old_fronts.reset();
    old_guard.reset();
    old_active.reset();
    old_imm.reset();

    CHECK(!weak_cat.expired());
    CHECK(!weak_prs.expired());
    CHECK(!weak_guard.expired());
    CHECK(!weak_manifest.expired());
    CHECK(!weak_fronts.expired());
    CHECK(!weak_active.expired());
    CHECK(!weak_imm.expired());

    old_rh = {};

    CHECK(weak_cat.expired());
    CHECK(weak_prs.expired());
    CHECK(weak_guard.expired());
    CHECK(weak_manifest.expired());
    CHECK(weak_fronts.expired());
    CHECK(weak_active.expired());
    CHECK(weak_imm.expired());
}

void
catalog_store_rejects_null_cat() {
    bool ctor_rejected = false;
    try {
        core::catalog_store store(nullptr);
        (void)store;
    } catch (const std::invalid_argument&) {
        ctor_rejected = true;
    }
    CHECK(ctor_rejected);

    auto cat = make_cat(make_prs(make_guard(), make_fronts(make_gen(1)), 1),
                        1,
                        1);
    core::catalog_store store(cat);

    bool install_rejected = false;
    try {
        store.install_cat(nullptr);
    } catch (const std::invalid_argument&) {
        install_rejected = true;
    }
    CHECK(install_rejected);
    CHECK(store.current_cat() == cat);
}

void
publish_catalog_rejects_guard_without_manifest() {
    auto empty_guard = std::make_shared<core::checkpoint_guard>();
    auto prs = make_prs(empty_guard, make_fronts(make_gen(1)), 30);

    bool rejected = false;
    try {
        auto cat = make_cat(prs, 1, 30);
        (void)cat;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

void
publish_catalog_rejects_empty_fronts() {
    auto empty_fronts = std::make_shared<const std::vector<core::front_read_set>>();
    auto prs = make_prs(make_guard(), empty_fronts, 31);

    bool rejected = false;
    try {
        auto cat = make_cat(prs, 1, 31);
        (void)cat;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

}  // namespace

int
main() {
    lookup_memtable_prefers_highest_visible_version_across_active_and_imms();
    lookup_memtable_respects_read_lsn();
    lookup_memtable_tombstone_masks_older_value();
    empty_front_read_set_returns_miss_and_empty_scan();
    scan_memtable_merges_winners_sorted_by_key();
    scan_memtable_respects_range_bounds_and_keeps_tombstones();
    scan_memtable_respects_read_lsn();
    scan_memtable_does_not_copy_value_body();
    acquire_read_handle_snapshots_cat_and_read_lsn();
    install_cat_switches_only_new_readers();
    old_handle_pins_old_prs_guard_and_front_gens();
    catalog_store_rejects_null_cat();
    publish_catalog_rejects_guard_without_manifest();
    publish_catalog_rejects_empty_fronts();
    return 0;
}
