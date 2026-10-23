// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureTools.cpp
//
// The texture displacement tools: the library of textures a layer can use, and (further tools) the layer stack,
// the unwrap, the bake. Their decisions apart from the app are OrcaMCPTextureDisplacement.cpp's; the owner of an
// object's stack, its parts and the bake frame are the texture panel's own functions over the model
// (TextureDisplacementObject.hpp), so a tool and the panel act on the same data.
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPNextSteps.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPPartTools.hpp"
#include "OrcaMCPTextureDisplacement.hpp"
#include "OrcaMCPUserView.hpp"

#include "slic3r/Utils/FixModelByCgal.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/log.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using Slic3r::DecodedHeightTexture;

namespace {

// The library as the picker lists it, copied on the main thread: texture_library() is a cache the panel and
// the importers rewrite there. No rescan, as the picker does not rescan.
struct LibraryCopy
{
    std::vector<Slic3r::GUI::TextureLibraryEntry> entries;
    std::vector<std::string>                      ids;
    std::string                                   user_folder;
};

LibraryCopy library_copy()
{
    LibraryCopy copy;
    copy.entries     = texture_library();
    copy.ids         = texture_ids(copy.entries);
    copy.user_folder = user_texture_dir();
    return copy;
}

nlohmann::json get_texture_library(const nlohmann::json& params)
{
    std::string                error;
    const std::optional<std::string> source = read_text(params, "source", error);
    if (!error.empty())
        return error_response(error);

    LibraryCopy library;
    run_on_main_thread([&library]() -> nlohmann::json {
        library = library_copy();
        return {{"status", "success"}};
    });

    // Each file's colour is read from its header, off the main thread.
    std::vector<std::optional<bool>> colors;
    colors.reserve(library.entries.size());
    for (const Slic3r::GUI::TextureLibraryEntry& entry : library.entries)
        colors.push_back(png_color_from_file(entry.path));

    nlohmann::json answer = library_list_json(library.entries, library.ids, colors, source, error);
    if (!error.empty())
        return error_response(error);
    answer["status"]      = "success";
    answer["user_folder"] = library.user_folder;
    return answer;
}

// Every layer, on any object, whose image is the file at `path`: what keeps working after the file is deleted.
nlohmann::json layers_using(const Slic3r::Model& model, const std::string& path)
{
    nlohmann::json users = nlohmann::json::array();
    for (size_t i = 0; i < model.objects.size(); ++i) {
        const Slic3r::ModelVolume* owner = texture_stack_owner(*model.objects[i]);
        if (owner == nullptr)
            continue;
        for (const Slic3r::TextureDisplacementLayer& layer : owner->texture_displacement_layers)
            if (layer.path == path)
                users.push_back({{"object_id", i}, {"layer", layer.slot}});
    }
    return users;
}

nlohmann::json delete_user_texture(const nlohmann::json& params)
{
    const std::string query = params.at("texture").is_string() ? params.at("texture").get<std::string>() : std::string();
    if (!params.at("texture").is_string() || query.empty())
        return error_response("texture must be a texture id from get_texture_library");

    return run_on_main_thread([&query]() -> nlohmann::json {
        McpDialogSuppressionGuard guard;
        const LibraryCopy         library = library_copy();
        const TextureLookup       found   = find_texture(library.entries, library.ids, query);
        if (found.index < 0)
            return error_response(found.error);
        const Slic3r::GUI::TextureLibraryEntry& entry = library.entries[size_t(found.index)];
        const std::string&                      id    = library.ids[size_t(found.index)];
        if (const auto refusal = user_texture_delete_refusal(entry, id))
            return error_response(*refusal);

        const nlohmann::json still_used_by = layers_using(wxGetApp().plater()->model(), entry.path);
        std::string          error;
        if (!remove_user_texture(entry.path, error))
            return guard.fail_on_errors(error_response(error));

        nlohmann::json answer = {{"status", "success"},
                                 {"deleted", {{"texture", id}, {"name", entry.name}, {"path", entry.path}}},
                                 {"still_used_by", still_used_by}};
        if (!still_used_by.empty())
            answer["note"] = "Those layers keep the image: they hold it themselves, and a saved project stores it. The texture is gone "
                             "from the library, so a layer given another texture cannot go back to it";
        return guard.report(std::move(answer));
    });
}


// ---- Images: reading, and reusing what the library already holds ----

struct ImageRead
{
    std::shared_ptr<std::vector<unsigned char>> data;
    DecodedHeightTexture                        decoded;
};

// The image at `path` as a layer would hold it (converted to a PNG when it is another format), decoded.
std::optional<ImageRead> read_image(const std::string& path, std::string& error)
{
    ImageRead out;
    out.data = load_texture_image_data(path, error);
    if (!out.data)
        return std::nullopt;
    Slic3r::TextureDisplacementLayer probe;
    probe.image_data = out.data;
    out.decoded      = Slic3r::decode_height_texture(probe, /*keep_decoded=*/false);
    if (out.decoded.empty()) {
        error = path + " is not an image a texture layer can use (a grayscale or RGB picture)";
        return std::nullopt;
    }
    return out;
}

// What a library file decoded to is cached by (size, mtime) so a second look at the library reads no file.
struct CachedIdentity
{
    std::uintmax_t           size  = 0;
    std::filesystem::file_time_type mtime;
    TextureIdentity          identity;
    bool                     readable = false;
};

std::map<std::string, CachedIdentity>& identity_cache()
{
    static std::map<std::string, CachedIdentity> cache; // main thread only
    return cache;
}

// The library texture whose decoded image is `wanted`: same size and pixels, and the same colours (or none).
std::optional<Slic3r::GUI::TextureLibraryEntry> find_identical_texture(const DecodedHeightTexture& wanted)
{
    const TextureIdentity id = texture_identity(wanted);
    for (const Slic3r::GUI::TextureLibraryEntry& entry : texture_library()) {
        std::error_code ec;
        const std::filesystem::path file = std::filesystem::u8path(entry.path);
        const auto size  = std::filesystem::file_size(file, ec);
        const auto mtime = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(file, ec);
        if (ec)
            continue;
        CachedIdentity& cached = identity_cache()[entry.path];
        if (cached.size != size || cached.mtime != mtime || (!cached.readable && cached.size == 0)) {
            std::string error;
            const auto  read = read_image(entry.path, error);
            cached           = CachedIdentity{size, mtime, read ? texture_identity(read->decoded) : TextureIdentity{}, read.has_value()};
        }
        if (!cached.readable || cached.identity.hash != id.hash || cached.identity.width != id.width || cached.identity.height != id.height)
            continue;
        std::string error;
        if (const auto read = read_image(entry.path, error); read && same_texture(read->decoded, wanted))
            return entry;
    }
    return std::nullopt;
}

struct LibraryImage
{
    Slic3r::GUI::TextureLibraryEntry            entry;
    std::shared_ptr<std::vector<unsigned char>> data;
    bool                                        reused = false;
};

std::optional<LibraryImage> library_image(const Slic3r::GUI::TextureLibraryEntry& entry, bool reused, std::string& error)
{
    LibraryImage out;
    out.entry  = entry;
    out.reused = reused;
    out.data   = load_texture_image_data(entry.path, error);
    if (!out.data)
        return std::nullopt;
    return out;
}

// An image file given to a layer: the identical texture the library holds, else imported into it.
std::optional<LibraryImage> import_or_reuse(const std::string& path, const DecodedHeightTexture& decoded, std::string& error)
{
    if (const auto same = find_identical_texture(decoded))
        return library_image(*same, true, error);
    const auto entry = import_texture_to_library(path, error);
    if (!entry)
        return std::nullopt;
    return library_image(*entry, false, error);
}

// An edited image (adjust_image): the identical texture the library holds, else saved into it.
std::optional<LibraryImage> save_or_reuse(const DecodedHeightTexture& image, const std::string& stem, std::string& error)
{
    if (const auto same = find_identical_texture(image))
        return library_image(*same, true, error);
    const auto entry = save_texture_to_library(image, stem, error);
    if (!entry)
        return std::nullopt;
    return library_image(*entry, false, error);
}

// ---- Where a view projects from ----

using Axes = std::pair<Slic3r::Vec3d, Slic3r::Vec3d>; // right, up in plate axes

// A named view of the 3D view's cameras, or `current`: the camera now.
Axes view_axes_of(const std::string& name)
{
    Slic3r::GUI::Camera        local;
    const Slic3r::GUI::Camera* camera = &local;
    if (name == "current")
        camera = &wxGetApp().plater()->get_camera();
    else
        local.select_view(camera_named(name)->direction);
    const Slic3r::Matrix3d rotation = camera->get_view_matrix().matrix().block<3, 3>(0, 0);
    return {rotation.row(0).transpose(), rotation.row(1).transpose()};
}

bool near(const Slic3r::Vec3f& a, const Slic3r::Vec3f& b) { return (a - b).norm() < 1e-5f; }

// ---- One call's edit of a stack ----

// What the edit will do, worked out from the model with nothing changed.
struct EditPlan
{
    Slic3r::ModelObject*                     object = nullptr;
    Slic3r::ModelVolume*                     owner  = nullptr;
    int                                      object_id = -1;
    LayerRequest                             request;
    int                                      slot = -1;                   // the layer edited, or the slot an add takes
    std::optional<Slic3r::TextureDisplacementLayer> before;               // edit, remove, move
    Slic3r::TextureDisplacementLayer         after;                       // the layer as it will be (image placeholders apply later)
    Slic3r::TextureDisplacementOptions       options;                     // the stack's options as they will be
    std::optional<ImageRead>                 picked;                      // texture: the library entry's image
    std::optional<Slic3r::GUI::TextureLibraryEntry> picked_entry;
    std::optional<ImageRead>                 file;                        // file_path: the candidate image
    std::optional<DecodedHeightTexture>      adjusted;                    // adjust_image: the image after it
    std::string                              adjust_stem;
    bool                                     image_changed = false;
    bool                                     changed       = false;
    int                                      move_from = 0, move_to = 0;
    std::string                              snapshot;
};

const Slic3r::TextureDisplacementLayer* layer_in_slot(const std::vector<Slic3r::TextureDisplacementLayer>& stack, int slot)
{
    for (const Slic3r::TextureDisplacementLayer& layer : stack)
        if (layer.slot == slot)
            return &layer;
    return nullptr;
}

// The layer's own image, as the file or library held it (no smoothing, no panel adjustment).
DecodedHeightTexture raw_image_of(Slic3r::TextureDisplacementLayer layer)
{
    layer.smoothing      = 0.f;
    layer.preview_adjust = Slic3r::TextureAdjustments{};
    return Slic3r::decode_height_texture(layer, /*keep_decoded=*/false);
}

// Resolves the call's texture (an id, a file, or for an add the library's first) into `plan`. Why not, or nothing.
std::optional<std::string> resolve_image(const LibraryCopy& library, EditPlan& plan)
{
    const LayerRequest& request = plan.request;
    std::string         error;
    if (request.file_path) {
        const std::filesystem::path file = std::filesystem::u8path(*request.file_path);
        std::error_code             ec;
        if (!std::filesystem::is_regular_file(file, ec))
            return "file_path " + *request.file_path + " is not a file";
        plan.file = read_image(*request.file_path, error);
        if (!plan.file)
            return error;
        return std::nullopt;
    }
    std::string query;
    if (request.texture)
        query = *request.texture;
    else if (request.action == LayerAction::add) {
        if (library.entries.empty())
            return "The texture library is empty, so a new layer has no texture: give file_path an image";
        plan.picked_entry = library.entries.front();
    }
    if (!request.texture && !plan.picked_entry)
        return std::nullopt;
    if (request.texture) {
        const TextureLookup found = find_texture(library.entries, library.ids, query);
        if (found.index < 0)
            return found.error;
        plan.picked_entry = library.entries[size_t(found.index)];
    }
    plan.picked = read_image(plan.picked_entry->path, error);
    if (!plan.picked)
        return "The texture " + plan.picked_entry->name + " could not be read: " + error;
    return std::nullopt;
}

// The image the call gives the layer, if any.
const DecodedHeightTexture* given_image(const EditPlan& plan)
{
    if (plan.file)
        return &plan.file->decoded;
    if (plan.picked)
        return &plan.picked->decoded;
    return nullptr;
}

// Everything the call decides, from the model as it is. Main thread; changes nothing.
std::optional<std::string> plan_edit(const nlohmann::json& params, const LayerRequest& request, EditPlan& plan)
{
    if (Slic3r::cgal_repair_dialog_running())
        return "The app is running its own mesh repair, so no texture layer was changed: call again once it has finished";
    Slic3r::GUI::Plater* plater = wxGetApp().plater();
    std::string          error;
    plan.request = request;
    plan.object  = resolve_object_id(params, plater->model(), plan.object_id, error);
    if (plan.object == nullptr)
        return error;
    if (const auto refusal = edit_job_refusal(!plater->get_ui_job_worker().is_idle(), "set_texture_displacement"))
        return refusal;
    plan.owner = texture_stack_owner(*plan.object);
    if (plan.owner == nullptr)
        return "Object " + std::to_string(plan.object_id) + " has no model part, so it cannot hold texture layers";

    const std::vector<Slic3r::TextureDisplacementLayer>& stack = plan.owner->texture_displacement_layers;
    plan.options = plan.owner->texture_displacement_options;
    if (request.layer)
        if (const auto* found = layer_in_slot(stack, *request.layer))
            plan.before = *found;
    plan.slot = request.layer ? *request.layer : first_free_slot(stack).value_or(0);
    plan.after = plan.before ? *plan.before : new_layer(plan.slot, plan.owner->has_texture_uv());

    const bool edits_layer = request.action == LayerAction::add || request.action == LayerAction::edit;
    LibraryCopy library;
    if (edits_layer && (request.texture || request.file_path || request.action == LayerAction::add)) {
        library = library_copy();
        if (const auto refusal = resolve_image(library, plan))
            return refusal;
    }
    if (const DecodedHeightTexture* image = given_image(plan)) {
        const std::shared_ptr<std::vector<unsigned char>>& data = plan.file ? plan.file->data : plan.picked->data;
        plan.after.image_data = data;
        plan.after.name       = plan.picked_entry ? plan.picked_entry->name : std::filesystem::u8path(*request.file_path).stem().u8string();
        plan.after.path       = plan.picked_entry ? plan.picked_entry->path : *request.file_path;
        plan.image_changed    = !plan.before || plan.before->empty() || !same_texture(raw_image_of(*plan.before), *image);
    }

    StackFacts facts;
    facts.owner_has_uv        = plan.owner->has_texture_uv();
    facts.physical_filaments  = int(Slic3r::GUI::GLGizmoTextureDisplacement::palette_inputs(plan.options, *wxGetApp().preset_bundle,
                                                                                            plater->get_extruders_colors()).filaments.size());
    facts.color_mix_count     = plan.options.color_mix_count;
    if (const DecodedHeightTexture* image = given_image(plan))
        facts.texture_has_color = image->has_color();
    else if (!plan.after.empty())
        facts.texture_has_color = Slic3r::height_texture_has_color(plan.after);

    LayerSettings settings = request.settings;
    bool          layer_changed = edits_layer && apply_layer_settings(plan.after, settings);
    bool          stack_changed = edits_layer && apply_stack_settings(plan.options, settings);
    if (const auto refusal = layer_edit_refusal(plan.object_id, stack, request, plan.before ? &*plan.before : nullptr, plan.after, facts))
        return refusal;

    bool view_changed = false;
    if (edits_layer) {
        const Slic3r::Transform3d to_world = Slic3r::texture_displacement_volume_to_world(*plan.owner);
        const bool became_view = plan.after.projection_method == Slic3r::TextureProjectionMethod::ViewProjected &&
                                 (!plan.before || plan.before->projection_method != Slic3r::TextureProjectionMethod::ViewProjected);
        std::optional<Axes> axes;
        if (settings.projection_frame) {
            const ProjectionFrameRequest& frame = *settings.projection_frame;
            const Axes frame_axes = frame.view ? view_axes_of(*frame.view) : Axes{*frame.right, *frame.up};
            const nlohmann::json* given = params.contains("projection_frame") ? &params.at("projection_frame") : nullptr;
            const Slic3r::Vec3d center = (given != nullptr && given->contains("center"))
                                             ? frame.center
                                             : Slic3r::Vec3d(plan.object->instance_bounding_box(0).center());
            const auto matrix = orthographic_frame_matrix(frame_axes.first, frame_axes.second, center, frame.width_mm, frame.height_mm, to_world);
            axes = frame_axes;
            view_changed |= !plan.after.view_project_projective || plan.after.view_project_matrix != matrix || plan.after.tile_enabled ||
                            plan.after.projection_method != Slic3r::TextureProjectionMethod::ViewProjected;
            plan.after.projection_method       = Slic3r::TextureProjectionMethod::ViewProjected;
            plan.after.view_project_projective = true;
            plan.after.view_project_matrix     = matrix;
            plan.after.tile_enabled            = false;
        } else if (settings.view_axes) {
            axes = *settings.view_axes;
        } else if (settings.view_direction) {
            axes = view_axes_of(*settings.view_direction);
        } else if (became_view) {
            axes = view_axes_of("current"); // as the panel captures the camera on choosing From view
        }
        if (axes) {
            const auto [right, up] = view_projector_axes(axes->first, axes->second, to_world);
            if (!near(plan.after.view_project_right, right) || !near(plan.after.view_project_up, up) || became_view)
                view_changed = true;
            plan.after.view_project_right = right;
            plan.after.view_project_up    = up;
        }
        if (settings.adjust_image) {
            const DecodedHeightTexture* given = given_image(plan);
            const DecodedHeightTexture  source = given != nullptr ? *given : raw_image_of(plan.after);
            DecodedHeightTexture        adjusted = Slic3r::apply_texture_adjustments(source, adjustments_of(*settings.adjust_image));
            if (!same_texture(adjusted, source)) {
                plan.adjusted     = std::move(adjusted);
                plan.adjust_stem  = plan.after.name.empty() ? std::string("texture") : plan.after.name;
            }
        }
    }

    switch (request.action) {
    case LayerAction::add:
        plan.changed  = true;
        plan.snapshot = _u8L("Add texture displacement layer");
        break;
    case LayerAction::remove:
        plan.changed  = true;
        plan.snapshot = _u8L("Remove texture displacement layer");
        break;
    case LayerAction::move: {
        plan.move_from = layer_position(stack, plan.slot).value_or(0);
        plan.move_to   = *request.to_position;
        plan.changed   = plan.move_from != plan.move_to;
        plan.snapshot  = _u8L("Reorder texture displacement layers");
        break;
    }
    case LayerAction::edit: {
        const bool image = plan.image_changed || plan.adjusted.has_value();
        plan.changed     = layer_changed || stack_changed || view_changed || image;
        const bool frame_only = settings.projection_frame && !layer_changed && !stack_changed && !image;
        plan.snapshot = image && !layer_changed && !stack_changed && !view_changed ? _u8L("Change texture displacement texture")
                        : frame_only ? _u8L("Apply texture projection frame")
                                     : _u8L("Texture displacement editing");
        break;
    }
    }
    return std::nullopt;
}

// What the answer needs of the model once the edit is done, copied so the mask reports can be made off the main thread.
struct PendingLayer
{
    bool                                               present = false;
    Slic3r::TextureDisplacementLayer                   layer;
    int                                                position = 0;
    LayerTexture                                       texture;
    std::optional<bool>                                color_capable;
    std::vector<Slic3r::TextureDisplacementLayer>      stack;
    Slic3r::TextureDisplacementOptions                 options;
    Slic3r::Transform3d                                to_world = Slic3r::Transform3d::Identity();
    bool                                               owner_has_uv = false;
    std::vector<MaskInput>                             inputs;
};

PendingLayer capture_layer(const Slic3r::ModelObject& object, const Slic3r::ModelVolume& owner, int slot)
{
    PendingLayer out;
    const auto*  layer = layer_in_slot(owner.texture_displacement_layers, slot);
    if (layer == nullptr)
        return out;
    out.present      = true;
    out.layer        = *layer;
    out.stack        = owner.texture_displacement_layers;
    out.options      = owner.texture_displacement_options;
    out.position     = layer_position(out.stack, slot).value_or(0);
    out.to_world     = Slic3r::texture_displacement_volume_to_world(owner);
    out.owner_has_uv = owner.has_texture_uv();
    if (!layer->empty())
        out.color_capable = Slic3r::height_texture_has_color(*layer);
    const LibraryCopy library = library_copy();
    if (const auto index = library_index_of(library.entries, *layer)) {
        out.texture.id     = library.ids[size_t(*index)];
        out.texture.source = texture_source_name(library.entries[size_t(*index)].source);
    }
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const Slic3r::ModelVolume* v = object.volumes[i];
        if (!v->is_model_part())
            continue;
        MaskInput input;
        input.volume_id         = int(i);
        input.name              = v->name;
        input.mesh              = v->mesh_ptr();
        input.masks[size_t(slot)] = v->texture_displacement_facet(slot).get_data();
        out.inputs.push_back(std::move(input));
    }
    return out;
}

