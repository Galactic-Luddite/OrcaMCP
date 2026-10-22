#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/OrcaMCP/OrcaMCPMeshRepair.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPUiJob.hpp"
#include "slic3r/Utils/FixModelByCgal.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include "mesh_fixtures.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// The object list's Repair, split in two so no thread but the main one changes the model: a plan
// worked out on the meshes alone (upstream's repair_mesh on copies), and the end of upstream's
// ModelVolume::repair applying it. The object list's Repair, the cut gizmo and MCP's repair_mesh all run
// it, so what is checked here is what each of them does. The meshes are built with known defects: a
// hole, a flat stray sheet, a closed stray shell.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using namespace mesh_fixtures;
using Catch::Matchers::WithinAbs;
using json = nlohmann::json;

namespace {

const std::function<bool()> never = [] { return false; };

CgalRepairPlan planned(const ModelObject& object, int volume_idx = -1, bool keep_painting = false)
{
    return plan_cgal_repair(capture_cgal_repair(object, volume_idx, keep_painting), {}, never);
}

CgalRepairResult repaired(ModelObject& object, int volume_idx = -1, bool keep_painting = false)
{
    CgalRepairPlan plan = planned(object, volume_idx, keep_painting);
    return apply_cgal_repair(object, keep_painting, plan);
}

int open_edges(const ModelObject& object) { return object.get_object_stl_stats().open_edges; }

bool mentions(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

RepairedMeshErrors reversed_facets(int count)
{
    RepairedMeshErrors errors;
    errors.facets_reversed = count;
    return errors;
}

// The facets of the object's first volume that `paint` picks painted with filament 2, and every one
// a support enforcer.
void paint_first_volume(ModelObject& object, const std::function<bool(size_t)>& paint = [](size_t) { return true; })
{
    ModelVolume&     volume = *object.volumes.front();
    TriangleSelector colour(volume.mesh());
    TriangleSelector support(volume.mesh());
    for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i) {
        if (paint(i))
            colour.set_facet(int(i), EnforcerBlockerType::Extruder2);
        support.set_facet(int(i), EnforcerBlockerType::ENFORCER);
    }
    volume.mmu_segmentation_facets.set(colour);
    volume.supported_facets.set(support);
}

void check_same_box(const BoundingBoxf3& actual, const BoundingBoxf3& expected)
{
    for (int axis = 0; axis < 3; ++axis) {
        CHECK_THAT(actual.min[axis], WithinAbs(expected.min[axis], 1e-4));
        CHECK_THAT(actual.max[axis], WithinAbs(expected.max[axis], 1e-4));
    }
}

indexed_triangle_set fixture_mesh(const std::string& name)
{
    if (name == "hole")
        return cube_missing_facet();
    if (name == "flat sheet")
        return cube_with(flat_square());
    if (name == "closed stray shell")
        return cube_with(its_make_cube(1.0, 1.0, 1.0));
    if (name == "holed stray shell")
        return cube_with(cube_missing_facet());
    return its_make_cube(10.0, 10.0, 10.0);
}

} // namespace

TEST_CASE("A hole is closed, and the object keeps its one volume", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    REQUIRE(open_edges(*f.object) == 3);

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.volumes_changed() == 1);
    CHECK(plan.totals().filled_holes >= 1);
    CHECK(plan.error.empty());
    CHECK_FALSE(plan.changes_nothing());

    const CgalRepairResult result = apply_cgal_repair(*f.object, false, plan);
    CHECK(result.error.empty());
    CHECK(result.volumes_changed == 1);
    CHECK(f.object->volumes.size() == 1);
    CHECK(open_edges(*f.object) == 0);
    // The repaired mesh records no repairs, so the object list shows no warning for it.
    CHECK_FALSE(f.object->get_object_stl_stats().repaired());
}

