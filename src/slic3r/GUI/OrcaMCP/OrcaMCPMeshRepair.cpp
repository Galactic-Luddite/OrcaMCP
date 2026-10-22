// src/slic3r/GUI/OrcaMCP/OrcaMCPMeshRepair.cpp
#include "OrcaMCPMeshRepair.hpp"

#include "OrcaMCPCommon.hpp"
#include "OrcaMCPMeshHealth.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPUiJob.hpp"

#include "slic3r/Utils/FixModelByCgal.hpp"
#include "libslic3r/Model.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI { namespace OrcaMCP {

namespace {

std::string object_label(int object_id) { return "object " + std::to_string(object_id); }

std::string subject(int object_id, int volume_id)
{
    return volume_id < 0 ? object_label(object_id) : "volume " + std::to_string(volume_id) + " of " + object_label(object_id);
}

std::string without_final_period(std::string text)
{
    while (!text.empty() && (text.back() == '.' || text.back() == ' '))
        text.pop_back();
    return text;
}

std::string nothing_to_repair(int object_id, int volume_id, const nlohmann::json& before)
{
    std::string message = "Nothing to repair: " + subject(object_id, volume_id) +
                          " is already closed, consistently oriented and free of self-intersections, so nothing was changed.";
    const int recorded = before.value("errors_repaired", 0);
    if (volume_id < 0 && before.value("mesh_warning", false) && recorded > 0)
        message += " Its mesh_warning comes from the " + std::to_string(recorded) + (recorded == 1 ? " repair" : " repairs") +
                   " recorded when the mesh was loaded: the mesh is closed and prints as it is, and a repair leaves that "
                   "record as it is.";
    return message;
}

std::string timed_out(int object_id, int volume_id, double wait_cap_s, const nlohmann::json& before)
{
    std::string message = "Repairing " + subject(object_id, volume_id) + " took longer than this call may wait (" +
                          std::to_string(int(std::lround(wait_cap_s))) +
                          " s), so nothing was changed. That wait is set a little under the MCP bridge's ORCAMCP_TIMEOUT: "
                          "a larger mesh needs a larger ORCAMCP_TIMEOUT.";
    if (volume_id < 0 && before.value("volumes", 0) > 1)
        message += " volume_id repairs one volume at a time.";
    return message;
}

std::string failed(const CgalRepairPlan& plan, int object_id)
{
    const std::string where = plan.error_volume >= 0 ? " on volume " + std::to_string(plan.error_volume) : std::string();
    return "Repairing " + object_label(object_id) + " failed" + where + ": " + without_final_period(plan.error) +
           ". Nothing was changed. A volume from which no solid can be built (a flat or empty one) cannot be repaired; "
           "delete_object with volume_id removes one, or the object.";
}

std::string changed_meanwhile_message(int object_id)
{
    return "Object " + std::to_string(object_id) + " changed in the app while it was being repaired, so nothing was changed on it: "
           "call repair_mesh with object_id " + std::to_string(object_id) + " again";
}

std::string deleted_meanwhile_message(const BatchRepairItem& item)
{
    return "Object " + std::to_string(item.object_id) + " (\"" + item.object_name +
           "\") was deleted in the app while it was being repaired, so nothing was changed";
}

std::string timed_out_in_batch_message(int object_id, double wait_cap_s)
{
    return "Object " + std::to_string(object_id) + " was not repaired within this call's wait (" +
           std::to_string(int(std::lround(wait_cap_s))) +
           " s), so nothing was changed on it. That wait is set a little under the MCP bridge's ORCAMCP_TIMEOUT; repair_mesh with "
           "object_id " + std::to_string(object_id) + " repairs it on its own";
}

const char* batch_result_name(BatchRepairResult result)
{
    switch (result) {
    case BatchRepairResult::repaired: return "repaired";
    case BatchRepairResult::nothing_to_repair: return "nothing_to_repair";
    case BatchRepairResult::failed: return "failed";
    case BatchRepairResult::changed_meanwhile: return "changed_meanwhile";
    case BatchRepairResult::deleted_meanwhile: return "deleted_meanwhile";
    case BatchRepairResult::timed_out: return "timed_out";
    }
    return "timed_out";
}

// Why an object that is not a clean repaired one came out as it did; "" for one that is.
std::string batch_repair_message(const BatchRepairItem& item, double wait_cap_s)
{
    switch (item.result) {
    case BatchRepairResult::repaired: return {};
    case BatchRepairResult::nothing_to_repair:
        return repair_step_message(RepairStep::nothing_to_repair, *item.plan, item.object_id, -1, wait_cap_s, item.before);
    case BatchRepairResult::failed:
        return item.changed ? unrepaired_volumes_message(*item.plan)
                            : repair_step_message(RepairStep::failed, *item.plan, item.object_id, -1, wait_cap_s, item.before);
    case BatchRepairResult::changed_meanwhile: return changed_meanwhile_message(item.object_id);
    case BatchRepairResult::deleted_meanwhile: return deleted_meanwhile_message(item);
    case BatchRepairResult::timed_out: return timed_out_in_batch_message(item.object_id, wait_cap_s);
    }
    return {};
}

std::vector<int> object_ids_with(const std::vector<BatchRepairItem>& items, BatchRepairResult result)
{
    std::vector<int> ids;
    for (const BatchRepairItem& item : items)
        if (item.result == result)
            ids.push_back(item.object_id);
    return ids;
}

std::string plural_objects(std::size_t count) { return std::to_string(count) + (count == 1 ? " object" : " objects"); }

// The answer's one sentence: who was repaired, who failed and why, who needed nothing, who was not reached.
std::string batch_repair_summary(const std::vector<BatchRepairItem>& items, double wait_cap_s)
{
    std::string summary;
    const auto  add = [&summary](const std::string& sentence) { summary += (summary.empty() ? "" : " ") + sentence; };
    if (const auto repaired = object_ids_with(items, BatchRepairResult::repaired); !repaired.empty())
        add("Repaired " + std::string(repaired.size() == 1 ? "object " : "objects ") + listed_ids(repaired) + ".");
    std::string failed_text;
    for (const BatchRepairItem& item : items)
        if (item.result == BatchRepairResult::failed || item.result == BatchRepairResult::changed_meanwhile ||
            item.result == BatchRepairResult::deleted_meanwhile)
            failed_text += (failed_text.empty() ? "" : "; ") + ("object " + std::to_string(item.object_id) + " (\"" + item.object_name +
                                                                  "\"): " + without_final_period(batch_repair_message(item, wait_cap_s)));
    if (!failed_text.empty())
        add("Failed: " + failed_text + ".");
    if (const auto clean = object_ids_with(items, BatchRepairResult::nothing_to_repair); !clean.empty())
        add(plural_objects(clean.size()) + " needed no repair.");
    if (const auto late = object_ids_with(items, BatchRepairResult::timed_out); !late.empty())
        add(plural_objects(late.size()) + " (" + listed_ids(late) + ") not repaired within this call's wait (" +
            std::to_string(int(std::lround(wait_cap_s))) + " s); repair_mesh with object_id repairs one on its own.");
    return summary;
}

bool any_volume_painted(const ModelObject& object)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [](const ModelVolume* volume) { return volume->is_any_painted(); });
}

} // namespace