bool slot_painted_anywhere(const Slic3r::ModelObject& object, int slot)
{
    return std::any_of(object.volumes.begin(), object.volumes.end(), [&](const Slic3r::ModelVolume* v) {
        return v->is_model_part() && !v->texture_displacement_facet(slot).empty();
    });
}

// Writes the plan into the model. Main thread, after the snapshot. Returns the slot the layer is in now (-1 after a remove).
int write_edit(EditPlan& plan)
{
    Slic3r::ModelVolume& owner = *plan.owner;
    auto&                stack = owner.texture_displacement_layers;
    switch (plan.request.action) {
    case LayerAction::add:
        stack.push_back(plan.after);
        plan.options.active_slot = plan.slot;
        owner.texture_displacement_options = plan.options;
        return plan.slot;
    case LayerAction::edit:
        for (Slic3r::TextureDisplacementLayer& layer : stack)
            if (layer.slot == plan.slot)
                layer = plan.after;
        owner.texture_displacement_options = plan.options;
        return plan.slot;
    case LayerAction::remove: {
        Slic3r::GUI::remove_texture_layer(*plan.object, plan.slot);
        Slic3r::TextureDisplacementOptions& options = owner.texture_displacement_options;
        if (options.active_slot == plan.slot) {
            int lowest = 0;
            if (!stack.empty())
                lowest = std::min_element(stack.begin(), stack.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; })->slot;
            options.active_slot = lowest;
        }
        return -1;
    }
    case LayerAction::move: {
        int active = owner.texture_displacement_options.active_slot;
        Slic3r::GUI::move_texture_layer(*plan.object, plan.slot, move_insertion_index(plan.move_from, plan.move_to), [&](int a, int b) {
            if (active == a)
                active = b;
            else if (active == b)
                active = a;
        });
        owner.texture_displacement_options.active_slot = active;
        std::vector<int> slots;
        for (const auto& layer : stack)
            slots.push_back(layer.slot);
        std::sort(slots.begin(), slots.end());
        return slots[size_t(plan.move_to)];
    }
    }
    return plan.slot;
}

