#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp"
#include "slic3r/GUI/TextureLibrary.hpp"

// The texture displacement tools' decisions apart from the app: names, the library by id, one call's layer
// edit, masks, the answers' shapes.

using namespace Slic3r;
using namespace Slic3r::GUI;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

TextureLibraryEntry entry(const std::string& name, const std::string& path, TextureSource source, const std::string& pack = {})
{
    TextureLibraryEntry e;
    e.name      = name;
    e.path      = path;
    e.source    = source;
    e.pack_name = pack;
    return e;
}

std::optional<LayerRequest> read(const json& params, std::string& error)
{
    return read_layer_request(params, error);
}

std::string refusal_of(const json& params)
{
    std::string error;
    const auto  req = read(params, error);
    REQUIRE_FALSE(req.has_value());
    return error;
}

TextureDisplacementLayer layer_in(int slot)
{
    TextureDisplacementLayer l;
    l.slot       = slot;
    l.image_data = std::make_shared<std::vector<unsigned char>>(4, 1);
    return l;
}

ModelObject* cube_object(Model& model)
{
    ModelObject* object = model.add_object();
    object->add_instance();
    object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)), ModelVolumeType::MODEL_PART, false);
    return object;
}

void paint_facets(ModelVolume& volume, int slot, int count)
{
    TriangleSelector selector(volume.mesh());
    for (int i = 0; i < count; ++i)
        selector.set_facet(i, EnforcerBlockerType::ENFORCER);
    volume.texture_displacement_facet(slot).set(selector);
}

} // namespace

TEST_CASE("names round-trip", "[McpTexture][orcamcp]")
{
    for (const std::string& n : projection_names())
        CHECK(projection_name(*projection_named(n)) == n);
    for (const std::string& n : blend_names())
        CHECK(blend_name(*blend_named(n)) == n);
    for (const std::string& n : tile_method_names())
        CHECK(tile_method_name(*tile_method_named(n)) == n);
    CHECK(*projection_named("VIEW") == TextureProjectionMethod::ViewProjected);
    CHECK(*projection_named("unwrap") == TextureProjectionMethod::LSCM);
    CHECK(*blend_named("override") == TextureBlendMode::Override);
    CHECK(blend_names().front() == "add");
    CHECK_FALSE(projection_named("planar").has_value());
    CHECK(std::string(texture_source_name(TextureSource::Plugin)) == "pack");
}

TEST_CASE("texture ids are unique and say where a texture comes from", "[McpTexture][orcamcp]")
{
    const std::vector<TextureLibraryEntry> lib = {
        entry("Bark", "/res/Bark.png", TextureSource::Shipped),
        entry("a", "/user/a.png", TextureSource::User),
        entry("a", "/user/a.jpg", TextureSource::User),
        entry("Bark", "/packs/one/Bark.png", TextureSource::Plugin, "One"),
        entry("Bark", "/packs/two/Bark.png", TextureSource::Plugin, "Two"),
        entry("Bark", "/packs/one2/Bark.png", TextureSource::Plugin, "One"),
    };
    const std::vector<std::string> ids = texture_ids(lib);
    REQUIRE(ids.size() == lib.size());
    CHECK(ids[0] == "builtin/Bark.png");
    CHECK(ids[1] == "user/a.png");
    CHECK(ids[2] == "user/a.jpg");
    CHECK(ids[3] == "pack/One/Bark.png");
    CHECK(ids[4] == "pack/Two/Bark.png");
    CHECK(ids[5] == "pack/One/Bark.png #2");
    CHECK(std::set<std::string>(ids.begin(), ids.end()).size() == ids.size());
    CHECK(texture_ids(lib) == ids);

    SECTION("a lookup by id, by stem in any case, ambiguous and unknown")
    {
        CHECK(find_texture(lib, ids, "user/a.jpg").index == 2);
        CHECK(find_texture(lib, ids, "USER/A.PNG").index == 1);
        CHECK(find_texture(lib, ids, "bark").error.find("more than one") != std::string::npos);
        CHECK(find_texture(lib, ids, "bark").error.find("builtin/Bark.png") != std::string::npos);
        const TextureLookup unknown = find_texture(lib, ids, "marble");
        CHECK(unknown.index == -1);
        CHECK(unknown.error.find("get_texture_library") != std::string::npos);
        const std::vector<TextureLibraryEntry> one = {entry("Wood", "/res/Wood.png", TextureSource::Shipped)};
        CHECK(find_texture(one, texture_ids(one), "wood").index == 0);
    }
    SECTION("a layer's image is found by its path")
    {
        TextureDisplacementLayer l;
        l.path = "/user/a.jpg";
        CHECK(*library_index_of(lib, l) == 2);
        l.path = "/elsewhere/a.jpg";
        CHECK_FALSE(library_index_of(lib, l).has_value());
    }
    SECTION("an entry's json")
    {
        const json j = library_entry_json(lib[3], ids[3], true);
        CHECK(j["pack"] == "One");
        CHECK(j["color"] == true);
        CHECK(j["file"] == "Bark.png");
        CHECK(library_entry_json(lib[0], ids[0], std::nullopt)["color"].is_null());
    }
}