std::optional<std::string> repair_refusal(const PipelineState& pipeline, bool ui_job_running, bool repair_dialog_running)
{
    if (pipeline_busy(pipeline) != PipelineBusy::idle)
        return pipeline_busy_text(pipeline) + ", so nothing was repaired: call wait_for_slice, then repair_mesh again";
    if (ui_job_running)
        return ui_job_busy_message("repair_mesh");
    if (repair_dialog_running)
        return std::string("The app is running its own mesh repair, so nothing was repaired: call repair_mesh again once it has finished");
    return std::nullopt;
}

std::optional<std::string> repair_volume_error(int object_id, std::size_t volumes, std::optional<int> volume_id)
{
    if (!volume_id || (*volume_id >= 0 && std::size_t(*volume_id) < volumes))
        return std::nullopt;
    const std::string range = volumes == 1 ? "1 volume, volume_id 0" :
                                             std::to_string(volumes) + " volumes, volume_id 0 to " + std::to_string(volumes - 1);
    return "volume_id " + std::to_string(*volume_id) + " is out of range: " + object_label(object_id) + " has " + range +
           "; leave volume_id out to repair every volume";
}

RepairStep repair_step(const CgalRepairPlan& plan, bool past_wait_cap)
{
    if (plan.canceled || past_wait_cap)
        return RepairStep::timed_out;
    if (!plan.changes_nothing())
        return RepairStep::apply;
    return plan.error.empty() ? RepairStep::nothing_to_repair : RepairStep::failed;
}