// The edit on the main thread. `closed_tool` names a toolbar tool it closed. Fills `pending` for the answer's layer.
nlohmann::json edit_stack(const nlohmann::json& params, const LayerRequest& request, PendingLayer& pending, std::string& closed_tool)
{
    wxLogNull                 no_log; // a bad image would otherwise open a log window at idle
    McpDialogSuppressionGuard guard;
    Slic3r::GUI::Plater*      plater = wxGetApp().plater();
    EditPlan                  plan;
    if (const auto refusal = plan_edit(params, request, plan))
        return guard.report(error_response(*refusal));

    nlohmann::json answer = {{"status", "success"},
                             {"changed", plan.changed},
                             {"action", request.action == LayerAction::add ? "add" : request.action == LayerAction::edit ? "edit"
                                        : request.action == LayerAction::remove ? "remove" : "move"},
                             {"object_id", plan.object_id}};
    const auto finish = [&](nlohmann::json result) { return guard.report(with_closed_tool(std::move(result), closed_tool)); };
    const auto fail   = [&](const std::string& message) { return finish(error_response(message)); };

    int  slot = plan.slot;
    bool had_paint = false;
    std::string removed_name;
    if (plan.changed) {
        // The images go to the library first: a file that cannot be written leaves the scene as it was.
        std::optional<LibraryImage> stored;
        std::string                 error;
        if (plan.file) {
            stored    = import_or_reuse(*request.file_path, plan.file->decoded, error);
        }
        if (plan.adjusted)
            stored = save_or_reuse(*plan.adjusted, plan.adjust_stem, error);
        if ((plan.file || plan.adjusted) && !stored)
            return fail(error);
        if (stored) {
            plan.after.image_data = stored->data;
            plan.after.name       = stored->entry.name;
            plan.after.path       = stored->entry.path;
            plan.after.preview_adjust = Slic3r::TextureAdjustments{};
            const LibraryCopy library = library_copy();
            const auto        index   = library_index_of(library.entries, plan.after);
            answer["imported_texture"] = {{"texture", index ? nlohmann::json(library.ids[size_t(*index)]) : nlohmann::json(nullptr)},
                                          {"name", stored->entry.name},
                                          {"path", stored->entry.path},
                                          {"reused", stored->reused}};
        }
        if (plan.picked_entry && plan.picked_entry->source == TextureSource::User)
            touch_user_texture(plan.picked_entry->path);
        if (stored && stored->entry.source == TextureSource::User)
            touch_user_texture(stored->entry.path);

        had_paint = request.action == LayerAction::remove && slot_painted_anywhere(*plan.object, plan.slot);
        removed_name = plan.before ? plan.before->name : std::string();

        // Every check is behind us and the call changes something: close an open panel, as the user would, right before the step.
        if (const auto failed = close_toolbar_tool_for_change(*plater, plan.object_id, plan.object->volumes.size(), closed_tool))
            return guard.report(with_closed_tool(*failed, closed_tool));
        plater->take_snapshot(plan.snapshot);
        slot = write_edit(plan);
        if (Slic3r::GUI::GLCanvas3D* canvas = plater->get_view3D_canvas3D())
            canvas->set_as_dirty();
    }

    answer["stack_options"] = stack_options_json(plan.owner->texture_displacement_options);
    answer["needs_bake"]    = needs_bake_fast(*plan.object, plan.owner);
    if (request.action == LayerAction::remove) {
        answer["removed_layer"] = {{"layer", plan.slot}, {"name", removed_name}, {"had_paint", had_paint}};
    } else {
        pending = capture_layer(*plan.object, *plan.owner, slot);
    }
    return finish(std::move(answer));
}

