// The texture displacement gizmo's palette helpers live in libslic3r_gui; this is the suite that links it.
// Same Windows include prologue as test_filament_bitmap_utils.cpp (wx pulls in <windows.h>; keep
// WIN32_LEAN_AND_MEAN / NOMINMAX ahead of the Catch2 headers).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <set>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/Jobs/TextureDisplacementBakeJob.hpp"
#include "slic3r/GUI/TextureDisplacementObject.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Gizmo     = Slic3r::GUI::GLGizmoTextureDisplacement;
using Entry     = Gizmo::PaletteEntry;
using MixTarget = Gizmo::MixTarget;

namespace {

const ColorRGBA BLACK{ 0.f, 0.f, 0.f, 1.f };
const ColorRGBA WHITE{ 1.f, 1.f, 1.f, 1.f };
const ColorRGBA RED{ 1.f, 0.f, 0.f, 1.f };
const ColorRGBA BLUE{ 0.f, 0.f, 1.f, 1.f };
const ColorRGBA YELLOW{ 1.f, 1.f, 0.f, 1.f };

MixTarget target(const Vec3f &rgb, float weight)
{
    MixTarget t;
    RGB2Lab(rgb.x(), rgb.y(), rgb.z(), &t.lab.x(), &t.lab.y(), &t.lab.z());
    t.weight = weight;
    return t;
}

// The colour a mixed slot of these two filaments shows, as the sidebar computes it.
Vec3f slot_color(const ColorRGBA &a, const ColorRGBA &b, int a_percent)
{
    ColorRGB c;
    REQUIRE(decode_color(blend_color_multi({ encode_color(a), encode_color(b) }, { a_percent, 100 - a_percent }), c));
    return Vec3f(c.r(), c.g(), c.b());
}

// A colour image layer, through Slic3r's own PNG writer so decode_height_texture() reads it the way it
// reads an imported texture.
TextureDisplacementLayer color_layer(int w, int h, const std::vector<uint8_t> &rgb)
{
    ScopedTemporaryFile png(".png");
    REQUIRE(png::write_rgb_to_file(png.string(), size_t(w), size_t(h), rgb));
    std::ifstream              in(png.string(), std::ios::binary);
    std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    REQUIRE_FALSE(bytes.empty());

    TextureDisplacementLayer layer;
    layer.image_data    = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    layer.color_enabled = true;
    return layer;
}

// A red/green ramp over a fixed blue: colours spread over many bins, so the image is not flat-colour.
TextureDisplacementLayer gradient_layer()
{
    const int            n = 64;
    std::vector<uint8_t> rgb;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            rgb.push_back(uint8_t(x * 4));
            rgb.push_back(uint8_t(y * 4));
            rgb.push_back(128);
        }
    return color_layer(n, n, rgb);
}

// A 2048x1100 image, over mix_targets()' sampling budget, that is pure red wherever `red(x, y)` holds and
// elsewhere a gradient spread over far more than eight coarse bins, so the image is not flat-colour.
template<class RedFn> TextureDisplacementLayer striped_layer(RedFn red)
{
    const int            w = 2048, h = 1100;
    std::vector<uint8_t> rgb;
    rgb.reserve(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool is_red = red(x, y);
            rgb.push_back(is_red ? 255 : uint8_t(x * 255 / w));
            rgb.push_back(is_red ? 0 : uint8_t(y * 255 / h));
            rgb.push_back(is_red ? 0 : 128);
        }
    return color_layer(w, h, rgb);
}

// How much of the targets' weight is pure red.
float red_weight(const std::vector<MixTarget> &targets)
{
    Vec3f red;
    RGB2Lab(1.f, 0.f, 0.f, &red.x(), &red.y(), &red.z());
    float weight = 0.f;
    for (const MixTarget &t : targets)
        if ((t.lab - red).norm() < 3.f)
            weight += t.weight;
    return weight;
}

} // namespace

