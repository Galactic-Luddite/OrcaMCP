# Texture displacement through MCP: the whole feature

Planned 2026-10-24 (Opus) against `mcp` at 4028fc367e; every upstream symbol named here was read on that commit. The
user's scope: everything the texture displacement panel does, in one increment, plus model UVs at load and the
existing paint and mesh-edit tools taught about texture paint. User decisions already taken: (1) `paint_object` closes
an open paint panel in every mode; (2) an image import reuses an identical texture already in the user's library
(identity by decoded content, below) and the answer says which happened.

## Contents

- What the code actually does (corrections to the brief)
- Rules for every task
- Order, parallel groups, hours
- T0 upstream moves: the bake, prepare and panel logic as functions over the model
- T1 the pure core, T2 the paint channel
- T3 paint_object texture mode and `visible`, T4 library tools, T5 set_texture_displacement, T6 get_texture_displacement
- T7 the unwrap tools, T8 bake_texture_displacement, T9 prepare_texture_mesh
- T10 the other paint tools, T11 texture state in scene and object info, T12 mesh edits report what they drop
- T13 textured OBJ UVs and the project round trip
- T14 docs and counts, T15 live check
- Risks, decisions for the user

## Commands

```bash
cd /Users/hanan/Projects/OrcaMCP
cmake --build build/arm64 --config RelWithDebInfo --target slic3rutils_tests -- -j 12
cmake --build build/arm64 --config RelWithDebInfo --target OrcaSlicer -- -j 12          # every app-side task
T=/Users/hanan/Projects/OrcaMCP/build/arm64/tests/slic3rutils/RelWithDebInfo/slic3rutils_tests.app/Contents/MacOS/slic3rutils_tests
(cd build/arm64/tests/slic3rutils && $T "[McpTexture]")                                  # each task names its tags
(cd build/arm64/tests/slic3rutils && ORCAMCP_UPDATE_TOOLS_GOLDEN=1 $T "[orcamcp][tools]" && $T "[orcamcp][tools]")
ORCAMCP_PORT=1 python3 -m unittest discover -s scripts/tests -t scripts
# T0 also: the upstream texture suites stay green
cmake --build build/arm64 --config RelWithDebInfo --target libslic3r_tests fff_print_tests -- -j 12
(cd build/arm64/tests/libslic3r && ./RelWithDebInfo/libslic3r_tests "[TextureDisplacement],[3mf]")
(cd build/arm64/tests/slic3rutils && $T "[TextureDisplacement],[TextureColorPalette],[TextureIslandView]")
```

RelWithDebInfo is -O0 here (upstream CMake): fine for tests, slow for bakes and slices (CGAL and the refinement are
header code). A new `.cpp` goes into `src/slic3r/CMakeLists.txt` (OrcaMCP files beside `GUI/OrcaMCP/OrcaMCPBedExclusion.*`
~line 504; a GUI file beside `GUI/TextureLibrary.*`) or `tests/slic3rutils/CMakeLists.txt`; the build reconfigures.

## What the code actually does (corrections to the brief)

