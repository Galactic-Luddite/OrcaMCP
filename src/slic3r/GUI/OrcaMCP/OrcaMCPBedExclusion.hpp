// src/slic3r/GUI/OrcaMCP/OrcaMCPBedExclusion.hpp
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"

// A printer's bed exclusion regions as the 3D view draws them -- the legacy keep-out area
// (bed_exclude_area) and the toolhead collision volumes (bed_exclude_volumes, or per extruder
// extruder_bed_exclude_volumes) -- for get_scene_info and render_plate_view, apart from the app, so they are
// tested without it (tests/slic3rutils/test_mcp_bed_exclusion.cpp). The regions come from upstream's
// get_bed_excluded_regions_by_extruder; what is decided here is which of them the plate draws (PartPlate::
// calc_triangles' dedupe), in plate mm, with the extruders each applies to.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// One region as the 3D view draws it, in plate mm (the plate's origin added).
struct ExclusionRegion
{
    std::vector<Vec2d> outline;                  // counter-clockwise
    BoundingBoxf       footprint;
    double             z_min_mm        = 0.;
    double             z_max_mm        = 0.;
    bool               height_limited  = false;  // BedExcludeRegion::has_z_range
    bool               legacy          = false;  // bed_exclude_area, a material keep-out
    std::string        source;                   // "bed_exclude_area" | "bed_exclude_volumes" | "extruder_bed_exclude_volumes"
    std::vector<int>   extruders;                // 0-based physical extruders it applies to
    bool               all_extruders   = true;
};

// The regions the plate draws, from upstream's regions of every physical extruder (`by_extruder`): a shared
// definition once, the legacy area once, and in toolhead-offset or per-extruder mode every extruder's own
// collision volumes. Regions identical in outline and Z across extruders merge, with every extruder they apply to.
std::vector<ExclusionRegion> resolve_exclusion_regions(const std::vector<std::vector<BedExcludeRegion>>& by_extruder,
                                                       BedExcludeVolumeMode mode, const Vec2d& plate_origin);

// An axis-aligned rectangle of four corners: its footprint is the whole region.
bool outline_is_rectangle(const std::vector<Vec2d>& outline);

// min_x, min_y, max_x, max_y, size_x, size_y: the one shape of a rectangle on the bed.
nlohmann::json rect_json(const BoundingBoxf& box);

// get_scene_info's `exclusion_regions` entry: kind ("collision_volume" or "material_keepout"), legacy, source,
// outline, footprint, z_min_mm, z_max_mm, height_limited, extruders, all_extruders.
nlohmann::json exclusion_region_json(const ExclusionRegion& region);

// get_scene_info's `occupancy` entry of a collision volume: its footprint's bounding rectangle, and the Z range an
// object may pass under or over.
nlohmann::json exclusion_occupancy_json(const ExclusionRegion& region);

// A region as render_plate_view draws it. `active` false: outside the slab a layer plan shows, drawn dashed.
struct ExcludedRegionOverlay
{
    std::vector<Vec2d> outline;
    double             z_min          = 0.;
    double             z_max          = 0.;
    bool               height_limited = false;
    bool               active         = true;
};

// What render_plate_view draws: the legacy areas (`legacy_boxes`, PartPlate::get_exclude_areas) always active, and
// every collision volume of `regions`; inside a `slab` (the Z range a layer plan shows) only those whose Z range meets
// it are active.
std::vector<ExcludedRegionOverlay> exclusion_overlays(const std::vector<BoundingBoxf3>& legacy_boxes,
                                                      const std::vector<ExclusionRegion>& regions,
                                                      std::optional<std::pair<double, double>> slab);

}}} // namespace Slic3r::GUI::OrcaMCP