TEST_CASE("mix targets cover each colouring photo layer once and skip the rest", "[TextureColorPalette][TextureDisplacement]")
{
    TextureDisplacementLayer photo = gradient_layer();

    const std::vector<MixTarget> targets = Gizmo::mix_targets({ photo });
    REQUIRE_FALSE(targets.empty());
    float total = 0.f;
    for (const MixTarget &t : targets)
        total += t.weight;
    REQUIRE_THAT(total, WithinAbs(1., 1e-4));

    SECTION("A layer that does not colour gives no targets") {
        photo.color_enabled = false;
        REQUIRE(Gizmo::mix_targets({ photo }).empty());
    }
    SECTION("A flat-colour image prints in single filaments, so it gives no targets") {
        const TextureDisplacementLayer flat = color_layer(8, 8, std::vector<uint8_t>(8 * 8 * 3, 200));
        REQUIRE(Gizmo::mix_targets({ flat }).empty());
    }
}

TEST_CASE("mix targets of a large image weigh each colour by its share, whatever its stripes", "[TextureColorPalette][TextureDisplacement]")
{
    // Sampled rather than read in full, these must not line up with the samples: a fixed sampling step
    // sees only one phase of a stripe pattern, and a generator whose offsets repeat sees only some.
    SECTION("Red on every other column") {
        CHECK_THAT(red_weight(Gizmo::mix_targets({ striped_layer([](int x, int) { return x % 2 == 0; }) })),
                   WithinAbs(1. / 2., 0.03));
    }
    SECTION("Red on every fourth column") {
        CHECK_THAT(red_weight(Gizmo::mix_targets({ striped_layer([](int x, int) { return x % 4 == 0; }) })),
                   WithinAbs(1. / 4., 0.03));
    }
    SECTION("Red on diagonals") {
        CHECK_THAT(red_weight(Gizmo::mix_targets({ striped_layer([](int x, int y) { return (x - y) % 3 == 0; }) })),
                   WithinAbs(1. / 3., 0.03));
    }
}

TEST_CASE("the mix ranked first is the one the image needs most", "[TextureColorPalette][TextureDisplacement]")
{
    // The whole image is exactly the colour of a 1:1 black/white slot.
    const std::vector<MixTarget> targets = { target(slot_color(BLACK, WHITE, 50), 1.f) };
    const std::vector<Entry>     ranked  = Gizmo::rank_mixes({ BLACK, WHITE }, targets, 1);
    REQUIRE(ranked.size() == 1);
    CHECK(ranked.front().a == 0);
    CHECK(ranked.front().b == 1);
    CHECK(ranked.front().num * 2 == ranked.front().den);
}

TEST_CASE("an image the filaments already match ranks no mixes", "[TextureColorPalette][TextureDisplacement]")
{
    const std::vector<MixTarget> targets = { target(Vec3f(1.f, 0.f, 0.f), 0.5f), target(Vec3f(0.f, 0.f, 1.f), 0.5f) };
    REQUIRE(Gizmo::rank_mixes({ RED, BLUE }, targets, 8).empty());
}

TEST_CASE("ranked mixes stay within the limit and show their slot's colour", "[TextureColorPalette][TextureDisplacement]")
{
    const std::vector<ColorRGBA> filaments = { RED, BLUE, YELLOW };
    const std::vector<MixTarget> targets   = Gizmo::mix_targets({ gradient_layer() });

    for (const int limit : { 1, 3 }) {
        const std::vector<Entry> ranked = Gizmo::rank_mixes(filaments, targets, limit);
        CHECK(int(ranked.size()) <= limit);
        CHECK_FALSE(ranked.empty());
    }

    const std::vector<Entry> ranked = Gizmo::rank_mixes(filaments, targets, 6);
    for (const Entry &e : ranked) {
        REQUIRE(e.is_mix());
        const Vec3f expected = slot_color(filaments[size_t(e.a)], filaments[size_t(e.b)], e.a_percent());
        CHECK_THAT(e.rgb.x(), WithinAbs(expected.x(), 1e-6));
        CHECK_THAT(e.rgb.y(), WithinAbs(expected.y(), 1e-6));
        CHECK_THAT(e.rgb.z(), WithinAbs(expected.z(), 1e-6));
    }
}