TEST_CASE("a PNG header says what decode_height_texture will find", "[McpTexture][orcamcp]")
{
    const std::filesystem::path dir = std::filesystem::path(PROFILES_DIR).parent_path() / "textures" / "displacement";
    REQUIRE(std::filesystem::is_directory(dir));
    int total = 0, colour = 0;
    for (const auto& file : std::filesystem::directory_iterator(dir)) {
        if (file.path().extension() != ".png")
            continue;
        std::ifstream in(file.path(), std::ios::binary);
        auto          bytes = std::make_shared<std::vector<unsigned char>>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        TextureDisplacementLayer layer;
        layer.image_data = bytes;
        const DecodedHeightTexture decoded = decode_height_texture(layer, false);
        REQUIRE_FALSE(decoded.empty());
        const auto header = png_has_color(bytes->data(), bytes->size());
        INFO(file.path().filename().string());
        REQUIRE(header.has_value());
        CHECK(*header == decoded.has_color());
        CHECK(png_color_from_file(file.path().string()) == header);
        ++total;
        colour += *header ? 1 : 0;
    }
    CHECK(total == 44);
    CHECK(colour == 6);
    const unsigned char jpg[] = {0xff, 0xd8, 0xff, 0xe0, 0, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(png_has_color(jpg, sizeof jpg).has_value());
    CHECK_FALSE(png_color_from_file("/no/such/file.png").has_value());
}

TEST_CASE("two encodings of one picture are the same texture", "[McpTexture][orcamcp]")
{
    DecodedHeightTexture grey;
    grey.width  = 4;
    grey.height = 2;
    grey.pixels = {0, 10, 20, 30, 40, 50, 60, 70};
    DecodedHeightTexture rgb = grey;
    for (uint8_t v : grey.pixels)
        rgb.rgb.insert(rgb.rgb.end(), {v, v, v});
    REQUIRE(rgb.has_color());

    CHECK(same_texture(grey, rgb));
    CHECK(texture_identity(grey).hash == texture_identity(rgb).hash);

    DecodedHeightTexture changed = grey;
    changed.pixels[3] = 31;
    CHECK_FALSE(same_texture(grey, changed));
    CHECK(texture_identity(grey).hash != texture_identity(changed).hash);

    DecodedHeightTexture tinted = rgb;
    tinted.rgb[0] = 200;
    CHECK_FALSE(same_texture(rgb, tinted));
    CHECK_FALSE(same_texture(grey, tinted));

    DecodedHeightTexture wider = grey;
    wider.width  = 2;
    wider.height = 4;
    CHECK_FALSE(same_texture(grey, wider));
}

TEST_CASE("every number is held to the panel's range", "[McpTexture][orcamcp]")
{
    const SettingBound& bound = GENERATE_REF(from_range(layer_setting_bounds()));
    INFO(bound.key);
    const bool is_adjust = std::string(bound.key) == "brightness" || std::string(bound.key) == "contrast" ||
                           std::string(bound.key) == "sharpness" || std::string(bound.key) == "blur" || std::string(bound.key) == "hue_deg";
    const bool is_int = std::string(bound.key) == "color_mix_count" || std::string(bound.key) == "color_denoise";
    const auto make = [&](double v) {
        json value = is_int ? json(long(v)) : json(v);
        return is_adjust ? json{{"action", "edit"}, {"layer", 0}, {"adjust_image", {{bound.key, value}}}}
                         : json{{"action", "edit"}, {"layer", 0}, {bound.key, value}};
    };
    std::string error;
    for (double v : {bound.min, bound.max, (bound.min + bound.max) / 2.}) {
        if (is_int)
            v = std::round(v);
        CHECK(read(make(v), error).has_value());
    }
    const double below = is_int ? bound.min - 1. : bound.min - 0.001, above = is_int ? bound.max + 1. : bound.max + 0.001;
    for (double v : {below, above}) {
        REQUIRE_FALSE(read(make(v), error).has_value());
        CHECK(error.find(bound.key) != std::string::npos);
        CHECK(error.find(bound.panel_label) != std::string::npos);
    }
    CHECK_FALSE(read(is_adjust ? json{{"action", "edit"}, {"layer", 0}, {"adjust_image", {{bound.key, "x"}}}}
                               : json{{"action", "edit"}, {"layer", 0}, {bound.key, "x"}}, error).has_value());
}

TEST_CASE("a layer request is read for its structure", "[McpTexture][orcamcp]")
{
    std::string error;
    SECTION("what is read")
    {
        const auto req = read({{"action", "edit"}, {"layer", 3}, {"depth_mm", 0.5}, {"offset", {0.25, -1}}, {"invert", true},
                               {"blend", "override"}, {"projection", "view"}, {"tile_method", "mirrored_repeat"},
                               {"view_direction", "Top"}, {"color_mix", false}, {"color_mix_count", 4}}, error);
        REQUIRE(req.has_value());
        CHECK(req->action == LayerAction::edit);
        CHECK(*req->layer == 3);
        CHECK_THAT(*req->settings.depth_mm, WithinAbs(0.5, 1e-6));
        CHECK(req->settings.offset->y() == -1.f);
        CHECK(*req->settings.blend == TextureBlendMode::Override);
        CHECK(*req->settings.projection == TextureProjectionMethod::ViewProjected);
        CHECK(*req->settings.tile_method == TextureTileMethod::MirroredRepeat);
        CHECK(*req->settings.view_direction == "top");
        CHECK(*req->settings.color_mix_count == 4);
        CHECK_FALSE(req->settings.empty());
        CHECK(read({{"action", "edit"}, {"layer", 0}}, error)->settings.empty());
    }
    SECTION("add takes the first free slot")
    {
        CHECK(read({{"action", "add"}, {"texture", "builtin/Bark.png"}}, error).has_value());
        CHECK(refusal_of({{"action", "add"}, {"layer", 2}}).find("first free slot") != std::string::npos);
    }
    SECTION("edit, remove and move need a slot")
    {
        for (const char* action : {"edit", "remove", "move"})
            CHECK(refusal_of({{"action", action}, {"to_position", 1}}).find("needs layer") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 8}}).find("0 to 7") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 1.5}}).find("integer") != std::string::npos);
    }
    SECTION("remove and move take nothing else")
    {
        CHECK(refusal_of({{"action", "remove"}, {"layer", 0}, {"depth_mm", 1}}).find("takes no depth_mm") != std::string::npos);
        CHECK(refusal_of({{"action", "move"}, {"layer", 0}, {"to_position", 1}, {"texture", "x"}}).find("takes no texture") != std::string::npos);
        CHECK(refusal_of({{"action", "move"}, {"layer", 0}}).find("to_position") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"to_position", 1}}).find("move") != std::string::npos);
        CHECK(read({{"action", "remove"}, {"layer", 0}, {"object_id", 1}}, error).has_value());
        CHECK(read({{"action", "move"}, {"layer", 0}, {"to_position", 1}}, error).has_value());
    }
    SECTION("the action and the types")
    {
        CHECK(refusal_of({{"layer", 0}}).find("action is required") != std::string::npos);
        CHECK(refusal_of({{"action", "spin"}}).find("add, edit, remove or move") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"invert", "yes"}}).find("true or false") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection", "flat"}}).find("one of triplanar") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"texture", 3}}).find("string") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_direction", "up"}}).find("one of current") != std::string::npos);
    }
    SECTION("texture and file_path are not both")
    {
        CHECK(refusal_of({{"action", "add"}, {"texture", "a"}, {"file_path", "/b.png"}}).find("send one") != std::string::npos);
    }
    SECTION("offset is two finite numbers")
    {
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"offset", {1}}}).find("2 numbers") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"offset", {1, "x"}}}).find("finite") != std::string::npos);
    }
    SECTION("a view is a direction or axes")
    {
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_direction", "top"},
                          {"view_axes", {{"right", {1, 0, 0}}, {"up", {0, 1, 0}}}}}).find("send one") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_axes", {{"right", {1, 0, 0}}, {"up", {2, 0, 0}}}}}).find("parallel") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"view_axes", {{"right", {1, 0, 0}}}}}).find("right and up") != std::string::npos);
        const auto req = read({{"action", "edit"}, {"layer", 0}, {"view_axes", {{"right", {1, 0, 0}}, {"up", {0, 0, 1}}}}}, error);
        REQUIRE(req.has_value());
        CHECK(req->settings.view_axes->second.z() == 1.);
    }
    SECTION("a projection frame")
    {
        const json frame = {{"view", "front"}, {"center", {1, 2, 3}}, {"width_mm", 20}, {"height_mm", 10}};
        const auto req   = read({{"action", "edit"}, {"layer", 0}, {"projection_frame", frame}}, error);
        REQUIRE(req.has_value());
        CHECK(req->settings.projection_frame->width_mm == 20.);
        CHECK(req->settings.projection_frame->center.z() == 3.);
        CHECK(read({{"action", "edit"}, {"layer", 0}, {"projection_frame", false}}, error)->settings.clear_projection_frame);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", true}}).find("not a frame") != std::string::npos);
        json bad = frame;
        bad["width_mm"] = 0;
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("greater than 0") != std::string::npos);
        bad = frame;
        bad.erase("view");
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("view") != std::string::npos);
        bad = frame;
        bad["right"] = {1, 0, 0};
        bad["up"]    = {0, 1, 0};
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("not both") != std::string::npos);
        bad = frame;
        bad["colour"] = 1;
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"projection_frame", bad}}).find("no key \"colour\"") != std::string::npos);
    }
    SECTION("adjust_image needs a field")
    {
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"adjust_image", json::object()}}).find("at least one") != std::string::npos);
        CHECK(refusal_of({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"gamma", 1}}}}).find("no key \"gamma\"") != std::string::npos);
        const auto req = read({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"hue_deg", -90}}}}, error);
        REQUIRE(req.has_value());
        CHECK(adjustments_of(*req->settings.adjust_image).hue_deg == -90.f);
        CHECK(adjustments_of(*req->settings.adjust_image).brightness == 0.f);
    }
}