std::string repair_step_message(RepairStep step, const CgalRepairPlan& plan, int object_id, int volume_id, double wait_cap_s,
                                const nlohmann::json& before)
{
    switch (step) {
    case RepairStep::nothing_to_repair: return nothing_to_repair(object_id, volume_id, before);
    case RepairStep::timed_out: return timed_out(object_id, volume_id, wait_cap_s, before);
    case RepairStep::failed: return failed(plan, object_id);
    case RepairStep::apply: break;
    }
    return {};
}

nlohmann::json repair_facts_json(const ModelObject& object)
{
    const MeshHealth health = object_mesh_health(object);
    nlohmann::json   facts  = mesh_numbers_json(health);
    facts["mesh_warning"]   = health.warning;
    facts["volumes"]        = object.volumes.size();
    facts["painted"]        = any_volume_painted(object);
    const Vec3d centre      = object_world_box(object).center();
    facts["position"]       = {{"x", centre.x()}, {"y", centre.y()}, {"z", centre.z()}};
    return facts;
}

nlohmann::json repair_stats_json(const std::vector<const CgalRepairPlan*>& plans)
{
    MeshRepairStats totals;
    std::size_t     volumes = 0;
    for (const CgalRepairPlan* plan : plans) {
        const MeshRepairStats each = plan->totals();
        totals.welded_vertices += each.welded_vertices;
        totals.removed_triangles += each.removed_triangles;
        totals.reversed_shells += each.reversed_shells;
        totals.filled_holes += each.filled_holes;
        totals.removed_shells += each.removed_shells;
        totals.self_intersections += each.self_intersections;
        volumes += plan->volumes_changed();
    }
    return {{"volumes_repaired", volumes},           {"welded_vertices", totals.welded_vertices},
            {"removed_triangles", totals.removed_triangles}, {"reversed_shells", totals.reversed_shells},
            {"filled_holes", totals.filled_holes},   {"removed_shells", totals.removed_shells},
            {"self_intersections", totals.self_intersections}};
}

nlohmann::json repair_stats_json(const CgalRepairPlan& plan)
{
    return repair_stats_json(std::vector<const CgalRepairPlan*>{&plan});
}

std::string unrepaired_volumes_message(const CgalRepairPlan& plan)
{
    if (plan.error.empty())
        return {};
    return "Not every volume could be repaired: " + without_final_period(plan.error) +
           ". Those volumes were left as they were; the others were repaired.";
}

nlohmann::json repair_answer_json(int object_id, const std::string& object_name, int volume_id, bool keep_painting,
                                  bool keep_painting_given, const nlohmann::json& before, const nlohmann::json& after,
                                  const CgalRepairPlan& plan)
{
    nlohmann::json answer = {{"status", "success"}, {"changed", true}, {"object_id", object_id}, {"object_name", object_name}};
    if (volume_id >= 0)
        answer["volume_id"] = volume_id;
    answer["keep_painting"]      = keep_painting;
    answer["keep_painting_from"] = keep_painting_given ? "argument" : "app_setting";
    answer["repair"]             = repair_stats_json(plan);
    if (const std::string unrepaired = unrepaired_volumes_message(plan); !unrepaired.empty())
        answer["message"] = unrepaired;
    answer["before"]             = before;
    answer["after"]              = after;
    return answer;
}


