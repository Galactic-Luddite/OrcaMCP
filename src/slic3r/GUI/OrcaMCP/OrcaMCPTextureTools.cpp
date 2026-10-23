// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureTools.cpp
//
// The texture displacement tools: the library of textures a layer can use, and (further tools) the layer stack,
// the unwrap, the bake. Their decisions apart from the app are OrcaMCPTextureDisplacement.cpp's; the owner of an
// object's stack, its parts and the bake frame are the texture panel's own functions over the model
// (TextureDisplacementObject.hpp), so a tool and the panel act on the same data.
#include "OrcaMCPServer.hpp"
#include "OrcaMCPCommon.hpp"
#include "OrcaMCPPartEdits.hpp"
#include "OrcaMCPTextureDisplacement.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"
#include "libslic3r/Model.hpp"

#include <optional>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;

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

} // namespace

void OrcaMCPServer::register_texture_tools()
{
    register_tool({
        "get_texture_library",
        ToolCategory::TextureDisplacement,
        "List the textures a layer can use",
        "The textures a texture displacement layer can use, as its picker lists them: built-in, imported into this data "
        "folder's texture library (an image given by file_path to a layer is imported there; an identical image already "
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
}
