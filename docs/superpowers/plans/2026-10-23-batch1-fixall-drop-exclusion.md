# Batch 1: Fix All for repair_mesh, drop_to_bed, 3D bed exclusion volumes

Planned 2026-10-23 (Opus), every upstream symbol checked against `mcp`. Decisions taken: (1) a repair of several
objects applies those planned in time and leaves the rest `timed_out`, as the app's Cancel applies what was planned;
(2) `plate_index` does not make the plate current; (3) exactly one of `object_id` / `plate_index` / `all_plates:
true`, so `object_id` is no longer `required`; (4) `drop_to_bed` is a new tool, `volume_id` with `instance_id`
refused, the follow-up drop only for the target object; (5) exclusion extruders 0-based, identical per-extruder
copies merged, the legacy area in `exclusion_regions` with `legacy: true`, `excluded_areas` unchanged.

Order: **B** (drop_to_bed), **A** (Fix All), **C** (exclusion volumes); one commit each, after its build, tests,
golden regeneration and docs.

```bash
cmake --build build/arm64 --config Release --target OrcaSlicer slic3rutils_tests -- -j 12
T=build/arm64/tests/slic3rutils/Release/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests   # run from build/arm64/tests/slic3rutils
ORCAMCP_UPDATE_TOOLS_GOLDEN=1 $T "[orcamcp][tools]"; $T "[orcamcp][tools]"
python3 -m unittest discover -s scripts/tests -t scripts
```

Rules for all three: every decision in a pure, tested function; the app side on the main thread inside
`run_on_main_thread`; every refusal before any change; one undo step per change, none for no change; `next_steps`
only through `OrcaMCPNextSteps.cpp` builders; no modal dialog under MCP -- never call `ObjectList::fix_all_through_cgal`
(it opens a `wxPD_APP_MODAL` ProgressDialog); agent-facing text never sends anyone to a GUI button (new tests check
"click", "Click", "the user", "GUI", "button", as `test_mesh_repair.cpp` does). The tool-name checker
(`tests/slic3rutils/mcp_tool_references.hpp`) flags snake_case words in tool text that begin like a tool name
(`auto_`, `get_`, `drop_`): never write `auto_drop` in tool text, write "its auto drop setting".

---

## B. `drop_to_bed` (new tool, Transforms)