TEST_CASE("a layer edit is refused for the stack it lands on", "[McpTexture][orcamcp]")
{
    std::vector<TextureDisplacementLayer> stack = {layer_in(0), layer_in(2)};
    StackFacts                            facts;
    facts.physical_filaments = 4;
    const auto check = [&](const json& params, const TextureDisplacementLayer& after) {
        std::string error;
        const auto  req = read(params, error);
        REQUIRE(req.has_value());
        return layer_edit_refusal(1, stack, *req, after, facts);
    };

    SECTION("no free slot")
    {
        for (int s = 0; s < 8; ++s)
            if (s != 0 && s != 2)
                stack.push_back(layer_in(s));
        const auto why = check({{"action", "add"}}, layer_in(0));
        REQUIRE(why.has_value());
        CHECK(why->find("object 1") != std::string::npos);
        CHECK(why->find("8 texture layers") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 2}}, layer_in(2)).has_value());
    }
    SECTION("no layer in that slot")
    {
        const auto why = check({{"action", "remove"}, {"layer", 1}}, layer_in(1));
        REQUIRE(why.has_value());
        CHECK(why->find("slot 1") != std::string::npos);
        CHECK(why->find("slots 0, 2") != std::string::npos);
        stack.clear();
        CHECK(check({{"action", "edit"}, {"layer", 0}}, layer_in(0))->find("add one") != std::string::npos);
    }
    SECTION("colour on a grayscale texture")
    {
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"color", true}}, layer_in(0))->find("no colours") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color", false}}, layer_in(0)).has_value());
        facts.texture_has_color = true;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color", true}}, layer_in(0)).has_value());
    }
    SECTION("model_uv without UV")
    {
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"projection", "model_uv"}}, layer_in(0))->find("UV") != std::string::npos);
        facts.owner_has_uv = true;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"projection", "model_uv"}}, layer_in(0)).has_value());
    }
    SECTION("more mixed colours than slots")
    {
        facts.physical_filaments = 30;
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 3}}, layer_in(0))->find("room for") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 2}}, layer_in(0)).has_value());
        facts.physical_filaments = 40;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 1}}, layer_in(0)).has_value());
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"color_mix_count", 2}}, layer_in(0)).has_value());
    }
    SECTION("a view for a layer that does not project from one")
    {
        TextureDisplacementLayer after = layer_in(0);
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"view_direction", "top"}}, after)->find("projection view") != std::string::npos);
        after.projection_method = TextureProjectionMethod::ViewProjected;
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"view_direction", "top"}}, after).has_value());
    }
    SECTION("move to a position that is not there")
    {
        CHECK(check({{"action", "move"}, {"layer", 0}, {"to_position", 2}}, layer_in(0))->find("past the last position, 1") != std::string::npos);
        CHECK_FALSE(check({{"action", "move"}, {"layer", 0}, {"to_position", 1}}, layer_in(0)).has_value());
    }
    SECTION("adjusting a layer with no image")
    {
        TextureDisplacementLayer bare;
        bare.slot = 0;
        CHECK(check({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"blur", 0.5}}}}, bare)->find("no image") != std::string::npos);
        CHECK_FALSE(check({{"action", "edit"}, {"layer", 0}, {"adjust_image", {{"blur", 0.5}}}}, layer_in(0)).has_value());
    }
}