TEST_CASE("picked mixes never need more slots than the project has free", "[TextureColorPalette][TextureDisplacement]")
{
    const std::vector<Entry> ranking = { { Vec3f::Zero(), 0, 1, 1, 2 }, { Vec3f::Zero(), 0, 1, 1, 3 }, { Vec3f::Zero(), 0, 1, 2, 3 } };
    const auto               reusable_second = [](const Entry &e) { return e.num == 1 && e.den == 3; };

    SECTION("The count caps the pick") {
        REQUIRE(Gizmo::pick_mixes(ranking, 2, 10, nullptr).size() == 2);
    }
    SECTION("With no free slot only a mix that already has one is kept") {
        const std::vector<Entry> picked = Gizmo::pick_mixes(ranking, 3, 0, reusable_second);
        REQUIRE(picked.size() == 1);
        CHECK(picked.front().den == 3);
        CHECK(picked.front().num == 1);
    }
    SECTION("A reusable mix costs no free slot") {
        const std::vector<Entry> picked = Gizmo::pick_mixes(ranking, 3, 1, reusable_second);
        REQUIRE(picked.size() == 2);
        CHECK(picked[0].den == 2);
        CHECK(picked[1].den == 3);
    }
}

TEST_CASE("a baked mix paints the slot it was given, wherever that slot sits", "[TextureColorPalette][TextureDisplacement]")
{
    // Two filaments, then two mixes of them. Palette index 2 is a mix, but its slot need not be
    // filament 2: a project that already holds other mixed slots puts it further along.
    const std::vector<Entry> palette = Gizmo::make_palette({ BLACK, WHITE }, { { Vec3f::Zero(), 0, 1, 1, 2 }, { Vec3f::Zero(), 0, 1, 1, 3 } });
    REQUIRE(palette.size() == 4);
    // Filament 0, the 1:1 mix twice, nothing: the 1:2 mix is never used.
    const std::vector<uint8_t> triangle_color = { 1, 3, 3, 0 };

    std::vector<Entry> asked;
    const auto         slot_seven = [&asked](const Entry &e) {
        asked.push_back(e);
        return 7;
    };
    const std::vector<int> filament = Gizmo::palette_filaments(palette, triangle_color, slot_seven);
    REQUIRE(filament.size() == 4);
    CHECK(filament[0] == 0);
    CHECK(filament[1] == 1);
    CHECK(filament[2] == 7);
    CHECK(filament[3] == -1);
    // Asking creates a slot, so an unused mix is never asked for.
    REQUIRE(asked.size() == 1);
    CHECK(asked.front().den == 2);

    SECTION("A mix that gets no slot prints in its dominant component") {
        const std::vector<int> fallback = Gizmo::palette_filaments(palette, { 3, 4 }, [](const Entry &) { return -1; });
        CHECK(fallback[2] == 0); // 1:1 - the first component
        CHECK(fallback[3] == 1); // 1 part black in 3 - white dominates
    }
}

TEST_CASE("the model's colour paint is drawn by filament, and only where no layer paint covers it", "[TextureColorPalette][TextureDisplacement]")
{
    // A strip of four triangles: filament 2 on the first and last, filament 5 on the second, and the third
    // left to the volume's own filament.
    indexed_triangle_set strip;
    strip.vertices = { Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(1, 1, 0), Vec3f(0, 2, 0), Vec3f(1, 2, 0) };
    strip.indices  = { stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(1, 3, 2), stl_triangle_vertex_indices(2, 3, 4),
                       stl_triangle_vertex_indices(3, 5, 4) };
    const TriangleMesh mesh(strip);
    TriangleSelector   paint(mesh);
    paint.set_facet(0, EnforcerBlockerType(2));
    paint.set_facet(1, EnforcerBlockerType(5));
    paint.set_facet(3, EnforcerBlockerType(2));

    const Gizmo::PaintedColors colors = Gizmo::painted_colors(mesh, paint.serialize());
    // Grouped by filament; the triangle in the volume's own filament is never drawn.
    REQUIRE(colors.facets.indices.size() == 3);
    REQUIRE(colors.source.size() == 3);
    REQUIRE(colors.state.size() == 3);
    CHECK(colors.state == std::vector<int>{ 2, 2, 5 });
    CHECK(colors.source == std::vector<int>{ 0, 3, 1 });

    SECTION("A model triangle a layer's paint covers is left to the preview") {
        std::vector<bool> excluded(mesh.its.indices.size(), false);
        excluded[3] = true;
        const std::vector<size_t> kept = colors.outside(excluded);
        REQUIRE(kept.size() == 2);
        CHECK(colors.source[kept[0]] == 0);
        CHECK(colors.source[kept[1]] == 1);
    }
    SECTION("A mask shorter than the model leaves the rest drawn") {
        CHECK(colors.outside({ true }).size() == 2);
    }
}

