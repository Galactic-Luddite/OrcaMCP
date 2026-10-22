#include <catch2/catch_all.hpp>
#include <algorithm>
#include <utility>
#include <vector>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

// convex_hull_2d does not clip geometry below the bed, so these cases avoid
// sinking transforms.
TEST_CASE("A part's 2D convex hull is its footprint projected onto the bed", "[Model]")
{
    Model model;
    ModelObject* object = model.add_object();
    // Keep the cube's raw coordinates ([0,20] on every axis): the default
    // add_volume re-centers the geometry, which would move the footprint.
    object->add_volume(make_cube(20, 20, 20), ModelVolumeType::MODEL_PART, false);

    SECTION("identity transform yields the 20 mm square") {
        const Polygon hull   = object->convex_hull_2d(Geometry::Transformation{}.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(0.));
        CHECK(bb.min.y() == scaled(0.));
        CHECK(bb.max.x() == scaled(20.));
        CHECK(bb.max.y() == scaled(20.));
    }

    SECTION("scaling and offset move and grow the footprint") {
        Geometry::Transformation t;
        t.set_scaling_factor({2, 2, 2}); // cube now spans [0,40]
        t.set_offset({10, 5, 0});        // then shift +10 in X, +5 in Y

        const Polygon hull   = object->convex_hull_2d(t.get_matrix());
        const BoundingBox bb = hull.bounding_box();
        CHECK(hull.size() == 4);
        CHECK(bb.min.x() == scaled(10.));
        CHECK(bb.min.y() == scaled(5.));
        CHECK(bb.max.x() == scaled(50.));
        CHECK(bb.max.y() == scaled(45.));
    }
}

// Every layer range must carry a layer_height: the slicer reads it unchecked
// (layer_height_profile_from_ranges), and so do the GUI's object list and Print::apply's range
// comparison. The object list gives a new range one; a file can carry a range without it, and so
// did MCP's set_object_layer_range, and the next slice crashed. A range is completed from the ACTIVE
// print settings the caller passes (the app's presets, the CLI's config), never from a guess.
namespace {
// Active settings: a layer height, and one nozzle per entry with its layer-height limits.
DynamicPrintConfig active_config(double layer_height, std::vector<double> nozzles = {0.4},
                                 std::vector<double> min_heights = {0.08}, std::vector<double> max_heights = {0.0})
{
    DynamicPrintConfig config;
    config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzles));
    config.set_key_value("min_layer_height", new ConfigOptionFloats(min_heights));
    config.set_key_value("max_layer_height", new ConfigOptionFloats(max_heights));
    return config;
}
} // namespace

TEST_CASE("A new layer range starts at the object's layer height, else the active one, on the object's extruder", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* object = model.add_object();
    const DynamicPrintConfig from_active = layer_range_defaults(*object, active_config(0.2));
    CHECK_THAT(from_active.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-9));
    CHECK(from_active.opt_int("extruder") == 0);

    object->config.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    CHECK_THAT(layer_range_defaults(*object, active_config(0.2)).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.16, 1e-9));
}

TEST_CASE("A new layer range's height is one the object's nozzle can print", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* object = model.add_object();
    // A 0.4 mm nozzle with no maximum prints up to 0.3 mm, and from its minimum up.
    CHECK_THAT(layer_range_defaults(*object, active_config(0.5)).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
    CHECK_THAT(layer_range_defaults(*object, active_config(0.05)).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.08, 1e-9));

    // On a toolchanger, the nozzle of the object's own extruder.
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    const DynamicPrintConfig toolchanger = active_config(0.5, {0.4, 0.8}, {0.08, 0.2}, {0.0, 0.0});
    CHECK_THAT(layer_range_defaults(*object, toolchanger).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.5, 1e-9));
}