nlohmann::json set_texture_displacement(const nlohmann::json& params)
{
    std::string error;
    const auto  request = read_layer_request(params, error);
    if (!request)
        return error_response(error);

    PendingLayer pending;
    std::string  closed_tool;
    nlohmann::json answer = run_on_main_thread([&]() { return edit_stack(params, *request, pending, closed_tool); });
    if (answer.value("status", "") != "success" || !pending.present)
        return answer;

    // The mask reports walk the paint, so they are made here, off the main thread, from the copies.
    const auto reports = mask_reports(pending.inputs);
    std::vector<MaskReport> paint;
    for (const auto& row : reports)
        paint.push_back(row[size_t(pending.layer.slot)]);
    answer["layer"] = layer_json(pending.layer, pending.position, pending.position == 0, pending.texture, pending.color_capable, paint,
                                 pending.owner_has_uv, pending.stack, pending.options, pending.to_world);
    if (request->action != LayerAction::remove)
        add_next_steps(answer, texture_paint_next_steps(answer.at("object_id").get<int>(), pending.layer.slot, answer["layer"].at("painted").get<bool>()));
    return answer;
}


// A number property held to the panel's range for `key` (layer_setting_bounds), so the schema and the reader cannot drift.
nlohmann::json bounded_property(const char* key, const char* type, const std::string& description)
{
    const SettingBound* bound = setting_bound(key);
    nlohmann::json      out   = {{"type", type}, {"description", description}};
    if (bound != nullptr) {
        out["minimum"] = std::string(type) == "integer" ? nlohmann::json(long(bound->min)) : nlohmann::json(bound->min);
        out["maximum"] = std::string(type) == "integer" ? nlohmann::json(long(bound->max)) : nlohmann::json(bound->max);
    }
    return out;
}