// ---------------------------------------------------------------------------------------------------------
// The panel's logic over the model: the same functions the gizmo's members and OrcaMCP call.
// ---------------------------------------------------------------------------------------------------------

namespace {

using namespace Slic3r::GUI;

// A modifier, then two model parts of 10 mm cubes, the second `second_z` mm up. Model volumes are kept where
// they are put, so the second part's bottom lies `second_z` above the first's.
ModelObject *two_part_object(Model &model, double second_z)
{
    ModelObject *object = model.add_object();
    object->add_instance();
    object->add_volume(make_cube(5, 5, 5), ModelVolumeType::PARAMETER_MODIFIER, false);
    object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    ModelVolume *upper = object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
    upper->set_offset(Vec3d(0., 0., second_z));
    return object;
}

void paint_facets(ModelVolume &volume, int slot, int count)
{
    TriangleSelector selector(volume.mesh());
    for (int i = 0; i < count; ++i)
        selector.set_facet(i, EnforcerBlockerType::ENFORCER);
    volume.texture_displacement_facet(slot).set(selector);
}

TextureDisplacementLayer slot_layer(int slot)
{
    TextureDisplacementLayer layer;
    layer.slot  = slot;
    layer.name  = "layer " + std::to_string(slot);
    layer.depth_mm = 1.f;
    return layer;
}

// A grayscale ramp image, through Slic3r's own PNG writer.
TextureDisplacementLayer gray_ramp_layer(int slot)
{
    const int            n = 16;
    std::vector<uint8_t> gray;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            gray.push_back(uint8_t((x * 16 + y * 5) % 256));
    ScopedTemporaryFile png(".png");
    REQUIRE(png::write_gray_to_file(png.string(), size_t(n), size_t(n), gray));
    std::ifstream              in(png.string(), std::ios::binary);
    std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    REQUIRE_FALSE(bytes.empty());

    TextureDisplacementLayer layer = slot_layer(slot);
    layer.image_data        = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    layer.tiling_scale      = 10.f;
    layer.projection_method = TextureProjectionMethod::Triplanar;
    return layer;
}

// A flat sheet of `cells` x `cells` squares, 20 mm across.
indexed_triangle_set sheet(int cells)
{
    indexed_triangle_set its;
    for (int y = 0; y <= cells; ++y)
        for (int x = 0; x <= cells; ++x)
            its.vertices.emplace_back(20.f * float(x) / float(cells), 20.f * float(y) / float(cells), 0.f);
    const auto at = [cells](int x, int y) { return y * (cells + 1) + x; };
    for (int y = 0; y < cells; ++y)
        for (int x = 0; x < cells; ++x) {
            its.indices.emplace_back(at(x, y), at(x + 1, y), at(x + 1, y + 1));
            its.indices.emplace_back(at(x, y), at(x + 1, y + 1), at(x, y + 1));
        }
    return its;
}

} // namespace

TEST_CASE("the layer stack belongs to the first model part, whatever comes before it", "[TextureObject][TextureDisplacement]")
{
    Model        model;
    ModelObject *object = two_part_object(model, 5.);
    const std::vector<ModelVolume *> parts = texture_parts(*object);

    REQUIRE(parts.size() == 2);
    CHECK(texture_stack_owner(*object) == parts.front());
    CHECK(texture_stack_owner(*object) != object->volumes.front()); // the modifier is first in the list
    CHECK(texture_anchor(*object).isApprox(Vec3d(texture_displacement_volume_to_world(*parts.front()).translation())));

    SECTION("An object without a model part has no owner") {
        Model        empty;
        ModelObject *bare = empty.add_object();
        CHECK(texture_stack_owner(*bare) == nullptr);
        CHECK(layers_for_part(*bare, *parts.front()).empty());
    }
    SECTION("Only the parts with paint are painted parts") {
        CHECK(painted_texture_parts(*object).empty());
        paint_facets(*parts.back(), 3, 2);
        REQUIRE(painted_texture_parts(*object).size() == 1);
        CHECK(painted_texture_parts(*object).front() == parts.back());
    }
}

