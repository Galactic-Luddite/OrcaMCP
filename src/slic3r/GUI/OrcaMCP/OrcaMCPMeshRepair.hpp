// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshRepair.hpp
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "OrcaMCPSliceProgress.hpp"

// repair_mesh's decisions and answer, apart from the app, so they are tested without it
// (tests/slic3rutils/test_mesh_repair.cpp). The repair is the object list's own (FixModelByCgal.hpp);
// the tool (OrcaMCPMeshTools.cpp) runs it in three steps: capture on the main thread, plan on the
// HTTP thread, apply on the main thread.

namespace Slic3r {
class ModelObject;
class CgalRepairPlan;
namespace GUI { namespace OrcaMCP {

// Why repair_mesh must not start, or change the object, now: the slicing pipeline is busy (the
// repair would restart it), an arrange or orient job runs, or the app's own Repair runs, whose
// progress dialog lets calls through while it works out its plan.
std::optional<std::string> repair_refusal(const PipelineState& pipeline, bool ui_job_running, bool repair_dialog_running);

// What is wrong with `volume_id` for object `object_id` of `volumes` volumes, or nothing: any value
// outside 0 to volumes - 1, negative ones included. None given: the whole object.
std::optional<std::string> repair_volume_error(int object_id, std::size_t volumes, std::optional<int> volume_id);

// What a plan leads to once its object is found as it was captured. Only `apply` changes the object,
// and only it takes an undo snapshot: it changes every volume the repair could repair, and leaves one it
// could not (no solid can be built from it) as it is. `failed`: no volume changed, and one could not be
// repaired.
enum class RepairStep { apply, nothing_to_repair, timed_out, failed };
RepairStep repair_step(const CgalRepairPlan& plan, bool past_wait_cap);

// Why a step other than `apply` changed nothing, for volume `volume_id` (-1: the whole object) of
// object `object_id`. `before` is repair_facts_json of the object; `wait_cap_s` how long the call
// could wait.
std::string repair_step_message(RepairStep step, const CgalRepairPlan& plan, int object_id, int volume_id, double wait_cap_s,
                                const nlohmann::json& before);

// An object as repair_mesh reports it before and after: get_mesh_health's numbers for the object
// with its mesh_warning, `volumes` (how many), `painted` (any volume has painting of any kind) and
// `position` (the centre of its box, plate mm).
nlohmann::json repair_facts_json(const ModelObject& object);

// What the repair did, summed over the volumes (Slic3r::MeshRepairStats): volumes_repaired, and the
// vertices welded, triangles removed, shells reversed, holes filled, shells removed (fewer than 4 facets)
// and self-intersections removed.
nlohmann::json repair_stats_json(const CgalRepairPlan& plan);
// The same summed over several plans (a repair of several objects).
nlohmann::json repair_stats_json(const std::vector<const CgalRepairPlan*>& plans);

// The volumes the repair could not repair, which it left as they were, in the repair's words; "" when
// none.
std::string unrepaired_volumes_message(const CgalRepairPlan& plan);

// The answer to a repair that was applied, before the tool adds the volume rows and the placement.
// `volume_id` -1: the whole object. `keep_painting_given`: the call said, rather than the app's setting.
nlohmann::json repair_answer_json(int object_id, const std::string& object_name, int volume_id, bool keep_painting,
                                  bool keep_painting_given, const nlohmann::json& before, const nlohmann::json& after,
                                  const CgalRepairPlan& plan);


// ---- repair_mesh of several objects: a plate's Fix All, or Fix All Plates ----

// Which objects a call names: one (object_id, where volume_id may also be given), every object on a plate
// (plate_index) or every object in the project (all_plates).
enum class RepairScope { object, plate, all_plates };

// Why the call names none of the three, or several, or a volume_id without object_id, or nothing.
// `all_plates` is the flag's value: false names nothing.
std::optional<std::string> repair_scope_error(bool object_given, bool volume_given, bool plate_given, bool all_plates);

// `object_indices` ascending, each once, as ObjectList::fix_all_through_cgal sorts them.
std::vector<int> repair_order(std::vector<int> object_indices);

// How one object of several came out. timed_out: not planned within the call's wait (nothing was changed on it);
// changed_meanwhile: no longer what was captured when its plan was to be applied (nothing was changed on it).
enum class BatchRepairResult { repaired, nothing_to_repair, failed, changed_meanwhile, timed_out };

// `plan` null: not planned. `planned_in_time`: the plan finished before the call's wait ran out.
// `unchanged_in_app`: the object is still as it was captured. A plan with a volume that could not be repaired,
// beside ones that could, is `failed` though the others are applied.
BatchRepairResult batch_repair_result(const CgalRepairPlan* plan, bool planned_in_time, bool unchanged_in_app);

// One object of a repair of several, as the tool leaves it for the answer. `plan` outlives the item.
struct BatchRepairItem
{
    int                   object_id = -1;
    std::string           object_name;
    nlohmann::json        before;
    nlohmann::json        after;
    nlohmann::json        placement;
    BatchRepairResult     result  = BatchRepairResult::timed_out;
    bool                  changed = false; // some volume of it was repaired and applied
    const CgalRepairPlan* plan    = nullptr;
};

// The object's entry in the answer: its result, and what came of it.
nlohmann::json batch_repair_object_json(const BatchRepairItem& item, double wait_cap_s);

// The answer to a repair of several objects (`scope` plate or all_plates; `plate_index` only for a plate).
nlohmann::json batch_repair_answer_json(RepairScope scope, std::optional<int> plate_index, bool keep_painting,
                                        bool keep_painting_given, const std::vector<BatchRepairItem>& items, double wait_cap_s);

}} // namespace GUI::OrcaMCP
} // namespace Slic3r
