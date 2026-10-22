// src/slic3r/GUI/OrcaMCP/OrcaMCPBedExclusion.cpp
#include "OrcaMCPBedExclusion.hpp"

#include "libslic3r/ExclusionVolumeGeometry.hpp"

#include <algorithm>
#include <set>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

double signed_area(const std::vector<Vec2d>& outline)
{
    double twice = 0.;
    for (std::size_t i = 0; i < outline.size(); ++i) {
        const Vec2d& a = outline[i];
        const Vec2d& b = outline[(i + 1) % outline.size()];
        twice += a.x() * b.y() - b.x() * a.y();
    }
    return twice / 2.;
}

std::vector<Vec2d> counter_clockwise(std::vector<Vec2d> outline)
{
    if (signed_area(outline) < 0.)
        std::reverse(outline.begin(), outline.end());
    return outline;
}

BoundingBoxf footprint_of_outline(const std::vector<Vec2d>& outline)
{
    BoundingBoxf box;
    for (const Vec2d& point : outline)
        box.merge(point);
    return box;
}

ExclusionRegion region_in_plate_mm(const BedExcludeRegion& region, const Vec2d& plate_origin, BedExcludeVolumeMode mode)
{
    ExclusionRegion out;
    std::vector<Vec2d> outline;
    for (const Point& point : region.polygon.points)
        outline.push_back(Vec2d(unscale<double>(point.x()), unscale<double>(point.y())) + plate_origin);
    out.outline        = counter_clockwise(std::move(outline));
    out.footprint      = footprint_of_outline(out.outline);
    out.z_min_mm       = region.z_min;
    out.z_max_mm       = region.z_max;
    out.height_limited = region.has_z_range;
    out.legacy         = !region.is_collision_volume();
    out.source         = out.legacy ? "bed_exclude_area" :
                         mode == BedExcludeVolumeMode::PerExtruder ? "extruder_bed_exclude_volumes" : "bed_exclude_volumes";
    return out;
}

bool same_region(const ExclusionRegion& a, const ExclusionRegion& b)
{
    return a.outline == b.outline && a.z_min_mm == b.z_min_mm && a.z_max_mm == b.z_max_mm && a.height_limited == b.height_limited &&
           a.legacy == b.legacy && a.source == b.source;
}

std::vector<int> every_extruder(std::size_t count)
{
    std::vector<int> all;
    for (std::size_t i = 0; i < count; ++i)
        all.push_back(int(i));
    return all;
}

} // namespace

std::vector<ExclusionRegion> resolve_exclusion_regions(const std::vector<std::vector<BedExcludeRegion>>& by_extruder,
                                                       BedExcludeVolumeMode mode, const Vec2d& plate_origin)
{
    // PartPlate::calc_triangles' choice: a shared definition is the same for every extruder, so group 0 only; the
    // legacy area is the same in every group, so group 0's; every group's collision volumes otherwise.
    std::vector<ExclusionRegion> regions;
    for (std::size_t extruder = 0; extruder < by_extruder.size(); ++extruder) {
        if (mode == BedExcludeVolumeMode::Shared && extruder > 0)
            break;
        for (const BedExcludeRegion& source : by_extruder[extruder]) {
            if (!source.is_collision_volume() && extruder > 0)
                continue;
            ExclusionRegion region = region_in_plate_mm(source, plate_origin, mode);
            const bool      every  = region.legacy || mode == BedExcludeVolumeMode::Shared;
            region.extruders       = every ? every_extruder(by_extruder.size()) : std::vector<int>{int(extruder)};
            const auto same = std::find_if(regions.begin(), regions.end(), [&region](const ExclusionRegion& r) { return same_region(r, region); });
            if (same != regions.end())
                same->extruders.insert(same->extruders.end(), region.extruders.begin(), region.extruders.end());
            else
                regions.push_back(std::move(region));
        }
    }
    for (ExclusionRegion& region : regions) {
        std::sort(region.extruders.begin(), region.extruders.end());
        region.extruders.erase(std::unique(region.extruders.begin(), region.extruders.end()), region.extruders.end());
        region.all_extruders = region.extruders.size() == by_extruder.size();
    }
    return regions;
}

bool outline_is_rectangle(const std::vector<Vec2d>& outline)
{
    if (outline.size() != 4)
        return false;
    const BoundingBoxf box = footprint_of_outline(outline);
    std::set<std::pair<bool, bool>> corners;
    for (const Vec2d& point : outline) {
        const bool on_x = point.x() == box.min.x() || point.x() == box.max.x();
        const bool on_y = point.y() == box.min.y() || point.y() == box.max.y();
        if (!on_x || !on_y)
            return false;
        corners.insert({point.x() == box.max.x(), point.y() == box.max.y()});
    }
    return corners.size() == 4;
}

nlohmann::json rect_json(const BoundingBoxf& box)
{
    return {{"min_x", box.min.x()}, {"min_y", box.min.y()},
            {"max_x", box.max.x()}, {"max_y", box.max.y()},
            {"size_x", box.max.x() - box.min.x()}, {"size_y", box.max.y() - box.min.y()}};
}

nlohmann::json exclusion_region_json(const ExclusionRegion& region)
{
    nlohmann::json outline = nlohmann::json::array();
    for (const Vec2d& point : region.outline)
        outline.push_back({point.x(), point.y()});
    return {{"kind", region.legacy ? "material_keepout" : "collision_volume"},
            {"legacy", region.legacy},
            {"source", region.source},
            {"outline", std::move(outline)},
            {"footprint", rect_json(region.footprint)},
            {"z_min_mm", region.z_min_mm},
            {"z_max_mm", region.z_max_mm},
            {"height_limited", region.height_limited},
            {"extruders", region.extruders},
            {"all_extruders", region.all_extruders}};
}

nlohmann::json exclusion_occupancy_json(const ExclusionRegion& region)
{
    return {{"kind", "exclusion_volume"},
            {"name", "Collision volume"},
            {"footprint", rect_json(region.footprint)},
            {"includes_brim", false},
            {"footprint_is_exact", outline_is_rectangle(region.outline)},
            {"z_min_mm", region.z_min_mm},
            {"z_max_mm", region.z_max_mm},
            {"extruders", region.extruders}};
}

std::vector<ExcludedRegionOverlay> exclusion_overlays(const std::vector<BoundingBoxf3>& legacy_boxes,
                                                      const std::vector<ExclusionRegion>& regions,
                                                      std::optional<std::pair<double, double>> slab)
{
    std::vector<ExcludedRegionOverlay> overlays;
    for (const BoundingBoxf3& box : legacy_boxes) {
        ExcludedRegionOverlay overlay;
        overlay.outline = {Vec2d(box.min.x(), box.min.y()), Vec2d(box.max.x(), box.min.y()), Vec2d(box.max.x(), box.max.y()),
                           Vec2d(box.min.x(), box.max.y())};
        overlays.push_back(std::move(overlay));
    }
    for (const ExclusionRegion& region : regions) {
        if (region.legacy)
            continue; // drawn from legacy_boxes
        ExcludedRegionOverlay overlay;
        overlay.outline        = region.outline;
        overlay.z_min          = region.z_min_mm;
        overlay.z_max          = region.z_max_mm;
        overlay.height_limited = region.height_limited;
        overlay.active         = !slab || bed_exclusion_z_ranges_overlap(region.z_min_mm, region.z_max_mm, slab->first, slab->second);
        overlays.push_back(std::move(overlay));
    }
    return overlays;
}

}}} // namespace Slic3r::GUI::OrcaMCP