// A range can name its own extruder, and it prints at that extruder's nozzle: its default height is
// clamped to that nozzle, not the object's. On a toolchanger with a 0.4 mm T1 and a 0.8 mm T2, a
// range moved to T1 of an object printing at 0.5 mm on T2 kept 0.5 mm on the 0.4 mm nozzle.
TEST_CASE("A layer range is completed within the nozzle of the extruder that prints it", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* object = model.add_object();
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    object->config.set_key_value("layer_height", new ConfigOptionFloat(0.5));
    const DynamicPrintConfig toolchanger = active_config(0.2, {0.4, 0.8}, {0.08, 0.2}, {0.0, 0.0});

    ModelConfig on_t1;
    on_t1.set_key_value("extruder", new ConfigOptionInt(1));
    complete_layer_range(on_t1, *object, toolchanger);
    CHECK_THAT(on_t1.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
    CHECK(on_t1.opt_int("extruder") == 1);

    // Extruder 0, or none, is the object's.
    ModelConfig on_object;
    on_object.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    complete_layer_range(on_object, *object, toolchanger);
    CHECK_THAT(on_object.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.5, 1e-9));
    CHECK(on_object.opt_int("extruder") == 0);

    // A model's ranges each by their own extruder.
    object->layer_config_ranges[{1.0, 2.0}].assign_config(on_t1.get());
    ModelConfig bare_t1;
    bare_t1.set_key_value("extruder", new ConfigOptionInt(1));
    object->layer_config_ranges[{3.0, 4.0}].assign_config(bare_t1);
    complete_layer_ranges(model, toolchanger);
    CHECK_THAT(object->layer_config_ranges.at({3.0, 4.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.3, 1e-9));
}

TEST_CASE("Completing a layer range adds what it lacks and keeps what it has", "[Model][LayerRanges]")
{
    ModelConfig range;
    range.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    range.set_key_value("extruder", new ConfigOptionInt(2));
    DynamicPrintConfig defaults;
    defaults.set_key_value("layer_height", new ConfigOptionFloat(0.24));
    defaults.set_key_value("extruder", new ConfigOptionInt(0));
    complete_layer_range(range, defaults);
    CHECK_THAT(range.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.24, 1e-9));
    CHECK(range.opt_int("extruder") == 2);
    CHECK(range.has("sparse_infill_density"));
}

