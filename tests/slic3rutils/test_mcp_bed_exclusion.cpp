#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPBedExclusion.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <string>
#include <vector>

// The bed exclusion regions get_scene_info reports and render_plate_view draws: which of upstream's regions the
// plate shows (PartPlate::calc_triangles' dedupe), in plate millimetres, with the extruders each applies to. The
// regions come from upstream's own resolver, run on a config built here.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;
using json = nlohmann::json;

namespace {

const Vec2d plate_origin(300., 0.);

DynamicPrintConfig exclusion_config(BedExcludeVolumeMode mode)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_key_value("printable_height", new ConfigOptionFloat(250.));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.4});
    config.set_key_value("extruder_offset", new ConfigOptionPoints{Vec2d(0., 0.), Vec2d(25., 0.)});
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints{Vec2d(0., 0.), Vec2d(20., 0.), Vec2d(20., 20.), Vec2d(0., 20.)});
    config.set_key_value("bed_exclude_volumes",
                         new ConfigOptionString("30x30,40x30,40x40,30x40|5..15;100x100,120x100,120x120,100x120"));
    config.set_key_value("bed_exclude_volume_mode", new ConfigOptionEnum<BedExcludeVolumeMode>(mode));
    return config;
}

std::vector<ExclusionRegion> resolved(const DynamicPrintConfig& config)
{
    return resolve_exclusion_regions(get_bed_excluded_regions_by_extruder(config), active_bed_exclude_volume_mode(config), plate_origin);
}

const ExclusionRegion* region_at(const std::vector<ExclusionRegion>& regions, double min_x, std::optional<int> extruder = std::nullopt)
{
    for (const ExclusionRegion& region : regions)
        if (std::abs(region.footprint.min.x() - (plate_origin.x() + min_x)) < 1e-6 &&
            (!extruder || region.extruders == std::vector<int>{*extruder}))
            return &region;
    return nullptr;
}

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

} // namespace

TEST_CASE("In shared mode the plate shows the legacy area and each volume once, offset by the plate's origin", "[McpBedExclusion][orcamcp]")
{
    const std::vector<ExclusionRegion> regions = resolved(exclusion_config(BedExcludeVolumeMode::Shared));
    REQUIRE(regions.size() == 3);

    const ExclusionRegion* legacy = region_at(regions, 0.);
    REQUIRE(legacy != nullptr);
    CHECK(legacy->legacy);
    CHECK(legacy->source == "bed_exclude_area");
    CHECK_FALSE(legacy->height_limited);
    CHECK_THAT(legacy->z_min_mm, WithinAbs(0., 1e-9));
    CHECK_THAT(legacy->z_max_mm, WithinAbs(250., 1e-9));

    const ExclusionRegion* full = region_at(regions, 30.);
    REQUIRE(full != nullptr);
    CHECK_FALSE(full->legacy);
    CHECK(full->source == "bed_exclude_volumes");
    CHECK_FALSE(full->height_limited);
    CHECK_THAT(full->z_max_mm, WithinAbs(250., 1e-9));
    CHECK_THAT(full->footprint.max.x(), WithinAbs(340., 1e-6));

    const ExclusionRegion* limited = region_at(regions, 100.);
    REQUIRE(limited != nullptr);
    CHECK(limited->height_limited);
    CHECK_THAT(limited->z_min_mm, WithinAbs(5., 1e-9));
    CHECK_THAT(limited->z_max_mm, WithinAbs(15., 1e-9));

    for (const ExclusionRegion& region : regions) {
        CHECK(region.extruders == std::vector<int>{0, 1});
        CHECK(region.all_extruders);
    }
}

TEST_CASE("In toolhead-offset mode each extruder has its own copy of a volume, shifted by its offset", "[McpBedExclusion][orcamcp]")
{
    const std::vector<ExclusionRegion> regions = resolved(exclusion_config(BedExcludeVolumeMode::ToolheadOffset));
    CHECK(std::count_if(regions.begin(), regions.end(), [](const ExclusionRegion& r) { return r.legacy; }) == 1);
    REQUIRE(regions.size() == 5);

    const ExclusionRegion* first = region_at(regions, 30., 0);
    REQUIRE(first != nullptr);
    CHECK_FALSE(first->all_extruders);
    const ExclusionRegion* second = nullptr;
    for (const ExclusionRegion& region : regions)
        if (region.extruders == std::vector<int>{1} && std::abs(region.footprint.size().y() - 10.) < 1e-6 && region.footprint.min.y() > 25. && region.footprint.min.y() < 35.)
            second = &region;
    REQUIRE(second != nullptr);
    CHECK_THAT(std::abs(second->footprint.min.x() - first->footprint.min.x()), WithinAbs(25., 1e-6));
}