TEST_CASE("a part other than the owner gets the stack without the fields that index the owner's mesh", "[TextureObject][TextureDisplacement]")
{
    Model        model;
    ModelObject *object = two_part_object(model, 5.);
    const std::vector<ModelVolume *> parts = texture_parts(*object);
    TextureDisplacementLayer         layer = slot_layer(0);
    layer.lscm_seam_edges = { { 0, 1 } };
    layer.islands         = { TextureIsland{} };
    layer.island_groups   = { 0 };
    layer.depth_mm        = 2.5f;
    parts.front()->texture_displacement_layers = { layer };

    const std::vector<TextureDisplacementLayer> own   = layers_for_part(*object, *parts.front());
    const std::vector<TextureDisplacementLayer> other = layers_for_part(*object, *parts.back());
    REQUIRE(own.size() == 1);
    REQUIRE(other.size() == 1);
    CHECK(own.front().lscm_seam_edges.size() == 1);
    CHECK(own.front().islands.size() == 1);
    CHECK(own.front().island_groups.size() == 1);
    CHECK(other.front().lscm_seam_edges.empty());
    CHECK(other.front().islands.empty());
    CHECK(other.front().island_groups.empty());
    CHECK_THAT(other.front().depth_mm, WithinAbs(2.5, 1e-6));
}

TEST_CASE("a part held above the lowest one rests that much higher", "[TextureObject][TextureDisplacement]")
{
    Model        model;
    ModelObject *object = two_part_object(model, 5.);
    const std::vector<ModelVolume *> parts = texture_parts(*object);

    const std::vector<float> drops = resting_drops(*object, parts);
    REQUIRE(drops.size() == 2);
    CHECK_THAT(drops[0], WithinAbs(0., 1e-5));
    CHECK_THAT(drops[1], WithinAbs(5., 1e-5));

    SECTION("From bare meshes and transforms") {
        const indexed_triangle_set cube = its_make_cube(10, 10, 10);
        Transform3d                up   = Transform3d::Identity();
        up.translate(Vec3d(0., 0., 7.));
        const std::vector<std::pair<const indexed_triangle_set *, Transform3d>> all{ { &cube, Transform3d::Identity() }, { &cube, up } };
        const std::vector<float> both = resting_drops(all, { 1, 0 });
        REQUIRE(both.size() == 2);
        CHECK_THAT(both[0], WithinAbs(7., 1e-5));
        CHECK_THAT(both[1], WithinAbs(0., 1e-5));
    }
    SECTION("One part is sampled in its own coordinates, several in the bake frame") {
        Model        single_model;
        ModelObject *single = single_model.add_object();
        single->add_instance();
        ModelVolume *only = single->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
        CHECK(prepare_texture_frame(*single, *only).isApprox(Transform3d::Identity()));
        CHECK_FALSE(prepare_texture_frame(*object, *parts.back()).isApprox(Transform3d::Identity()));
    }
}

TEST_CASE("removing a layer clears its slot on every part", "[TextureObject][TextureDisplacement]")
{
    Model        model;
    ModelObject *object = two_part_object(model, 5.);
    const std::vector<ModelVolume *> parts = texture_parts(*object);
    parts.front()->texture_displacement_layers = { slot_layer(0), slot_layer(1) };
    for (ModelVolume *part : parts) {
        paint_facets(*part, 0, 2);
        paint_facets(*part, 1, 3);
    }

    CHECK(remove_texture_layer(*object, 1));
    REQUIRE(parts.front()->texture_displacement_layers.size() == 1);
    CHECK(parts.front()->texture_displacement_layers.front().slot == 0);
    for (const ModelVolume *part : parts) {
        CHECK(part->texture_displacement_facet(1).empty());
        CHECK_FALSE(part->texture_displacement_facet(0).empty());
    }
    CHECK_FALSE(remove_texture_layer(*object, 1)); // already gone
}