nlohmann::json vector3_property(const std::string& description)
{
    return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", description}};
}

nlohmann::json axes_properties()
{
    return {{"right", vector3_property("The projector's right axis in plate axes")}, {"up", vector3_property("The projector's up axis in plate axes")}};
}

nlohmann::json set_texture_displacement_schema()
{
    nlohmann::json frame_properties = axes_properties();
    frame_properties["view"]        = {{"type", "string"}, {"enum", view_direction_names()}, {"description", "A named view to project through, or current: the 3D view's camera now. Instead of right and up"}};
    frame_properties["center"]      = vector3_property("The frame's centre in plate mm (default: the object's centre)");
    frame_properties["width_mm"]    = {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Width in mm, greater than 0"}};
    frame_properties["height_mm"]   = {{"type", "number"}, {"exclusiveMinimum", 0}, {"description", "Height in mm, greater than 0"}};

    nlohmann::json properties = {
        {"object_id", {{"type", "integer"}, {"minimum", 0}, {"description", "Object index (0-based). The layers belong to the object"}}},
        {"action", {{"type", "string"}, {"enum", {"add", "edit", "remove", "move"}}, {"description", "add: a new layer in the first free slot; edit: change the layer in slot layer; remove: drop it and its paint; move: put it at to_position"}}},
        {"layer", {{"type", "integer"}, {"minimum", 0}, {"maximum", int(Slic3r::TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1}, {"description", "The layer's slot (edit, remove, move). add takes the first free slot"}}},
        {"to_position", {{"type", "integer"}, {"minimum", 0}, {"description", "move: the 0-based position in slot order"}}},
        {"texture", {{"type", "string"}, {"description", "A texture id from get_texture_library (a name alone works when only one texture has it)"}}},
        {"file_path", {{"type", "string"}, {"description", "An image file (png, jpg, bmp) to use as the texture: imported into the texture library, or the identical texture already there reused. Instead of texture"}}},
        {"depth_mm", bounded_property("depth_mm", "number", "Maximum displacement along the surface normal, in mm")},
        {"tile_size_mm", bounded_property("tile_size_mm", "number", "Size of one texture tile, in mm")},
        {"rotation_deg", bounded_property("rotation_deg", "number", "Texture rotation in degrees")},
        {"midlevel", bounded_property("midlevel", "number", "The height that does not move the surface: 0 only raises, 0.5 lets dark texels cut in")},
        {"smoothing", bounded_property("smoothing", "number", "Blur of the height map before it is sampled (0 raw, 1 strongest)")},
        {"edge_fade_amount", bounded_property("edge_fade_amount", "number", "How far edge_fade reaches into the painted patch")},
        {"offset", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "Placement [u, v] in texture repeats"}}},
        {"invert", {{"type", "boolean"}, {"description", "Swap raised and cut texels"}}},
        {"edge_fade", {{"type", "boolean"}, {"description", "Fade the displacement toward the edge of the painted patch"}}},
        {"tile", {{"type", "boolean"}, {"description", "Repeat the texture (false: placed once)"}}},
        {"color", {{"type", "boolean"}, {"description", "Colour the painted area from the texture's own colours (a colour texture only)"}}},
        {"color_mix", {{"type", "boolean"}, {"description", "Stack-wide: interleave filament pairs to reach colours between them"}}},
        {"tile_method", {{"type", "string"}, {"enum", tile_method_names()}, {"description", "How the texture repeats"}}},
        {"projection", {{"type", "string"}, {"enum", projection_names()}, {"description", "How the texture is mapped: triplanar, cylindrical, spherical, unwrap, view (from a view), model_uv (the model's own UV channel)"}}},
        {"blend", {{"type", "string"}, {"enum", blend_names()}, {"description", "How the layer combines with the layers below it (the base layer ignores it)"}}},
        {"view_direction", {{"type", "string"}, {"enum", view_direction_names()}, {"description", "projection view: project from a named view of the 3D view's cameras, or current: its camera now"}}},
        {"view_axes", {{"type", "object"}, {"properties", axes_properties()}, {"additionalProperties", false}, {"description", "projection view: the projector's right and up axes in plate axes, as a layer reports them"}}},
        {"projection_frame", {{"type", {"object", "boolean"}}, {"properties", frame_properties}, {"additionalProperties", false}, {"description", "Project the texture once through a rectangle (view or right and up, center, width_mm, height_mm) with tiling off; false clears the frame"}}},
        {"adjust_image", {{"type", "object"},
                          {"properties", {{"brightness", bounded_property("brightness", "number", "-1 black to 1 white")},
                                          {"contrast", bounded_property("contrast", "number", "-1 flat to 1 hard threshold")},
                                          {"sharpness", bounded_property("sharpness", "number", "0 unchanged to 1")},
                                          {"blur", bounded_property("blur", "number", "0 unchanged to 1")},
                                          {"hue_deg", bounded_property("hue_deg", "number", "Hue shift in degrees (colour only)")}}},
                          {"additionalProperties", false},
                          {"description", "Save an adjusted copy of the layer's image into the library and use it"}}},
        {"color_mix_count", bounded_property("color_mix_count", "integer", "Stack-wide: the most mixed colors the palette may offer")},
        {"color_denoise", bounded_property("color_denoise", "integer", "Stack-wide: passes that clear colour specks")}};
    return {{"type", "object"}, {"properties", properties}, {"required", {"object_id", "action"}}};
}

} // namespace