TEST_CASE("In per-extruder mode an extruder's own volume is listed for it alone", "[McpBedExclusion][orcamcp]")
{
    DynamicPrintConfig config = exclusion_config(BedExcludeVolumeMode::PerExtruder);
    config.set_key_value("bed_exclude_volumes", new ConfigOptionString());
    config.set_key_value("extruder_bed_exclude_volumes", new ConfigOptionStrings{"", "10..20;50x50,60x50,60x60,50x60"});
    const std::vector<ExclusionRegion> regions = resolved(config);
    REQUIRE(regions.size() == 2);
    const ExclusionRegion* own = region_at(regions, 50., 1);
    REQUIRE(own != nullptr);
    CHECK(own->source == "extruder_bed_exclude_volumes");
    CHECK(own->height_limited);
    CHECK_FALSE(own->all_extruders);
    CHECK(region_at(regions, 0.) != nullptr);

    // The same definition on both extruders is one region that applies to both.
    config.set_key_value("extruder_bed_exclude_volumes",
                         new ConfigOptionStrings{"10..20;50x50,60x50,60x60,50x60", "10..20;50x50,60x50,60x60,50x60"});
    const std::vector<ExclusionRegion> merged = resolved(config);
    REQUIRE(merged.size() == 2);
    const ExclusionRegion* both = region_at(merged, 50.);
    REQUIRE(both != nullptr);
    CHECK(both->extruders == std::vector<int>{0, 1});
    CHECK(both->all_extruders);
}

TEST_CASE("The default bed has no exclusion regions", "[McpBedExclusion][orcamcp]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    CHECK(resolved(config).empty());
    config.set_key_value("bed_exclude_area", new ConfigOptionPoints{Vec2d(0., 0.)});
    CHECK(resolved(config).empty());
}

TEST_CASE("A collision volume is reported with its Z range, and exactly when its outline is a rectangle", "[McpBedExclusion][orcamcp]")
{
    const std::vector<ExclusionRegion> regions = resolved(exclusion_config(BedExcludeVolumeMode::Shared));
    const ExclusionRegion*             limited = region_at(regions, 100.);
    REQUIRE(limited != nullptr);

    const json occupancy = exclusion_occupancy_json(*limited);
    CHECK(occupancy["kind"] == "exclusion_volume");
    CHECK(occupancy["footprint_is_exact"] == true);
    CHECK(occupancy["includes_brim"] == false);
    CHECK(occupancy["z_min_mm"] == 5.);
    CHECK(occupancy["z_max_mm"] == 15.);
    CHECK(occupancy["extruders"] == json::array({0, 1}));
    CHECK(occupancy["footprint"]["min_x"] == 400.);

    const json described = exclusion_region_json(*limited);
    CHECK(described["kind"] == "collision_volume");
    CHECK(described["legacy"] == false);
    CHECK(described["height_limited"] == true);
    CHECK(described["outline"].size() == 4);
    CHECK(described["all_extruders"] == true);
    CHECK(exclusion_region_json(*region_at(regions, 0.))["kind"] == "material_keepout");

    ExclusionRegion triangle = *limited;
    triangle.outline         = {Vec2d(0., 0.), Vec2d(10., 0.), Vec2d(0., 10.)};
    CHECK(exclusion_occupancy_json(triangle)["footprint_is_exact"] == false);
    CHECK_FALSE(outline_is_rectangle(triangle.outline));
    CHECK(outline_is_rectangle(limited->outline));
}

TEST_CASE("A layer plan draws a collision volume active only where its Z range meets the layer", "[McpBedExclusion][orcamcp]")
{
    const std::vector<ExclusionRegion> regions = resolved(exclusion_config(BedExcludeVolumeMode::Shared));
    const std::vector<BoundingBoxf3>   legacy_boxes{BoundingBoxf3(Vec3d(300., 0., 0.), Vec3d(320., 20., 250.))};

    const auto active_at = [](const std::vector<ExcludedRegionOverlay>& overlays, double min_x) {
        for (const ExcludedRegionOverlay& overlay : overlays)
            if (std::abs(overlay.outline.front().x() - min_x) < 1e-6)
                return overlay.active;
        FAIL("no overlay at " << min_x);
        return false;
    };

    const auto in_slab = exclusion_overlays(legacy_boxes, regions, std::make_pair(0., 0.2));
    REQUIRE(in_slab.size() == 3); // the legacy box from legacy_boxes (not twice), and the two volumes
    CHECK(active_at(in_slab, 300.));
    CHECK(active_at(in_slab, 330.));
    CHECK_FALSE(active_at(in_slab, 400.));
    CHECK_THAT(in_slab.back().z_max, WithinAbs(15., 1e-9));
    CHECK(in_slab.back().height_limited);

    const auto at_height = exclusion_overlays(legacy_boxes, regions, std::make_pair(9., 9.2));
    CHECK(active_at(at_height, 400.));

    const auto in_3d = exclusion_overlays(legacy_boxes, regions, std::nullopt);
    CHECK(active_at(in_3d, 300.));
    CHECK(active_at(in_3d, 330.));
    CHECK(active_at(in_3d, 400.));
}

TEST_CASE("The text of an exclusion region sends no one to a button", "[McpBedExclusion][orcamcp]")
{
    const std::vector<ExclusionRegion> regions = resolved(exclusion_config(BedExcludeVolumeMode::Shared));
    for (const ExclusionRegion& region : regions) {
        const std::string text = exclusion_region_json(region).dump() + exclusion_occupancy_json(region).dump();
        for (const char* gui : {"click", "Click", "the user", "GUI", "button"})
            CHECK_FALSE(mentions(text, gui));
    }
}