TEST_CASE("moving a layer exchanges slots one neighbour at a time and keeps the others in order", "[TextureObject][TextureDisplacement]")
{
    Model        model;
    ModelObject *object = two_part_object(model, 5.);
    const std::vector<ModelVolume *> parts = texture_parts(*object);
    parts.front()->texture_displacement_layers = { slot_layer(0), slot_layer(1), slot_layer(2) };
    for (ModelVolume *part : parts)
        for (int slot = 0; slot < 3; ++slot)
            paint_facets(*part, slot, slot + 1); // slot n holds n + 1 facets: tells the masks apart

    const auto facets_in = [&parts](int slot) { return parts.back()->texture_displacement_facet(slot).get_data().triangles_to_split.size(); };
    const auto name_in   = [&parts](int slot) {
        for (const TextureDisplacementLayer &l : parts.front()->texture_displacement_layers)
            if (l.slot == slot)
                return l.name;
        return std::string();
    };

    SECTION("To the end of the stack") {
        const std::vector<std::pair<int, int>> swaps = texture_layer_move_swaps(*object, 0, 3);
        CHECK(swaps == std::vector<std::pair<int, int>>{ { 0, 1 }, { 1, 2 } });

        std::vector<std::pair<int, int>> told;
        CHECK(move_texture_layer(*object, 0, 3, [&told](int a, int b) { told.emplace_back(a, b); }));
        CHECK(told == swaps);
        // The moved layer is last now; the two others shifted down one, in their order.
        CHECK(name_in(2) == "layer 0");
        CHECK(name_in(0) == "layer 1");
        CHECK(name_in(1) == "layer 2");
        CHECK(facets_in(2) == 1);
        CHECK(facets_in(0) == 2);
        CHECK(facets_in(1) == 3);
    }
    SECTION("Back to the front") {
        CHECK(move_texture_layer(*object, 2, 0));
        CHECK(name_in(0) == "layer 2");
        CHECK(name_in(1) == "layer 0");
        CHECK(name_in(2) == "layer 1");
        CHECK(facets_in(0) == 3);
    }
    SECTION("A move to where it is, an unknown slot and equal slots change nothing") {
        CHECK_FALSE(move_texture_layer(*object, 1, 1));
        CHECK_FALSE(move_texture_layer(*object, 1, 2)); // inserting after itself
        CHECK_FALSE(move_texture_layer(*object, 5, 0));
        CHECK_FALSE(swap_texture_layer_slots(*object, 1, 1));
        CHECK_FALSE(swap_texture_layer_slots(*object, 1, 9));
        CHECK(name_in(1) == "layer 1");
        CHECK(facets_in(1) == 2);
    }
}

TEST_CASE("a seam path follows real edges between two corners of a cube", "[TextureObject][TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10, 10, 10);
    const auto corner = [&cube](float v) {
        for (size_t i = 0; i < cube.vertices.size(); ++i)
            if (cube.vertices[i].isApprox(Vec3f(v, v, v)))
                return int(i);
        FAIL("no such corner");
        return -1;
    };
    const int from = corner(0.f), to = corner(10.f);

    std::set<std::pair<int, int>> mesh_edges;
    for (const stl_triangle_vertex_indices &tri : cube.indices)
        for (int i = 0; i < 3; ++i)
            mesh_edges.insert({ std::min(tri[i], tri[(i + 1) % 3]), std::max(tri[i], tri[(i + 1) % 3]) });

    const std::vector<std::pair<int, int>> path = seam_path_edges(cube, from, to);
    REQUIRE_FALSE(path.empty());
    for (const std::pair<int, int> &edge : path)
        CHECK(mesh_edges.count(edge) == 1);
    // Walked from the end back to the start, each edge begins where the last one ended.
    int at = to;
    for (const std::pair<int, int> &edge : path) {
        REQUIRE((edge.first == at || edge.second == at));
        at = edge.first == at ? edge.second : edge.first;
    }
    CHECK(at == from);

    SECTION("Nothing for the same vertex or one the mesh lacks") {
        CHECK(seam_path_edges(cube, from, from).empty());
        CHECK(seam_path_edges(cube, from, 999).empty());
        CHECK(seam_path_edges(cube, -1, to).empty());
    }
}