TEST_CASE("A flat stray sheet is removed from its volume, and the rest stays where it was", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_with(flat_square()))};
    REQUIRE(open_edges(*f.object) == 4);
    REQUIRE(f.object->volumes.front()->mesh().stats().number_of_parts == 2);
    const Vec3d cube_min = f.object->instance_bounding_box(0).min;

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.totals().removed_shells == 1);

    REQUIRE(apply_cgal_repair(*f.object, false, plan).error.empty());
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(open_edges(*f.object) == 0);
    CHECK(f.object->volumes.front()->mesh().stats().number_of_parts == 1);
    CHECK_THAT(f.object->instance_bounding_box(0).min.z(), WithinAbs(cube_min.z(), 1e-4));
}

TEST_CASE("A closed stray shell stays in its volume, and is not split off", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_with(its_make_cube(1.0, 1.0, 1.0)))};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.changes_nothing());
    CHECK(plan.error.empty());

    CHECK(apply_cgal_repair(*f.object, false, plan).error.empty());
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
}

TEST_CASE("A closed one-shell mesh has nothing to repair, and its recorded repairs stay", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();
    const ObjectID id_before  = f.object->volumes.front()->id();

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.changes_nothing());

    // The object list's Repair still applies it: the volume is left as it was.
    const CgalRepairResult result = apply_cgal_repair(*f.object, false, plan);
    CHECK(result.error.empty());
    CHECK(result.volumes_changed == 0);
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
    CHECK(f.object->volumes.front()->id() == id_before);
    CHECK(f.object->get_object_stl_stats().repaired());
}

TEST_CASE("Repairing one volume leaves the object's other volumes as they were", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(20.f, 0.f, 0.f))));
    const auto     untouched_mesh = f.object->volumes[0]->mesh_ptr();
    const ObjectID untouched_id   = f.object->volumes[0]->id();

    CgalRepairPlan plan = planned(*f.object, 1);
    CHECK(plan.volume_index_in(*f.object) == 1);
    const CgalRepairResult result = apply_cgal_repair(*f.object, false, plan);
    CHECK(result.error.empty());
    REQUIRE(f.object->volumes.size() == 2);
    CHECK(f.object->volumes[0]->mesh_ptr() == untouched_mesh);
    CHECK(f.object->volumes[0]->id() == untouched_id);
    CHECK(f.object->volumes[1]->mesh().stats().open_edges == 0);
}

// The object list's Repair used to repair in place on its worker thread; MCP never did. Both now plan
// first and apply after, and this is what holds them to upstream's result: the planned repair and
// upstream's ModelVolume::repair, volume by volume, build the same object.
TEST_CASE("The planned repair builds the same object as the repair done in place", "[MeshRepair]")
{
    const std::string fixture       = GENERATE(as<std::string>{}, "hole", "flat sheet", "closed stray shell", "holed stray shell", "clean");
    const bool        keep_painting = GENERATE(false, true);
    INFO("fixture: " << fixture << ", keep_painting: " << keep_painting);
    OnePartObject planned_object{TriangleMesh(fixture_mesh(fixture))};
    OnePartObject in_place_object{TriangleMesh(fixture_mesh(fixture))};
    // Colour on the facets of the first shell only (the first 12 or 11); support everywhere.
    for (ModelObject* object : {planned_object.object, in_place_object.object})
        paint_first_volume(*object, [](size_t facet) { return facet < 11; });

    CgalRepairPlan         plan           = planned(*planned_object.object, -1, keep_painting);
    const CgalRepairResult planned_result = apply_cgal_repair(*planned_object.object, keep_painting, plan);
    CHECK(planned_result.error.empty());
    for (ModelVolume* volume : in_place_object.object->volumes)
        volume->repair(keep_painting);

    const ModelObject& a = *planned_object.object;
    const ModelObject& b = *in_place_object.object;
    REQUIRE(a.volumes.size() == b.volumes.size());
    for (size_t i = 0; i < a.volumes.size(); ++i) {
        INFO("volume " << i);
        CHECK(a.volumes[i]->name == b.volumes[i]->name);
        CHECK(a.volumes[i]->mesh().its.vertices == b.volumes[i]->mesh().its.vertices);
        CHECK(a.volumes[i]->mesh().its.indices == b.volumes[i]->mesh().its.indices);
        CHECK(a.volumes[i]->get_matrix().isApprox(b.volumes[i]->get_matrix()));
        CHECK(a.volumes[i]->get_convex_hull().its.vertices == b.volumes[i]->get_convex_hull().its.vertices);
        CHECK(a.volumes[i]->get_convex_hull().its.indices == b.volumes[i]->get_convex_hull().its.indices);
        // The painting the plan remapped off the main thread is the painting upstream's repair remaps.
        CHECK(a.volumes[i]->is_mm_painted() == b.volumes[i]->is_mm_painted());
        CHECK(a.volumes[i]->is_fdm_support_painted() == b.volumes[i]->is_fdm_support_painted());
        CHECK((a.volumes[i]->mmu_segmentation_facets.get_data() == b.volumes[i]->mmu_segmentation_facets.get_data()));
        CHECK((a.volumes[i]->supported_facets.get_data() == b.volumes[i]->supported_facets.get_data()));
    }
}

