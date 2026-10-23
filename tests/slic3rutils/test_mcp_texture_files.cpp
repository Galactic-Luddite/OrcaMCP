#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPPaintModel.hpp"
#include "slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.hpp"
#include "test_utils.hpp"

// A textured OBJ's UV channel, and a texture-displacement stack with its paint through a saved and reopened
// project. The fixture is tests/data/textured_cube: eight vertices, twelve triangles, a distinct vt for each
// corner of each face, and one material with a map_Kd.

using namespace Slic3r;
using namespace Slic3r::GUI::OrcaMCP;
using Catch::Matchers::WithinAbs;

namespace {

boost::filesystem::path fixture(const char* name)
{
    return boost::filesystem::path(TEST_DATA_DIR) / "textured_cube" / name;
}

std::vector<unsigned char> file_bytes(const boost::filesystem::path& path)
{
    std::ifstream in(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

Model load_obj_model(const boost::filesystem::path& path)
{
    return Model::read_from_file(path.string());
}

const ModelVolume& only_volume(const Model& model)
{
    REQUIRE(model.objects.size() == 1);
    REQUIRE(model.objects.front()->volumes.size() == 1);
    return *model.objects.front()->volumes.front();
}

// The fixture's vt, read from the file as the importer reads it, per corner of its faces in the file's order.
// The mesh the importer builds keeps the faces in the file's order (quads split into two triangles).
std::vector<Vec2f> fixture_vts()
{
    std::vector<Vec2f> vts;
    std::ifstream      in(fixture("cube.obj").string());
    for (std::string line; std::getline(in, line);)
        if (line.rfind("vt ", 0) == 0) {
            Vec2f uv;
            REQUIRE(std::sscanf(line.c_str(), "vt %f %f", &uv.x(), &uv.y()) == 2);
            vts.push_back(uv);
        }
    return vts;
}

void store_project(Model& model, const std::string& path)
{
    ScopedTemporaryDir backup_dir("orca_texfiles_src");
    model.set_backup_path(backup_dir.string());
    DynamicPrintConfig cfg;
    PlateData          plate;
    plate.plate_index = 0;
    StoreParams sp;
    sp.path     = path.c_str();
    sp.model    = &model;
    sp.config   = &cfg;
    sp.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    sp.plate_data_list.push_back(&plate);
    REQUIRE(store_bbs_3mf(sp));
}

bool load_project(const std::string& path, Model& model, const ScopedTemporaryDir& backup_dir)
{
    model.set_backup_path(backup_dir.string());
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs             plates;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    Semver                    file_version;
    const bool                loaded = load_bbs_3mf(path.c_str(), &config, &ctxt, &model, &plates, &project_presets, &is_bbl_3mf,
                                                    &is_orca_3mf, &file_version, nullptr, LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    release_PlateData_list(plates);
    return loaded;
}

} // namespace

TEST_CASE("A textured OBJ keeps its UV channel, V flipped", "[McpTexture][3mf]")
{
    const Model        model  = load_obj_model(fixture("cube.obj"));
    const ModelVolume& volume = only_volume(model);
    REQUIRE(volume.has_texture_uv());
    CHECK(volume.texture_uv.size() == 12 * 3);

    // Every corner of the channel is one of the file's vt, with v turned to 1 - v.
    const std::vector<Vec2f> vts = fixture_vts();
    REQUIRE(vts.size() == 24);
    for (const Vec2f& uv : volume.texture_uv) {
        const bool known = std::any_of(vts.begin(), vts.end(), [&](const Vec2f& vt) {
            return std::abs(vt.x() - uv.x()) < 1e-6f && std::abs((1.f - vt.y()) - uv.y()) < 1e-6f;
        });
        CHECK(known);
    }

    // A layer made on a part with a UV channel maps through it; on one without, it does not.
    CHECK(new_layer(0, volume.has_texture_uv()).projection_method == TextureProjectionMethod::ModelUV);
    CHECK(new_layer(0, false).projection_method != TextureProjectionMethod::ModelUV);
}

TEST_CASE("The same OBJ without its material library has no UV channel", "[McpTexture][3mf]")
{
    // The importer fills the channel only for faces that name a known material (correction 13): a material
    // library that is not there leaves the model as plain geometry.
    ScopedTemporaryDir dir("orca_texfiles_obj");
    const auto         obj = boost::filesystem::path(dir.string()) / "cube.obj";
    boost::filesystem::copy_file(fixture("cube.obj"), obj);
    const Model model = load_obj_model(obj);
    CHECK_FALSE(only_volume(model).has_texture_uv());
}

namespace {

TextureDisplacementLayer make_layer(int slot, const std::vector<unsigned char>& png)
{
    TextureDisplacementLayer layer = new_layer(slot, true);
    layer.name                     = "Bricks";
    layer.depth_mm                 = 1.25f;
    layer.tiling_scale             = 8.f;
    layer.rotation_deg             = 30.f;
    layer.offset                   = Vec2f(0.125f, -0.25f);
    layer.blend_mode               = TextureBlendMode::Subtract;
    layer.projection_method        = TextureProjectionMethod::Cylindrical;
    layer.edge_smoothing           = true;
    layer.edge_smoothing_amount    = 0.3f;
    layer.color_enabled            = true;
    layer.image_data               = std::make_shared<std::vector<unsigned char>>(png);
    return layer;
}

} // namespace

TEST_CASE("A texture layer, its image and its paint survive a project round trip", "[McpTexture][3mf]")
{
    Model model = load_obj_model(fixture("cube.obj"));
    REQUIRE(only_volume(model).has_texture_uv());
    ModelVolume&                   volume = *model.objects.front()->volumes.front();
    const std::vector<unsigned char> png  = file_bytes(fixture("cube.png"));
    REQUIRE_FALSE(png.empty());

    const TextureDisplacementLayer layer = make_layer(3, png);
    volume.texture_displacement_layers.push_back(layer);
    volume.texture_displacement_options.active_slot     = 3;
    volume.texture_displacement_options.smooth_enabled  = true;
    volume.texture_displacement_options.smooth_strength = 0.5f;

    // Four facets painted on the layer's mask.
    std::vector<int> states(volume.mesh().its.indices.size(), -1);
    for (int facet : {0, 3, 4, 9})
        states[size_t(facet)] = 1;
    PaintWrite write;
    REQUIRE(build_paint_write(volume.mesh(), PaintMode::Texture, states, true, {}, write));
    REQUIRE(apply_paint_data(volume, PaintChannel::texture(3), std::move(write.data)));

    ScopedTemporaryFile file(".3mf");
    store_project(model, file.string());

    ScopedTemporaryDir backup_dir("orca_texfiles_dst");
    Model              reloaded;
    REQUIRE(load_project(file.string(), reloaded, backup_dir));
    const ModelVolume& back = only_volume(reloaded);

    REQUIRE(back.texture_displacement_layers.size() == 1);
    const TextureDisplacementLayer& l = back.texture_displacement_layers.front();
    CHECK(l.slot == 3);
    CHECK(l.name == layer.name);
    CHECK_THAT(l.depth_mm, WithinAbs(layer.depth_mm, 1e-6));
    CHECK_THAT(l.tiling_scale, WithinAbs(layer.tiling_scale, 1e-6));
    CHECK_THAT(l.rotation_deg, WithinAbs(layer.rotation_deg, 1e-6));
    CHECK_THAT(l.offset.x(), WithinAbs(layer.offset.x(), 1e-6));
    CHECK_THAT(l.offset.y(), WithinAbs(layer.offset.y(), 1e-6));
    CHECK(l.blend_mode == TextureBlendMode::Subtract);
    CHECK(l.projection_method == TextureProjectionMethod::Cylindrical);
    CHECK(l.edge_smoothing);
    CHECK_THAT(l.edge_smoothing_amount, WithinAbs(layer.edge_smoothing_amount, 1e-6));
    CHECK(l.color_enabled);
    REQUIRE(l.image_data);
    CHECK(*l.image_data == png);
    CHECK(back.texture_displacement_options.active_slot == 3);
    CHECK(back.texture_displacement_options.smooth_enabled);
    CHECK_THAT(back.texture_displacement_options.smooth_strength, WithinAbs(0.5, 1e-6));

    // The mask reads back as the four painted facets.
    const std::vector<PaintedStateInfo> painted = read_volume_paint(back, PaintChannel::texture(3));
    const auto                          state1  = std::find_if(painted.begin(), painted.end(), [](const PaintedStateInfo& p) { return p.state == 1; });
    REQUIRE(state1 != painted.end());
    CHECK(state1->facet_count == 4);

    // The part's UV channel is kept, every corner as the part held it (V as flipped at the OBJ load).
    const ModelVolume& original = *model.objects.front()->volumes.front();
    REQUIRE(back.has_texture_uv());
    REQUIRE(back.texture_uv.size() == original.texture_uv.size());
    for (size_t i = 0; i < original.texture_uv.size(); ++i) {
        CHECK_THAT(back.texture_uv[i].x(), WithinAbs(original.texture_uv[i].x(), 1e-6));
        CHECK_THAT(back.texture_uv[i].y(), WithinAbs(original.texture_uv[i].y(), 1e-6));
    }
    // So a model_uv layer still maps through it after the reopen.
    CHECK(l.projection_method == TextureProjectionMethod::Cylindrical);
    CHECK(new_layer(4, back.has_texture_uv()).projection_method == TextureProjectionMethod::ModelUV);
}

TEST_CASE("A model_uv layer is still a model_uv layer after a project round trip", "[McpTexture][3mf]")
{
    Model model = load_obj_model(fixture("cube.obj"));
    REQUIRE(only_volume(model).has_texture_uv());
    const std::vector<unsigned char> png = file_bytes(fixture("cube.png"));
    REQUIRE_FALSE(png.empty());

    TextureDisplacementLayer layer = make_layer(0, png);
    layer.projection_method        = TextureProjectionMethod::ModelUV;
    model.objects.front()->volumes.front()->texture_displacement_layers.push_back(layer);

    ScopedTemporaryFile file(".3mf");
    store_project(model, file.string());

    ScopedTemporaryDir backup_dir("orca_texfiles_modeluv");
    Model              reloaded;
    REQUIRE(load_project(file.string(), reloaded, backup_dir));
    const ModelVolume& back = only_volume(reloaded);
    REQUIRE(back.texture_displacement_layers.size() == 1);
    CHECK(back.texture_displacement_layers.front().projection_method == TextureProjectionMethod::ModelUV);
}