**Upstream** (`src/slic3r/GUI/Selection.cpp:553-590`, commit 6a7763f08d): `Selection::get_drop_min_z()` -- Volume
mode: the selected volume's convex-hull box `min.z()` (modifiers too); Instance mode: min over the selected instances
of `ModelObject::get_instance_min_z` (model parts only). `can_drop()`: `std::abs(min_z) > -SINKING_Z_THRESHOLD`
(0.001 mm, up or down). `drop()`: `take_snapshot(L("Move object"))`, translate (0,0,-min_z), `do_move("")`, whose
follow-up drops every instance with `ModelObject::auto_drop` on and `get_instance_min_z > SINKING_Z_THRESHOLD && != 0`.
A whole object means all its instances, one shift. A sinking object is lifted whatever its auto drop. Entry points:
the object / part menus' Drop, the Move gizmo's button, `NativeCommands.cpp:368`. We drop only the target object's
instances afterwards (the GUI's do_move drops every floating auto-drop object in the scene).

**Pure, `OrcaMCPPartEdits.{hpp,cpp}`** (include `OrcaMCPInstanceEdits.hpp` in the .cpp):

```cpp
// Selection::can_drop: a lowest point off the bed by more than 0.001 mm (SINKING_Z_THRESHOLD), above or below.
bool can_drop_to_bed(double lowest_z);
// Selection::get_drop_min_z for `instances`: their lowest point, from the model parts (instance_min_z); 0 when none.
double instances_drop_min_z(const ModelObject& object, const std::vector<std::size_t>& instances);
// The same for volume `volume_idx` on instance 0: its own lowest point, a modifier's too (volume_world_box(...).min.z()).
double volume_drop_min_z(const ModelObject& object, std::size_t volume_idx);
// Why drop_to_bed cannot drop what it is asked: volume_id with instance_id (a volume drops on instance 0, as the
// volume transforms move it); then volume_transform_refusal for volume_id; then instance_id_error for instance_id.
std::optional<std::string> drop_refusal(int object_id, const ModelObject& object, std::optional<int> volume_id,
                                        std::optional<int> instance_id, const std::vector<int>& cut_siblings);
// Selection::drop then do_move: `instances` moved by -min_z along Z, then every instance of the object left floating
// dropped unless its auto drop is off (drop_after_volume_change, VolumeChange::move). Each instance's Z change.
std::vector<double> drop_instances_to_bed(ModelObject& object, const std::vector<std::size_t>& instances, double min_z);
```
`drop_instances_to_bed`: record each offset, `set_offset(offset - Vec3d(0,0,min_z))` for `instances`,
`object.invalidate_bounding_box()`, `drop_after_volume_change(object, {}, VolumeChange::move)`, return z after - before
per instance.

**App, `OrcaMCPPartTools.cpp`** (`after_volume_change`, `close_toolbar_tool_before_change`, `ui_job_refusal`,
`vec_json` are in scope). `drop_to_bed_on_main_thread(params)`:
1. `resolve_object_id`, `read_volume_id`, `read_instance_id`, `read_flag(params, "include_preview", ...)`;
   `drop_refusal(..., cut_siblings(model, object_id))`; `ui_job_refusal(*plater, "drop_to_bed")`.
2. `min_z` = `volume_drop_min_z`, or `instances_drop_min_z` over instance_id's instance or every instance.
3. `!can_drop_to_bed(min_z)`: `{status: success, changed: false, object_id, (volume_id|instance_id), lowest_z_mm,
   message: "Object N already rests on the bed (its lowest point is X mm from it), so nothing was moved"}` +
   `report_placement`; no undo step.
4. Volume form: `close_toolbar_tool_before_change`; `lowest_before = instances_lowest_z(*object)`;
   `plater->take_snapshot(L("Move object"))`; `translate_volume_in_plate_frame(*object, v, Vec3d(0,0,-min_z))`;
   `dropped = drop_after_volume_change(*object, lowest_before, VolumeChange::move)`; answer `volume_row_json`,
   `previous_position`, `moved_z_mm = -min_z`, `dropped_to_bed_mm = dropped[0]`; `after_volume_change(*plater, object_id, answer)`.
5. Object / instance form (no tool closed, as move_object): `previous_position` = `object_world_box` centre;
   `take_snapshot(L("Move object"))`; `drop_instances_to_bed`; `plater->update()`;
   `rehome_and_report_placement(answer, object_id, true)`; `ObjectList::update_info_items`,
   `update_plate_values_for_items`; answer instance 0's `position`, `rotation_degrees`, `scale` (as move_object),
   `lowest_z_before_mm`, `moved_z_mm`, `instances_moved: [{instance_id, z_change_mm}]` for nonzero changes.
6. `active_warnings`; `add_turntable_preview_if_requested(answer, include_preview)`.

**Registration** (`register_part_tools`, `ToolCategory::Transforms`). Summary: "Drop an object or part onto the bed".
Description: "Drop an object, one of its instances, or one of its volumes onto the bed, as the app's Drop does: the
lowest point goes to Z=0 along the plate's Z, down from above the bed or up from below it. An object's lowest point is
its parts' (modifiers, negative volumes, support blockers and enforcers do not count). The whole object (the default)
moves every instance together, by the lowest one's distance; then, as after any move, an instance still above the bed
comes down too, unless the object's auto drop setting is off. With instance_id only that instance moves. With volume_id
that volume (a part or modifier) moves within the object, on instance 0, until its own lowest point is at Z=0; then the
object drops as the app drops it after a part moves (dropped_to_bed_mm). One undo step; an object already resting on
the bed (within 0.001 mm) is changed: false, with no undo step. Answers previous_position, position, moved_z_mm and the
placement fields (instance_placement, plate_index, plate_indices, on_bed). A one-volume object has no part to drop on
its own (leave volume_id out), and a cut object's solid parts wait for invalidate_cut_info. Refused while an arrange,
orient or bed fill runs."
Schema: `object_id` (integer, min 0, "Object index (0-based)", required), `instance_id` (integer, min 0, "Drop only
this instance, as get_object_info's instance_placement lists them. Left out: every instance"), `volume_id` (integer,
min 0, "Drop this volume (as get_object_info lists them) within the object instead, on instance 0. Not with
instance_id"), `include_preview` (boolean, "Return turntable preview path").

**Tests** `tests/slic3rutils/test_part_edits.cpp`, `[PartEdits][orcamcp]` (helpers `cube_at`, `two_part_object`,
`add_part`, `check_vec`): a floating object drops and one resting (0.0005) does not; a sinking object is lifted
whatever its auto drop; instances at -2 and +4: auto drop on -> both 0, off -> 0 and 6; instance_id drops one
instance only; a modifier below the part does not change `instances_drop_min_z`; a raised part drops within the
object and a modifier's drop leaves the object where it is; `drop_refusal` for volume_id+instance_id, a one-volume
object's volume, a cut's part, a missing instance. Run `$T "[PartEdits],[McpToolArguments],[orcamcp][tools]"` and the
Python tests.

**Docs B**: golden (107 registered / 103 app); `docs/tools/reference.md` new `### drop_to_bed` after `### move_object`
(parameters, examples and responses for the object and volume forms); `OrcaMCPServerInfo.cpp` `tool_examples()` entry
`{"drop_to_bed", {{"example", R"({"object_id": 0})"}, {"when_to_use", "An object left floating or sunk: its lowest
point to Z=0"}}}` and `"drop_to_bed"` in `visual_preview.supported_tools`; counts 102->103 / 106->107 in CLAUDE.md
(tool section header and text), README.md, docs/README.md, docs/setup/configuration.md (grep for "102" and "106");
README tool table: `drop_to_bed` in Transforms by name order; CLAUDE.md Transforms row: "`drop_to_bed` (the app's
Drop: the lowest point -- the parts', for an object -- to Z=0, up or down; the whole object's instances together by the
lowest, then a floating one dropped unless its auto drop is off; `instance_id` one instance; `volume_id` the volume
within the object on instance 0, then the object's drop (`dropped_to_bed_mm`); one undo step "Move object", none when
resting already; only the target object's instances are dropped after, where the GUI's move drops every floating
object)".

---

## A. Fix All for `repair_mesh`

**Upstream** `ObjectList::fix_all_through_cgal(bool all_plates)` (`GUI_ObjectList.cpp:6357`): Fix All = the selected
plate's `get_objects_on_this_plate()` as sorted unique indices; Fix All Plates = every object 0..n-1 (on no plate and
on locked plates too); no lock check. Then `fix_through_cgal(obj_idxs, {})`: capture all, plan one after another under
one dialog (a Cancel stops the loop; those planned are still applied), one `TakeSnapshot("Repairing model object")`,
per object `cgal_repair_refusal` (counts as failed) then `apply_cgal_repair` and, when changed, `ensure_on_bed`,
`changed_mesh`, list updates. Counts: repaired (no error, changed), failed (any plan error or refusal, even with some
volumes applied), needed no repair.