// The object list captures every selected volume before its first dialog, and applies them one after
// the other: the first one's repair gives its volume a new id, so each is found again by its own.
TEST_CASE("Volumes captured before any repair are each repaired by their own id", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(40.f, 0.f, 0.f))));

    CgalRepairPlan first  = planned(*f.object, 0);
    CgalRepairPlan second = planned(*f.object, 1);

    REQUIRE(apply_cgal_repair(*f.object, false, first).error.empty());
    const auto first_mesh = f.object->volumes[0]->mesh_ptr();
    REQUIRE(second.applies_to(*f.object));
    const CgalRepairResult result = apply_cgal_repair(*f.object, false, second);
    CHECK(result.error.empty());
    CHECK(result.volumes_changed == 1);
    CHECK(f.object->volumes[1]->mesh().stats().open_edges == 0);
    CHECK(f.object->volumes[0]->mesh_ptr() == first_mesh);
}

TEST_CASE("A one-volume repair captured for a volume the object does not have repairs nothing", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    CgalRepairPlan plan = planned(*f.object, 3);
    CHECK_FALSE(plan.applies_to(*f.object));
    CHECK_FALSE(apply_cgal_repair(*f.object, false, plan).error.empty());
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
    CHECK(open_edges(*f.object) == 3);
}

// The object's first volume painted with `filament` on every facet.
void paint_first_volume_with(ModelObject& object, EnforcerBlockerType filament)
{
    ModelVolume&     volume = *object.volumes.front();
    TriangleSelector colour(volume.mesh());
    for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i)
        colour.set_facet(int(i), filament);
    volume.mmu_segmentation_facets.set(colour);
}

void check_same_painting(const ModelObject& a, const ModelObject& b)
{
    REQUIRE(a.volumes.size() == b.volumes.size());
    for (size_t i = 0; i < a.volumes.size(); ++i) {
        INFO("volume " << i);
        CHECK((a.volumes[i]->mmu_segmentation_facets.get_data() == b.volumes[i]->mmu_segmentation_facets.get_data()));
        CHECK((a.volumes[i]->supported_facets.get_data() == b.volumes[i]->supported_facets.get_data()));
    }
}

// The painting the plan remapped is the painting captured. Paint written before the repair is applied
// -- a paint tool, a stroke in the gizmo -- is remapped in place instead, as upstream's repair would, so
// the repair keeps the painting as it is when applied.
TEST_CASE("Painting changed between capture and apply is remapped as it is when applied", "[MeshRepair]")
{
    const std::string fixture = GENERATE(as<std::string>{}, "hole", "holed stray shell");
    INFO("fixture: " << fixture);
    OnePartObject planned_object{TriangleMesh(fixture_mesh(fixture))};
    OnePartObject reference{TriangleMesh(fixture_mesh(fixture))};
    paint_first_volume_with(*planned_object.object, EnforcerBlockerType::Extruder2);

    CgalRepairPlan plan = planned(*planned_object.object, -1, /*keep_painting=*/true);
    // Painted again, with another filament, after the capture.
    paint_first_volume_with(*planned_object.object, EnforcerBlockerType::Extruder3);
    CHECK_FALSE(plan.painting_unchanged(*planned_object.object->volumes.front()));
    REQUIRE(apply_cgal_repair(*planned_object.object, true, plan).error.empty());

    paint_first_volume_with(*reference.object, EnforcerBlockerType::Extruder3);
    reference.object->volumes.front()->repair(true);

    check_same_painting(*planned_object.object, *reference.object);
    CHECK(planned_object.object->volumes.front()->is_mm_painted());
}