1. **The layer stack belongs to the object.** The panel keeps it on the object's *first model part*
   (`GLGizmoTextureDisplacement::texture_volume()`, gizmo .cpp:3787). Every model part has its own eight masks
   (`ModelVolume::texture_displacement_facet(slot)`, Model.hpp:928-949) and is displaced by the owner's layers
   (`layers_for_part`, .cpp:3832, which strips the unwrap's island data for parts other than the owner). A stack on any
   other part (after `set_volume_type` made the first part a modifier, or `assemble_objects` of two textured objects) is
   ignored by the panel and by the bake. So layer tools take `object_id`; mask tools take `volume_id`.
2. **Layers are addressed by slot** (0..7, `TEXTURE_DISPLACEMENT_MAX_LAYERS`), assigned once and keying the masks. The
   panel lists layers by ascending slot; the lowest is the base layer, always Add. Reordering swaps slots and masks on
   every part (`move_texture_layer` / `swap_layer_slots`, .cpp:5178-5250). MCP's `layer` is the slot.
3. **Six projections:** `Triplanar`, `Cylindrical`, `Spherical`, `LSCM` (Unwrap), `ViewProjected` (From view: a planar
   projector from the camera's axes, `capture_view_projection` .cpp:3141, or a perspective projection frame,
   `apply_projection_frame` .cpp:3224), `ModelUV` (only a part with a UV channel, `has_texture_uv()`).
4. **Settings the brief left out:** blend (Add, Override, Subtract, Multiply, Divide; the combo's order), midlevel (0..10),
   smoothing (0..1), edge fade (and amount 0.02..1), invert, offset (placement, in texture repeats), tile on/off, Repeat /
   Mirrored repeat. Ranges: depth 0.01..10 mm, tile size 0.2..200 mm, rotation 0..360.
5. **Colour is the texture's own colours**, matched to the loaded filaments when baking; a bake can add mixed filament
   slots (`TextureDisplacementBakeJob::finalize` -> `Sidebar::ensure_mixed_filament`). Only a colour image can (the
   checkbox is disabled otherwise). Stack-wide colour options: Mix filaments, Mixed colors (1..32 minus physical slots),
   Denoise (0..6).
6. **Images are greyscale or colour PNGs** (`decode_height_texture`, TextureDisplacement.cpp ~306: grey PNG = height,
   any other = colour with height = luminance). Six of the 44 built-ins are RGB (Camouflage, Colour Bricks, Hex Tiles,
   Mosaic Tiles, Tartan, Terrazzo); comments saying the library is grayscale are stale. Imports keep colour and are
   copied into `<data_dir>/textures/displacement/`, uniquified, never deduplicated by the panel.
7. **Library names collide** (stem of the file name; built-in, user and pack can share one; `a.png` and `a.jpg` too).
   MCP passes ids.
8. **The slicer never reads any of it.** Only the bake turns layers into geometry. A bake replaces each painted part's
   mesh, carries the four paint channels across (`transfer_facet_paint` on the default pipeline), writes the colour
   result into colour paint, clears the masks it consumed (all of them on the default pipeline), resamples the UV
   channel, keeps the layers, then `Plater::changed_object` (which `ensure_on_bed`s the object). It refuses to commit a
   part the bake did not move, keeping its paint ("The bake produced no displacement..."). Its undo step is
   "Bake texture displacement"; the mixed filament slots it adds are not undone with it (slot changes are no undo step).
9. **Two pipelines.** Default (`options.pipeline_v2`, true): refine, displace, simplify in one run, with Resolution
   (`v2_refine_mm`, 0 = auto, 0.02..2) and Budget (`v2_max_triangles_k`, -1 = auto, 0..4000). Classic (false; the panel
   hides its toggle, `SHOW_PIPELINE_DEV_CONTROLS = false`, but a project can carry it): Standard's Bake runs a prepare
   (remesh 1 mm, adaptive subdivision with the `STD_*` constants, step cut, budget `m_subdivide_budget_k`, default 750,
   10..2000) then the topology-preserving bake, under one snapshot (`bake_standard`, `queue_prepare`). Pro mode adds
   Remesh, Subdivide (uniform 0..5 steps, or adaptive), Smooth baked mesh now, Displace up to the border, Smooth result
   (strength 1..100 %, passes 1..10, ignore outer ring). Standard pins `displace_border` true and `smooth_enabled` false.
10. **The panel's undo steps:** add, remove, reorder, texture change, seams, island edits, projection frame and every
    mesh operation take a `GizmoAction` snapshot. **Slider and checkbox edits take none**; they only rebuild the panel's
    preview. Nothing is invalidated: no plate, no object list row (there is no texture info item). MCP takes one undo
    step per call, per the project rule.
11. **Gizmo state the model does not hold**, and where each piece lives (T0 moves what a tool must reuse):
    - the active slot's masks in `m_triangle_selectors`, flushed back on every stroke end, layer switch, reorder and bake
      (`update_model_object`, .cpp:3879): MCP closes an open toolbar tool before any change here;
    - the bake's per-part inputs: `painted_parts()` (model parts with any mask), `layers_for_part()`, `texture_anchor()`
      (the owner's world translation), `resting_drops()` (each part's bottom above the object's lowest part),
      `prepare_texture_frame()` (identity for one part, else the bake frame) - members over `m_c->selection_info()`;
    - the colour palette: `cached_palette()` (.cpp:5718) from `filament_palette()` (physical filaments), the colouring
      layers' images (`mix_targets`), `color_mix_enabled` / `color_mix_count`, the free slot count
      (32 - `filament_presets.size()`) and the project's mixed slots (`find_fixed_mixed_filament`), cached in
      `m_palette_*`; `color_settings_for()` adds `palette_pure` and `color_despeckle`;
    - the classic budget `m_subdivide_budget_k` and the Pro remesh/subdivide sliders (`m_remesh_*`, `m_subdivide_*`):
      panel members only, defaults in the header;
    - the unwrap: `m_uv_editor_unwrap`, computed by `compute_patch_unwrap(extract_painted_patch(owner mesh, mask),
      seam angle, padding, seams)` (pure, cached under a mutex in libslic3r); chart ids are only meaningful against it;
    - "From view" reads the camera through the *selected* instance; the bake projects in instance 0's frame
      (`texture_displacement_volume_to_world`). MCP uses instance 0;
    - a new layer: first free slot, ModelUV when the owner has UV, the first library texture, the panel's active slot
      moved to it; the panel adds a layer by itself when opened on an object without one;
    - image adjustments (`preview_adjust`, `m_adjust`): never saved; "Save as copy" writes a new user texture
      (`commit_image_adjust`, .cpp:4960);
    - picking a user texture moves it to the front of the user list (`touch_user_texture`).
12. **Texture paint is dropped by every mesh edit.** `ModelVolume::restore_painting` (Model.cpp:2179) starts with
    `reset_extra_facets()` (all eight texture masks and the UV channel) and remaps only the four paint channels; so
    split, cut, merge, repair (our `apply_cgal_repair`), reload and replace lose texture paint with "keep painting" on or
    off. Reload and replace build a new volume from the file and copy neither the layers nor the masks
    (Plater.cpp:11960-11990): reloading the owner part loses the whole stack. Upstream keeps none of it; MCP reports it.
13. **Model UVs come only from an OBJ with an MTL** (`Model::read_from_file`, Model.cpp:358-376: `obj_info.uvs` fills
    only for faces naming a known material). GLB, GLTF and FBX UVs go to the colour importer (`model.texture_mesh`) and
    are not kept. Under MCP the colour import is answered Skip, which leaves the UV channel on the volume. **A 3MF does
    not store the UV channel** (bbs_3mf.cpp has no `texture_uv`): a ModelUV layer falls back to Triplanar after a save
    and reload.
14. **Masks can go stale unseen:** a mask indexing triangles a mesh no longer has; the panel treats it as unreadable
    (`update_from_model_object`). MCP reports `stale`.

## Rules for every task

- Every decision in a pure function, unit-tested in `tests/slic3rutils` without the app (tests/AGENTS.md: plain
  present-tense names, PascalCase tag, `WithinAbs` for floats, no `&&` in an assertion). App code on the main thread
  inside `run_on_main_thread`; mesh work (coverage, unwrap, bake, remesh) on the HTTP thread from captured copies.
- Long work follows `repair_mesh` exactly (CLAUDE.md "Mesh repair"; `OrcaMCPMeshTools.cpp` `start_repair` /
  `finish_repair`, `FixModelByCgal.hpp` capture / plan / apply): capture on the main thread; plan on the HTTP thread,
  pure, stopping when `this_thread_cancelled()` or past `tool_wait_cap()` (OrcaMCPUiJob.hpp); apply on the main thread
  after finding the object again by pointer and id and checking its inputs unchanged; nothing applied past the cap or
  after a change; an open toolbar tool closed only when there is something to apply, then re-checked.
- Every refusal before any change, closing a toolbar tool included. One undo step per call that changes something, with
  the panel's own undo names; none for a call that changes nothing (`changed: false`).
- Refused: a value the panel cannot set (colour on a grayscale texture, Model UV without UV), out of the panel's
  range, an unknown name, a wrong type. Accepted, stored and listed in `inactive_settings`: what the panel greys out but
  stores (tile size under Model UV, blend on the base layer, edge fade amount with fade off, tile method with tile off,
  colour options with no layer colouring, placement under a projection frame).
- `next_steps` only from builders in `OrcaMCPNextSteps.cpp`, each with a case in `test_mcp_next_steps.cpp`. Agent text
  never names a GUI button, "click" or "the user". The tool-name checker (`mcp_tool_references.hpp`) flags a snake_case
  word in tool text whose first word begins a tool name unless it is a parameter: response fields named in descriptions
  must not start with `get_`, `set_`, `paint_`, `bake_`, `delete_`, ... (`needs_bake`, `coverage_percent`, not
  `bake_pending`, `paint_coverage`).
- No modal dialog. `wxImage::LoadFile` reports a bad file through `wxLogError`, which pops a log window at idle: every
  image read under MCP runs inside a `wxLogNull` and a `McpDialogSuppressionGuard`. Never call the panel's members.
- Upstream files change only in T0 (and T13 only if the user says so), each change a move with a probe line in
  CLAUDE.md "Carried upstream fixes" (items **BW**, **BX**). Upstream tests stay green.
- Docs: each tool task writes its `docs/tools/reference.md` entry and `tool_examples()` entry; CLAUDE.md, README and the
  counts are T14's, so tasks that run in parallel never edit the same doc.

## Order, parallel groups, hours

| Task | What | Hours | Depends on | Files it alone edits |
|------|------|-------|------------|----------------------|
| T0 | Upstream moves (BW, BX) | 7 | - | Gizmos/GLGizmoTextureDisplacement.*, Jobs/TextureDisplacement{Bake,Prepare}Job.*, new GUI/TextureDisplacementObject.*, test_texture_color_palette.cpp |
| T1 | Pure core | 6 | - | new OrcaMCPTextureDisplacement.*, new test_mcp_texture_displacement.cpp |
| T2 | Paint channel | 2.5 | - | OrcaMCPPaintModel.*, OrcaMCPPaintSelect.* (state names), test_paint_remap.cpp |
| T3 | paint_object texture + visible, panels closed | 5 | T0, T1, T2 | OrcaMCPPaintTools.cpp, OrcaMCPPaintSelect.*, OrcaMCPNextSteps.* |
| T4 | get_texture_library, delete_user_texture, category | 3 | T1 | new OrcaMCPTextureTools.cpp, OrcaMCPServer.{hpp,cpp} |
| T5 | set_texture_displacement | 7 | T0, T4 | OrcaMCPTextureTools.cpp |
| T6 | get_texture_displacement | 2.5 | T5 | OrcaMCPTextureTools.cpp |
| T7 | get/set_texture_unwrap | 5 | T6 | OrcaMCPTextureTools.cpp |
| T8 | bake_texture_displacement | 7 | T0, T6 | OrcaMCPTextureTools.cpp, OrcaMCPNextSteps.* |
| T9 | prepare_texture_mesh | 3.5 | T8 | OrcaMCPTextureTools.cpp |
| T10 | get_object_paint, clear_object_paint, pick_facet, remap_paint | 2.5 | T3 | OrcaMCPPaintTools.cpp |
| T11 | Texture state in get_scene_info, get_object_info, load_model | 2.5 | T1 | OrcaMCPCommon.cpp, OrcaMCPPartEdits.cpp (volume_row_json), test_mcp_scene_description.cpp |
| T12 | Mesh edits report dropped texture paint | 3 | T0, T1, T4 | OrcaMCPMeshTools.cpp, OrcaMCPPartTools.cpp, OrcaMCPSourceFileTools.cpp, cut_object in OrcaMCPServer.cpp |
| T13 | Textured OBJ UVs, project round trip | 2.5 | T1, T2 | new test_mcp_texture_files.cpp, tests/data fixture |
| T13b | Model UVs in the 3MF (for an upstream PR) | 5 | T13 | Format/bbs_3mf.cpp, test_mcp_texture_files.cpp (its UV assertion) or a libslic3r 3mf test |
| T14 | Docs, counts, tables | 3 | all | CLAUDE.md, README.md, docs/README.md, docs/setup/configuration.md, workflows.md |
| T15 | Live check | 5 | all | none (scratch only) |
| T16 | Upstream PR of T13b | 1 | T15 | none in this checkout (a separate worktree) |

About **67 h**. Every task that changes the registry regenerates `scripts/orcamcp_tools.json`, which no two can do at
once, and writes `docs/tools/reference.md`: those run **one after another** in this order: T3, T4, T5, T6, T7, T8, T9,
T10, T11. Parallel groups (no file in common; each builds and tests green alone):

- **Group A**, first, in parallel: **T0 ∥ T1 ∥ T2**. (T1 and T0 both add CMake entries: T1 to `src/slic3r` and tests
  lists, T0 to `src/slic3r` only, a different line; merge order T1 then T0 if both land at once.)
- **Group B**, after A, the registry chain: T3 → T4 → T5 → T6 → T7 → T8 → T9 → T10 → T11.
- **Group C**, parallel with group B: **T13** once A is in; **T12** once T4 is in and merged before T11 starts (T12
  edits `cut_object` in `OrcaMCPServer.cpp`, which T4 touches only to call `register_texture_tools()` and T11 for the
  `get_object_info` / `get_scene_info` descriptions; T12 changes no tool text, so no golden regeneration). T12 must not
  edit `OrcaMCPTextureTools.cpp`, `OrcaMCPNextSteps.*`, `OrcaMCPPaintTools.cpp` or any doc.
- **Group C** also: **T13b** after T13.
- **Last:** T14, then T15, then T16.

Commit per task, subject `mcp: <what changed, from the user's view>`; docs-only commits end `[skip ci]`; never push, tag
or bump the version. End messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` (or the implementer's
own model line).

---

## T0. Upstream moves: the panel's logic as functions over the model (7 h, group A)

Goal: every piece of panel state a tool needs exists as a function of the model (and the preset bundle), called by the
panel and by MCP alike, so nothing is copied. Behaviour of the panel unchanged. Two probe items in CLAUDE.md.

**New fork file `src/slic3r/GUI/TextureDisplacementObject.{hpp,cpp}`** (namespace `Slic3r::GUI`, no wx, no `wxGetApp`;
list it beside `GUI/TextureLibrary.*`). Move the bodies, unchanged, out of the gizmo members, which become one-line
callers with `*m_c->selection_info()->model_object()`:

```cpp
std::vector<ModelVolume*> texture_parts(ModelObject&);                   // texture_parts()
std::vector<ModelVolume*> painted_texture_parts(ModelObject&);           // painted_parts()
ModelVolume*       texture_stack_owner(ModelObject&);                    // texture_volume()
const ModelVolume* texture_stack_owner(const ModelObject&);
std::vector<TextureDisplacementLayer> layers_for_part(const ModelObject&, const ModelVolume& part);   // layers_for_part()
Vec3d texture_anchor(const ModelObject&);                                // texture_anchor()
// resting_drops(): each part's bottom above the lowest model part's, from meshes and world transforms, so it can run
// on prepared meshes the model does not hold yet (T8's classic pipeline).
std::vector<float> resting_drops(const std::vector<std::pair<const indexed_triangle_set*, Transform3d>>& all_parts,
                                 const std::vector<std::size_t>& parts);
Transform3d prepare_texture_frame(const ModelObject&, const ModelVolume& part);      // prepare_texture_frame()
// The model half of remove_texture_layer / swap_layer_slots / move_texture_layer: the stack and every part's masks.
bool remove_texture_layer(ModelObject&, int slot);
void swap_texture_layer_slots(ModelObject&, int slot_a, int slot_b);
bool move_texture_layer(ModelObject&, int slot, int to_index);           // false when nothing moves
// mark_seam_path's Dijkstra over the owner's edge graph; empty when unreachable.
std::vector<std::pair<int, int>> seam_path_edges(const indexed_triangle_set&, int v_from, int v_to);
// cut_island's seams: `chart`'s edges crossing the plane through its middle, across its longest axis, added to `seams`.
std::vector<std::pair<int, int>> cut_island_seams(const indexed_triangle_set&, const PatchUnwrap&, int chart,
                                                  const std::vector<std::pair<int, int>>& seams);
// JoinSelected's choice: the neighbour placed nearest and the placement joining `chart` to it, or nullopt.
std::optional<std::pair<int, TextureIsland>> join_target(const PatchUnwrap&, const std::vector<TextureIsland>&, int chart);
// smooth_model's displaced-surface relaxation on the owner, as a pure step: the smoothed mesh, or nullopt for no change.
std::optional<indexed_triangle_set> smoothed_baked_mesh(const ModelVolume& owner, const TextureDisplacementOptions&);
```

The gizmo keeps its own state swaps in `swap_layer_slots` (thumbnails, expanded flags, active and picker slots) after
calling `swap_texture_layer_slots`. `island_group_of` / `join_island_groups` are already static: leave them, make them
public if private.

**Palette** (`GLGizmoTextureDisplacement`, public static, beside `make_palette`): move the composition out of
`cached_palette()` and `color_settings_for()` into

```cpp
struct PaletteInputs { std::vector<ColorRGBA> filaments; bool mixing; int mix_count; int free_slots; };
static PaletteInputs palette_inputs(const TextureDisplacementOptions&, const PresetBundle&);   // filament_palette() + counts
static std::vector<PaletteEntry> compose_palette(const PaletteInputs&, const std::optional<std::vector<PaletteEntry>>& ranking,
                                                 const DynamicPrintConfig& project_config);   // pick_mixes + make_palette
static TextureColorSettings color_settings(const std::vector<TextureDisplacementLayer>&, const TextureDisplacementOptions&,
                                           const PaletteInputs&, const DynamicPrintConfig& project_config);
```

`cached_palette()` keeps its two-level cache and calls `compose_palette`; `color_settings_for()` calls
`color_settings` with the cached ranking avoided (an uncached call ranks again: fine for a bake, tens of ms).
`filament_palette()` reads `wxGetApp()`: `palette_inputs` takes the bundle and the extruder colours as arguments; the
app side (`Plater::get_extruders_colors()`) is read by the caller on the main thread.

**Bake core** (`Jobs/TextureDisplacementBakeJob.{hpp,cpp}`): split the job like `FixModelByCgal`:

```cpp
// Main thread: the job's input for `object`, as GLGizmoTextureDisplacement::bake builds it (painted parts, their layers,
// the owner's options, anchor, drops), plus each part's mesh pointer and mask data for the unchanged check.
TextureDisplacementBakeInput capture_texture_bake(const ModelObject&, const TextureColorSettings&, bool take_snapshot);
struct TextureBakeVolumeResult { TriangleMesh mesh; std::vector<uint8_t> triangle_color; TextureBakeStats stats; };
struct TextureBakePlan { std::vector<TextureBakeVolumeResult> results; bool canceled = false; };
// Any thread: process()'s loop. `progress(percent)` returning false stops it (canceled, nothing may be applied).
TextureBakePlan plan_texture_bake(const TextureDisplacementBakeInput&, const std::function<bool(int)>& progress);
// The input still describes the model: every part found by id, same mesh, same masks, same layers and options.
bool texture_bake_applies_to(const Model&, const TextureDisplacementBakeInput&);
// finalize()'s per-part rule: the result is not empty and some vertex moved more than 1e-5 mm.
bool texture_bake_moved(const TextureDisplacementBakeInput&, const TextureBakePlan&, std::size_t part);
struct TextureBakeOutcome {
    std::vector<ObjectID> committed, unmoved;         // parts baked / parts the bake did not move (kept as they were)
    std::vector<int> mixed_filaments;                 // 0-based slots the colour bake named, each created or reused
    int filaments_before = 0, filaments_after = 0;    // to tell created from reused
    std::size_t refined = 0, budget = 0;              // the budget notice's numbers, 0 when it did not cap
    ModelObject* object = nullptr;
};
// Main thread: finalize()'s body after its cancel check (moved(), the commit lambda, the budget sum), under the
// snapshot when input.take_snapshot. Shows no dialog and pushes no notification: the caller does.
TextureBakeOutcome apply_texture_bake(Plater&, TextureDisplacementBakeInput&, TextureBakePlan&);
```

`process()` becomes `m_plan = plan_texture_bake(m_input, [&ctl...]{...})`; `finalize()` calls `apply_texture_bake`
then shows the "produced no displacement" error when nothing committed and pushes the budget notification, as now.

**Prepare core** (`Jobs/TextureDisplacementPrepareJob.{hpp,cpp}`) the same way: `capture_texture_prepare(object, params,
scope, color, snapshot_name)`, `plan_texture_prepare(input, progress)` (the `prepare_mesh` loop),
`apply_texture_prepare(plater, input, results) -> outcome` (finalize's body; the snapshot only when `snapshot_name` is
not empty, so T8 can apply a prepare and a bake under one step). `queue_prepare` and `bake_standard` call them.

**Tests** in `tests/slic3rutils/test_texture_color_palette.cpp` (existing file, no CMake change; tag also
`[TextureObject]`): on a headless two-part `Model` - `texture_stack_owner` is the first model part after a modifier;
`layers_for_part` strips islands for the second part only; `resting_drops` 0 for the lowest part and 5 for one 5 mm up;
`remove_texture_layer` clears the slot on both parts; `move_texture_layer` 0 -> 2 moves masks and slots and keeps the
order of the others; `seam_path_edges` on a cube from corner to opposite corner gives a connected path of real edges;
`cut_island_seams` on a long box's unwrap adds edges crossing its middle only; `plan_texture_bake` then
`apply_texture_bake` on a model through a test `Plater` is not possible headless, so test `plan_texture_bake` against
`build_texture_displacement` called directly (same mesh) and `texture_bake_applies_to` false after a mask edit, a mesh
change and a layer edit. Run the upstream suites listed in Commands too.

**CLAUDE.md probes** (add to the block and a paragraph each, as items V and AI are written):

```bash
echo "BW the texture bake and prepare jobs run their work inline in process()/finalize(), which MCP's bake runs as capture / plan / apply (a move: 0 = keep ours): $(U src/slic3r/GUI/Jobs/TextureDisplacementBakeJob.cpp | grep -c 'plan_texture_bake') / $(U src/slic3r/GUI/Jobs/TextureDisplacementPrepareJob.cpp | grep -c 'plan_texture_prepare')"
echo "BX the texture panel's bake inputs, palette, layer moves, seams and island joins are gizmo members over its selection (a move: 0 = keep ours): $(U src/slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.cpp | grep -c 'texture_stack_owner(') / $(U src/slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp | grep -c 'compose_palette')"
```

Commit `Texture displacement: the panel's bake, palette and layer logic as functions of the model`.

---

## T1. The pure core: `OrcaMCPTextureDisplacement.{hpp,cpp}` (6 h, group A)

New `src/slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.{hpp,cpp}`, namespace `Slic3r::GUI::OrcaMCP`. Includes
`libslic3r/Model.hpp`, `libslic3r/TextureDisplacement.hpp`, `slic3r/GUI/TextureLibrary.hpp` (no wx in it),
`OrcaMCPPaintModel.hpp`, `nlohmann/json.hpp`. No `wxGetApp`, no `texture_library()`, no file I/O except the two marked.
Nothing here depends on T0 (owner lookups are T0's and are passed in as arguments).

```cpp
// ---- Names ----
const std::vector<std::string>& projection_names();   // triplanar cylindrical spherical unwrap view model_uv (enum order)
std::optional<TextureProjectionMethod> projection_named(const std::string&);   std::string projection_name(TextureProjectionMethod);
const std::vector<std::string>& blend_names();        // add override subtract multiply divide (the panel's order)
std::optional<TextureBlendMode> blend_named(const std::string&);               std::string blend_name(TextureBlendMode);
const std::vector<std::string>& tile_method_names();  // repeat mirrored_repeat
std::optional<TextureTileMethod> tile_method_named(const std::string&);        std::string tile_method_name(TextureTileMethod);
const char* texture_source_name(TextureSource);       // builtin | user | pack

// ---- The library, by id ----
// "builtin/Bark.png", "user/My bricks.png", "pack/<pack name>/<file name>": the file name with its extension. Ids still
// equal get " #2", " #3" in library order.
std::vector<std::string> texture_ids(const std::vector<TextureLibraryEntry>&);
struct TextureLookup { int index = -1; std::string error; };
// An id, or a stem (any case) exactly one entry has; ambiguous lists the ids; unknown points at get_texture_library.
TextureLookup find_texture(const std::vector<TextureLibraryEntry>&, const std::vector<std::string>& ids, const std::string& query);
std::optional<int> library_index_of(const std::vector<TextureLibraryEntry>&, const TextureDisplacementLayer&);  // by path
// A PNG's colour as decode_height_texture reads it, from its header: false for colour type 0 at 8 bits, true for the
// types decode_height_texture takes as colour (verify 2, 3, 4, 6 against the decoder in the test and follow it), nullopt
// for a non-PNG (a jpg in the user folder).
std::optional<bool> png_has_color(const unsigned char* bytes, std::size_t size);
std::optional<bool> png_color_from_file(const std::string& path);   // I/O: the first 33 bytes
nlohmann::json library_entry_json(const TextureLibraryEntry&, const std::string& id, std::optional<bool> color);
// Identity of an image for the import's reuse (user decision 2): width, height, the height pixels, and the colour
// pixels when some pixel is not grey. Two encodings of one picture (an RGB PNG of grey pixels and its grayscale PNG, a
// jpg and its PNG) are the same texture; a hash match is confirmed by comparing the decoded data.
struct TextureIdentity { std::uint64_t hash = 0; int width = 0, height = 0; };
TextureIdentity texture_identity(const DecodedHeightTexture&);
bool same_texture(const DecodedHeightTexture&, const DecodedHeightTexture&);

// ---- One call's layer edit ----
struct ImageAdjustRequest { std::optional<float> brightness, contrast, sharpness, blur, hue_deg; };  // panel ranges
struct ProjectionFrameRequest {           // an orthographic frame through which the texture is projected once
    std::optional<std::string> view;      // a named view (below), or right/up
    std::optional<Vec3d> right, up;       // plate axes, unit after normalising, not parallel
    Vec3d center = Vec3d::Zero();         // plate mm
    double width_mm = 0, height_mm = 0;   // > 0
};
struct LayerSettings {                    // every field optional: only what the call gives
    std::optional<float> depth_mm, tile_size_mm, rotation_deg, midlevel, smoothing, edge_fade_amount;
    std::optional<Vec2f> offset;
    std::optional<bool>  invert, edge_fade, tile, color;
    std::optional<TextureTileMethod> tile_method;
    std::optional<TextureProjectionMethod> projection;
    std::optional<TextureBlendMode> blend;
    std::optional<std::string> view_direction;        // current | iso | top | bottom | front | back | left | right
    std::optional<std::pair<Vec3d, Vec3d>> view_axes; // explicit right/up in plate axes, instead of view_direction
    std::optional<ProjectionFrameRequest> projection_frame;
    bool clear_projection_frame = false;              // projection_frame: false
    std::optional<ImageAdjustRequest> adjust_image;
    std::optional<bool> color_mix; std::optional<int> color_mix_count, color_denoise;   // stack-wide
    bool empty() const;
};
enum class LayerAction { add, edit, remove, move };
struct LayerRequest {
    LayerAction action = LayerAction::edit;
    std::optional<int> layer, to_position;              // slot; move: 0-based position in slot order
    std::optional<std::string> texture, file_path;
    LayerSettings settings;
};
// The bounds every number is held to, one table for read_layer_request and the schema's minimum/maximum (T5).
struct SettingBound { const char* key; double min, max; const char* unit; const char* panel_label; };
const std::vector<SettingBound>& layer_setting_bounds();
// Types, names, ranges and structure: action required; edit, remove and move need layer (0..7); add takes none ("add
// takes the first free slot, as the panel does"); remove takes nothing else; move takes to_position and nothing else;
// texture and file_path not both; offset two finite numbers; view_direction and view_axes not both; a frame's width and
// height > 0 and its axes not parallel; adjust_image with at least one field.
std::optional<LayerRequest> read_layer_request(const nlohmann::json& params, std::string& error);

struct StackFacts { bool owner_has_uv = false; int physical_filaments = 1; bool texture_has_color = false; };
// Refusals that need the stack: no free slot; no layer in that slot (lists the slots); colour on grayscale; model_uv
// without UV; color_mix_count above max(1, 32 - physical_filaments); view_direction / view_axes when the projection after
// the call is not view; move to_position out of range; adjust_image on a layer with no image.
std::optional<std::string> layer_edit_refusal(int object_id, const std::vector<TextureDisplacementLayer>& stack,
                                              const LayerRequest&, const TextureDisplacementLayer& after, const StackFacts&);
std::optional<int> first_free_slot(const std::vector<TextureDisplacementLayer>&);
TextureDisplacementLayer new_layer(int slot, bool owner_has_uv);           // add_texture_layer's defaults
bool apply_layer_settings(TextureDisplacementLayer&, const LayerSettings&); // field mapping, true when changed;
     // clear_projection_frame -> view_project_projective false and tile_enabled true (the panel's Clear)
bool apply_stack_settings(TextureDisplacementOptions&, const LayerSettings&);
TextureAdjustments adjustments_of(const ImageAdjustRequest&);
// capture_view_projection's arithmetic: the projector axes in the volume's frame from world right/up and its transform.
std::pair<Vec3f, Vec3f> view_projector_axes(const Vec3d& right_world, const Vec3d& up_world, const Transform3d& volume_to_world);
// An orthographic projection frame as the row-major 3x4 view_project_matrix in the volume's frame: uv = ((p - corner) .
// right / width, (p - corner) . down / height) with row 2 = (0, 0, 0, 1), where corner is the frame's top left and v runs
// down the image, as apply_projection_frame's window does. project_uv_projective reads it back.
std::array<float, 12> orthographic_frame_matrix(const Vec3d& right_world, const Vec3d& up_world, const Vec3d& center_world,
                                                double width_mm, double height_mm, const Transform3d& volume_to_world);
nlohmann::json inactive_settings(const std::vector<TextureDisplacementLayer>& stack, const TextureDisplacementLayer&,
                                 const TextureDisplacementOptions&, bool owner_has_uv, bool texture_has_color);
std::vector<std::string> layer_warnings(const TextureDisplacementLayer&);  // midlevel > 0 and depth > 1 mm

// ---- Masks ----
bool mask_is_stale(const TriangleSelector::TriangleSplittingData&, std::size_t facet_count);
struct MaskInput { int volume_id; std::string name; std::shared_ptr<const TriangleMesh> mesh; std::array<PaintData, 8> masks; };
struct MaskReport { int volume_id; std::string name; bool stale = false; int painted_facets = 0; double area_ratio = 0.; };
std::vector<std::array<MaskReport, 8>> mask_reports(const std::vector<MaskInput>&);       // any thread, state 1 only
bool needs_bake(const std::vector<TextureDisplacementLayer>&, const std::vector<std::array<MaskReport, 8>>&);
// Cheap form for every-object reports (T11): FacetsAnnotation::empty() per slot, no selector.
bool needs_bake_fast(const ModelObject&, const ModelVolume* owner);

// ---- What a mesh edit dropped (T12) ----
struct TextureState { struct Part { ObjectID id; std::string name; std::vector<int> painted_slots; int layers = 0; }; std::vector<Part> parts; };
TextureState texture_state_of(const ModelObject&);
// {paint_lost: [{volume_id, name, layers: [slots]}], layers_lost: n, stacks_unused: [{volume_id, layers}]} comparing
// before with the objects after (one for most edits, several for a split); empty object when nothing was lost.
nlohmann::json texture_changes_json(const TextureState& before, const std::vector<const ModelObject*>& after,
                                    const ModelVolume* owner_after);

// ---- The unwrap request (T7) ----
struct IslandEdit { int chart; std::optional<Vec2f> offset; std::optional<float> rotation_deg, scale; };
struct UnwrapRequest {
    std::optional<float> seam_angle_deg;                 // 5..90 (SetSeamAngle's clamp, refused outside)
    std::optional<float> island_padding_mm;              // -1 auto, else >= 0
    std::optional<bool>  auto_connect_islands;
    std::vector<std::pair<int, int>> add_seams, remove_seams;   // mesh vertex pairs
    std::vector<std::pair<Vec3d, Vec3d>> seam_paths;            // plate mm, snapped to the nearest vertices
    std::vector<int> cut_islands, unjoin;
    std::vector<std::pair<int, std::optional<int>>> join;       // chart, to (nullopt: the nearest neighbour, as the pane)
    std::vector<IslandEdit> islands;
    std::vector<std::pair<int, Vec2f>> uv_edits;               // unwrapped vertex, raw unwrap mm (lscm_uv_override_key)
    bool clear_seams = false, clear_uv_edits = false, average_scale = false, relayout = false;
};
std::optional<UnwrapRequest> read_unwrap_request(const nlohmann::json& params, std::string& error);
nlohmann::json unwrap_json(const PatchUnwrap&, const TextureDisplacementLayer&, bool include_vertices);

// ---- Answers ----
// {layer, position, base, texture {texture (id or null), name, source ("project" for an image only the project holds),
// path}, color_capable, depth_mm, tile_size_mm, rotation_deg, offset, invert, midlevel, smoothing, edge_fade,
// edge_fade_amount, tile, tile_method, projection, blend, color, view {right, up} (view), projection_frame (view),
// unwrap {seam_angle_deg, island_padding_mm, auto_connect_islands, seams, placed_islands, uv_edits} (unwrap), paint [...],
// painted, inactive_settings, warnings}
nlohmann::json layer_json(/* the layer, its position, base flag, library match and id, colour, mask reports, owner_has_uv */);
nlohmann::json stack_options_json(const TextureDisplacementOptions&);
// {pipeline, displace_border, smooth {enabled, strength, passes, ignore_outer_ring}, resolution_mm (null = auto),
//  budget_k (null = auto), color_mix, color_mix_count, color_denoise}
```

**Tests** (new `tests/slic3rutils/test_mcp_texture_displacement.cpp`, `[McpTexture][orcamcp]`, add to the tests
CMakeLists): names round-trip; ids unique and stable over a synthetic list (built-ins, a user `a.png` and `a.jpg`, two
packs, a colliding pair); `find_texture` by id, stem in any case, ambiguous, unknown; `png_has_color` agrees with
`decode_height_texture(layer, false).has_color()` for all 44 files in `path(PROFILES_DIR).parent_path() / "textures" /
"displacement"` (6 colour); `texture_identity` / `same_texture` equal for a grayscale PNG and an RGB PNG of the same
grey pixels, different for one changed pixel; `read_layer_request` for every bound in and out (`GENERATE` over
`layer_setting_bounds()`), wrong types, each structural refusal; `layer_edit_refusal` for each case; `new_layer` with and
without UV; `apply_layer_settings` maps each field and reports no change for equal values; `orthographic_frame_matrix`
puts the frame's corners at uv (0,0), (1,0), (0,1) through `project_uv_projective`, identity and rotated-and-scaled
volumes; `view_projector_axes` equals `capture_view_projection`'s steps on `Camera c; c.select_view("top")` (also front,
left, iso; `slic3r/GUI/Camera.hpp`, default-constructible); `inactive_settings` per case; `mask_is_stale`;
`mask_reports` on a 12-facet cube with 4 facets in slot 2 (area ratio 1/3) and a stale mask; `needs_bake` and
`needs_bake_fast` agree; `texture_changes_json` for a part whose masks were reset, a stack that moved to the second part,
and no change; `read_unwrap_request` bounds and shapes. Run `$T "[McpTexture]"`. Commit `mcp: texture displacement
decisions, apart from the app`.

---

## T2. A paint channel per texture layer (2.5 h, group A)

`OrcaMCPPaintModel.{hpp,cpp}`:

```cpp
enum class PaintMode { Color, Support, Seam, FuzzySkin, Texture };
struct PaintChannel {
    PaintMode mode = PaintMode::Color;
    int texture_layer = -1;                                   // the slot, Texture only
    PaintChannel(PaintMode m) : mode(m) {}                    // implicit: the four modes pass as before
    static PaintChannel texture(int slot);
};
FacetsAnnotation&       annotation_for(ModelVolume&, const PaintChannel&);   // texture_displacement_facet(slot)
const FacetsAnnotation& annotation_for(const ModelVolume&, const PaintChannel&);
```

- `annotation_for_mode` stays for the four modes and forwards; it asserts on Texture.
- `const PaintChannel&` replaces `PaintMode` in `apply_paint_data`, `capture_paint_base`, `paint_base_unchanged`,
  `apply_facet_states`, `read_volume_paint`, `has_volume_paint`, `clear_volume_paint`. `PaintMode` stays where only the
  bound or the names matter (`build_paint_write`, `build_remap_write`, `max_paint_state_for`, `parse_paint_state`,
  `paint_state_label`, `paint_mode_name`, `state_mapping_error`).
- Texture: name "texture"; bound `ENFORCER` (the panel's masks hold one state: `deserialize(data, false, ENFORCER)`);
  states "none" -> 0, "painted" or "enforcer" -> 1; labels 0 "none", 1 "painted". Every `switch (mode)` in the OrcaMCP
  paint files gets its case (-Wswitch finds them).
- `OrcaMCPPaintSelect.cpp`, `parse_state_param`: the state list in its messages from one helper `state_names(PaintMode)`
  ("none or painted" for texture).
- `OrcaMCPPaintTools.cpp`, `resolve_paint_modes`: "texture" refused for now ("mode texture needs texture_layer:
  get_texture_displacement reports each layer's paint"); T10 replaces this.

Tests (`test_paint_remap.cpp`, `[PaintRemap]`): a texture write of 0/1 states reads back `{0: n-4, 1: 4}`; state 2
refused; `annotation_for(texture(3))` is `texture_displacement_facet(3)` and a write there leaves the other masks and the
four channels empty; labels round-trip. Run `$T "[PaintRemap],[PaintGeometry]"`. Commit `mcp: paint data addresses a
texture layer's mask`.

---

## T3. `paint_object`: mode texture, selection visible, open panels closed (5 h, chain 1)

**Schema** (`register_paint_tools`, OrcaMCPPaintTools.cpp:776): `mode` enum gains `"texture"`; new `texture_layer`
(integer 0..7, "mode texture: the layer's slot, as get_texture_displacement lists it"); `"painted"` joins the enums of
`state`, `match_state`, `bands.items.state`; `selection` gains `"visible"` with a new `view` object
(`additionalProperties: false`: `direction` (a named view: iso top bottom front back left right, or current),
`vector` ([x,y,z], the direction the view looks along, plate axes), `frame_layer` (integer 0..7: only facets inside that
layer's projection frame)). Summary `"Paint color/support/seam/fuzzy/texture"` (38). Description: add "texture paints
where one texture displacement layer applies (texture_layer; state painted or none); visible paints the facets seen from
view (front-facing and not hidden by the object's other facets), as the texture panel's Project only on visible does,
and in any mode."

**Handler**, hop 1 (the gate ~line 958): build `PaintChannel`; refuse texture without `texture_layer` and
`texture_layer` with another mode; refuse a slot the stack (`texture_stack_owner`, T0) has no layer in (message names
`set_texture_displacement` with action add); texture: `edit_job_refusal(!plater.get_ui_job_worker().is_idle(),
"paint_object")` (a bake running would reset the mask it captured). After every refusal, **in every mode**,
`close_toolbar_tool_for_change(*plater, object_id, volumes, closed_tool)` (OrcaMCPPartTools.hpp) before
`capture_paint_base` (user decision 1: an open paint panel's next flush overwrote MCP's paint in the four existing modes
too). `visible`: capture the view on the main thread - named: `Camera c; c.select_view(camera_named(name)->direction)`
(OrcaMCPUserView.hpp; iso is the constructor's orientation); current: `plater->get_camera()`'s forward, eye and type;
vector: as given - and, with `frame_layer`, that layer's `view_project_matrix` (refused unless it has a frame). Pass the
channel to every PaintModel call in hops 1-3 and in `paint_by_state` / `run_paint_remap`.

**Pure, `OrcaMCPPaintSelect.{hpp,cpp}`**: `FacetAssignment assign_visible(const std::vector<PaintPlanVolume>&, const
VisibleView&, int state, std::size_t volume_index)` - a facet is visible when its plate normal faces the view (per facet
from the eye under perspective, `fwd` under ortho, as `select_visible_faces` does) and a ray from its centroid toward the
viewer hits no facet of any model part (an `AABBMesh` per part in plate frame, `query_ray_hit`, offset 1e-4 mm), and,
with a frame, its centroid projects inside [0,1]^2 through `project_uv_projective` in the owner's frame. Hop 2 runs it.
Tests in `test_paint_select.cpp` (`[PaintSelect]`): a cube from top selects its 2 top facets; an L-shape from the front
excludes the hidden back face; a frame keeps only the facets inside it.

Hop 3, texture: snapshot `_u8L("Paint Object") + " (texture)"`; `refresh_after_texture_paint` (new, beside
`refresh_after_paint`): `obj_list()->update_info_items(object_id)` and the canvas' `EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS`,
what `GLGizmoTextureDisplacement::update_model_object` does; not the plate notification (the slicer never reads texture
paint, so no plate loses its slice). Answer: `modes.texture`, `texture_layer`, `needs_bake`, `closed_toolbar_tool`.

**Next step** (`OrcaMCPNextSteps`): `texture_paint_next_steps(int object_id, int slot, bool painted)` -> none when
painted, else `{"paint_object", "texture layer S of object N is painted nowhere, so it applies nowhere: paint where it
goes", {object_id, mode: texture, texture_layer: S, selection: all, state: painted}}`. Its case in
`test_mcp_next_steps.cpp`. (The bake step comes with T8.)

Docs: reference `### paint_object` (mode, `texture_layer`, `painted`, `visible` and `view`, a "Texture layers"
paragraph, `closed_toolbar_tool` in every mode). Golden. Run `$T "[McpNextSteps],[McpToolArguments],[PaintSelect],[orcamcp][tools]"`.
Commit `mcp: paint_object paints texture layers and what a view sees, and closes an open paint panel first`.

---

## T4. `get_texture_library`, `delete_user_texture`, the category (3 h, chain 2)

**Category** `OrcaMCPServer::ToolCategory::TextureDisplacement`, after `Painting` in the enum and in
`all_tool_categories()`, named "Texture displacement" in `tool_category_name`: a feature of eight tools that neither
Painting nor Models describes. `paint_object` stays in Painting.

**New app file `src/slic3r/GUI/OrcaMCP/OrcaMCPTextureTools.cpp`** with `void OrcaMCPServer::register_texture_tools()`
(declared in OrcaMCPServer.hpp beside `register_view_tools`, called after `register_paint_tools()` at
OrcaMCPServer.cpp:6112). All texture tools live here.

**Library helpers in the same file** (anonymous namespace; T5 reuses them):
- `library_copy()`: main thread, a copy of `texture_library()` (no rescan, as the picker) and its `texture_ids`.
- `find_identical_user_texture(const DecodedHeightTexture&)`: the user entries decoded (`load_texture_image_data` ->
  `decode_height_texture(probe, false)`), compared by `texture_identity` then `same_texture`; decoded identities cached
  in a static map keyed by (path, size, mtime), main thread only.
- `import_or_reuse(path)` / `save_or_reuse(decoded, stem)`: read the candidate (`load_texture_image_data`, under
  `wxLogNull`), decode, look for an identical user texture; found -> that entry, `reused: true`; else
  `import_texture_to_library` / `save_texture_to_library`, `reused: false`. Answer shape `imported_texture {texture,
  name, path, reused}`.

**`get_texture_library`**: summary "List the textures a layer can use" (33). Description: "The textures a texture
displacement layer can use, as its picker lists them: built-in, imported into this data folder's texture library
(set_texture_displacement's file_path imports one; an identical image already there is reused), and those of installed
texture packs. Each has a texture id for set_texture_displacement (a name alone works when only one texture has it),
and color: true when it has colours a layer can print with filaments." Schema: `source` (enum builtin/user/pack).
Handler: main thread copies; HTTP thread `png_color_from_file` and `library_entry_json`. Answer `{textures, counts,
user_folder}` (`user_texture_dir()` read on the main thread).

**`delete_user_texture`**: summary "Delete a texture you imported" (29). Schema: `texture` (string, required).
Refuses anything not `source: user` ("Only imported textures can be deleted; built-in and pack textures are read-only"),
then `remove_user_texture(path, error)` (TextureLibrary.hpp). Layers using it keep their image (it lives in the layer and
the project): the answer lists those objects' layers (`still_used_by`). No undo step (a file delete, as in the panel).

Tests: `[McpTexture]` for the answers' shape from synthetic lists. Golden; `tool_examples()` for both; reference: a new
`## Texture Displacement Tools` section after Painting Tools (intro: the stack, slots, masks per part, nothing prints
until baked; the Quick Reference row), `### get_texture_library`, `### delete_user_texture`. Commit `mcp: list and
delete texture displacement textures`.

---

## T5. `set_texture_displacement` (7 h, chain 3)

**Schema** (`ToolCategory::TextureDisplacement`, summary "Add, edit, move or remove a texture layer" (40)):
`object_id` (required), `action` (add/edit/remove/move, required), `layer` (0..7), `to_position` (integer, min 0),
`texture` (string), `file_path` (string; the bridge's Windows path rewrite covers it), the numbers with minimum/maximum
from `layer_setting_bounds()` (`depth_mm`, `tile_size_mm`, `rotation_deg`, `midlevel`, `smoothing`,
`edge_fade_amount`), `offset` (array of 2 numbers, "placement in texture repeats"), booleans `invert`, `edge_fade`,
`tile`, `color`, `color_mix`, enums `tile_method`, `projection`, `blend`, `view_direction`, `view_axes` (object
`{right, up}`, `additionalProperties: false`), `projection_frame` (object `{view, right, up, center, width_mm,
height_mm}`, `additionalProperties: false`, or `false`: schema `{"type": ["object", "boolean"]}`; handler refuses
`true`), `adjust_image` (object `{brightness, contrast, sharpness, blur, hue_deg}`, `additionalProperties: false`),
`color_mix_count`, `color_denoise` (integers). Every request object a response returns (`view`, a frame) is accepted
back: add the response's extra fields to the strict objects or keep the response shapes identical, and test it ("What a
tool returns, sent back" in `test_mcp_tool_arguments.cpp`).

Description (keep it within what agents need): "Add, edit, move or remove one texture displacement layer of an object,
as the texture displacement panel does. An object holds up to 8 layers, combined in slot order; each applies where
paint_object (mode texture) paints it, and nothing reaches the print until bake_texture_displacement bakes it into the
mesh. add takes the first free slot with texture (an id from get_texture_library) or the library's first; file_path
imports an image (an identical one already imported is reused); edit changes only what is given; move puts a layer at
to_position in slot order, its paint with it; remove drops it and its paint. projection view projects from view_direction
(a named view or current: the 3D view's camera now) or view_axes; projection_frame projects once through an orthographic
frame (center, width_mm, height_mm, facing view) with tiling off, false clears it; adjust_image saves an adjusted copy of
the texture into the library and uses it. One undo step; nothing to change is changed: false. Refused while an arrange,
orient or bake runs; an open toolbar tool is closed first."

**Handler** (`McpDialogSuppressionGuard`, `wxLogNull`), main thread:
1. `read_layer_request`; `resolve_object_id`; `texture_stack_owner` (T0); `edit_job_refusal(...)`;
   `cgal_repair_dialog_running()` refusal.
2. The texture: `texture` via `find_texture` over `library_copy()`, then `load_texture_image_data` and refuse an image
   `decode_height_texture(probe, false)` reads as empty; `file_path`: refuse a missing file; add without either: the
   library's first entry (`add_texture_layer`).
3. `after` = the layer or `new_layer(first_free_slot, owner->has_texture_uv())`, with the texture and
   `apply_layer_settings`; `StackFacts` (`owner_has_uv`, `GLGizmoTextureDisplacement::palette_inputs(...).filaments.size()`
   (T0), `height_texture_has_color(after)` when known); `layer_edit_refusal`. A `file_path` or `adjust_image` with
   `color: true` is checked after the import (step 4); refused then, the answer still names the imported texture so a
   retry reuses it.
4. Every refusal passed: `close_toolbar_tool_for_change`. `file_path` -> `import_or_reuse`; `adjust_image` ->
   `apply_texture_adjustments(decode_height_texture(layer), adjustments_of(...))` then `save_or_reuse(adjusted,
   layer.name)` (`commit_image_adjust`'s steps; `preview_adjust` stays default). A user texture used ->
   `touch_user_texture`. View axes: named view via `Camera` as T3, current via `plater->get_camera()`, explicit
   `view_axes`; then `view_projector_axes(right, up, texture_displacement_volume_to_world(*owner))`; also when the
   projection becomes view without a direction (current, as the panel captures on choosing it). Frame:
   `orthographic_frame_matrix(...)` into `view_project_matrix`, `view_project_projective = true`, `tile_enabled = false`,
   projection view, and the axes too (as `apply_projection_frame` keeps them).
5. No change -> `{changed: false, layer}`, no undo step.
6. `plater->take_snapshot(name)`: add "Add texture displacement layer", remove "Remove texture displacement layer", move
   "Reorder texture displacement layers", texture or adjust only "Change texture displacement texture", frame "Apply
   texture projection frame", else "Texture displacement editing" (`get_action_snapshot_name`). Write: add -> push_back,
   `options.active_slot = slot`; edit -> assign; remove -> `remove_texture_layer` (T0); move -> `move_texture_layer` (T0).
   Stack options -> `apply_stack_settings`. No plate notification, no object list change; `canvas->set_as_dirty()`.
7. Answer `{status, changed, action, object_id, layer (layer_json; for remove `removed_layer`), stack_options,
   imported_texture, needs_bake, inactive_settings, warnings, closed_toolbar_tool}`, `next_steps` =
   `texture_paint_next_steps(object_id, slot, painted)` for add, edit and move, `guard.report(...)`.

Tests: a T1-file test that every `layer_setting_bounds()` entry matches the registered schema's minimum/maximum; the
"sent back" test for `view_axes` and `projection_frame`. Golden; `tool_examples()` (`{"object_id": 0, "action": "add",
"texture": "builtin/Knurl.png", "depth_mm": 0.4}`); reference `### set_texture_displacement` (parameters with ranges,
refusals, `inactive_settings`, examples: add by id, import, view from top, a frame, move, adjust, remove; responses);
next-steps table row. Commit `mcp: set_texture_displacement adds, edits, moves and removes texture layers`.

---

## T6. `get_texture_displacement` (2.5 h, chain 4)

Schema: `object_id` (required), `volume_id` (min 0: that model part's paint only). Summary "Read an object's texture
layers" (31). Description: layers in slot order with every setting set_texture_displacement takes, texture id (or source
project), `inactive_settings`, paint per model part (`coverage_percent`, `stale`), `stack_options`, `needs_bake`,
`free_layers`, `stack_volume_id`, `ignored_stacks`.

Handler: hop 1 (main): resolve; `volume_id` a model part; copy the owner's stack and options, `library_copy()`,
`has_texture_uv()`, per part a `MaskInput` (`mesh_ptr()`, eight `get_data()`), and the unused stacks (layers on parts
other than the owner). Hop 2: `mask_reports`, `height_texture_has_color` per layer, `library_index_of`, `layer_json`,
`inactive_settings`, `needs_bake`. `next_steps` from `texture_paint_next_steps` for the first unpainted layer.

Tests: a two-layer, two-part headless stack's answer has `base` on the lower slot only, the second part's paint, and
`needs_bake` true. Golden; `tool_examples()`; reference with a response example. Commit `mcp: get_texture_displacement
reports an object's texture layers and their paint`.

---

## T7. `get_texture_unwrap`, `set_texture_unwrap` (5 h, chain 5)

The UV editor pane's work for a layer mapped by `unwrap` (LSCM), on the owner part (the only part whose islands count,
`layers_for_part`). Both refuse a layer whose projection is not unwrap, or with nothing painted on the owner.

**`get_texture_unwrap`** (summary "Read a layer's unwrap: islands, seams" (37)): schema `object_id`, `layer` (both
required), `include_vertices` (boolean: each unwrapped vertex with its mesh vertex, chart and uv, capped at 20,000 with
`truncated`). Hop 1 captures the owner's mesh pointer, mask and the layer; hop 2 `extract_painted_patch` +
`compute_patch_unwrap(patch, seam angle, padding, seams)` + `apply_lscm_uv_overrides` and answers `unwrap_json`: charts
`{chart, area_mm2, centroid, bounding_box (unwrap mm), placement {offset, rotation_deg, scale}, group, neighbours}`,
`seams` (count and the pairs), `uv_edits`, `seam_angle_deg`, `island_padding_mm`, `auto_connect_islands`, and that chart
ids hold only for this unwrap.

**`set_texture_unwrap`** (summary "Edit a layer's unwrap: seams, islands" (37)): schema from `UnwrapRequest` (seams as
arrays of two integers, `seam_paths` as `[{from: [x,y,z], to: [x,y,z]}]`, `islands` items `{chart, offset, rotation_deg,
scale}`, `join` items `{chart, to}`, `uv_edits` items `{vertex, uv}`; nested objects `additionalProperties: false`, and
`islands` accepts back the extra fields `get_texture_unwrap`'s placement carries). Steps in the pane's order on a copy of
the layer: settings (seam angle 5..90, padding, auto connect: when turned on, the islands become
`compute_connected_net(unwrap)` as `SetConnectIslands` does), seams (add, remove, clear, `seam_paths` snapped to the
owner's nearest vertices in plate frame and walked with `seam_path_edges` (T0), `cut_islands` with `cut_island_seams`
(T0)), then the unwrap recomputed once if seams or angle changed (charts renumber: island edits in the same call
are refused then), then `relayout` (`compute_connected_net`), island placement, `join` (`join_target` (T0) or an
explicit neighbour through `join_chart_placement`, groups by `join_island_groups`), `unjoin`, `average_scale`
(`average_island_scales`), `uv_edits` (keys `lscm_uv_override_key`), `clear_uv_edits`. Refusals: an unknown chart, a
join of non-neighbours, a vertex out of range, a seam that is not a mesh edge. Plan (the unwrap) on the HTTP thread;
apply on the main thread after checking mesh, mask and layer unchanged; one snapshot named as the pane names the edit
when the call makes one kind ("Mark texture seam path", "Cut texture island", "Join texture island", "Unjoin texture
island", "Average island scale", "Clear texture seams", "Clear texture UV edits", "Move texture island", "Edit texture
UV"), else "Texture displacement editing". Answer: `unwrap_json` after.

Tests (`[McpTexture]`): `read_unwrap_request`'s refusals; on a painted cube's unwrap, a join of two neighbours places
the child edge to edge (compare with `join_chart_placement`), an island move, `average_scale` equal scales, a seam path
corner to corner. Golden; reference entries. Commit `mcp: read and edit a texture layer's unwrap`.

---

## T8. `bake_texture_displacement` (7 h, chain 6)

Summary "Bake texture layers into the mesh" (34). Schema: `object_id` (required); options, each stored into the
owner's `TextureDisplacementOptions` in the bake's undo step, as the panel's controls store them: `pipeline`
(default/classic; classic is the panel's hidden experimental path a project may carry), `resolution_mm` (0.02..2, or 0 =
auto), `budget_k` (0..4000, -1 auto; default pipeline), `classic_budget_k` (10..2000, default 750, the Standard bake's
subdivision budget; classic only, not stored: a panel member), `displace_border`, `smooth` (object `{enabled, strength
(1..100 %), passes (1..10), ignore_outer_ring}`, strict), `include_preview`. Description: "Turn an object's painted
texture layers into real geometry, as the texture panel's Bake does, on every painted model part: each part's mesh is
rebuilt with the relief (classic: remeshed and refined first, then displaced), its other paint carried across, a
colouring layer's colours written as colour paint (adding mixed filament slots when its mixes need them), the baked
layers' paint cleared and the layers kept, and the object dropped onto the bed. One undo step (the mixed filament slots
it adds are not undone with it). Answers each part's facet counts before and after and the options used. Runs in the
call's wait (a little under the bridge's ORCAMCP_TIMEOUT): past it nothing changes. Refused while another job runs, the
app's own bake or repair runs, or nothing is painted."

**Capture (main)**: resolve; refusals (UI worker busy, `cgal_repair_dialog_running`, nothing painted: "Object N has no
texture paint: paint_object mode texture paints a layer", no layer with an image); the options after the call (a copy
of the owner's with the arguments applied: Standard's pins only when no argument says otherwise, i.e. `displace_border`
default true, smooth off, as Standard mode bakes); the palette: `GLGizmoTextureDisplacement::color_settings(layers,
options, palette_inputs(options, *preset_bundle) + get_extruders_colors(), project_config)` (T0); `capture_texture_bake(
object, color, /*take_snapshot*/ false)` with the copied options; classic: also `capture_texture_prepare(object,
standard params with classic_budget_k, PaintedParts, color, "")`; `facets_before` per part.

**Plan (HTTP thread)**, one deadline `start + tool_wait_cap()`: default -> `plan_texture_bake(input, progress)` where
progress returns false when `this_thread_cancelled()` or past the deadline. Classic -> `plan_texture_prepare` then, on
its results (none needed doing: the captured meshes), a bake input rebuilt from the prepared meshes, masks and UVs, with
`resting_drops` (T0) recomputed from them and the captured transforms, then `plan_texture_bake`. A plan stopped by the
deadline is `timed_out`.

**Apply (main)**: `McpDialogSuppressionGuard`; refuse when `texture_bake_applies_to(model, input)` is false ("object N
changed in the app during the bake, so nothing was changed: call again"); close an open toolbar tool only now, then
re-check. If no part moved (T0 exposes finalize's per-part rule as `bool texture_bake_moved(const
TextureDisplacementBakeInput&, const TextureBakePlan&, std::size_t part)`), answer the panel's words ("The bake produced
no displacement, so nothing was changed and the paint was kept: check that the painted layer has a texture and a
non-zero depth") with no undo step. Otherwise one `Plater::TakeSnapshot(plater, _u8L("Bake texture displacement"))`,
inside it the stored options, the classic prepare's `apply_texture_prepare` (no snapshot of its own) and
`apply_texture_bake`. After: placement via `rehome_and_report_placement` as the transforms do (`apply_texture_bake`
already ran `update_info_items` and `changed_object`, which drops the object onto the bed).

**Answer**: `{status, changed, object_id, pipeline, options (stack_options_json), parts: [{volume_id, name, facets_before,
facets_after, baked}], unmoved_parts, mixed_filaments: [{slot (1-based), created}], filaments_added (count),
budget_limited {needed, kept} (the panel notification's numbers), placement fields, needs_bake (false unless some
part's paint was not baked), undo_warning (when slots were added: undo keeps them; `delete_mixed_filament` removes one),
closed_toolbar_tool}`, `active_warnings`, `include_preview`'s turntable. Timed out: `{status: error, timed_out: true,
message: "...not finished within this call's wait (W s); nothing was changed. Raise ORCAMCP_TIMEOUT, or lower
resolution_mm's detail (a larger value) or budget_k"}`.

**Next steps**: `bake_next_steps(int object_id, bool needs_bake)` in OrcaMCPNextSteps -> `{"bake_texture_displacement",
"object N's texture layers are painted but not baked, so the print does not show them yet", {{"object_id", N}}}`; wire
it into `paint_object` texture (T3's answer), `set_texture_displacement` and `get_texture_displacement` when
`needs_bake` (a layer still unpainted keeps its `texture_paint_next_steps` step too: at most one step per tool). No
step after a bake: `include_preview` shows the result. Its case in `test_mcp_next_steps.cpp`.

Tests: the options' read and bounds in T1's style (`read_bake_request` added to T1's file, with its bounds table used by
the schema); `texture_bake_applies_to` covered by T0. Golden; `tool_examples()`; reference with the response; CLAUDE.md
"Waiting" sections are T14's. Commit `mcp: bake_texture_displacement bakes texture layers into the mesh`.

---

## T9. `prepare_texture_mesh` (3.5 h, chain 7)

The Pro mode mesh tools, through T0's prepare core. Summary "Remesh, subdivide or smooth for textures" (40). Schema:
`object_id` (required), `operation` (remesh / subdivide / smooth, required); remesh: `target_edge_mm` (0.1..20, default
the panel's seed), `keep_sharp_edges` (default true), `sharp_angle_deg` (default 40); subdivide: `adaptive` (default
true), uniform `steps` (1..5), adaptive `max_edge_mm`, `detail_mm` (0.001..1), `min_edge_mm` (0.001..20), `edge_detail_mm`
(0..5), `color_detail_mm` (0..5), `follow_texture` (feature mode), `budget_k` (10..2000); smooth: the stored smooth
options (`strength`, `passes`, `ignore_outer_ring`). Same capture / plan / apply as T8: remesh -> `PrepareScope::AllParts`
("Remesh model for texture displacement"); adaptive -> painted parts, refused when nothing is painted ("Adaptive
subdivide for texture displacement"); uniform -> every part, `subdivide_mesh_uniform` in the plan and the panel's
save/restore painting in the apply ("Subdivide model for texture displacement"; texture paint dropped, as the panel's,
said in the answer); smooth -> `smoothed_baked_mesh` (T0) on the owner ("Smooth texture displacement"). The prepare's
outcomes map to answers: Unchanged -> `changed: false` with the panel's words, PaintLost -> error with its words (without
"Switch to Pro mode": "the painted area could not be carried onto the remeshed model, so nothing was changed").
Answer: per part facets before/after, `texture_paint` kept or dropped. Golden; reference. Commit `mcp:
prepare_texture_mesh remeshes, subdivides and smooths for texture displacement`.

---

## T10. The other paint tools learn texture masks (2.5 h, chain 8)

- `get_object_paint`: `mode` enum gains `texture` with `texture_layer` (optional: every layer slot that has a layer);
  without `mode`, the answer adds `texture: {layers: [{layer, painted_facets, coverage_percent}]}` per volume beside the
  four modes. `resolve_paint_modes` replaces T2's refusal.
- `clear_object_paint`: `mode: texture` clears one `texture_layer`'s mask, or all eight without it (layers kept); without
  `mode` it still clears only the four paint channels (texture masks are not print paint), and the description says so.
- `pick_facet`: the hit facet's `texture_layers`: slots whose mask paints it (`texture_displacement_facet(s)
  .get_triangle_as_string(facet)` not empty; one state, so non-empty means painted or partly painted).
- `remap_paint` stays colour-only; its description says layer reordering is `set_texture_displacement` action move.

Golden; reference updates for the four. Commit `mcp: the paint tools read, clear and pick texture paint`.

---

## T11. Texture state in `get_scene_info`, `get_object_info`, `load_model` (2.5 h, chain 9)

- `model_object_summary_json` (OrcaMCPCommon.cpp; used by `get_scene_info` and `load_model`'s `loaded_objects`): adds
  `texture_layers` (count on the owner) and `needs_bake` (`needs_bake_fast`, T1: no selector per object), and
  `model_uv` (some model part has `has_texture_uv()`).
- `volume_row_json` (OrcaMCPPartEdits.cpp; `get_object_info`'s volumes): `model_uv`, `texture_layers` (owner only),
  `texture_paint_layers` (slots with a mask).
- `get_scene_info` and `get_object_info` descriptions name the fields.
- `docs/tools/reference.md`: the `get_scene_info` example (after `<!-- get_scene_info example`) gains the keys per object;
  `test_mcp_scene_description.cpp` (`[McpSceneDescription]`) then holds them; run it.

Golden. Commit `mcp: scene and object info say which objects carry texture layers`.

---

## T12. Mesh edits report the texture paint they drop (3 h, group C after T4)

Upstream keeps none of it (correction 12), so report, never silently lose. In each handler, `TextureState before =
texture_state_of(object)` (T1) before the change and `texture_changes_json(before, after objects, owner)` after; when
not empty, the answer gets `texture_changes` and one `info_messages` line ("object N lost the paint of texture layers 0
and 2 on part 1: the mesh changed, and texture paint is not carried across; paint it again with paint_object mode
texture"; for a reload of the owner: "its texture layers were lost with the part").
Tools: `repair_mesh` (both scopes, OrcaMCPMeshTools.cpp), `split_object` and `merge_parts` (OrcaMCPPartTools.cpp),
`cut_object` (OrcaMCPServer.cpp ~5916), `reload_from_disk` and `replace_volume_with_file` (OrcaMCPSourceFileTools.cpp),
and the stack-moving ones: `set_volume_type`, `delete_object` with `volume_id`, `assemble_objects` (`stacks_unused`).
No description or schema change (no golden); the reference's response tables are updated in T14.

Tests: T1 covers the json; add one per tool kind? The decisions are pure; live check T15 covers a repair and a split.
Commit `mcp: mesh edits say which texture paint they dropped`.

---

## T13. Textured OBJ UVs and the project round trip (2.5 h, group C after A)

New `tests/slic3rutils/test_mcp_texture_files.cpp` (`[McpTexture][3mf]`), a fixture `tests/data/textured_cube/
cube.obj`, `cube.mtl`, `cube.png` (8 faces of a cube with vt per corner, one material with `map_Kd`; small, synthetic):
- `Model::read_from_file` of the OBJ gives the volume `has_texture_uv()` with V flipped, and `new_layer(slot,
  owner_has_uv)` picks ModelUV; the same OBJ without its MTL gives no UV (correction 13, documented).
- A layer made with T1's functions (depth 1.25, tile 8, rotation 30, offset, blend subtract, cylindrical, edge fade 0.3,
  colour on, the built-in Colour Bricks image), a texture mask through `build_paint_write` / `apply_paint_data(
  PaintChannel::texture(slot))`, `store_bbs_3mf` / `load_bbs_3mf` as `tests/libslic3r/test_3mf.cpp`'s `store_project` /
  `load_project` (lines 436-485): every field equal (`WithinAbs`), image bytes equal, the mask reads back `{1: 4}`, and
  the UV channel is gone after the reload (pin today's behaviour, so a change is noticed).

Docs (T14 writes them): the `load_model` reference notes that an OBJ with an MTL keeps its UVs for `projection:
model_uv`, and that a saved project does not.

---

## T13b. The model's UVs in the 3MF, offered upstream (5 h, group C after T13)

The user's decision (2026-10-24): a saved project keeps each part's UV channel (`ModelVolume::texture_uv`), so a
`model_uv` layer survives a save and reload, and the change is offered upstream as a pull request. So write it as
upstream would accept it: in `src/libslic3r/Format/bbs_3mf.cpp` only (plus a test), no MCP code, no fork-only names.
- Prefer the 3MF Materials and Properties extension's own form (`m:texture2dgroup` of `m:tex2coord` u/v, triangles
  naming `pid`/`p1`/`p2`/`p3`) when the writer and reader can carry it without breaking the triangle splitting
  `bbs_3mf` does for its sub-objects; else a per-triangle attribute beside `paint_color` and the texture masks (three
  u,v pairs per triangle, `.9g` like the vertices). Decide by reading the code, and say why in the commit message.
- A file without UVs loads as before; a reader that does not know the attribute ignores it (check that an unchanged
  upstream build loads the new file: the reader must not refuse unknown attributes there). Write UVs only for a part
  that `has_texture_uv()`.
- V is stored as the volume holds it (already flipped at OBJ load); the reload's `has_texture_uv()` is true and every
  pair equal (`WithinAbs(1e-6)`).
- Change T13's pinned assertion ("the UV channel is gone after the reload") to "kept"; add a case with two parts, one
  with UVs and one without; and a mesh whose facets the writer reorders or splits, if it does.
- Docs (T14): `load_model` and `load_project` notes say a saved project keeps the UVs. No carried-fix probe: it is a
  feature; record it in CLAUDE.md's carried list as item "BZ ... (a feature: 0 = keep ours)" with a probe that greps
  upstream's bbs_3mf.cpp for the attribute name, so a sync notices when upstream merges it.
- Commit it as its own commit touching only bbs_3mf.cpp, its test and CLAUDE.md, so it cherry-picks cleanly onto
  upstream for the PR (T16).

## T16. The upstream pull request (1 h, after T15)

As PR OrcaSlicer/OrcaSlicer#16774 was done (merged 2026-10): in a separate worktree, a branch on `okets/OrcaMCP`
(e.g. `feat/3mf-texture-uv`) from `upstream/main` with one commit, T13b's bbs_3mf.cpp and test change only (not
CLAUDE.md); build and run its test there; push the branch to `origin`. The body follows the memory note
carry-fixes-no-upstream-prs: a friendly opening (what OrcaMCP is, link https://github.com/okets/OrcaMCP, that we follow
OrcaSlicer's releases closely, asking what upstream thinks), what a user sees (model-UV texture layers fall back to
triplanar after save and reopen), the cause and the change in plain words (file and function on one "Where:" line; the
format chosen and why; older builds ignore it), then a Tests section. Show the user the title and body before
`gh pr create -R OrcaSlicer/OrcaSlicer --head okets:<branch>`.

## T14. Docs, counts and tables (3 h, last)

- Golden re-run; `get_server_info`'s default under 6 KB (eight names and a category add ~200 bytes).
- Server instructions (`OrcaMCPServerInfo.cpp:562`): "- Painting: paint_object (color, support, seam, fuzzy_skin)." ->
  "...fuzzy_skin, texture)." and one line "- Texture displacement: set_texture_displacement, then paint_object (mode
  texture), then bake_texture_displacement." (the order is the footgun: nothing prints before the bake). Under 1,600.
- Counts 103 -> 111 in the app, 107 -> 115 reachable: CLAUDE.md (Transport row, "### MCP Tools (111 in the app, 115
  reachable)", "The registry holds 115: the app serves 111"), README.md (lines 23, 198, 237), docs/README.md (12, 40,
  64-65), docs/setup/configuration.md (49); re-run CLAUDE.md's grep.
- CLAUDE.md: a **Texture displacement** row in the tool table (each tool, one clause each, as the other rows); Painting
  row (`paint_object` texture and visible, panels closed in every mode; `get_object_paint` / `clear_object_paint` /
  `pick_facet` texture); Scene and Models rows (`texture_layers`, `needs_bake`, `model_uv`; `texture_changes` on mesh
  edits); a "Texture bake" paragraph under Threading Model (capture / plan / apply, one undo step, the mixed slots);
  Key Files rows (`OrcaMCPTextureDisplacement.cpp`, `OrcaMCPTextureTools.cpp`, `GUI/TextureDisplacementObject.cpp`);
  "Endpoints with Dialog Suppression": a Texture displacement row (`set_texture_displacement`'s image reads,
  `bake_texture_displacement`, `prepare_texture_mesh`); probes BW, BX are T0's.
- README: the tool table and category list; a prompt example ("Give the handle a knurled texture, only where I hold
  it, and bake it"). docs/README.md categories. `docs/tools/workflows.md`: "Texture a part" (library -> layer -> paint
  -> get -> bake -> slice -> render -> save).
- Reference: T12's `texture_changes` in the six tools' responses; next-steps table rows for `texture_paint_next_steps`
  and `bake_next_steps`.
- Run `$T "[orcamcp]"`, the upstream texture suites and the Python tests. Commit `mcp: texture displacement in the docs`.

---

## T15. Live check: the whole chain (5 h, last)

Through MCP on a test instance that cannot reach the user's windows, data or printer.

1. **Port and processes.** `lsof -nP -iTCP -sTCP:LISTEN | grep -E ':136(1[89]|2[0-7])'`, `pgrep -x OrcaSlicer; pgrep -x
   OrcaMCP`. Note what listens; never call a port you did not start. The user's app stays untouched.
2. **Data copy.** `S=<session scratchpad>/texdisp`; copy `~/Library/Application Support/OrcaMCP/` to `$S/data`; sanitize
   before launch: blank `print_host`, `printhost_apikey`, `flashforge_serial_number`, any Obico token in
   `user/*/machine/*.json`; delete `user_backup-*`; drop `local_machines` and `user_last_selected_machine` from
   `OrcaMCP.conf`. Report key names only.
3. **Fixtures** in `$S/fixtures` from a script in `$S/scripts`: a 30 mm cube STL, two stacked cubes as one OBJ of two
   objects (multi-part with `load_model multipart: merge`), the T13 textured OBJ, a 64x64 colour PNG and a grayscale PNG.
4. **Launch your build only**: `open -n -a /Users/hanan/Projects/OrcaMCP/build/arm64/src/RelWithDebInfo/OrcaSlicer.app
   --env ORCAMCP_SKIP_CLOUD_LOGIN=1 --env ORCAMCP_INSTANCES_DIR=$S/instances --args --datadir $S/data` (retry on
   LaunchServices -600). Wait for `$S/instances/<pid>.json`, read its `port`, check the log is under `$S/data/log/`. If
   either fails, quit it and stop.
5. **Drive it through the bridge**, pinned: `claude -p --strict-mcp-config --mcp-config $S/mcp.json` with the bridge
   (`python3 /Users/hanan/Projects/OrcaMCP/scripts/orcamcp-bridge.py`) given `ORCAMCP_PORT=<port>`,
   `ORCAMCP_INSTANCES_DIR=$S/instances`, `ORCAMCP_TIMEOUT=600` (the -O0 bake); fallback a stdio JSON-RPC driver in
   `$S/scripts` starting the same bridge. Never curl the app. First `get_server_info`: `server.instance.pid` is yours.
6. **Library**: 44 built-in, 6 `color: true`; import the colour PNG twice through `set_texture_displacement file_path`:
   the second answers `reused: true` and no second file appears; `delete_user_texture` of a built-in is refused.
7. **Layers** on the cube: add (slot 0, first texture, next step paint_object), add Colour Bricks with `color: true`,
   `blend: subtract`; edit slot 0 `blend: multiply` -> `inactive_settings` names it; same values -> `changed: false`;
   `undo` reverts one call; refusals: colour on grayscale, `model_uv` on the STL, `depth_mm: 11`; `projection: view,
   view_direction: top`; a `projection_frame` over the top face; `adjust_image {contrast: 0.5}` -> a new user texture;
   move slot 1 to position 0 -> masks and slots swapped; fill to 8 and a ninth refused; remove one.
8. **Paint**: `paint_object` texture box on the top face (coverage about 16.7 %), `visible` from front, texture layer
   without a layer refused; `get_object_paint` without mode lists the texture paint; `pick_facet` on the top reports the
   layer; `clear_object_paint mode: texture, texture_layer: 1`.
9. **Unwrap**: a layer with `projection: unwrap` painted on the whole cube; `get_texture_unwrap` lists 6 charts (cube
   faces at the 30 degree seam angle); join two, move one, a seam path; `get_texture_unwrap` again.
10. **Bake**: `slice_all` + `wait_for_slice` first, then paint texture: the plate is still sliced. Then
    `bake_texture_displacement` on the cube: `parts[0].facets_after` far above 12, the object on the bed
    (`on_bed: true`), `needs_bake: false`, the colour layer's `mixed_filaments` listed when the project has two or more
    filaments (add a slot first with `add_filament_slot` if the printer allows); `undo` restores 12 facets; redo; bake
    the two-part object with paint on both parts: both parts baked; bake the textured OBJ with a `model_uv` layer.
    `pipeline: classic` once on a small cube. `prepare_texture_mesh remesh` on another cube.
11. **Slice and render**: `slice_all`, `wait_for_slice`, `get_print_estimate` (more time than before the bake);
    `render_plate_view` of the baked object, `uniform_image: false`.
12. **Save and reload**: `export_3mf $S/out/tex.3mf`, `new_project`, `load_project`, `get_texture_displacement` on each
    object: layers, settings, textures and paint as before (the OBJ's `model_uv` now in `inactive_settings`: no UV after
    the reload, correction 13).
13. **Mesh edits**: paint a texture layer, `repair_mesh` or `split_object` -> `texture_changes.paint_lost` names it.
14. **Open panel path**: not drivable without clicks; leave it to the audit's code reading. Do not capture the screen.
15. **Quit** with `quit_app`; the process gone, `$S/instances` empty. Never kill another pid.

Report every answer that disagreed with this plan. Fix bugs in this increment's code; report others.

---

## Risks

- **T0 moves touch upstream's newest and largest panel** (8,900 lines, changing every sync): merges will conflict; the
  BW/BX probes find them, and the moved functions keep upstream's bodies verbatim so a re-move is mechanical.
- **Bake time on the -O0 build and on large meshes** can exceed the call's wait: nothing is applied and the answer says
  how to raise `ORCAMCP_TIMEOUT` or lower the detail; the release build is 20-70x faster.
- **The colour bake's mixed filament slots survive an undo**, as in the app (slot changes are no undo step). Reported
  with `undo_warning`.
- **A 3MF drops the model's UV channel**: ModelUV layers fall back to Triplanar after a save and reload, in the app too.
  Fixing it is a project format change (decision 2).
- **Texture paint and whole stacks are lost by mesh edits**, upstream too; MCP now reports it but does not keep it.
- **Orphaned stacks** after part-type changes, deletes and assembles: reported, not moved.
- **Undo memory**: every snapshot of a textured part carries its layers' images (a few MB each), as the panel's do.
- **`visible` is MCP's own occlusion test** (AABB rays from facet centroids), not the panel's screen raycaster: results
  can differ at grazing angles; orthographic named views are exact, `current` under perspective approximates.
- **The projection frame is orthographic** (an explicit plate frame), where the panel's is the camera's perspective
  frustum through a window; perspective frames loaded from projects are reported and kept, not editable.
- **A UI job blocks texture calls**: with the panel open and its live preview computing, a call is refused with the
  arrange/orient wording of `ui_job_busy_message` before it could close the panel. A retry passes.
- **Closing paint panels in every `paint_object` mode** changes existing behaviour: an agent painting colour closes the
  user's open colour panel (its own "Leaving ..." undo step, as when the user closes it).
- **Unwrap chart ids renumber** on any seam or angle change: island edits are refused in a call that also re-segments,
  and every answer says the ids hold for that unwrap only.
- **No live coverage of the open-panel path** (T15 step 14).
- **Image identity reuse** compares decoded pixels: two files that look identical but differ by one pixel are two
  textures; a texture edited outside the app keeps its old cached identity until its size or mtime changes.

## Decisions for the user

1. **Other formats' UVs.** Upstream keeps model UVs only from an OBJ with an MTL. GLB, GLTF and FBX UVs reach only the
   colour importer, and an OBJ with `vt` but no MTL keeps none. Keeping them for `model_uv` would be a fork change to
   `Model::read_from_file` (about 3 h with tests and a probe). Decided 2026-10-24: not done (match the app), documented.
2. **UVs in the project file.** Decided 2026-10-24: done (T13b), and offered upstream as a PR (T16).
3. **The bake's mixed filament slots.** Decided 2026-10-24: created as the panel creates them, listed, with
   `undo_warning`.