**Arguments**: keep `object_id`, `volume_id`, `keep_painting`; add `plate_index` (integer, min 0) and `all_plates`
(boolean). Exactly one of `object_id`, `plate_index`, `all_plates: true`; `volume_id` only with `object_id`. Remove
`required` from the schema (the handler refuses instead); `[McpToolArguments]` must stay green.

**Pure, `OrcaMCPMeshRepair.{hpp,cpp}`**:
```cpp
enum class RepairScope { object, plate, all_plates };
std::optional<std::string> repair_scope_error(bool object_given, bool volume_given, bool plate_given, bool all_plates);
std::vector<int> repair_order(std::vector<int> object_indices);           // ascending, unique
enum class BatchRepairResult { repaired, nothing_to_repair, failed, changed_meanwhile, timed_out };
BatchRepairResult batch_repair_result(const CgalRepairPlan* plan, bool planned_in_time, bool unchanged_in_app);
struct BatchRepairItem {
    int object_id = -1; std::string object_name; nlohmann::json before, after, placement;
    BatchRepairResult result = BatchRepairResult::timed_out; bool changed = false; const CgalRepairPlan* plan = nullptr;
};
nlohmann::json batch_repair_object_json(const BatchRepairItem& item, double wait_cap_s);
nlohmann::json batch_repair_answer_json(RepairScope scope, std::optional<int> plate_index, bool keep_painting,
                                        bool keep_painting_given, const std::vector<BatchRepairItem>& items, double wait_cap_s);
```
Messages of `repair_scope_error`: none given (also `all_plates: false` alone): "Give object_id to repair one object,
plate_index to repair every object on that plate, or all_plates: true to repair every object in the project";
several: "Give one of object_id, plate_index and all_plates, not several"; volume_id without object_id: "volume_id names
a volume of object_id's object: give object_id with it, or leave volume_id out to repair whole objects".
`batch_repair_result`: no plan / `!planned_in_time` / `plan->canceled` -> timed_out; `!unchanged_in_app` ->
changed_meanwhile; `changes_nothing()` -> nothing_to_repair if `error` empty else failed; otherwise repaired if `error`
empty else failed (applied, changed true).
Object JSON: `{object_id, object_name, result, changed, repair (repair_stats_json, when changed), open_edges_before,
open_edges_after and mesh_warning_after (when changed), plate_index and on_bed (from placement, when changed), message
(every result but a clean repaired)}`. Messages: nothing_to_repair -> `repair_step_message(RepairStep::nothing_to_repair,
...)`; failed unchanged -> `repair_step_message(RepairStep::failed, ...)`; failed changed ->
`unrepaired_volumes_message(plan)`; changed_meanwhile -> "Object N changed in the app while it was being repaired, so
nothing was changed on it: call repair_mesh with object_id N again"; timed_out -> "Object N was not repaired within this
call's wait (W s), so nothing was changed on it. That wait is set a little under the MCP bridge's ORCAMCP_TIMEOUT;
repair_mesh with object_id N repairs it on its own".
Answer: `{status: success, changed, scope: "plate"|"all_plates", plate_index (plate scope only), keep_painting,
keep_painting_from, counts: {repaired, failed (changed_meanwhile included), nothing_to_repair, timed_out}, repair (summed
over applied plans: a helper beside repair_stats_json summing totals() and volumes_changed(); repair_stats_json calls it
with one plan), objects (repair order), message ("Repaired objects 0 and 4. Failed: object 2 ("x"): <why>. 3 objects
needed no repair.")}`.