void OrcaMCPServer::register_texture_tools()
{
    register_tool({
        "get_texture_library",
        ToolCategory::TextureDisplacement,
        "List the textures a layer can use",
        "The textures a texture displacement layer can use, as its picker lists them: built-in, imported into this data "
        "folder's texture library (set_texture_displacement's file_path imports one there; an identical image already "
        "there is reused), and those of installed texture packs. Each has a texture id a layer takes (a name alone works "
        "when only one texture has it), and color: true when it has colours a layer can print with filaments, false for a "
        "grayscale height map, null when the file is not a PNG that is read directly. Reading it changes nothing.",
        {
            {"type", "object"},
            {"properties", {
                {"source", {
                    {"type", "string"},
                    {"enum", {"builtin", "user", "pack"}},
                    {"description", "List only the textures of one source (default: all). counts always covers the whole library"}
                }}
            }}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return get_texture_library(params);
        }
    });

    register_tool({
        "delete_user_texture",
        ToolCategory::TextureDisplacement,
        "Delete a texture you imported",
        "Delete a texture from this data folder's texture library: only one that was imported (source user), never a "
        "built-in or pack texture. Layers using it keep their image, which they hold themselves and a saved project "
        "stores; the answer lists them (still_used_by). A file delete: not an undo step.",
        {
            {"type", "object"},
            {"properties", {
                {"texture", {
                    {"type", "string"},
                    {"description", "The texture's id from get_texture_library, e.g. user/My bricks.png"}
                }}
            }},
            {"required", {"texture"}}
        },
        [](const nlohmann::json& params) -> nlohmann::json {
            return delete_user_texture(params);
        }
    });

    register_tool({
        "set_texture_displacement",
        ToolCategory::TextureDisplacement,
        "Add, edit, move or remove texture layers",
        "Add, edit, move or remove one texture displacement layer of an object, as the texture displacement panel does. "
        "An object holds up to 8 layers, combined in slot order; each applies where paint_object (mode texture) paints "
        "it, and nothing reaches the print until the layers are baked into the mesh. add takes the first free slot with "
        "texture (an id from get_texture_library) or the library's first; file_path imports an image (an identical one "
        "already in the library is reused, imported_texture says which); edit changes only what is given; move puts a "
        "layer at to_position in slot order, its paint with it (it may take another slot: layer in the answer); remove "
        "drops it and its paint. projection view projects from view_direction (a named view, or current: the 3D view's "
        "camera now) or view_axes; projection_frame projects once through a rectangle (view or right and up, center, "
        "width_mm, height_mm) with tiling off, false clears it; adjust_image saves an adjusted copy of the texture into "
        "the library and uses it. One undo step; nothing to change is changed: false. Refused while an arrange, orient "
        "or bake runs; an open toolbar tool is closed first when the call changes something.",
        set_texture_displacement_schema(),
        [](const nlohmann::json& params) -> nlohmann::json {
            return set_texture_displacement(params);
        }
    });
}