TEST_CASE("a new layer takes the first free slot and the model's UV", "[McpTexture][orcamcp]")
{
    CHECK(*first_free_slot({}) == 0);
    CHECK(*first_free_slot({layer_in(0), layer_in(1), layer_in(3)}) == 2);
    std::vector<TextureDisplacementLayer> full;
    for (int s = 0; s < 8; ++s)
        full.push_back(layer_in(s));
    CHECK_FALSE(first_free_slot(full).has_value());
    CHECK(new_layer(4, false).slot == 4);
    CHECK(new_layer(4, false).projection_method == TextureProjectionMethod::Triplanar);
    CHECK(new_layer(4, true).projection_method == TextureProjectionMethod::ModelUV);
}

TEST_CASE("layer settings map onto the layer and report a change", "[McpTexture][orcamcp]")
{
    TextureDisplacementLayer layer = layer_in(0);
    std::string              error;
    const auto               req = read({{"action", "edit"}, {"layer", 0}, {"depth_mm", 1.5}, {"tile_size_mm", 20}, {"rotation_deg", 45},
                                         {"midlevel", 0.5}, {"smoothing", 0.25}, {"edge_fade", true}, {"edge_fade_amount", 0.75},
                                         {"offset", {0.5, 0.25}}, {"invert", true}, {"tile", false}, {"color", true},
                                         {"tile_method", "mirrored_repeat"}, {"projection", "spherical"}, {"blend", "multiply"}}, error);
    REQUIRE(req.has_value());
    CHECK(apply_layer_settings(layer, req->settings));
    CHECK(layer.depth_mm == 1.5f);
    CHECK(layer.tiling_scale == 20.f);
    CHECK(layer.rotation_deg == 45.f);
    CHECK(layer.midlevel == 0.5f);
    CHECK(layer.smoothing == 0.25f);
    CHECK(layer.edge_smoothing);
    CHECK(layer.edge_smoothing_amount == 0.75f);
    CHECK(layer.offset == Vec2f(0.5f, 0.25f));
    CHECK(layer.invert);
    CHECK_FALSE(layer.tile_enabled);
    CHECK(layer.color_enabled);
    CHECK(layer.tile_method == TextureTileMethod::MirroredRepeat);
    CHECK(layer.projection_method == TextureProjectionMethod::Spherical);
    CHECK(layer.blend_mode == TextureBlendMode::Multiply);
    CHECK_FALSE(apply_layer_settings(layer, req->settings));
    CHECK_FALSE(apply_layer_settings(layer, LayerSettings{}));

    SECTION("the panel's Clear for a projection frame")
    {
        layer.view_project_projective = true;
        LayerSettings clear;
        clear.clear_projection_frame = true;
        CHECK(apply_layer_settings(layer, clear));
        CHECK_FALSE(layer.view_project_projective);
        CHECK(layer.tile_enabled);
        CHECK_FALSE(apply_layer_settings(layer, clear));
    }
    SECTION("the stack's colour options")
    {
        TextureDisplacementOptions options;
        const auto stack_req = read({{"action", "edit"}, {"layer", 0}, {"color_mix", false}, {"color_mix_count", 5}, {"color_denoise", 4}}, error);
        REQUIRE(stack_req.has_value());
        CHECK(apply_stack_settings(options, stack_req->settings));
        CHECK_FALSE(options.color_mix_enabled);
        CHECK(options.color_mix_count == 5);
        CHECK(options.color_despeckle == 4);
        CHECK_FALSE(apply_stack_settings(options, stack_req->settings));
    }
}