// Two volumes can hold the same mesh with different painting; each keeps its own.
TEST_CASE("Volumes with the same mesh keep their own painting", "[MeshRepair]")
{
    auto build = [](OnePartObject& f) {
        f.object->add_volume(TriangleMesh(cube_missing_facet()));
        for (size_t v = 0; v < 2; ++v) {
            ModelVolume&     volume = *f.object->volumes[v];
            TriangleSelector colour(volume.mesh());
            for (size_t i = 0; i < volume.mesh().its.indices.size(); ++i)
                colour.set_facet(int(i), v == 0 ? EnforcerBlockerType::Extruder2 : EnforcerBlockerType::Extruder3);
            volume.mmu_segmentation_facets.set(colour);
        }
    };
    OnePartObject planned_object{TriangleMesh(cube_missing_facet())};
    OnePartObject reference{TriangleMesh(cube_missing_facet())};
    build(planned_object);
    build(reference);

    CgalRepairPlan plan = planned(*planned_object.object, -1, true);
    REQUIRE(apply_cgal_repair(*planned_object.object, true, plan).error.empty());
    for (ModelVolume* volume : reference.object->volumes)
        volume->repair(true);

    check_same_painting(*planned_object.object, *reference.object);
    CHECK_FALSE((planned_object.object->volumes[0]->mmu_segmentation_facets.get_data() ==
                 planned_object.object->volumes[1]->mmu_segmentation_facets.get_data()));
}

TEST_CASE("A volume no solid can be built from is left as it is, and the others are still repaired", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(flat_square())};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(20.f, 0.f, 0.f))));
    const auto flat_mesh = f.object->volumes[0]->mesh_ptr();

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.volumes_changed() == 1);
    CHECK_FALSE(plan.error.empty());
    CHECK(plan.error_volume == 0);

    const CgalRepairResult result = apply_cgal_repair(*f.object, false, plan);
    CHECK(result.error.empty());
    CHECK(result.volumes_changed == 1);
    REQUIRE(f.object->volumes.size() == 2);
    CHECK(f.object->volumes[0]->mesh_ptr() == flat_mesh);
    CHECK(f.object->volumes[1]->mesh().stats().open_edges == 0);
}

TEST_CASE("An object no solid can be built from changes nothing", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(flat_square())};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.changes_nothing());
    CHECK_FALSE(plan.error.empty());
    CHECK(apply_cgal_repair(*f.object, false, plan).volumes_changed == 0);
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
}

TEST_CASE("Painting is cleared by a repair unless it is kept", "[MeshRepair]")
{
    const bool keep_painting = GENERATE(false, true);
    INFO("keep_painting: " << keep_painting);
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    paint_first_volume(*f.object);
    REQUIRE(f.object->volumes.front()->is_mm_painted());

    CHECK(repaired(*f.object, -1, keep_painting).error.empty());
    CHECK(f.object->volumes.front()->is_mm_painted() == keep_painting);
}

TEST_CASE("A canceled plan applies nothing", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_with(cube_missing_facet()))};
    const auto    mesh_before = f.object->volumes.front()->mesh_ptr();

    int            asked = 0;
    CgalRepairPlan plan  = plan_cgal_repair(capture_cgal_repair(*f.object, -1), {}, [&asked] { return ++asked > 1; });
    CHECK(plan.canceled);
    CHECK(asked >= 2);

    CHECK_FALSE(apply_cgal_repair(*f.object, false, plan).error.empty());
    REQUIRE(f.object->volumes.size() == 1);
    CHECK(f.object->volumes.front()->mesh_ptr() == mesh_before);
}