std::optional<std::string> repair_scope_error(bool object_given, bool volume_given, bool plate_given, bool all_plates)
{
    const int named = int(object_given) + int(plate_given) + int(all_plates);
    if (named == 0)
        return std::string("Give object_id to repair one object, plate_index to repair every object on that plate, or all_plates: "
                           "true to repair every object in the project");
    if (named > 1)
        return std::string("Give one of object_id, plate_index and all_plates, not several");
    if (volume_given && !object_given)
        return std::string("volume_id names a volume of object_id's object: give object_id with it, or leave volume_id out to repair "
                           "whole objects");
    return std::nullopt;
}

std::vector<int> repair_order(std::vector<int> object_indices)
{
    std::sort(object_indices.begin(), object_indices.end());
    object_indices.erase(std::unique(object_indices.begin(), object_indices.end()), object_indices.end());
    return object_indices;
}

bool batch_apply_time_left(std::chrono::steady_clock::duration elapsed, std::chrono::steady_clock::duration wait_cap)
{
    return elapsed <= wait_cap + batch_apply_reserve;
}

BatchRepairResult batch_repair_result(const CgalRepairPlan* plan, bool planned_in_time, bool unchanged_in_app, bool exists_in_app)
{
    if (plan == nullptr || !planned_in_time || plan->canceled)
        return BatchRepairResult::timed_out;
    if (!exists_in_app)
        return BatchRepairResult::deleted_meanwhile;
    if (!unchanged_in_app)
        return BatchRepairResult::changed_meanwhile;
    if (plan->changes_nothing())
        return plan->error.empty() ? BatchRepairResult::nothing_to_repair : BatchRepairResult::failed;
    return plan->error.empty() ? BatchRepairResult::repaired : BatchRepairResult::failed;
}

nlohmann::json batch_repair_object_json(const BatchRepairItem& item, double wait_cap_s)
{
    nlohmann::json entry = {{"object_id", item.object_id},
                            {"object_name", item.object_name},
                            {"result", batch_result_name(item.result)},
                            {"changed", item.changed}};
    if (item.changed) {
        entry["repair"]            = repair_stats_json(*item.plan);
        entry["open_edges_before"] = item.before.value("open_edges", 0);
        entry["open_edges_after"]  = item.after.value("open_edges", 0);
        entry["mesh_warning_after"] = item.after.value("mesh_warning", false);
        for (const char* key : {"plate_index", "on_bed"})
            if (item.placement.contains(key))
                entry[key] = item.placement[key];
    }
    if (const std::string message = batch_repair_message(item, wait_cap_s); !message.empty())
        entry["message"] = message;
    return entry;
}

nlohmann::json batch_repair_answer_json(RepairScope scope, std::optional<int> plate_index, bool keep_painting,
                                        bool keep_painting_given, const std::vector<BatchRepairItem>& items, double wait_cap_s)
{
    nlohmann::json                  objects = nlohmann::json::array();
    std::vector<const CgalRepairPlan*> applied;
    bool                            any_changed = false;
    for (const BatchRepairItem& item : items) {
        objects.push_back(batch_repair_object_json(item, wait_cap_s));
        if (item.changed) {
            applied.push_back(item.plan);
            any_changed = true;
        }
    }
    nlohmann::json answer = {{"status", "success"}, {"changed", any_changed}, {"scope", scope == RepairScope::plate ? "plate" : "all_plates"}};
    if (scope == RepairScope::plate && plate_index)
        answer["plate_index"] = *plate_index;
    answer["keep_painting"]      = keep_painting;
    answer["keep_painting_from"] = keep_painting_given ? "argument" : "app_setting";
    answer["counts"]             = {{"repaired", object_ids_with(items, BatchRepairResult::repaired).size()},
                                    {"failed", object_ids_with(items, BatchRepairResult::failed).size() +
                                                   object_ids_with(items, BatchRepairResult::changed_meanwhile).size() +
                                                   object_ids_with(items, BatchRepairResult::deleted_meanwhile).size()},
                                    {"nothing_to_repair", object_ids_with(items, BatchRepairResult::nothing_to_repair).size()},
                                    {"timed_out", object_ids_with(items, BatchRepairResult::timed_out).size()}};
    answer["repair"]  = repair_stats_json(applied);
    answer["objects"] = std::move(objects);
    answer["message"] = batch_repair_summary(items, wait_cap_s);
    return answer;
}

}}} // namespace Slic3r::GUI::OrcaMCP