TEST_CASE("a view projector is the camera's axes in the volume's frame", "[McpTexture][orcamcp]")
{
    Transform3d volume = Transform3d::Identity();
    volume.translate(Vec3d(5, -3, 2));
    volume.rotate(Eigen::AngleAxisd(0.7, Vec3d(1, 2, 3).normalized()));
    volume.scale(Vec3d(2., 1., 0.5));

    for (const char* view : {"top", "front", "left", "iso"}) {
        Camera camera;
        camera.select_view(view);
        const Matrix3d  view_rot = camera.get_view_matrix().matrix().block<3, 3>(0, 0);
        const Vec3d     right = view_rot.row(0).transpose(), up = view_rot.row(1).transpose();
        // capture_view_projection's own steps
        const Matrix3d  world_to_local = volume.matrix().block<3, 3>(0, 0).inverse();
        const Vec3d     local_right = world_to_local * right, local_up = world_to_local * up;
        const Vec3f     expect_right(local_right.normalized().cast<float>()), expect_up(local_up.normalized().cast<float>());
        const auto [r, u] = view_projector_axes(camera.get_dir_right(), camera.get_dir_up(), volume);
        INFO(view);
        CHECK((r - expect_right).norm() < 1e-6f);
        CHECK((u - expect_up).norm() < 1e-6f);
    }
    const auto [r, u] = view_projector_axes(Vec3d::Zero(), Vec3d::Zero(), volume);
    CHECK(r == Vec3f::UnitX());
    CHECK(u == Vec3f::UnitY());
}

TEST_CASE("an orthographic frame puts its corners at the image's corners", "[McpTexture][orcamcp]")
{
    const Vec3d right(1, 0, 0), up(0, 0, 1), center(10, 5, 8);
    const double width = 20., height = 10.;
    // The frame's top left is centre - right * w/2 + up * h/2; v runs down the image.
    const Vec3d top_left = center - right * (width / 2) + up * (height / 2);

    Transform3d identity = Transform3d::Identity();
    Transform3d moved    = Transform3d::Identity();
    moved.translate(Vec3d(4, -2, 1));
    moved.rotate(Eigen::AngleAxisd(0.9, Vec3d(0, 0, 1)));
    moved.scale(Vec3d(2., 3., 1.5));

    for (const Transform3d& volume : {identity, moved}) {
        const std::array<float, 12> m = orthographic_frame_matrix(right, up, center, width, height, volume);
        const Transform3d           back = volume.inverse();
        const auto uv_of = [&](const Vec3d& world) {
            Vec2f uv(-1e9f, -1e9f);
            REQUIRE(project_uv_projective(m, (back * world).cast<float>(), uv));
            return uv;
        };
        const Vec2f a = uv_of(top_left), b = uv_of(top_left + right * width), c = uv_of(top_left - up * height);
        CHECK_THAT(a.x(), WithinAbs(0., 1e-4));
        CHECK_THAT(a.y(), WithinAbs(0., 1e-4));
        CHECK_THAT(b.x(), WithinAbs(1., 1e-4));
        CHECK_THAT(b.y(), WithinAbs(0., 1e-4));
        CHECK_THAT(c.x(), WithinAbs(0., 1e-4));
        CHECK_THAT(c.y(), WithinAbs(1., 1e-4));
        const Vec2f mid = uv_of(center);
        CHECK_THAT(mid.x(), WithinAbs(0.5, 1e-4));
        CHECK_THAT(mid.y(), WithinAbs(0.5, 1e-4));
    }
    CHECK(orthographic_frame_matrix(right, up, center, width, height, identity)[11] == 1.f);
}