**Next step, `OrcaMCPNextSteps.{hpp,cpp}`**: `std::vector<NextStep> repair_timed_out_next_steps(const std::vector<int>&
timed_out);` -> `{"repair_mesh", "objects " + listed_ids(timed_out) + " were not repaired within the call's wait:
repair_mesh with object_id repairs one at a time", {{"object_id", timed_out.front()}}}`; nothing for an empty list.

**App, `OrcaMCPMeshTools.cpp`** (include `OrcaMCPPartEdits.hpp` for `read_flag`, `OrcaMCPPlateSettings.hpp` for
`read_plate_index` / `plate_index_error`, `OrcaMCPNextSteps.hpp`). `repair_mesh_tool`: read `plate_index`
(`read_plate_index`), `all_plates` (`read_flag`), `repair_scope_error(...)`; object scope -> existing
`start_repair`/`finish_repair` unchanged; otherwise `repair_objects_tool`:
1. Main thread (`start_batch`): `keep_painting` as `start_repair`; `app_repair_refusal`; plate scope:
   `plate_index_error(i, plates.get_plate_count())`, indices = `repair_order` of `model_object_index(o)` for
   `o` in `objects_on_plate(*plates.get_plate(i))` (empty -> "Plate N has no objects to repair"); all plates: 0..n-1
   (empty -> "The project has no objects to repair"); per index `item.before = repair_facts_json(*obj)`, name,
   `inputs.push_back(capture_cgal_repair(*obj, -1, keep_painting))`.