TEST_CASE("cutting an island adds the edges that cross its middle, and only those", "[TextureObject][TextureDisplacement]")
{
    const indexed_triangle_set box = its_make_cube(60, 10, 10);
    const PatchUnwrap          unwrap = compute_patch_unwrap(box);
    REQUIRE(unwrap.chart_count > 1);

    bool any_cut = false;
    for (int chart = 0; chart < unwrap.chart_count; ++chart) {
        const std::vector<std::pair<int, int>> cut = cut_island_seams(box, unwrap, chart, {});
        // The chart's own extent, as the cut measures it.
        Vec3f lo = Vec3f::Constant(std::numeric_limits<float>::max()), hi = Vec3f::Constant(std::numeric_limits<float>::lowest());
        for (size_t v = 0; v < unwrap.vertex_chart.size(); ++v)
            if (unwrap.vertex_chart[v] == chart) {
                lo = lo.cwiseMin(box.vertices[size_t(unwrap.source_vertex[v])]);
                hi = hi.cwiseMax(box.vertices[size_t(unwrap.source_vertex[v])]);
            }
        const Vec3f ext  = hi - lo;
        const int   axis = (ext.x() >= ext.y() && ext.x() >= ext.z()) ? 0 : (ext.y() >= ext.z() ? 1 : 2);
        const float mid  = 0.5f * (lo[axis] + hi[axis]);
        for (const std::pair<int, int> &edge : cut) {
            CHECK((box.vertices[size_t(edge.first)][axis] < mid) != (box.vertices[size_t(edge.second)][axis] < mid));
            CHECK(edge.first < edge.second);
        }
        any_cut = any_cut || !cut.empty();
    }
    CHECK(any_cut);

    SECTION("Existing seams are kept, and a chart with no triangle changes nothing") {
        const std::vector<std::pair<int, int>> existing{ { 0, 1 } };
        const std::vector<std::pair<int, int>> cut = cut_island_seams(box, unwrap, 0, existing);
        CHECK(std::find(cut.begin(), cut.end(), existing.front()) != cut.end());
        CHECK(cut_island_seams(box, unwrap, 999, existing) == existing);
    }
    SECTION("A face joins the neighbour it shares an edge with") {
        const std::optional<std::pair<int, TextureIsland>> target = join_target(unwrap, {}, 0);
        REQUIRE(target.has_value());
        CHECK(target->first != 0);
        CHECK(target->first < unwrap.chart_count);
    }
}

TEST_CASE("the bake planned from a captured input is the bake of the same mesh, and goes stale when the model changes", "[TextureObject][TextureDisplacement]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->add_instance();
    ModelVolume *volume = object->add_volume(TriangleMesh(sheet(12)), ModelVolumeType::MODEL_PART, false);
    volume->texture_displacement_layers = { gray_ramp_layer(0) };
    volume->texture_displacement_options.pipeline_v2 = false; // moves the vertices it has: quick and exact
    paint_facets(*volume, 0, int(volume->mesh().its.indices.size()));

    TextureDisplacementBakeInput input = capture_texture_bake(*object, {}, false);
    REQUIRE(input.volumes.size() == 1);
    CHECK_FALSE(input.take_snapshot);
    CHECK(texture_bake_applies_to(model, input));

    int last_percent = 0;
    const TextureBakePlan plan = plan_texture_bake(input, [&last_percent](int percent) {
        last_percent = percent;
        return true;
    });
    REQUIRE_FALSE(plan.canceled);
    REQUIRE(plan.results.size() == 1);
    CHECK(last_percent > 0);

    const TextureDisplacementBakeVolume &captured = input.volumes.front();
    const indexed_triangle_set           direct   = build_texture_displacement(
        captured.base_mesh, captured.layers, captured.facets_data, captured.options, {}, nullptr, captured.volume_to_world, nullptr,
        nullptr, nullptr, &captured.texture_anchor, captured.resting_drop);
    CHECK(plan.results.front().mesh.its.indices == direct.indices);
    CHECK(plan.results.front().mesh.its.vertices == direct.vertices);
    CHECK(texture_bake_moved(input, plan, 0));

    SECTION("A progress hook that says no stops the plan") {
        const TextureBakePlan stopped = plan_texture_bake(input, [](int) { return false; });
        CHECK(stopped.canceled);
        CHECK_FALSE(texture_bake_moved(input, stopped, 0));
    }
    SECTION("A mask edited since the capture") {
        volume->texture_displacement_facet(0).reset();
        CHECK_FALSE(texture_bake_applies_to(model, input));
    }
    SECTION("A mesh changed since the capture") {
        indexed_triangle_set nudged = volume->mesh().its;
        nudged.vertices.front().z() += 0.5f;
        volume->set_mesh(TriangleMesh(nudged));
        CHECK_FALSE(texture_bake_applies_to(model, input));
    }
    SECTION("A layer edited since the capture") {
        volume->texture_displacement_layers.front().depth_mm += 1.f;
        CHECK_FALSE(texture_bake_applies_to(model, input));
    }
    SECTION("A part that is gone") {
        object->delete_volume(0);
        CHECK_FALSE(texture_bake_applies_to(model, input));
    }
    SECTION("An object with no paint captures nothing") {
        volume->texture_displacement_facet(0).reset();
        CHECK(capture_texture_bake(*object, {}, true).volumes.empty());
    }
}