TEST_CASE("what the panel greys out but stores is reported", "[McpTexture][orcamcp]")
{
    TextureDisplacementOptions options;
    TextureDisplacementLayer   base = layer_in(0), top = layer_in(1);
    const std::vector<TextureDisplacementLayer> stack = {base, top};

    json inactive = inactive_settings(stack, base, options, true, false);
    CHECK(inactive.contains("blend"));
    CHECK(inactive.contains("edge_fade_amount"));
    CHECK(inactive.contains("color_mix_count"));
    CHECK_FALSE(inactive.contains("tile_size_mm"));
    CHECK_FALSE(inactive.contains("tile_method"));
    CHECK_FALSE(inactive_settings(stack, top, options, true, false).contains("blend"));

    SECTION("tile size under model_uv")
    {
        top.projection_method = TextureProjectionMethod::ModelUV;
        CHECK(inactive_settings(stack, top, options, true, false).contains("tile_size_mm"));
        CHECK_FALSE(inactive_settings(stack, top, options, true, false).contains("projection"));
        CHECK(inactive_settings(stack, top, options, false, false).contains("projection"));
    }
    SECTION("edge fade, tile and colour")
    {
        top.edge_smoothing = true;
        top.tile_enabled   = false;
        top.color_enabled  = true;
        const std::vector<TextureDisplacementLayer> colouring = {base, top};
        const json                                  j         = inactive_settings(colouring, top, options, false, false);
        CHECK_FALSE(j.contains("edge_fade_amount"));
        CHECK(j.contains("tile_method"));
        CHECK(j.contains("color"));
        CHECK_FALSE(j.contains("color_mix_count"));
        CHECK_FALSE(inactive_settings(colouring, top, options, false, true).contains("color"));
    }
    SECTION("placement under a projection frame")
    {
        top.projection_method       = TextureProjectionMethod::ViewProjected;
        top.view_project_projective = true;
        top.tile_enabled            = false;
        const json j                = inactive_settings(stack, top, options, false, false);
        for (const char* key : {"tile_size_mm", "rotation_deg", "offset", "tile_method"})
            CHECK(j.contains(key));
    }
    CHECK(layer_warnings(base).empty());
    base.midlevel = 0.5f;
    base.depth_mm = 2.f;
    CHECK(layer_warnings(base).size() == 1);
}

TEST_CASE("a mask indexing facets the mesh lacks is stale", "[McpTexture][orcamcp]")
{
    PaintData data;
    CHECK_FALSE(mask_is_stale(data, 12));
    data.triangles_to_split.emplace_back(11, 0);
    CHECK_FALSE(mask_is_stale(data, 12));
    CHECK(mask_is_stale(data, 11));
    data.triangles_to_split.emplace_back(-1, 4);
    CHECK(mask_is_stale(data, 100));
}

TEST_CASE("mask reports count what each layer paints, and a stale mask apart", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    REQUIRE(part->mesh().its.indices.size() == 12);
    paint_facets(*part, 2, 4);

    MaskInput input;
    input.volume_id = 0;
    input.name      = "cube";
    input.mesh      = std::make_shared<TriangleMesh>(part->mesh());
    for (size_t slot = 0; slot < input.masks.size(); ++slot)
        input.masks[slot] = part->texture_displacement_facet(int(slot)).get_data();
    PaintData stale;
    stale.triangles_to_split.emplace_back(40, 0);
    input.masks[5] = stale;

    const auto reports = mask_reports({input});
    REQUIRE(reports.size() == 1);
    const MaskReport& painted = reports[0][2];
    CHECK(painted.painted_facets == 4);
    CHECK_THAT(painted.area_ratio, WithinAbs(1. / 3., 1e-6));
    CHECK(painted.name == "cube");
    CHECK_FALSE(painted.stale);
    CHECK(reports[0][5].stale);
    CHECK(reports[0][5].painted_facets == 0);
    CHECK(reports[0][0].painted_facets == 0);

    SECTION("a bake is needed for a painted layer only")
    {
        CHECK_FALSE(needs_bake({layer_in(0), layer_in(1)}, reports));
        CHECK(needs_bake({layer_in(2)}, reports));
        CHECK_FALSE(needs_bake({layer_in(5)}, reports));
        CHECK_FALSE(needs_bake({}, reports));

        part->texture_displacement_layers = {layer_in(2)};
        CHECK(needs_bake_fast(*object, part));
        part->texture_displacement_layers = {layer_in(1)};
        CHECK_FALSE(needs_bake_fast(*object, part));
        CHECK_FALSE(needs_bake_fast(*object, nullptr));
    }
}