TEST_CASE("A plan no longer applies once the mesh it read has changed", "[MeshRepair]")
{
    OnePartObject  f{TriangleMesh(cube_missing_facet())};
    CgalRepairPlan plan = planned(*f.object);
    CHECK(plan.applies_to(*f.object));

    f.object->volumes.front()->set_mesh(TriangleMesh(its_make_cube(5.0, 5.0, 5.0)));
    CHECK_FALSE(plan.applies_to(*f.object));
    CHECK_FALSE(apply_cgal_repair(*f.object, false, plan).error.empty());
    CHECK(f.object->volumes.front()->mesh().stats().number_of_facets == 12);
}

TEST_CASE("The captured object is found by its place in the model's list, and not once it is gone", "[MeshRepair]")
{
    Model        model;
    ModelObject* first  = model.add_object();
    ModelObject* second = model.add_object();
    first->add_volume(TriangleMesh(its_make_cube(10.0, 10.0, 10.0)));
    second->add_volume(TriangleMesh(cube_missing_facet()));
    CgalRepairPlan plan = planned(*second);

    CHECK(cgal_repair_object_index(model, plan) == 1);
    model.delete_object(size_t(0));
    CHECK(cgal_repair_object_index(model, plan) == 0);
    model.delete_object(size_t(0));
    CHECK(cgal_repair_object_index(model, plan) == -1);
}

TEST_CASE("The plan reports the progress the Repair dialog shows", "[MeshRepair]")
{
    OnePartObject f{TriangleMesh(cube_missing_facet())};
    f.object->add_volume(TriangleMesh(translated(cube_missing_facet(), Vec3f(20.f, 0.f, 0.f))));
    f.object->volumes[1]->name = "second";
    std::vector<std::string> messages;
    std::vector<unsigned>    percents;
    plan_cgal_repair(capture_cgal_repair(*f.object, -1),
                     [&](const std::string& message, unsigned percent) {
                         messages.push_back(message);
                         percents.push_back(percent);
                     },
                     never);

    REQUIRE_FALSE(messages.empty());
    // With several volumes, the one being repaired, as upstream's dialog names it.
    CHECK(std::find(messages.begin(), messages.end(), "second") != messages.end());
    CHECK(messages.back() == "Repair finished");
    CHECK(percents.back() == 100);
    CHECK(std::is_sorted(percents.begin(), percents.end()));
}

// ---- repair_mesh's decisions and answer ---------------------------------------------------------

TEST_CASE("repair_mesh is refused while the pipeline is busy, a job runs or the app's own Repair runs", "[MeshRepair][orcamcp]")
{
    PipelineState idle;
    CHECK_FALSE(repair_refusal(idle, false, false));

    PipelineState slicing;
    slicing.process_working = true;
    slicing.is_slicing      = true;
    const auto busy         = repair_refusal(slicing, false, false);
    REQUIRE(busy);
    CHECK(busy->find("a slice is in progress") != std::string::npos);
    CHECK(mentions(*busy, "wait_for_slice"));
    CHECK(mentions(*busy, "repair_mesh again"));

    const auto job = repair_refusal(idle, true, false);
    REQUIRE(job);
    CHECK(*job == ui_job_busy_message("repair_mesh"));

    const auto dialog = repair_refusal(idle, false, true);
    REQUIRE(dialog);
    CHECK(mentions(*dialog, "own mesh repair"));
    CHECK(mentions(*dialog, "repair_mesh again"));
}

TEST_CASE("repair_mesh refuses a volume_id the object does not have, a negative one too", "[MeshRepair][orcamcp]")
{
    CHECK_FALSE(repair_volume_error(0, 2, std::nullopt)); // none given: the whole object
    CHECK_FALSE(repair_volume_error(0, 2, 1));
    for (const int bad : {2, -1}) {
        const auto error = repair_volume_error(3, 2, bad);
        REQUIRE(error);
        CHECK(mentions(*error, "volume_id " + std::to_string(bad)));
        CHECK(mentions(*error, "object 3"));
        CHECK(mentions(*error, "volume_id 0 to 1"));
    }
}