2. HTTP thread, one deadline `Clock::now() + tool_wait_cap()`: for each input, stop when `this_thread_cancelled()` or
   past the deadline; `plans[i] = plan_cgal_repair(std::move(inputs[i]), {}, [deadline]{ return
   this_thread_cancelled() || Clock::now() > deadline; })`; `in_time[i] = !plans[i]->canceled && Clock::now() <=
   deadline`; break when not in time. (`std::vector<std::optional<CgalRepairPlan>>`.)
3. Main thread (`finish_batch`): `McpDialogSuppressionGuard guard`; `app_repair_refusal`; per planned item:
   `idx = cgal_repair_object_index(model, plan)`, `unchanged = idx >= 0 && plan.applies_to(*model.objects[idx])`,
   `item.object_id = idx >= 0 ? idx : item.object_id`, `item.result = batch_repair_result(...)`; to apply: repaired,
   or failed with `!changes_nothing()`. If any: `close_open_toolbar_tool(*plater)`; still open -> error "...did not
   close, so nothing was repaired" via `with_closed_tool`; otherwise re-check `applies_to` (failures ->
   changed_meanwhile). If any remain, in one scope `Plater::TakeSnapshot snapshot(plater, _u8L("Repairing model
   object"))`, per item `apply_cgal_repair`, and when `volumes_changed > 0`: `changed = true`,
   `update_after_repair(*plater, idx, -1, item.placement)`, `item.after = repair_facts_json(*obj)`, note painting
   removed (painted before, not after). `plater->notify_painting_removed()` once if any. Answer:
   `batch_repair_answer_json`, `add_next_steps(answer, repair_timed_out_next_steps(...))`, `active_warnings`,
   `guard.report(with_closed_tool(answer, closed_tool))`. No undo step when nothing applied.