TEST_CASE("what a mesh edit dropped is told from the state before", "[McpTexture][orcamcp]")
{
    Model        model;
    ModelObject* object = cube_object(model);
    ModelVolume* part   = object->volumes.front();
    part->texture_displacement_layers = {layer_in(0), layer_in(3)};
    paint_facets(*part, 0, 2);
    paint_facets(*part, 3, 5);
    const TextureState before = texture_state_of(*object);
    REQUIRE(before.parts.size() == 1);
    CHECK(before.parts[0].painted_slots == std::vector<int>{0, 3});
    CHECK(before.parts[0].layers == 2);

    SECTION("nothing lost")
    {
        CHECK(texture_changes_json(before, {object}, part).empty());
    }
    SECTION("the masks were reset")
    {
        part->texture_displacement_facet(0).reset();
        part->texture_displacement_facet(3).reset();
        const json j = texture_changes_json(before, {object}, part);
        REQUIRE(j.contains("paint_lost"));
        CHECK(j["paint_lost"][0]["layers"] == json::array({0, 3}));
        CHECK(j["paint_lost"][0]["name"] == part->name);
        CHECK_FALSE(j.contains("layers_lost"));
    }
    SECTION("the part is gone, and its stack with it")
    {
        Model        other;
        ModelObject* fresh = cube_object(other);
        const json   j     = texture_changes_json(before, {fresh}, fresh->volumes.front());
        CHECK(j["paint_lost"][0]["layers"] == json::array({0, 3}));
        CHECK(j["layers_lost"] == 2);
    }
    SECTION("a stack that moved to the second part is left unused")
    {
        ModelVolume* second = object->add_volume(TriangleMesh(its_make_cube(5., 5., 5.)), ModelVolumeType::MODEL_PART, false);
        second->texture_displacement_layers = part->texture_displacement_layers;
        part->texture_displacement_layers.clear();
        part->texture_displacement_facet(0).reset();
        const json j = texture_changes_json(before, {object}, part);
        REQUIRE(j.contains("stacks_unused"));
        CHECK(j["stacks_unused"][0]["volume_id"] == 1);
        CHECK(j["stacks_unused"][0]["layers"] == 2);
        CHECK(j["layers_lost"] == 2);
        CHECK(j.contains("paint_lost"));
    }
}

TEST_CASE("an unwrap request is read for its shapes and bounds", "[McpTexture][orcamcp]")
{
    std::string error;
    const auto  read_unwrap = [&](const json& params) { return read_unwrap_request(params, error); };
    const auto  refused     = [&](const json& params) {
        REQUIRE_FALSE(read_unwrap(params).has_value());
        return error;
    };

    const auto req = read_unwrap({{"seam_angle_deg", 45}, {"island_padding_mm", -1}, {"auto_connect_islands", false},
                                  {"add_seams", {{7, 3}}}, {"remove_seams", {{1, 2}}},
                                  {"seam_paths", {{{"from", {0, 0, 0}}, {"to", {1, 2, 3}}}}}, {"cut_islands", {0, 2}}, {"unjoin", {1}},
                                  {"join", {{{"chart", 1}, {"to", 0}}, {{"chart", 2}}}},
                                  {"islands", {{{"chart", 0}, {"offset", {1, 2}}, {"rotation_deg", 90}, {"scale", 2}}}},
                                  {"uv_edits", {{{"vertex", 4}, {"uv", {0.5, 1.5}}}}}, {"clear_seams", true}, {"relayout", true}});
    REQUIRE(req.has_value());
    CHECK(*req->seam_angle_deg == 45.f);
    CHECK(*req->island_padding_mm == -1.f);
    CHECK_FALSE(*req->auto_connect_islands);
    CHECK(req->add_seams == std::vector<std::pair<int, int>>{{3, 7}});
    CHECK(req->seam_paths[0].second == Vec3d(1, 2, 3));
    CHECK(req->cut_islands == std::vector<int>{0, 2});
    CHECK(req->join[0].second == 0);
    CHECK_FALSE(req->join[1].second.has_value());
    CHECK(req->islands[0].scale == 2.f);
    CHECK(req->uv_edits[0].second == Vec2f(0.5f, 1.5f));
    CHECK(req->clear_seams);
    CHECK(req->relayout);
    CHECK_FALSE(req->average_scale);

    CHECK(read_unwrap(json::object()).has_value());
    CHECK(refused({{"seam_angle_deg", 4}}).find("5 to 90") != std::string::npos);
    CHECK(refused({{"seam_angle_deg", 91}}).find("5 to 90") != std::string::npos);
    CHECK(refused({{"island_padding_mm", -0.5}}).find("-1") != std::string::npos);
    CHECK(read_unwrap({{"island_padding_mm", 0}}).has_value());
    CHECK(refused({{"auto_connect_islands", 1}}).find("true or false") != std::string::npos);
    CHECK(refused({{"add_seams", {{1, 1}}}}).find("one vertex twice") != std::string::npos);
    CHECK(refused({{"add_seams", {{-1, 2}}}}).find("whole number") != std::string::npos);
    CHECK(refused({{"add_seams", {{1}}}}).find("pair") != std::string::npos);
    CHECK(refused({{"seam_paths", {{{"from", {0, 0, 0}}}}}}).find("from and to") != std::string::npos);
    CHECK(refused({{"cut_islands", {"a"}}}).find("cut_islands[0]") != std::string::npos);
    CHECK(refused({{"join", {{{"to", 1}}}}}).find("chart") != std::string::npos);
    CHECK(refused({{"islands", {{{"chart", 0}, {"scale", 0}}}}}).find("0.001 to 1000") != std::string::npos);
    CHECK(refused({{"islands", {{{"chart", 0}, {"size", 1}}}}}).find("no key \"size\"") != std::string::npos);
    CHECK(refused({{"uv_edits", {{{"vertex", 1}, {"uv", {1}}}}}}).find("2 numbers") != std::string::npos);
}