TEST_CASE("repair_mesh applies only a finished plan that changes something, in time", "[MeshRepair][orcamcp]")
{
    OnePartObject hole{TriangleMesh(cube_missing_facet())};
    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0))};
    OnePartObject sheet{TriangleMesh(flat_square())};

    CHECK(repair_step(planned(*hole.object), false) == RepairStep::apply);
    // The one step that changes the object is the only one with an undo snapshot.
    CHECK(repair_step(planned(*clean.object), false) == RepairStep::nothing_to_repair);
    CHECK(repair_step(planned(*hole.object), true) == RepairStep::timed_out);
    CHECK(repair_step(planned(*sheet.object), false) == RepairStep::failed);

    CgalRepairPlan canceled = planned(*hole.object);
    canceled.canceled       = true;
    CHECK(repair_step(canceled, false) == RepairStep::timed_out);
    // A volume that could not be repaired beside one that was: the repaired one is applied.
    CgalRepairPlan partly = planned(*hole.object);
    partly.error          = "No solid can be built from the mesh.";
    partly.error_volume   = 1;
    CHECK(repair_step(partly, false) == RepairStep::apply);
}

TEST_CASE("repair_mesh says why nothing was changed, and what to call instead", "[MeshRepair][orcamcp]")
{
    OnePartObject clean{TriangleMesh(its_make_cube(10.0, 10.0, 10.0), reversed_facets(1))};
    OnePartObject hole{TriangleMesh(cube_missing_facet())};
    OnePartObject sheet{TriangleMesh(flat_square())};

    const std::string nothing = repair_step_message(RepairStep::nothing_to_repair, planned(*clean.object), 0, -1, 105.0,
                                                    repair_facts_json(*clean.object));
    CHECK(nothing.find("Nothing to repair") != std::string::npos);
    CHECK(nothing.find("1 repair recorded") != std::string::npos);

    const std::string timed_out = repair_step_message(RepairStep::timed_out, planned(*hole.object), 0, -1, 105.0,
                                                      repair_facts_json(*hole.object));
    CHECK(timed_out.find("105 s") != std::string::npos);
    CHECK(timed_out.find("ORCAMCP_TIMEOUT") != std::string::npos);
    CHECK(timed_out.find("nothing was changed") != std::string::npos);

    const CgalRepairPlan flat    = planned(*sheet.object);
    const std::string    failure = repair_step_message(RepairStep::failed, flat, 0, -1, 105.0, repair_facts_json(*sheet.object));
    CHECK(mentions(failure, "failed on volume 0"));
    CHECK(mentions(failure, "Nothing was changed"));
    CHECK(mentions(failure, "delete_object"));

    // Never a GUI button: what an agent is told to do, it can do itself.
    for (const std::string& message : {nothing, timed_out, failure})
        for (const char* gui : {"click", "Click", "the user", "GUI", "button"}) {
            INFO(message << " names " << gui);
            CHECK(message.find(gui) == std::string::npos);
        }
}