TEST_CASE("A model's layer ranges are completed from each object's layer height and the active one", "[Model][LayerRanges]")
{
    Model        model;
    ModelObject* own    = model.add_object();
    ModelObject* global = model.add_object();
    own->config.set_key_value("layer_height", new ConfigOptionFloat(0.12));
    ModelConfig bare;
    bare.set_key_value("sparse_infill_density", new ConfigOptionPercent(30));
    own->layer_config_ranges[{2.0, 5.0}].assign_config(bare);
    global->layer_config_ranges[{1.0, 3.0}].assign_config(bare);
    ModelConfig set;
    set.set_key_value("layer_height", new ConfigOptionFloat(0.08));
    global->layer_config_ranges[{4.0, 6.0}].assign_config(set);

    complete_layer_ranges(model, active_config(0.28));
    CHECK_THAT(own->layer_config_ranges.at({2.0, 5.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.12, 1e-9));
    CHECK_THAT(global->layer_config_ranges.at({1.0, 3.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.28, 1e-9));
    CHECK_THAT(global->layer_config_ranges.at({4.0, 6.0}).opt_float("layer_height"), Catch::Matchers::WithinAbs(0.08, 1e-9));
    CHECK(global->layer_config_ranges.at({1.0, 3.0}).opt_int("extruder") == 0);
}

TEST_CASE("An object's raw mesh keeps the triangles of each part on its own vertices", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    TriangleMesh second = make_cube(10, 10, 10);
    second.translate(30, 0, 0);
    object->add_volume(std::move(second), ModelVolumeType::MODEL_PART, false);

    // Two separate cubes stay two closed components, one around each cube.
    const std::vector<indexed_triangle_set> parts = its_split(object->raw_indexed_triangle_set());
    REQUIRE(parts.size() == 2);
    std::vector<double> min_x;
    for (const indexed_triangle_set &part : parts) {
        CHECK(part.indices.size() == 12);
        min_x.push_back(bounding_box(part).min.x());
    }
    std::sort(min_x.begin(), min_x.end());
    CHECK_THAT(min_x.front(), Catch::Matchers::WithinAbs(0., 1e-4));
    CHECK_THAT(min_x.back(), Catch::Matchers::WithinAbs(30., 1e-4));
}

TEST_CASE("An instance added from another's scale, rotation and mirror matches it", "[Model]")
{
    // Add instance and Fill bed copy an instance this way.
    const int case_idx = GENERATE(0, 1);
    Transform3d trafo = Transform3d::Identity();
    if (case_idx == 0)
        trafo.linear() = Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitZ()).toRotationMatrix() * Vec3d(-1., 1., 1.).asDiagonal();
    else
        trafo.linear() << 4.4408921e-16,   0.819152044,    0.573576436,
                          1.0,            -4.4408921e-16, -1.11022302e-16,
                         -5.55111512e-17, -0.573576436,    0.819152044;
    CAPTURE(case_idx);

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(make_cube(10, 20, 30));
    ModelInstance *original = object->add_instance();
    original->set_transformation(Geometry::Transformation(trafo));
    const ModelInstance *copy = object->add_instance(original->get_offset(), original->get_scaling_factor(),
                                                     original->get_rotation(), original->get_mirror());
    CHECK(copy->get_matrix().linear().isApprox(original->get_matrix().linear(), 1e-9));
}

namespace {

// A flat triangle first, then a cube for each offset, all in one mesh.
TriangleMesh flat_triangle_then_cubes(const std::vector<double> &cube_offsets)
{
    indexed_triangle_set its;
    its.vertices = {{-20.f, 0.f, 0.f}, {-10.f, 0.f, 0.f}, {-15.f, 5.f, 0.f}};
    its.indices  = {{0, 1, 2}};
    for (double offset : cube_offsets) {
        TriangleMesh cube = make_cube(10, 10, 10);
        cube.translate(float(offset), 0.f, 0.f);
        its_merge(its, cube.its);
    }
    return TriangleMesh(its);
}

} // namespace

TEST_CASE("Splitting a part drops pieces enclosing no volume and keeps the part itself", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(flat_triangle_then_cubes({0., 30.}), ModelVolumeType::MODEL_PART, false);
    REQUIRE(volume->is_splittable());

    CHECK(volume->split(1, false) == 2);
    REQUIRE(object->volumes.size() == 2);
    CHECK(std::find(object->volumes.begin(), object->volumes.end(), volume) != object->volumes.end());
    for (const ModelVolume *part : object->volumes) {
        CHECK(part->mesh().facets_count() == 12);
        CHECK_FALSE(part->is_splittable());
    }
}

TEST_CASE("Splitting a part with a single solid piece keeps just that piece", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(flat_triangle_then_cubes({0.}), ModelVolumeType::MODEL_PART, false);

    CHECK(volume->split(1, false) == 1);
    REQUIRE(object->volumes.size() == 1);
    CHECK(object->volumes.front() == volume);
    CHECK(volume->mesh().facets_count() == 12);
}

TEST_CASE("Splitting a part made only of pieces enclosing no volume leaves it whole", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    indexed_triangle_set its;
    its.vertices = {{0.f, 0.f, 0.f}, {10.f, 0.f, 0.f}, {5.f, 5.f, 0.f}, {20.f, 0.f, 0.f}, {30.f, 0.f, 0.f}, {25.f, 5.f, 0.f}};
    its.indices  = {{0, 1, 2}, {3, 4, 5}};
    ModelVolume *volume = object->add_volume(TriangleMesh(its), ModelVolumeType::MODEL_PART, false);

    CHECK(volume->split(1, false) == 1);
    REQUIRE(object->volumes.size() == 1);
    CHECK(object->volumes.front() == volume);
    CHECK(volume->mesh().facets_count() == 2);
}

TEST_CASE("Replacing a part's mesh updates whether it can be split", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    TriangleMesh two_cubes = make_cube(10, 10, 10);
    TriangleMesh second    = make_cube(10, 10, 10);
    second.translate(30.f, 0.f, 0.f);
    two_cubes.merge(second);
    ModelVolume *volume = object->add_volume(std::move(two_cubes), ModelVolumeType::MODEL_PART, false);
    REQUIRE(volume->is_splittable());

    volume->set_mesh(make_cube(10, 10, 10));
    CHECK_FALSE(volume->is_splittable());
}

TEST_CASE("Repairing a part replaces its mesh only when the repair changes it", "[Model]")
{
    Model model;
    ModelObject *object = model.add_object();
    TriangleMesh flipped = make_cube(10, 10, 10);
    std::swap(flipped.its.indices.front()[1], flipped.its.indices.front()[2]);
    ModelVolume *broken = object->add_volume(std::move(flipped), ModelVolumeType::MODEL_PART, false);
    TriangleMesh cube = make_cube(10, 10, 10);
    cube.translate(30.f, 0.f, 0.f);
    ModelVolume *valid = object->add_volume(std::move(cube), ModelVolumeType::MODEL_PART, false);
    const ObjectID broken_id = broken->id();
    const ObjectID valid_id  = valid->id();

    MeshRepairStats stats;
    REQUIRE(broken->repair(false, &stats));
    CHECK(stats.modified);
    CHECK(broken->id() != broken_id);
    CHECK_THAT(its_volume(broken->mesh().its), Catch::Matchers::WithinRel(1000., 1e-6));
    CHECK_FALSE(broken->get_convex_hull().empty());

    REQUIRE(valid->repair(false, &stats));
    CHECK_FALSE(stats.modified);
    CHECK(valid->id() == valid_id);
}
