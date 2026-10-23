// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"

#include <nlohmann/json.hpp>

#include "OrcaMCPPaintModel.hpp"

// The decisions of the texture displacement tools, apart from the app: names, the library by id, one call's
// layer edit (read, refused, applied), the layer masks as reports, what a mesh edit dropped, the unwrap
// request and the answers' shapes. Owner lookups are the texture panel's (TextureDisplacementObject.hpp)
// and come in as arguments. No wx, no wxGetApp, no texture_library(); file I/O only where marked.
namespace Slic3r { namespace GUI { namespace OrcaMCP {

// ---- Names ----

// triplanar cylindrical spherical unwrap view model_uv (the enum's order).
const std::vector<std::string>&        projection_names();
std::optional<TextureProjectionMethod> projection_named(const std::string&);
std::string                            projection_name(TextureProjectionMethod);
// add override subtract multiply divide (the panel's combo order).
const std::vector<std::string>& blend_names();
std::optional<TextureBlendMode> blend_named(const std::string&);
std::string                     blend_name(TextureBlendMode);
// repeat mirrored_repeat
const std::vector<std::string>& tile_method_names();
std::optional<TextureTileMethod> tile_method_named(const std::string&);
std::string                      tile_method_name(TextureTileMethod);
// current iso top bottom front back left right
const std::vector<std::string>& view_direction_names();
// builtin | user | pack
const char* texture_source_name(TextureSource);

// ---- The library, by id ----

// "builtin/Bark.png", "user/My bricks.png", "pack/<pack name>/<file name>": the file name with its
// extension. Ids still equal get " #2", " #3" in library order.
std::vector<std::string> texture_ids(const std::vector<TextureLibraryEntry>&);

struct TextureLookup
{
    int         index = -1;
    std::string error;
};
// An id, or a stem (any case) exactly one entry has; ambiguous lists the ids; unknown points at
// get_texture_library.
TextureLookup find_texture(const std::vector<TextureLibraryEntry>&, const std::vector<std::string>& ids, const std::string& query);
// The entry a layer's image came from, by path.
std::optional<int> library_index_of(const std::vector<TextureLibraryEntry>&, const TextureDisplacementLayer&);

// A PNG's colour as decode_height_texture reads it, from its header: false for an 8-bit grayscale, true for
// the 8-bit RGB and RGBA it takes as colour, nullopt for anything it cannot decode (a jpg in the user folder,
// a palette, grayscale-with-alpha or 16-bit PNG).
std::optional<bool> png_has_color(const unsigned char* bytes, std::size_t size);
// I/O: the first 33 bytes of the file.
std::optional<bool> png_color_from_file(const std::string& path);
// {id, name, source, pack?, file, path, color (true / false / null: not read)}
nlohmann::json library_entry_json(const TextureLibraryEntry&, const std::string& id, std::optional<bool> color);

// Identity of an image for the import's reuse: width, height, the height pixels, and the colour pixels when
// some pixel is not grey. Two encodings of one picture (an RGB PNG of grey pixels and its grayscale PNG, a
// jpg and its PNG) are the same texture; a hash match is confirmed by comparing the decoded data.
struct TextureIdentity
{
    std::uint64_t hash   = 0;
    int           width  = 0;
    int           height = 0;
};
TextureIdentity texture_identity(const DecodedHeightTexture&);
bool            same_texture(const DecodedHeightTexture&, const DecodedHeightTexture&);

// ---- One call's layer edit ----

struct ImageAdjustRequest
{
    std::optional<float> brightness, contrast, sharpness, blur, hue_deg;
};
// An orthographic frame through which the texture is projected once.
struct ProjectionFrameRequest
{
    std::optional<std::string> view;       // a named view (view_direction_names), or right/up
    std::optional<Vec3d>       right, up;  // plate axes, unit after normalising, not parallel
    Vec3d                      center = Vec3d::Zero(); // plate mm
    double                     width_mm = 0, height_mm = 0; // > 0
};
// Every field optional: only what the call gives.
struct LayerSettings
{
    std::optional<float>                     depth_mm, tile_size_mm, rotation_deg, midlevel, smoothing, edge_fade_amount;
    std::optional<Vec2f>                     offset;
    std::optional<bool>                      invert, edge_fade, tile, color;
    std::optional<TextureTileMethod>         tile_method;
    std::optional<TextureProjectionMethod>   projection;
    std::optional<TextureBlendMode>          blend;
    std::optional<std::string>               view_direction;  // current | iso | top | bottom | front | back | left | right
    std::optional<std::pair<Vec3d, Vec3d>>  view_axes;       // explicit right/up in plate axes, instead of view_direction
    std::optional<ProjectionFrameRequest>    projection_frame;
    bool                                     clear_projection_frame = false; // projection_frame: false
    std::optional<ImageAdjustRequest>        adjust_image;
    std::optional<bool>                      color_mix;      // stack-wide from here
    std::optional<int>                       color_mix_count, color_denoise;
    bool empty() const;
};

enum class LayerAction { add, edit, remove, move };
struct LayerRequest
{
    LayerAction                action = LayerAction::edit;
    std::optional<int>         layer, to_position; // slot; move: 0-based position in slot order
    std::optional<std::string> texture, file_path;
    LayerSettings              settings;
};

// The bounds every number is held to, one table for read_layer_request and the schema's minimum/maximum.
// `key` is the argument's name (an adjust_image field by its own name).
struct SettingBound
{
    const char* key;
    double      min, max;
    const char* unit;        // "" when unitless
    const char* panel_label; // the panel's name for it
};
const std::vector<SettingBound>& layer_setting_bounds();
const SettingBound*              setting_bound(const std::string& key);

// Types, names, ranges and structure: action required; edit, remove and move need layer (0..7); add takes
// none (it takes the first free slot, as the panel does); remove takes nothing else; move takes to_position
// and nothing else; to_position only with move; texture and file_path not both; offset two finite numbers;
// view_direction and view_axes not both; a frame's width and height > 0, its axes not parallel and given as
// a view or as right and up; adjust_image with at least one field; projection_frame true refused (false
// clears). Returns nullopt with `error` set otherwise. `object_id` is not read.
std::optional<LayerRequest> read_layer_request(const nlohmann::json& params, std::string& error);

struct StackFacts
{
    bool owner_has_uv = false;
    int  physical_filaments = 1;
    bool texture_has_color = false;
};
// Refusals that need the stack: no free slot; no layer in that slot (lists the slots); colour on grayscale;
// model_uv without UV; color_mix_count above max(1, 32 - physical_filaments); view_direction / view_axes when
// the projection after the call is not view; move to_position out of range; adjust_image on a layer with no
// image. `after` is the layer with the request's settings applied (for add, the new layer).
std::optional<std::string> layer_edit_refusal(int object_id, const std::vector<TextureDisplacementLayer>& stack, const LayerRequest&,
                                              const TextureDisplacementLayer& after, const StackFacts&);
std::optional<int>         first_free_slot(const std::vector<TextureDisplacementLayer>&);
// add_texture_layer's defaults: the slot, and ModelUV when the owner has a UV channel. No image.
TextureDisplacementLayer new_layer(int slot, bool owner_has_uv);
// The field mapping; true when something changed. The settings that need the model or the camera
// (view_direction, view_axes, projection_frame, adjust_image) are the caller's; clear_projection_frame is
// the panel's Clear: view_project_projective false and tile_enabled true.
bool apply_layer_settings(TextureDisplacementLayer&, const LayerSettings&);
bool apply_stack_settings(TextureDisplacementOptions&, const LayerSettings&);
TextureAdjustments adjustments_of(const ImageAdjustRequest&);
// capture_view_projection's arithmetic: the projector axes in the volume's frame from world right/up and the
// volume's transform (unit, or the x/y axis when the transform collapses one).
std::pair<Vec3f, Vec3f> view_projector_axes(const Vec3d& right_world, const Vec3d& up_world, const Transform3d& volume_to_world);
// An orthographic projection frame as the row-major 3x4 view_project_matrix in the volume's frame:
// uv = ((p - corner) . right / width, (p - corner) . down / height), row 2 = (0, 0, 0, 1), where corner is the
// frame's top left and v runs down the image, as apply_projection_frame's window does. `up` is made
// perpendicular to `right`. project_uv_projective reads it back.
std::array<float, 12> orthographic_frame_matrix(const Vec3d& right_world, const Vec3d& up_world, const Vec3d& center_world,
                                                double width_mm, double height_mm, const Transform3d& volume_to_world);
// {setting: why} for what the panel greys out but stores: tile_size_mm under model_uv, blend on the base
// layer, edge_fade_amount with fade off, tile_method with tile off, colour options with no layer colouring,
// colour on a texture without colours, placement under a projection frame, model_uv without UV.
nlohmann::json inactive_settings(const std::vector<TextureDisplacementLayer>& stack, const TextureDisplacementLayer&,
                                 const TextureDisplacementOptions&, bool owner_has_uv, bool texture_has_color);
// midlevel > 0 and depth > 1 mm: the panel's deep inward displacement warning.
std::vector<std::string> layer_warnings(const TextureDisplacementLayer&);

// ---- Masks ----

// A mask indexing a triangle the mesh no longer has: the panel treats it as unreadable.
bool mask_is_stale(const PaintData&, std::size_t facet_count);
struct MaskInput
{
    int                                 volume_id = -1;
    std::string                         name;
    std::shared_ptr<const TriangleMesh> mesh;
    std::array<PaintData, TEXTURE_DISPLACEMENT_MAX_LAYERS> masks;
};
struct MaskReport
{
    int         volume_id = -1;
    std::string name;
    bool        stale = false;
    int         painted_facets = 0;
    double      area_ratio = 0.;
};
// Any thread, state 1 only: one report per slot for each part.
std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>> mask_reports(const std::vector<MaskInput>&);
// Some layer of the stack is painted, readably, on some part.
bool needs_bake(const std::vector<TextureDisplacementLayer>&, const std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>>&);
// The cheap form for every-object reports: FacetsAnnotation::empty() per slot, no selector.
bool needs_bake_fast(const ModelObject&, const ModelVolume* owner);

// ---- What a mesh edit dropped ----

struct TextureState
{
    struct Part
    {
        ObjectID         id;
        int              volume_id = -1; // position among the object's volumes
        std::string      name;
        std::vector<int> painted_slots;
        int              layers = 0;     // the part's own stack
    };
    std::vector<Part> parts;
};
TextureState texture_state_of(const ModelObject&);
// {paint_lost: [{volume_id, name, layers: [slots]}], layers_lost: n, stacks_unused: [{volume_id, layers}]}
// comparing `before` with the objects after (one for most edits, several for a split). A part no longer found
// lost all its paint and layers. stacks_unused: a stack on a part that is not its object's owner (`owner_after`
// for the first object). An empty object when nothing was lost.
nlohmann::json texture_changes_json(const TextureState& before, const std::vector<const ModelObject*>& after, const ModelVolume* owner_after);

// ---- The unwrap request ----

struct IslandEdit
{
    int                  chart = 0;
    std::optional<Vec2f> offset;
    std::optional<float> rotation_deg, scale;
};
struct UnwrapRequest
{
    std::optional<float> seam_angle_deg;       // 5..90 (SetSeamAngle's clamp, refused outside)
    std::optional<float> island_padding_mm;    // -1 auto, else >= 0
    std::optional<bool>  auto_connect_islands;
    std::vector<std::pair<int, int>>                 add_seams, remove_seams; // mesh vertex pairs
    std::vector<std::pair<Vec3d, Vec3d>>             seam_paths;              // plate mm, snapped to the nearest vertices
    std::vector<int>                                 cut_islands, unjoin;
    std::vector<std::pair<int, std::optional<int>>>  join;                    // chart, to (nullopt: the nearest neighbour)
    std::vector<IslandEdit>                          islands;
    std::vector<std::pair<int, Vec2f>>               uv_edits;                // unwrapped vertex, raw unwrap mm
    bool clear_seams = false, clear_uv_edits = false, average_scale = false, relayout = false;
};
// The shapes: add_seams / remove_seams [[v, v]], seam_paths [{from: [x,y,z], to: [x,y,z]}], cut_islands /
// unjoin [chart], join [{chart, to?}], islands [{chart, offset?, rotation_deg?, scale?}], uv_edits
// [{vertex, uv: [u, v]}]; island scale 0.001..1000 (the panel's clamp).
std::optional<UnwrapRequest> read_unwrap_request(const nlohmann::json& params, std::string& error);
// {chart_count, vertex_count, triangle_count, seam_angle_deg, island_padding_mm, auto_connect_islands, seams,
// islands [{chart, centroid, offset, rotation_deg, scale, group}], uv_edits, vertices? [{vertex, chart, uv}]}
nlohmann::json unwrap_json(const PatchUnwrap&, const TextureDisplacementLayer&, bool include_vertices);

// ---- Answers ----

// A layer's texture as the library knows it: the id, and the source (builtin, user, pack, or "project" for an
// image only the project holds).
struct LayerTexture
{
    std::optional<std::string> id;
    std::string                source = "project";
};
// {layer, position, base, texture {texture (id or null), name, source, path}, color_capable, depth_mm,
// tile_size_mm, rotation_deg, offset, invert, midlevel, smoothing, edge_fade, edge_fade_amount, tile,
// tile_method, projection, blend, color, view {right, up} (view), projection_frame {active} (view with a
// frame), unwrap {seam_angle_deg, island_padding_mm, auto_connect_islands, seams, placed_islands, uv_edits}
// (unwrap), paint [{volume_id, name, painted, stale, painted_facets, coverage_percent}], painted,
// inactive_settings, warnings}
nlohmann::json layer_json(const TextureDisplacementLayer& layer, int position, bool base, const LayerTexture& texture,
                          std::optional<bool> color_capable, const std::vector<MaskReport>& paint, bool owner_has_uv,
                          const std::vector<TextureDisplacementLayer>& stack, const TextureDisplacementOptions& options);
// {pipeline (default | classic), displace_border, smooth {enabled, strength (percent), passes,
// ignore_outer_ring}, resolution_mm (null = auto), budget_k (null = auto), color_mix, color_mix_count,
// color_denoise}
nlohmann::json stack_options_json(const TextureDisplacementOptions&);

}}} // namespace Slic3r::GUI::OrcaMCP