TEST_CASE("a layer and the stack's options are told", "[McpTexture][orcamcp]")
{
    TextureDisplacementOptions options;
    TextureDisplacementLayer   layer = layer_in(2);
    layer.name                       = "Bark";
    layer.path                       = "/res/Bark.png";
    layer.projection_method          = TextureProjectionMethod::ViewProjected;
    layer.view_project_projective    = true;
    MaskReport report;
    report.volume_id      = 1;
    report.name           = "cube";
    report.painted_facets = 4;
    report.area_ratio     = 1. / 3.;
    LayerTexture texture;
    texture.id     = "builtin/Bark.png";
    texture.source = "builtin";

    const json j = layer_json(layer, 0, true, texture, false, {report}, false, {layer}, options);
    CHECK(j["layer"] == 2);
    CHECK(j["base"] == true);
    CHECK(j["texture"]["texture"] == "builtin/Bark.png");
    CHECK(j["projection"] == "view");
    CHECK(j["projection_frame"]["active"] == true);
    CHECK(j["view"].contains("right"));
    CHECK(j["painted"] == true);
    CHECK_THAT(j["paint"][0]["coverage_percent"].get<double>(), WithinAbs(33.33, 1e-6));
    CHECK(j["color_capable"] == false);
    CHECK(j.contains("inactive_settings"));
    CHECK(j["warnings"].empty());

    LayerTexture project_only;
    CHECK(layer_json(layer, 0, true, project_only, std::nullopt, {}, false, {layer}, options)["texture"]["source"] == "project");
    CHECK(layer_json(layer, 0, true, project_only, std::nullopt, {}, false, {layer}, options)["color_capable"].is_null());

    const json o = stack_options_json(options);
    CHECK(o["pipeline"] == "default");
    CHECK(o["resolution_mm"].is_null());
    CHECK(o["budget_k"].is_null());
    CHECK(o["smooth"]["enabled"] == false);
    CHECK(o["smooth"]["passes"] == 2);
    CHECK(o["smooth"]["ignore_outer_ring"] == true);
    CHECK_THAT(o["smooth"]["strength"].get<double>(), WithinAbs(30., 1e-6));
    CHECK(o["color_mix_count"] == 8);
    options.pipeline_v2        = false;
    options.v2_refine_mm       = 0.5f;
    options.v2_max_triangles_k = 0;
    const json c               = stack_options_json(options);
    CHECK(c["pipeline"] == "classic");
    CHECK(c["resolution_mm"] == 0.5);
    CHECK(c["budget_k"] == 0);

    SECTION("an unwrap")
    {
        PatchUnwrap unwrap;
        unwrap.chart_count    = 2;
        unwrap.chart_centroid = {Vec2f(1, 2), Vec2f(3, 4)};
        unwrap.uvs            = {Vec2f(0, 0), Vec2f(1, 1)};
        unwrap.source_vertex  = {5, 6};
        unwrap.vertex_chart   = {0, 1};
        layer.lscm_seam_edges = {{1, 2}};
        layer.islands         = {TextureIsland{}};
        layer.island_groups   = {7};
        const json u = unwrap_json(unwrap, layer, true);
        CHECK(u["chart_count"] == 2);
        CHECK(u["seams"] == json::array({json::array({1, 2})}));
        CHECK(u["islands"][0]["group"] == 7);
        CHECK(u["islands"][1]["group"] == -1);
        CHECK(u["vertices"][1]["source_vertex"] == 6);
        CHECK_FALSE(unwrap_json(unwrap, layer, false).contains("vertices"));
    }
}