TEST_CASE("repair_mesh's answer carries the numbers before and after, the repair, painting and volumes", "[MeshRepair][orcamcp]")
{
    OnePartObject  f{TriangleMesh(cube_with(flat_square()))};
    const json     before = repair_facts_json(*f.object);
    CgalRepairPlan plan   = planned(*f.object);
    REQUIRE(apply_cgal_repair(*f.object, false, plan).error.empty());
    const json after = repair_facts_json(*f.object);

    for (const char* key : {"facets", "shells", "open_edges", "manifold", "repaired", "errors_repaired", "repaired_errors",
                            "mesh_warning", "volumes", "painted", "position"})
        CHECK(before.contains(key));
    CHECK(before["open_edges"] == 4);
    CHECK(before["mesh_warning"] == true);
    CHECK(after["open_edges"] == 0);
    CHECK(after["shells"] == 1);
    CHECK(after["mesh_warning"] == false);

    const json answer = repair_answer_json(0, "Test object", -1, false, false, before, after, plan);
    CHECK(answer["status"] == "success");
    CHECK(answer["changed"] == true);
    CHECK(answer["object_id"] == 0);
    CHECK(answer["object_name"] == "Test object");
    CHECK_FALSE(answer.contains("volume_id"));
    CHECK(answer["keep_painting"] == false);
    CHECK(answer["keep_painting_from"] == "app_setting");
    REQUIRE(answer.contains("repair"));
    CHECK(answer["repair"]["volumes_repaired"] == 1);
    CHECK(answer["repair"]["removed_shells"] == 1);
    for (const char* key : {"welded_vertices", "removed_triangles", "reversed_shells", "filled_holes", "self_intersections"})
        CHECK(answer["repair"].contains(key));
    CHECK_FALSE(answer.contains("message"));
    CHECK(answer["before"] == before);
    CHECK(answer["after"] == after);

    const json one_volume = repair_answer_json(0, "Test object", 0, true, true, before, after, plan);
    CHECK(one_volume["volume_id"] == 0);
    CHECK(one_volume["keep_painting"] == true);
    CHECK(one_volume["keep_painting_from"] == "argument");

    CgalRepairPlan partly = plan;
    partly.error          = "flat: No solid can be built from the mesh.";
    const json partial    = repair_answer_json(0, "Test object", -1, false, false, before, after, partly);
    CHECK(mentions(partial["message"].get<std::string>(), "Not every volume could be repaired"));
    CHECK(mentions(partial["message"].get<std::string>(), "left as they were"));
}

// Hidden ([.]): where the time goes on large meshes -- how long the main thread's two steps (capture,
// apply) and the plan take -- printed, not asserted, since it depends on the machine and the build.
// Run it by its tag; the painted cases take minutes on the -O0 build (the remap, in the plan).
TEST_CASE("The repair's steps are timed on large meshes", "[.][MeshRepairTiming]")
{
    using Clock  = std::chrono::steady_clock;
    auto seconds = [](Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); };
    auto holed   = [](indexed_triangle_set its) {
        its.indices.erase(its.indices.begin() + std::ptrdiff_t(its.indices.size() / 2));
        return its;
    };
    auto spheres = [&holed](int count) {
        indexed_triangle_set shells;
        for (int i = 0; i < count; ++i)
            its_merge(shells, translated(holed(its_make_sphere(1., 2. * PI / 32.)), Vec3f(3.f * float(i % 20), 3.f * float(i / 20), 0.f)));
        return shells;
    };
    const std::vector<std::pair<std::string, indexed_triangle_set>> cases = {
        {"one sphere with a hole", holed(its_make_sphere(20., 2. * PI / 707.))},
        {"one smaller sphere with a hole", holed(its_make_sphere(20., 2. * PI / 100.))},
        {"200 small spheres, each with a hole", spheres(200)},
        {"10 small spheres, each with a hole", spheres(10)}};

    for (const bool keep_painting : {false, true})
        for (const auto& [name, mesh] : cases) {
            if (keep_painting && mesh.indices.size() > 20000)
                continue; // the painting remap takes minutes per 10,000 painted facets on the dev build
            OnePartObject f{TriangleMesh(mesh)};
            if (keep_painting)
                paint_first_volume(*f.object);
            const auto       t0     = Clock::now();
            CgalRepairInput  input  = capture_cgal_repair(*f.object, -1, keep_painting);
            const auto       t1     = Clock::now();
            CgalRepairPlan   plan   = plan_cgal_repair(std::move(input), {}, never);
            const auto       t2     = Clock::now();
            CgalRepairResult result = apply_cgal_repair(*f.object, keep_painting, plan);
            const auto       t3     = Clock::now();
            WARN(name << ", " << mesh.indices.size() << " facets, keep_painting " << keep_painting << ": capture "
                      << seconds(t0, t1) << " s, plan " << seconds(t1, t2) << " s, apply " << seconds(t2, t3)
                      << " s; volumes repaired " << result.volumes_changed << ", holes filled " << plan.totals().filled_holes
                      << (plan.error.empty() ? "" : ", error " + plan.error));
        }
}