**Registration**: summary unchanged. Schema adds `plate_index` ("Repair every object with an instance on this plate
(0-based, get_scene_info's plate_index), as the plate's Fix All does. Instead of object_id; the current plate stays as
it is") and `all_plates` ("true: repair every object in the project -- on every plate, locked ones included, and on
none -- as Fix All Plates does. Instead of object_id"); `object_id`: "Object index (0-based): repair this one object.
Give object_id, plate_index or all_plates"; `volume_id` ends "Only with object_id". Description, after "...One undo
step.": "Give exactly one of object_id (one object; volume_id for one of its volumes), plate_index (every object with an
instance on that plate, as the plate's Fix All) or all_plates: true (every object in the project, as Fix All Plates;
locked plates are not skipped, as the app does not skip them). For several objects: they are repaired one after another
within this call's one wait; those finished in time are applied together in one undo step, and the rest are left as
they are. The answer's `objects` gives each one's result -- repaired, nothing_to_repair, failed (some volume no solid
can be built from; the others are still repaired), changed_meanwhile (changed in the app during the repair: left as it
is) or timed_out (next_steps names repair_mesh for it) -- `counts` as the app counts them, and `repair` summed."

**Tests** `test_mesh_repair.cpp` `[MeshRepair][orcamcp]` (helpers `planned()`, `OnePartObject`, `cube_missing_facet()`,
`its_make_cube`, `flat_square()`, `cube_with(flat_square())`): every argument combination of `repair_scope_error`;
`repair_order({5,2,5,0}) == {0,2,5}`; `batch_repair_result` for hole (repaired), clean (nothing_to_repair), sheet
(failed), a partly failed plan (failed), canceled / null / not in time (timed_out), changed in the app
(changed_meanwhile); the answer's counts, order, summed `volumes_repaired`, failed counting changed_meanwhile, the
timed-out message naming "105 s" and "ORCAMCP_TIMEOUT", no GUI words. `test_mcp_next_steps.cpp` `[McpNextSteps][orcamcp]`:
one step, `arguments == {{"object_id",4}}` for `{4,7}`, `why` mentions "4 and 7", `unusable(step) == std::nullopt`;
empty list -> none. Run `$T "[MeshRepair],[McpNextSteps],[McpToolArguments],[orcamcp][tools]"`.

**Docs A**: golden; reference `### repair_mesh`: parameters (`object_id` one of three, `plate_index`, `all_plates`,
`volume_id` needs `object_id`), a "Several objects (Fix All)" paragraph and a plate-scope response example (three
objects: repaired, nothing_to_repair, failed); next-steps table row "`repair_mesh` (plate_index / all_plates) |
`repair_mesh` with the first timed-out `object_id` | the wait ran out before some objects were repaired"; CLAUDE.md
Models row `repair_mesh`: "`plate_index` / `all_plates` repair every object of a plate or of the project, as the plate
menu's Fix All / Fix All Plates (locked plates included; the current plate unchanged): planned one after another within
the one wait cap, those finished applied under one undo step, per-object `result` and `counts` as the app's notification
counts them, `next_steps` for objects not reached"; CLAUDE.md "Mesh repair": a "repair_mesh of several objects"
paragraph summarising steps 1-3.

---

## C. 3D bed exclusion volumes in `get_scene_info` and `render_plate_view`

**Upstream** (merge f92f75e0fa): keys `bed_exclude_area` (legacy points, a material keep-out), `bed_exclude_volumes`
(string `ZMIN..ZMAX;XxY,...|...`), `bed_exclude_volume_mode` (`shared` / `toolhead_offset` / `per_extruder`),
`extruder_bed_exclude_volumes` (strings per extruder). Resolver (`src/libslic3r/PrintConfig.hpp:2543-2578`):
`struct BedExcludeRegion { Polygon polygon; double z_min, z_max; Purpose purpose; bool has_z_range; bool
is_collision_volume() const; }`; `get_bed_excluded_regions_by_extruder(const DynamicPrintConfig&)` -> one group per
physical extruder (`max(nozzle_diameter, extruder_offset, extruder_bed_exclude_volumes)` groups), plate-local scaled
coordinates; the legacy area is one polygon of every legacy point, z 0..printable_height, `has_z_range=false`, in every
group; toolhead-offset mode translates collision volumes per extruder; per-extruder mode appends each extruder's own.
`active_bed_exclude_volume_mode(cfg)`. `bed_exclusion_z_ranges_overlap(a_min, a_max, b_min, b_max)` in
`src/libslic3r/ExclusionVolumeGeometry.hpp`. The GUI's dedupe (`PartPlate::calc_triangles`, PartPlate.cpp:2147-2165):
shared mode -> group 0 only; otherwise the legacy area from group 0 only and every group's collision volumes; it reads
`m_plater->config()` and translates by the plate's `m_origin`. `PartPlate::get_exclude_areas()` is still the legacy
area only, as world boxes of 4 points (PartPlate.cpp:628): `excluded_areas` keeps it unchanged.

**Pure, new `src/slic3r/GUI/OrcaMCP/OrcaMCPBedExclusion.{hpp,cpp}`** (no wx; add to `src/slic3r/CMakeLists.txt`
beside `OrcaMCPPlateOccupancy`):
```cpp
struct ExclusionRegion {                     // one region as the 3D view draws it, in plate mm (origin added)
    std::vector<Vec2d> outline;              // counter-clockwise
    BoundingBoxf       footprint;
    double             z_min_mm = 0., z_max_mm = 0.;
    bool               height_limited = false; // BedExcludeRegion::has_z_range
    bool               legacy = false;         // bed_exclude_area
    std::string        source;                 // "bed_exclude_area" | "bed_exclude_volumes" | "extruder_bed_exclude_volumes"
    std::vector<int>   extruders;              // 0-based physical extruders
    bool               all_extruders = true;
};
std::vector<ExclusionRegion> resolve_exclusion_regions(const std::vector<std::vector<BedExcludeRegion>>& by_extruder,
                                                       BedExcludeVolumeMode mode, const Vec2d& plate_origin);
bool outline_is_rectangle(const std::vector<Vec2d>& outline);   // axis-aligned, 4 corners
nlohmann::json rect_json(const BoundingBoxf& box);              // min_x,min_y,max_x,max_y,size_x,size_y
nlohmann::json exclusion_region_json(const ExclusionRegion& r);
nlohmann::json exclusion_occupancy_json(const ExclusionRegion& r); // collision volumes only
struct ExcludedRegionOverlay { std::vector<Vec2d> outline; double z_min = 0., z_max = 0.; bool height_limited = false; bool active = true; };
std::vector<ExcludedRegionOverlay> exclusion_overlays(const std::vector<BoundingBoxf3>& legacy_boxes,
                                                      const std::vector<ExclusionRegion>& regions,
                                                      std::optional<std::pair<double, double>> slab);
```
`resolve_exclusion_regions` follows the GUI's dedupe; regions identical in outline and Z across groups merge, with every
extruder they apply to. `source` = legacy ? "bed_exclude_area" : mode == PerExtruder ? "extruder_bed_exclude_volumes" :
"bed_exclude_volumes"; `all_extruders` = `extruders.size() == by_extruder.size()`; shared mode and legacy regions list
every extruder; unscale with `unscale<double>(p.x())` and add `plate_origin`. Point `OrcaMCPPlateUtils.cpp`'s anonymous
`rect_to_json` at `OrcaMCP::rect_json` (one rectangle shape). `exclusion_overlays`: legacy boxes always active; every
collision volume; inside a slab only those whose Z range meets it are active.
`exclusion_region_json`: `{kind: "collision_volume"|"material_keepout", legacy, source, outline: [[x,y],...],
footprint, z_min_mm, z_max_mm, height_limited, extruders, all_extruders}`. `exclusion_occupancy_json`: `{kind:
"exclusion_volume", name: "Collision volume", footprint, includes_brim: false, footprint_is_exact:
outline_is_rectangle(outline), z_min_mm, z_max_mm, extruders}`.

**App**: `OrcaMCPPlateUtils.{hpp,cpp}`: `std::vector<OrcaMCP::ExclusionRegion> plate_exclusion_regions(PartPlate&
plate)` from `wxGetApp().plater()->config()` (empty when null), `get_bed_excluded_regions_by_extruder(*cfg)`,
`active_bed_exclude_volume_mode(*cfg)`, `plate.get_origin()`. `PlateEntry` gets `nlohmann::json exclusion_regions =
json::array();`, `PlateJson` writes `{"exclusion_regions", ...}` after `excluded_areas`. `GetPlates` (~line 1162),
after the `excluded_areas` loop: push every region into `entry.exclusion_regions`, and `exclusion_occupancy_json` of each
non-legacy one into `occupancy`. `set_prime_tower_position` conflicts (`OrcaMCPServer.cpp` ~4580): each non-legacy
region where `footprints_overlap(after.footprint, r.footprint)` and `bed_exclusion_z_ranges_overlap(r.z_min_mm,
r.z_max_mm, 0., after.size.z())` -> `{kind: "exclusion_volume", name: "Collision volume"}`.
Render: `draw_overlays`' `const std::vector<BoundingBoxf3>& excluded_areas` becomes `const
std::vector<ExcludedRegionOverlay>& excluded` (`OrcaMCPRenderOverlay.{hpp,cpp}`, include `OrcaMCPBedExclusion.hpp`). In
`if (options.excluded)`: base face at `height_limited ? z_min : plate z`; active -> the existing hatched brush and pen;
inactive -> `*wxTRANSPARENT_BRUSH`, `wxPen(wxColour(200,40,40,200), 1, wxPENSTYLE_SHORT_DASH)`; height_limited -> also
the top outline at z_max and each vertical edge (`paint.line`, same pen). Callers: `OrcaMCPPlateUtils.cpp` ~447/525
(3D views): `exclusion_overlays(plate->get_exclude_areas(), plate_exclusion_regions(*plate), std::nullopt)`;
`PlanTarget`: `excluded_areas` -> `std::vector<BoundingBoxf3> legacy_boxes; std::vector<OrcaMCP::ExclusionRegion>
exclusion_regions;`; `render_first_layer_view` slab `{0., initial_layer_print_height}`; `render_sliced_layer_view` slab
`{previous layer z or 0, layer z}`; `draw_first_layer_plan` (FirstLayerPlan.hpp:116/.cpp:302) and `draw_layer_plan`
(LayerPlan.hpp:277/.cpp:734) take `const std::vector<ExcludedRegionOverlay>&`.
Text: get_scene_info description "...and the printer's excluded bed areas and collision volumes (exclusion_regions: each
one's outline, Z range and the extruders it applies to)"; render_plate_view `overlays` "hatched excluded areas and
collision volumes (a height-limited one drawn at its height; in a layer plan, dashed when not at that layer)"; ServerInfo
`concepts.coordinate_system.find_free_space`: "...the prime tower, excluded bed areas and collision volumes (an
exclusion_volume entry's z_min_mm/z_max_mm: an object may pass above or below one)".

**Tests**: new `tests/slic3rutils/test_mcp_bed_exclusion.cpp` `[McpBedExclusion][orcamcp]` (add to
`tests/slic3rutils/CMakeLists.txt`), from `DynamicPrintConfig::full_print_config()` with `printable_height` 250,
`bed_exclude_area` "0x0,20x0,20x20,0x20", `bed_exclude_volumes` "30x30,40x30,40x40,30x40|5..15;100x100,120x100,120x120,100x120",
two `nozzle_diameter` and `extruder_offset` entries, plate origin (300,0), through the real
`get_bed_excluded_regions_by_extruder` / `active_bed_exclude_volume_mode`: shared -> three regions offset by the origin
(legacy 0..250 not height limited, full-height volume 0..250, limited 5..15 height_limited, all `[0,1]` and
all_extruders); toolhead_offset with offsets (0,0),(25,0) -> legacy once, each volume per extruder, extruder 1's shifted
25 mm in X, one extruder each; per_extruder with `["", "10..20;50x50,60x50,60x60,50x60"]` -> legacy + one volume `[1]`,
source extruder_bed_exclude_volumes; identical per-extruder definitions merge into one `[0,1]`; only the default
`bed_exclude_area` "0x0" -> no regions; occupancy `footprint_is_exact` true for a rectangle, false for a triangle, kind
exclusion_volume; `exclusion_overlays`: legacy always active, slab {0,0.2} -> the 5..15 volume inactive and the
full-height one active, no slab -> all active. `test_mcp_scene_description.cpp`: `check_keys("plates[0]", ...)` will
require `exclusion_regions` in the example; add "The reference's exclusion region example has the keys the builders
write", reading `reference_example("<!-- exclusion region example")` (`{"exclusion_regions": [one], "occupancy": [one
exclusion_volume]}`) and checking keys against `exclusion_region_json` / `exclusion_occupancy_json` of a sample. Run
`$T "[McpBedExclusion],[McpSceneDescription],[plate_occupancy],[ExclusionVolume],[orcamcp][tools]"`.

**Docs C**: golden; reference get_scene_info example `"exclusion_regions": [],` after `"excluded_areas": [],`; a paragraph
on `exclusion_regions` followed by `<!-- exclusion region example: test_mcp_scene_description.cpp holds its keys to what
the response writes -->` and its JSON block; occupancy table row "`exclusion_volume` | A toolhead collision volume
(`bed_exclude_volumes`, or per extruder `extruder_bed_exclude_volumes`): its footprint's bounding rectangle,
`z_min_mm`/`z_max_mm` (an object may pass under or over a height-limited one), `extruders` (0-based) it applies to";
render_plate_view `overlays` row; set_prime_tower_position conflicts. CLAUDE.md Scene row: "excluded bed areas" ->
"excluded bed areas and `exclusion_regions` (every exclusion region as the 3D view resolves it, upstream's
`get_bed_excluded_regions_by_extruder`: outline, footprint, Z range, `extruders`, `legacy`; collision volumes also in
`occupancy` as `exclusion_volume`)"; Visualization row: the overlay text.
