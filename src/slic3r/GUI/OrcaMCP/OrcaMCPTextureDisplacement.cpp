// src/slic3r/GUI/OrcaMCP/OrcaMCPTextureDisplacement.cpp
#include "OrcaMCPTextureDisplacement.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "slic3r/GUI/TextureDisplacementObject.hpp"

namespace Slic3r { namespace GUI { namespace OrcaMCP {

using nlohmann::json;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string join_names(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

std::string number_text(double v)
{
    std::ostringstream os;
    os << v;
    return os.str();
}

template<class E> std::optional<E> named(const std::vector<std::string>& names, const std::string& name)
{
    const auto it = std::find(names.begin(), names.end(), lower(name));
    if (it == names.end())
        return std::nullopt;
    return E(int(it - names.begin()));
}

std::string file_name_of(const TextureLibraryEntry& entry)
{
    return std::filesystem::path(entry.path).filename().string();
}

} // namespace

// ---- Names ----

const std::vector<std::string>& projection_names()
{
    static const std::vector<std::string> names{"triplanar", "cylindrical", "spherical", "unwrap", "view", "model_uv"};
    return names;
}
std::optional<TextureProjectionMethod> projection_named(const std::string& name)
{
    return named<TextureProjectionMethod>(projection_names(), name);
}
std::string projection_name(TextureProjectionMethod m) { return projection_names().at(size_t(m)); }

const std::vector<std::string>& blend_names()
{
    static const std::vector<std::string> names{"add", "override", "subtract", "multiply", "divide"};
    return names;
}
namespace {
// The panel's combo order is not the enum's (Add, Subtract, Multiply, Divide, Override).
constexpr TextureBlendMode blend_modes[] = {TextureBlendMode::Add, TextureBlendMode::Override, TextureBlendMode::Subtract,
                                            TextureBlendMode::Multiply, TextureBlendMode::Divide};
} // namespace
std::optional<TextureBlendMode> blend_named(const std::string& name)
{
    const auto it = std::find(blend_names().begin(), blend_names().end(), lower(name));
    if (it == blend_names().end())
        return std::nullopt;
    return blend_modes[it - blend_names().begin()];
}
std::string blend_name(TextureBlendMode m)
{
    for (size_t i = 0; i < std::size(blend_modes); ++i)
        if (blend_modes[i] == m)
            return blend_names()[i];
    return blend_names().front();
}

const std::vector<std::string>& tile_method_names()
{
    static const std::vector<std::string> names{"repeat", "mirrored_repeat"};
    return names;
}
std::optional<TextureTileMethod> tile_method_named(const std::string& name) { return named<TextureTileMethod>(tile_method_names(), name); }
std::string tile_method_name(TextureTileMethod m) { return tile_method_names().at(size_t(m)); }

const std::vector<std::string>& view_direction_names()
{
    static const std::vector<std::string> names{"current", "iso", "top", "bottom", "front", "back", "left", "right"};
    return names;
}

const char* texture_source_name(TextureSource source)
{
    switch (source) {
    case TextureSource::Shipped: return "builtin";
    case TextureSource::User: return "user";
    case TextureSource::Plugin: return "pack";
    }
    return "builtin";
}

// ---- The library, by id ----

std::vector<std::string> texture_ids(const std::vector<TextureLibraryEntry>& entries)
{
    std::vector<std::string>   ids;
    std::map<std::string, int> seen;
    ids.reserve(entries.size());
    for (const TextureLibraryEntry& e : entries) {
        std::string id = texture_source_name(e.source);
        id += "/";
        if (e.source == TextureSource::Plugin)
            id += e.pack_name + "/";
        id += file_name_of(e);
        const int n = ++seen[id];
        ids.push_back(n == 1 ? id : id + " #" + std::to_string(n));
    }
    return ids;
}

TextureLookup find_texture(const std::vector<TextureLibraryEntry>& entries, const std::vector<std::string>& ids, const std::string& query)
{
    TextureLookup out;
    for (size_t i = 0; i < ids.size() && i < entries.size(); ++i)
        if (ids[i] == query) {
            out.index = int(i);
            return out;
        }
    std::vector<size_t> matches;
    const std::string   q = lower(query);
    for (size_t i = 0; i < ids.size() && i < entries.size(); ++i)
        if (lower(ids[i]) == q)
            matches.push_back(i);
    if (matches.empty())
        for (size_t i = 0; i < entries.size(); ++i)
            if (lower(entries[i].name) == q)
                matches.push_back(i);
    if (matches.size() == 1) {
        out.index = int(matches.front());
        return out;
    }
    if (matches.empty()) {
        out.error = "No texture \"" + query + "\": get_texture_library lists the texture ids";
        return out;
    }
    std::vector<std::string> candidates;
    for (size_t i : matches)
        candidates.push_back(ids[i]);
    out.error = "\"" + query + "\" names more than one texture: send one id of " + join_names(candidates);
    return out;
}

std::optional<int> library_index_of(const std::vector<TextureLibraryEntry>& entries, const TextureDisplacementLayer& layer)
{
    if (layer.path.empty())
        return std::nullopt;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].path == layer.path)
            return int(i);
    return std::nullopt;
}

std::optional<bool> png_has_color(const unsigned char* bytes, std::size_t size)
{
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (bytes == nullptr || size < 26 || !std::equal(signature, signature + 8, bytes) || std::string(bytes + 12, bytes + 16) != "IHDR")
        return std::nullopt;
    const unsigned bit_depth = bytes[24], color_type = bytes[25];
    if (bit_depth != 8)
        return std::nullopt;
    if (color_type == 0)
        return false;
    if (color_type == 2 || color_type == 6)
        return true;
    return std::nullopt;
}

std::optional<bool> png_color_from_file(const std::string& path)
{
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in)
        return std::nullopt;
    unsigned char head[33] = {};
    in.read(reinterpret_cast<char*>(head), sizeof head);
    return png_has_color(head, size_t(in.gcount()));
}

json library_entry_json(const TextureLibraryEntry& entry, const std::string& id, std::optional<bool> color)
{
    json out = {{"id", id}, {"name", entry.name}, {"source", texture_source_name(entry.source)}};
    if (entry.source == TextureSource::Plugin)
        out["pack"] = entry.pack_name;
    out["file"]  = file_name_of(entry);
    out["path"]  = entry.path;
    out["color"] = color ? json(*color) : json(nullptr);
    return out;
}

namespace {

// The image has colour a layer could print with: some pixel is not grey.
bool has_real_color(const DecodedHeightTexture& t)
{
    if (!t.has_color())
        return false;
    for (size_t i = 0; i + 2 < t.rgb.size(); i += 3)
        if (t.rgb[i] != t.rgb[i + 1] || t.rgb[i] != t.rgb[i + 2])
            return true;
    return false;
}

void fnv(std::uint64_t& h, const std::uint8_t* data, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
}

} // namespace

TextureIdentity texture_identity(const DecodedHeightTexture& t)
{
    TextureIdentity id;
    id.width  = t.width;
    id.height = t.height;
    id.hash   = 1469598103934665603ull;
    const int dims[2] = {t.width, t.height};
    fnv(id.hash, reinterpret_cast<const std::uint8_t*>(dims), sizeof dims);
    fnv(id.hash, t.pixels.data(), t.pixels.size());
    if (has_real_color(t))
        fnv(id.hash, t.rgb.data(), t.rgb.size());
    return id;
}

bool same_texture(const DecodedHeightTexture& a, const DecodedHeightTexture& b)
{
    if (a.width != b.width || a.height != b.height || a.pixels != b.pixels)
        return false;
    const bool ca = has_real_color(a), cb = has_real_color(b);
    return ca == cb && (!ca || a.rgb == b.rgb);
}

// ---- One call's layer edit ----

bool LayerSettings::empty() const
{
    return !depth_mm && !tile_size_mm && !rotation_deg && !midlevel && !smoothing && !edge_fade_amount && !offset && !invert &&
           !edge_fade && !tile && !color && !tile_method && !projection && !blend && !view_direction && !view_axes &&
           !projection_frame && !clear_projection_frame && !adjust_image && !color_mix && !color_mix_count && !color_denoise;
}

const std::vector<SettingBound>& layer_setting_bounds()
{
    static const std::vector<SettingBound> bounds = {
        {"depth_mm", 0.01, 10., "mm", "Depth"},
        {"tile_size_mm", 0.2, 200., "mm", "Tile size"},
        {"rotation_deg", 0., 360., "degrees", "Rotation"},
        {"midlevel", 0., 10., "", "Midlevel"},
        {"smoothing", 0., 1., "", "Smoothing"},
        {"edge_fade_amount", 0.02, 1., "", "Edge fade"},
        {"color_mix_count", 1., 32., "", "Mixed colors"},
        {"color_denoise", 0., 6., "", "Denoise"},
        {"brightness", -1., 1., "", "Brightness"},
        {"contrast", -1., 1., "", "Contrast"},
        {"sharpness", 0., 1., "", "Sharpness"},
        {"blur", 0., 1., "", "Blur"},
        {"hue_deg", -180., 180., "degrees", "Hue"},
    };
    return bounds;
}

const SettingBound* setting_bound(const std::string& key)
{
    for (const SettingBound& b : layer_setting_bounds())
        if (key == b.key)
            return &b;
    return nullptr;
}

namespace {

constexpr int MAX_SLOT = int(TEXTURE_DISPLACEMENT_MAX_LAYERS) - 1;

struct Reader
{
    const json&  obj;
    std::string& error;

    bool has(const char* key) const { return obj.contains(key); }

    bool fail(const std::string& message)
    {
        if (error.empty())
            error = message;
        return false;
    }

    bool number(const char* key, double& out, const char* context = "")
    {
        const json& v = obj.at(key);
        if (!v.is_number() || !std::isfinite(v.get<double>()))
            return fail(std::string(context) + key + " must be a number");
        out = v.get<double>();
        return true;
    }

    // A number held to layer_setting_bounds().
    bool bounded(const char* key, std::optional<float>& out)
    {
        if (!has(key))
            return true;
        double v = 0;
        if (!number(key, v))
            return false;
        const SettingBound* b = setting_bound(key);
        if (b != nullptr && (v < b->min || v > b->max))
            return fail(std::string(key) + " must be from " + number_text(b->min) + " to " + number_text(b->max) +
                        (*b->unit ? std::string(" ") + b->unit : std::string()) + " (the panel's " + b->panel_label + "), not " +
                        number_text(v));
        out = float(v);
        return true;
    }

    bool bounded_int(const char* key, std::optional<int>& out)
    {
        if (!has(key))
            return true;
        if (!obj.at(key).is_number_integer())
            return fail(std::string(key) + " must be an integer");
        const long long v = obj.at(key).get<long long>();
        const SettingBound* b = setting_bound(key);
        if (b != nullptr && (double(v) < b->min || double(v) > b->max))
            return fail(std::string(key) + " must be from " + number_text(b->min) + " to " + number_text(b->max) + " (the panel's " +
                        b->panel_label + "), not " + std::to_string(v));
        out = int(v);
        return true;
    }

    bool boolean(const char* key, std::optional<bool>& out)
    {
        if (!has(key))
            return true;
        if (!obj.at(key).is_boolean())
            return fail(std::string(key) + " must be true or false");
        out = obj.at(key).get<bool>();
        return true;
    }

    bool string(const char* key, std::optional<std::string>& out)
    {
        if (!has(key))
            return true;
        if (!obj.at(key).is_string())
            return fail(std::string(key) + " must be a string");
        out = obj.at(key).get<std::string>();
        return true;
    }

    template<class E, class F> bool choice(const char* key, const std::vector<std::string>& names, F parse, std::optional<E>& out)
    {
        std::optional<std::string> text;
        if (!string(key, text))
            return false;
        if (!text)
            return true;
        const auto value = parse(*text);
        if (!value)
            return fail(std::string(key) + " must be one of " + join_names(names) + ", not \"" + *text + "\"");
        out = *value;
        return true;
    }
};

bool read_vec(const json& v, const std::string& what, size_t n, std::vector<double>& out, std::string& error)
{
    if (!v.is_array() || v.size() != n) {
        error = what + " must be an array of " + std::to_string(n) + " numbers";
        return false;
    }
    out.clear();
    for (const json& e : v) {
        if (!e.is_number() || !std::isfinite(e.get<double>())) {
            error = what + " must be an array of " + std::to_string(n) + " finite numbers";
            return false;
        }
        out.push_back(e.get<double>());
    }
    return true;
}

bool read_vec3(const json& v, const std::string& what, Vec3d& out, std::string& error)
{
    std::vector<double> c;
    if (!read_vec(v, what, 3, c, error))
        return false;
    out = Vec3d(c[0], c[1], c[2]);
    return true;
}

// Two unit axes that are neither zero nor parallel.
bool axes_usable(const Vec3d& right, const Vec3d& up)
{
    if (right.norm() < 1e-9 || up.norm() < 1e-9)
        return false;
    return right.normalized().cross(up.normalized()).norm() > 1e-6;
}

bool read_axes(const json& obj, const std::string& what, Vec3d& right, Vec3d& up, std::string& error)
{
    if (!obj.contains("right") || !obj.contains("up")) {
        error = what + " needs both right and up";
        return false;
    }
    if (!read_vec3(obj.at("right"), what + " right", right, error) || !read_vec3(obj.at("up"), what + " up", up, error))
        return false;
    if (!axes_usable(right, up)) {
        error = what + ": right and up must be non-zero and not parallel";
        return false;
    }
    return true;
}

bool read_frame(const json& v, ProjectionFrameRequest& frame, std::string& error)
{
    static const std::set<std::string> keys = {"view", "right", "up", "center", "width_mm", "height_mm"};
    if (!v.is_object()) {
        error = "projection_frame must be an object, or false to clear it";
        return false;
    }
    for (auto it = v.begin(); it != v.end(); ++it)
        if (!keys.count(it.key())) {
            error = "projection_frame has no key \"" + it.key() + "\". Its keys: view, right, up, center, width_mm, height_mm";
            return false;
        }
    Reader r{v, error};
    std::optional<std::string> view;
    if (!r.string("view", view))
        return false;
    const bool has_axes = v.contains("right") || v.contains("up");
    if (view && has_axes) {
        error = "projection_frame takes a view or right and up, not both";
        return false;
    }
    if (!view && !has_axes) {
        error = "projection_frame needs a view (a named view or current) or right and up";
        return false;
    }
    if (view) {
        const auto& names = view_direction_names();
        if (std::find(names.begin(), names.end(), lower(*view)) == names.end()) {
            error = "projection_frame view must be one of " + join_names(names) + ", not \"" + *view + "\"";
            return false;
        }
        frame.view = lower(*view);
    } else {
        Vec3d right, up;
        if (!read_axes(v, "projection_frame", right, up, error))
            return false;
        frame.right = right;
        frame.up    = up;
    }
    if (v.contains("center") && !read_vec3(v.at("center"), "projection_frame center", frame.center, error))
        return false;
    for (const char* key : {"width_mm", "height_mm"}) {
        if (!v.contains(key)) {
            error = std::string("projection_frame needs ") + key;
            return false;
        }
        double d = 0;
        if (!r.number(key, d, "projection_frame "))
            return false;
        if (!(d > 0.)) {
            error = std::string("projection_frame ") + key + " must be greater than 0 mm";
            return false;
        }
        (std::string(key) == "width_mm" ? frame.width_mm : frame.height_mm) = d;
    }
    return true;
}

bool read_adjust(const json& v, ImageAdjustRequest& adjust, std::string& error)
{
    static const std::set<std::string> keys = {"brightness", "contrast", "sharpness", "blur", "hue_deg"};
    if (!v.is_object()) {
        error = "adjust_image must be an object";
        return false;
    }
    for (auto it = v.begin(); it != v.end(); ++it)
        if (!keys.count(it.key())) {
            error = "adjust_image has no key \"" + it.key() + "\". Its keys: brightness, contrast, sharpness, blur, hue_deg";
            return false;
        }
    Reader r{v, error};
    if (!r.bounded("brightness", adjust.brightness) || !r.bounded("contrast", adjust.contrast) ||
        !r.bounded("sharpness", adjust.sharpness) || !r.bounded("blur", adjust.blur) || !r.bounded("hue_deg", adjust.hue_deg))
        return false;
    if (!adjust.brightness && !adjust.contrast && !adjust.sharpness && !adjust.blur && !adjust.hue_deg) {
        error = "adjust_image needs at least one of brightness, contrast, sharpness, blur, hue_deg";
        return false;
    }
    return true;
}

// Every argument that edits a layer or the stack: remove and move take none of them.
const char* const layer_edit_keys[] = {"texture", "file_path", "depth_mm", "tile_size_mm", "rotation_deg", "midlevel", "smoothing",
                                       "edge_fade_amount", "offset", "invert", "edge_fade", "tile", "color", "color_mix", "tile_method",
                                       "projection", "blend", "view_direction", "view_axes", "projection_frame", "adjust_image",
                                       "color_mix_count", "color_denoise"};

} // namespace

std::optional<LayerRequest> read_layer_request(const json& params, std::string& error)
{
    error.clear();
    if (!params.is_object()) {
        error = "arguments must be an object";
        return std::nullopt;
    }
    Reader       r{params, error};
    LayerRequest req;

    std::optional<std::string> action;
    if (!r.string("action", action))
        return std::nullopt;
    if (!action) {
        error = "action is required: add, edit, remove or move";
        return std::nullopt;
    }
    if (*action == "add") req.action = LayerAction::add;
    else if (*action == "edit") req.action = LayerAction::edit;
    else if (*action == "remove") req.action = LayerAction::remove;
    else if (*action == "move") req.action = LayerAction::move;
    else {
        error = "action must be add, edit, remove or move, not \"" + *action + "\"";
        return std::nullopt;
    }

    if (r.has("layer")) {
        if (!params.at("layer").is_number_integer()) {
            error = "layer must be an integer slot from 0 to " + std::to_string(MAX_SLOT);
            return std::nullopt;
        }
        const long long slot = params.at("layer").get<long long>();
        if (slot < 0 || slot > MAX_SLOT) {
            error = "layer must be a slot from 0 to " + std::to_string(MAX_SLOT) + ", not " + std::to_string(slot);
            return std::nullopt;
        }
        req.layer = int(slot);
    }
    if (r.has("to_position")) {
        if (!params.at("to_position").is_number_integer() || params.at("to_position").get<long long>() < 0) {
            error = "to_position must be an integer from 0 (the position in slot order)";
            return std::nullopt;
        }
        req.to_position = int(std::min<long long>(params.at("to_position").get<long long>(), 1000));
    }
    if (req.action == LayerAction::add && req.layer) {
        error = "add takes the first free slot, as the panel does: leave layer out";
        return std::nullopt;
    }
    if (req.action != LayerAction::add && !req.layer) {
        error = "action " + *action + " needs layer, the layer's slot (get_texture_displacement lists them)";
        return std::nullopt;
    }
    if (req.action == LayerAction::move && !req.to_position) {
        error = "action move needs to_position, the 0-based position in slot order";
        return std::nullopt;
    }
    if (req.action != LayerAction::move && req.to_position) {
        error = "to_position goes with action move only";
        return std::nullopt;
    }
    if (req.action == LayerAction::remove || req.action == LayerAction::move)
        for (const char* key : layer_edit_keys)
            if (r.has(key)) {
                error = "action " + *action + " takes no " + key;
                return std::nullopt;
            }

    LayerSettings& s = req.settings;
    if (!r.string("texture", req.texture) || !r.string("file_path", req.file_path))
        return std::nullopt;
    if (req.texture && req.file_path) {
        error = "texture and file_path are two ways to pick the image: send one";
        return std::nullopt;
    }
    if (!r.bounded("depth_mm", s.depth_mm) || !r.bounded("tile_size_mm", s.tile_size_mm) || !r.bounded("rotation_deg", s.rotation_deg) ||
        !r.bounded("midlevel", s.midlevel) || !r.bounded("smoothing", s.smoothing) || !r.bounded("edge_fade_amount", s.edge_fade_amount))
        return std::nullopt;
    if (r.has("offset")) {
        std::vector<double> c;
        if (!read_vec(params.at("offset"), "offset", 2, c, error))
            return std::nullopt;
        s.offset = Vec2f(float(c[0]), float(c[1]));
    }
    if (!r.boolean("invert", s.invert) || !r.boolean("edge_fade", s.edge_fade) || !r.boolean("tile", s.tile) ||
        !r.boolean("color", s.color) || !r.boolean("color_mix", s.color_mix))
        return std::nullopt;
    if (!r.choice("tile_method", tile_method_names(), tile_method_named, s.tile_method) ||
        !r.choice("projection", projection_names(), projection_named, s.projection) ||
        !r.choice("blend", blend_names(), blend_named, s.blend))
        return std::nullopt;

    if (!r.string("view_direction", s.view_direction))
        return std::nullopt;
    if (s.view_direction) {
        const auto& names = view_direction_names();
        s.view_direction  = lower(*s.view_direction);
        if (std::find(names.begin(), names.end(), *s.view_direction) == names.end()) {
            error = "view_direction must be one of " + join_names(names) + ", not \"" + *s.view_direction + "\"";
            return std::nullopt;
        }
    }
    if (r.has("view_axes")) {
        if (s.view_direction) {
            error = "view_direction and view_axes both say which way to project: send one";
            return std::nullopt;
        }
        const json& v = params.at("view_axes");
        if (!v.is_object()) {
            error = "view_axes must be an object {right, up}";
            return std::nullopt;
        }
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it.key() != "right" && it.key() != "up") {
                error = "view_axes has no key \"" + it.key() + "\". Its keys: right, up";
                return std::nullopt;
            }
        Vec3d right, up;
        if (!read_axes(v, "view_axes", right, up, error))
            return std::nullopt;
        s.view_axes = std::make_pair(right, up);
    }
    if (r.has("projection_frame")) {
        const json& v = params.at("projection_frame");
        if (v.is_boolean()) {
            if (v.get<bool>()) {
                error = "projection_frame: true is not a frame; send {view or right/up, center, width_mm, height_mm}, or false to clear it";
                return std::nullopt;
            }
            s.clear_projection_frame = true;
        } else {
            ProjectionFrameRequest frame;
            if (!read_frame(v, frame, error))
                return std::nullopt;
            s.projection_frame = frame;
        }
    }
    if (r.has("adjust_image")) {
        ImageAdjustRequest adjust;
        if (!read_adjust(params.at("adjust_image"), adjust, error))
            return std::nullopt;
        s.adjust_image = adjust;
    }
    if (!r.bounded_int("color_mix_count", s.color_mix_count) || !r.bounded_int("color_denoise", s.color_denoise))
        return std::nullopt;
    return req;
}

std::optional<int> first_free_slot(const std::vector<TextureDisplacementLayer>& stack)
{
    std::array<bool, TEXTURE_DISPLACEMENT_MAX_LAYERS> used{};
    for (const TextureDisplacementLayer& l : stack)
        if (l.slot >= 0 && size_t(l.slot) < used.size())
            used[size_t(l.slot)] = true;
    for (size_t i = 0; i < used.size(); ++i)
        if (!used[i])
            return int(i);
    return std::nullopt;
}

TextureDisplacementLayer new_layer(int slot, bool owner_has_uv)
{
    TextureDisplacementLayer layer;
    layer.slot = slot;
    if (owner_has_uv)
        layer.projection_method = TextureProjectionMethod::ModelUV;
    return layer;
}

std::optional<std::string> layer_edit_refusal(int object_id, const std::vector<TextureDisplacementLayer>& stack, const LayerRequest& req,
                                              const TextureDisplacementLayer& after, const StackFacts& facts)
{
    const std::string object = "object " + std::to_string(object_id);
    if (req.action == LayerAction::add) {
        if (!first_free_slot(stack))
            return object + " already holds " + std::to_string(TEXTURE_DISPLACEMENT_MAX_LAYERS) +
                   " texture layers, the most there are: remove one with action remove first";
    } else {
        const bool found = std::any_of(stack.begin(), stack.end(), [&](const TextureDisplacementLayer& l) { return l.slot == *req.layer; });
        if (!found) {
            std::string slots;
            for (const TextureDisplacementLayer& l : stack)
                slots += (slots.empty() ? "" : ", ") + std::to_string(l.slot);
            return object + " has no texture layer in slot " + std::to_string(*req.layer) +
                   (slots.empty() ? ": it has no layers, add one with action add" : ": its layers are in slots " + slots);
        }
    }
    if (req.action == LayerAction::remove)
        return std::nullopt;
    if (req.action == LayerAction::move) {
        if (*req.to_position >= int(stack.size()))
            return "to_position " + std::to_string(*req.to_position) + " is past the last position, " + std::to_string(stack.size() - 1) +
                   " (positions count the layers in slot order from 0)";
        return std::nullopt;
    }
    const LayerSettings& s = req.settings;
    if (s.color.value_or(false) && !facts.texture_has_color)
        return "The layer's texture has no colours, so it cannot colour the print: pick a colour texture (get_texture_library lists them) or leave color out";
    if (s.projection && *s.projection == TextureProjectionMethod::ModelUV && !facts.owner_has_uv)
        return object + "'s first part has no UV channel, so projection model_uv has nothing to map by (only a textured OBJ brings one)";
    if (s.color_mix_count) {
        const int most = std::max(1, 32 - facts.physical_filaments);
        if (*s.color_mix_count > most)
            return "color_mix_count " + std::to_string(*s.color_mix_count) + " is more than the " + std::to_string(most) +
                   " mixed colors the project has room for (32 filament slots less its " + std::to_string(facts.physical_filaments) +
                   " physical ones)";
    }
    if ((s.view_direction || s.view_axes) && after.projection_method != TextureProjectionMethod::ViewProjected)
        return "view_direction and view_axes aim a view projection: set projection view as well, or leave them out";
    if (s.adjust_image && after.empty())
        return "The layer has no image to adjust: give it a texture first";
    return std::nullopt;
}

namespace {

template<class T> bool assign(T& field, const std::optional<T>& value)
{
    if (!value || field == *value)
        return false;
    field = *value;
    return true;
}

} // namespace

bool apply_layer_settings(TextureDisplacementLayer& l, const LayerSettings& s)
{
    bool changed = false;
    if (s.clear_projection_frame && (l.view_project_projective || !l.tile_enabled)) {
        l.view_project_projective = false;
        l.tile_enabled            = true;
        changed                   = true;
    }
    changed |= assign(l.depth_mm, s.depth_mm);
    changed |= assign(l.tiling_scale, s.tile_size_mm);
    changed |= assign(l.rotation_deg, s.rotation_deg);
    changed |= assign(l.midlevel, s.midlevel);
    changed |= assign(l.smoothing, s.smoothing);
    changed |= assign(l.edge_smoothing_amount, s.edge_fade_amount);
    changed |= assign(l.offset, s.offset);
    changed |= assign(l.invert, s.invert);
    changed |= assign(l.edge_smoothing, s.edge_fade);
    changed |= assign(l.tile_enabled, s.tile);
    changed |= assign(l.color_enabled, s.color);
    changed |= assign(l.tile_method, s.tile_method);
    changed |= assign(l.projection_method, s.projection);
    changed |= assign(l.blend_mode, s.blend);
    return changed;
}

bool apply_stack_settings(TextureDisplacementOptions& o, const LayerSettings& s)
{
    bool changed = false;
    changed |= assign(o.color_mix_enabled, s.color_mix);
    changed |= assign(o.color_mix_count, s.color_mix_count);
    changed |= assign(o.color_despeckle, s.color_denoise);
    return changed;
}

TextureAdjustments adjustments_of(const ImageAdjustRequest& r)
{
    TextureAdjustments a;
    a.brightness = r.brightness.value_or(0.f);
    a.contrast   = r.contrast.value_or(0.f);
    a.sharpness  = r.sharpness.value_or(0.f);
    a.blur       = r.blur.value_or(0.f);
    a.hue_deg    = r.hue_deg.value_or(0.f);
    return a;
}

std::pair<Vec3f, Vec3f> view_projector_axes(const Vec3d& right_world, const Vec3d& up_world, const Transform3d& volume_to_world)
{
    const Matrix3d world_to_local = volume_to_world.matrix().block<3, 3>(0, 0).inverse();
    const Vec3d    right          = world_to_local * right_world;
    const Vec3d    up             = world_to_local * up_world;
    // Unit axes: the projected planar coordinate stays in mm, so the tile size keeps meaning mm.
    return {right.norm() > 1e-9 ? Vec3f(right.normalized().cast<float>()) : Vec3f::UnitX(),
            up.norm() > 1e-9 ? Vec3f(up.normalized().cast<float>()) : Vec3f::UnitY()};
}

std::array<float, 12> orthographic_frame_matrix(const Vec3d& right_world, const Vec3d& up_world, const Vec3d& center_world, double width_mm,
                                                double height_mm, const Transform3d& volume_to_world)
{
    const Vec3d right = right_world.normalized();
    const Vec3d up    = (up_world - right * right.dot(up_world)).normalized();
    const Vec3d down  = -up;
    const Vec3d corner = center_world - right * (width_mm * 0.5) + up * (height_mm * 0.5);

    const Matrix3d A = volume_to_world.matrix().block<3, 3>(0, 0);
    const Vec3d    t = volume_to_world.matrix().block<3, 1>(0, 3);
    const Vec3d    row_u = A.transpose() * right / width_mm, row_v = A.transpose() * down / height_mm;
    const double   off_u = right.dot(t - corner) / width_mm, off_v = down.dot(t - corner) / height_mm;

    std::array<float, 12> m{};
    for (int i = 0; i < 3; ++i) {
        m[size_t(i)]     = float(row_u[i]);
        m[size_t(4 + i)] = float(row_v[i]);
    }
    m[3]  = float(off_u);
    m[7]  = float(off_v);
    m[11] = 1.f;
    return m;
}

json inactive_settings(const std::vector<TextureDisplacementLayer>& stack, const TextureDisplacementLayer& layer,
                       const TextureDisplacementOptions& options, bool owner_has_uv, bool texture_has_color)
{
    (void) options;
    json out = json::object();
    const bool framed = layer.projection_method == TextureProjectionMethod::ViewProjected && layer.view_project_projective;
    if (layer.projection_method == TextureProjectionMethod::ModelUV) {
        if (!owner_has_uv)
            out["projection"] = "the first part has no UV channel, so the layer maps like triplanar";
        out["tile_size_mm"] = "the model's own UV layout is the placement, so there is no tile size";
    }
    if (framed)
        for (const char* key : {"tile_size_mm", "rotation_deg", "offset", "tile_method"})
            out[key] = "a projection frame is the placement";
    const bool base = std::none_of(stack.begin(), stack.end(), [&](const TextureDisplacementLayer& l) { return l.slot < layer.slot; });
    if (base)
        out["blend"] = "the base layer has nothing beneath it to blend with";
    if (!layer.edge_smoothing)
        out["edge_fade_amount"] = "edge_fade is off";
    if (!layer.tile_enabled && !framed)
        out["tile_method"] = "tile is off";
    if (layer.color_enabled && !texture_has_color)
        out["color"] = "the texture has no colours";
    if (!std::any_of(stack.begin(), stack.end(), [](const TextureDisplacementLayer& l) { return l.color_enabled; }))
        for (const char* key : {"color_mix", "color_mix_count", "color_denoise"})
            out[key] = "no layer colours the print";
    return out;
}

std::vector<std::string> layer_warnings(const TextureDisplacementLayer& layer)
{
    std::vector<std::string> out;
    if (layer.midlevel > 0.f && layer.depth_mm > 1.f)
        out.push_back("Deep inward displacement may self-intersect.");
    return out;
}

// ---- Masks ----

bool mask_is_stale(const PaintData& data, std::size_t facet_count)
{
    for (const TriangleSelector::TriangleBitStreamMapping& m : data.triangles_to_split)
        if (m.triangle_idx < 0 || size_t(m.triangle_idx) >= facet_count)
            return true;
    return false;
}

std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>> mask_reports(const std::vector<MaskInput>& inputs)
{
    std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>> out;
    out.reserve(inputs.size());
    for (const MaskInput& in : inputs) {
        std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS> row;
        for (size_t slot = 0; slot < row.size(); ++slot) {
            MaskReport& rep = row[slot];
            rep.volume_id   = in.volume_id;
            rep.name        = in.name;
            const PaintData& data = in.masks[slot];
            if (data.triangles_to_split.empty() || !in.mesh)
                continue;
            if (mask_is_stale(data, in.mesh->its.indices.size())) {
                rep.stale = true;
                continue;
            }
            for (const PaintedStateInfo& info : summarize_paint_data(*in.mesh, data))
                if (info.state == 1) {
                    rep.painted_facets = info.facet_count;
                    rep.area_ratio     = info.area_ratio;
                }
        }
        out.push_back(std::move(row));
    }
    return out;
}

bool needs_bake(const std::vector<TextureDisplacementLayer>& stack, const std::vector<std::array<MaskReport, TEXTURE_DISPLACEMENT_MAX_LAYERS>>& reports)
{
    for (const TextureDisplacementLayer& layer : stack) {
        if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
            continue;
        for (const auto& row : reports) {
            const MaskReport& rep = row[size_t(layer.slot)];
            if (!rep.stale && rep.painted_facets > 0)
                return true;
        }
    }
    return false;
}

bool needs_bake_fast(const ModelObject& object, const ModelVolume* owner)
{
    if (owner == nullptr)
        return false;
    for (const TextureDisplacementLayer& layer : owner->texture_displacement_layers) {
        if (layer.slot < 0 || size_t(layer.slot) >= TEXTURE_DISPLACEMENT_MAX_LAYERS)
            continue;
        for (const ModelVolume* v : object.volumes)
            if (v->is_model_part() && !v->texture_displacement_facet(layer.slot).empty())
                return true;
    }
    return false;
}

// ---- What a mesh edit dropped ----

TextureState texture_state_of(const ModelObject& object)
{
    TextureState state;
    for (size_t i = 0; i < object.volumes.size(); ++i) {
        const ModelVolume* v = object.volumes[i];
        if (!v->is_model_part())
            continue;
        TextureState::Part part;
        part.id        = v->id();
        part.volume_id = int(i);
        part.name      = v->name;
        part.layers    = int(v->texture_displacement_layers.size());
        for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
            if (!v->texture_displacement_facet(slot).empty())
                part.painted_slots.push_back(slot);
        state.parts.push_back(std::move(part));
    }
    return state;
}

json texture_changes_json(const TextureState& before, const std::vector<const ModelObject*>& after, const ModelVolume* owner_after)
{
    std::vector<TextureState::Part> after_parts;
    std::vector<bool>               unused_after; // parallel: holds a stack its object ignores
    for (size_t o = 0; o < after.size(); ++o) {
        const TextureState state = texture_state_of(*after[o]);
        const ModelVolume* owner = (o == 0 && owner_after != nullptr) ? owner_after : texture_stack_owner(*after[o]);
        for (const TextureState::Part& p : state.parts) {
            after_parts.push_back(p);
            unused_after.push_back(p.layers > 0 && (owner == nullptr || owner->id() != p.id));
        }
    }
    const auto find_after = [&](const TextureState::Part& b) {
        return std::find_if(after_parts.begin(), after_parts.end(), [&](const TextureState::Part& a) { return a.id == b.id; });
    };

    json lost = json::array();
    int  layers_lost = 0;
    for (const TextureState::Part& b : before.parts) {
        const auto       it = find_after(b);
        std::vector<int> slots;
        for (int slot : b.painted_slots)
            if (it == after_parts.end() || std::find(it->painted_slots.begin(), it->painted_slots.end(), slot) == it->painted_slots.end())
                slots.push_back(slot);
        if (!slots.empty())
            lost.push_back({{"volume_id", b.volume_id}, {"name", b.name}, {"layers", slots}});
        layers_lost += it == after_parts.end() ? b.layers : std::max(0, b.layers - it->layers);
    }

    // A stack the edit left on a part the object does not read it from (it was not so before).
    json unused = json::array();
    for (size_t i = 0; i < after_parts.size(); ++i) {
        if (!unused_after[i])
            continue;
        const auto was = std::find_if(before.parts.begin(), before.parts.end(), [&](const TextureState::Part& b) { return b.id == after_parts[i].id; });
        const bool unused_before = was != before.parts.end() && was->layers > 0 && was->id != before.parts.front().id;
        if (!unused_before)
            unused.push_back({{"volume_id", after_parts[i].volume_id}, {"layers", after_parts[i].layers}});
    }

    json out = json::object();
    if (!lost.empty())
        out["paint_lost"] = lost;
    if (layers_lost > 0)
        out["layers_lost"] = layers_lost;
    if (!unused.empty())
        out["stacks_unused"] = unused;
    return out;
}

// ---- The unwrap request ----

namespace {

bool read_index(const json& v, const std::string& what, int& out, std::string& error)
{
    if (!v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 100000000) {
        error = what + " must be a whole number from 0";
        return false;
    }
    out = int(v.get<long long>());
    return true;
}

bool read_index_list(const json& params, const char* key, std::vector<int>& out, std::string& error)
{
    if (!params.contains(key))
        return true;
    const json& v = params.at(key);
    if (!v.is_array()) {
        error = std::string(key) + " must be a list of whole numbers";
        return false;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        int n = 0;
        if (!read_index(v[i], std::string(key) + "[" + std::to_string(i) + "]", n, error))
            return false;
        out.push_back(n);
    }
    return true;
}

bool read_pairs(const json& params, const char* key, std::vector<std::pair<int, int>>& out, std::string& error)
{
    if (!params.contains(key))
        return true;
    const json& v = params.at(key);
    if (!v.is_array()) {
        error = std::string(key) + " must be a list of [vertex, vertex] pairs";
        return false;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string what = std::string(key) + "[" + std::to_string(i) + "]";
        int a = 0, b = 0;
        if (!v[i].is_array() || v[i].size() != 2 || !read_index(v[i][0], what, a, error) || !read_index(v[i][1], what, b, error)) {
            if (error.empty())
                error = what + " must be a [vertex, vertex] pair";
            return false;
        }
        if (a == b) {
            error = what + " names one vertex twice";
            return false;
        }
        out.emplace_back(std::min(a, b), std::max(a, b));
    }
    return true;
}

bool known_keys(const json& obj, const std::string& what, const std::set<std::string>& keys, std::string& error)
{
    if (!obj.is_object()) {
        error = what + " must be an object";
        return false;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (!keys.count(it.key())) {
            std::string list;
            for (const std::string& k : keys)
                list += (list.empty() ? "" : ", ") + k;
            error = what + " has no key \"" + it.key() + "\". Its keys: " + list;
            return false;
        }
    return true;
}

} // namespace

std::optional<UnwrapRequest> read_unwrap_request(const json& params, std::string& error)
{
    error.clear();
    if (!params.is_object()) {
        error = "arguments must be an object";
        return std::nullopt;
    }
    UnwrapRequest req;
    Reader        r{params, error};

    if (params.contains("seam_angle_deg")) {
        double v = 0;
        if (!r.number("seam_angle_deg", v))
            return std::nullopt;
        if (v < 5. || v > 90.) {
            error = "seam_angle_deg must be from 5 to 90 degrees (the panel's Seam angle), not " + number_text(v);
            return std::nullopt;
        }
        req.seam_angle_deg = float(v);
    }
    if (params.contains("island_padding_mm")) {
        double v = 0;
        if (!r.number("island_padding_mm", v))
            return std::nullopt;
        if (v != -1. && v < 0.) {
            error = "island_padding_mm must be -1 (automatic) or 0 and more mm, not " + number_text(v);
            return std::nullopt;
        }
        req.island_padding_mm = float(v);
    }
    if (!r.boolean("auto_connect_islands", req.auto_connect_islands))
        return std::nullopt;
    for (const char* key : {"clear_seams", "clear_uv_edits", "average_scale", "relayout"}) {
        std::optional<bool> v;
        if (!r.boolean(key, v))
            return std::nullopt;
        if (v.value_or(false)) {
            const std::string k = key;
            (k == "clear_seams" ? req.clear_seams : k == "clear_uv_edits" ? req.clear_uv_edits : k == "average_scale" ? req.average_scale : req.relayout) = true;
        }
    }
    if (!read_pairs(params, "add_seams", req.add_seams, error) || !read_pairs(params, "remove_seams", req.remove_seams, error))
        return std::nullopt;
    if (params.contains("seam_paths")) {
        const json& v = params.at("seam_paths");
        if (!v.is_array()) {
            error = "seam_paths must be a list of {from: [x, y, z], to: [x, y, z]}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "seam_paths[" + std::to_string(i) + "]";
            Vec3d from, to;
            if (!known_keys(v[i], what, {"from", "to"}, error) || !v[i].contains("from") || !v[i].contains("to")) {
                if (error.empty())
                    error = what + " needs from and to";
                return std::nullopt;
            }
            if (!read_vec3(v[i].at("from"), what + " from", from, error) || !read_vec3(v[i].at("to"), what + " to", to, error))
                return std::nullopt;
            req.seam_paths.emplace_back(from, to);
        }
    }
    if (!read_index_list(params, "cut_islands", req.cut_islands, error) || !read_index_list(params, "unjoin", req.unjoin, error))
        return std::nullopt;
    if (params.contains("join")) {
        const json& v = params.at("join");
        if (!v.is_array()) {
            error = "join must be a list of {chart, to?}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "join[" + std::to_string(i) + "]";
            int chart = 0;
            if (!known_keys(v[i], what, {"chart", "to"}, error) || !v[i].contains("chart") || !read_index(v[i].at("chart"), what + " chart", chart, error)) {
                if (error.empty())
                    error = what + " needs chart";
                return std::nullopt;
            }
            std::optional<int> to;
            if (v[i].contains("to")) {
                int n = 0;
                if (!read_index(v[i].at("to"), what + " to", n, error))
                    return std::nullopt;
                to = n;
            }
            req.join.emplace_back(chart, to);
        }
    }
    if (params.contains("islands")) {
        const json& v = params.at("islands");
        if (!v.is_array()) {
            error = "islands must be a list of {chart, offset?, rotation_deg?, scale?}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "islands[" + std::to_string(i) + "]";
            IslandEdit edit;
            if (!known_keys(v[i], what, {"chart", "offset", "rotation_deg", "scale"}, error) || !v[i].contains("chart") ||
                !read_index(v[i].at("chart"), what + " chart", edit.chart, error)) {
                if (error.empty())
                    error = what + " needs chart";
                return std::nullopt;
            }
            Reader ir{v[i], error};
            if (v[i].contains("offset")) {
                std::vector<double> c;
                if (!read_vec(v[i].at("offset"), what + " offset", 2, c, error))
                    return std::nullopt;
                edit.offset = Vec2f(float(c[0]), float(c[1]));
            }
            for (const char* key : {"rotation_deg", "scale"}) {
                if (!v[i].contains(key))
                    continue;
                double d = 0;
                if (!ir.number(key, d, (what + " ").c_str()))
                    return std::nullopt;
                if (std::string(key) == "scale") {
                    if (d < 0.001 || d > 1000.) {
                        error = what + " scale must be from 0.001 to 1000, not " + number_text(d);
                        return std::nullopt;
                    }
                    edit.scale = float(d);
                } else
                    edit.rotation_deg = float(d);
            }
            req.islands.push_back(edit);
        }
    }
    if (params.contains("uv_edits")) {
        const json& v = params.at("uv_edits");
        if (!v.is_array()) {
            error = "uv_edits must be a list of {vertex, uv: [u, v]}";
            return std::nullopt;
        }
        for (size_t i = 0; i < v.size(); ++i) {
            const std::string what = "uv_edits[" + std::to_string(i) + "]";
            int vertex = 0;
            if (!known_keys(v[i], what, {"vertex", "uv"}, error) || !v[i].contains("vertex") || !v[i].contains("uv") ||
                !read_index(v[i].at("vertex"), what + " vertex", vertex, error)) {
                if (error.empty())
                    error = what + " needs vertex and uv";
                return std::nullopt;
            }
            std::vector<double> c;
            if (!read_vec(v[i].at("uv"), what + " uv", 2, c, error))
                return std::nullopt;
            req.uv_edits.emplace_back(vertex, Vec2f(float(c[0]), float(c[1])));
        }
    }
    return req;
}

json unwrap_json(const PatchUnwrap& unwrap, const TextureDisplacementLayer& layer, bool include_vertices)
{
    json seams = json::array();
    for (const auto& s : layer.lscm_seam_edges)
        seams.push_back({s.first, s.second});
    json islands = json::array();
    for (int c = 0; c < unwrap.chart_count; ++c) {
        const TextureIsland isl = size_t(c) < layer.islands.size() ? layer.islands[size_t(c)] : TextureIsland{};
        const Vec2f centroid = size_t(c) < unwrap.chart_centroid.size() ? unwrap.chart_centroid[size_t(c)] : Vec2f::Zero();
        islands.push_back({{"chart", c},
                           {"centroid", {centroid.x(), centroid.y()}},
                           {"offset", {isl.offset.x(), isl.offset.y()}},
                           {"rotation_deg", isl.rotation_deg},
                           {"scale", isl.scale},
                           {"group", size_t(c) < layer.island_groups.size() ? layer.island_groups[size_t(c)] : -1}});
    }
    json out = {{"chart_count", unwrap.chart_count},
                {"vertex_count", unwrap.uvs.size()},
                {"triangle_count", unwrap.indices.size()},
                {"seam_angle_deg", layer.lscm_seam_angle_deg},
                {"island_padding_mm", layer.island_padding_mm},
                {"auto_connect_islands", layer.auto_connect_islands},
                {"seams", seams},
                {"islands", islands},
                {"uv_edits", layer.lscm_uv_overrides.size()}};
    if (include_vertices) {
        json vertices = json::array();
        for (size_t i = 0; i < unwrap.uvs.size(); ++i)
            vertices.push_back({{"vertex", i},
                                {"source_vertex", i < unwrap.source_vertex.size() ? unwrap.source_vertex[i] : -1},
                                {"chart", i < unwrap.vertex_chart.size() ? unwrap.vertex_chart[i] : -1},
                                {"uv", {unwrap.uvs[i].x(), unwrap.uvs[i].y()}}});
        out["vertices"] = vertices;
    }
    return out;
}

// ---- Answers ----

json layer_json(const TextureDisplacementLayer& layer, int position, bool base, const LayerTexture& texture, std::optional<bool> color_capable,
                const std::vector<MaskReport>& paint, bool owner_has_uv, const std::vector<TextureDisplacementLayer>& stack,
                const TextureDisplacementOptions& options)
{
    json out = {{"layer", layer.slot},
                {"position", position},
                {"base", base},
                {"texture",
                 {{"texture", texture.id ? json(*texture.id) : json(nullptr)},
                  {"name", layer.name},
                  {"source", texture.source},
                  {"path", layer.path}}},
                {"color_capable", color_capable ? json(*color_capable) : json(nullptr)},
                {"depth_mm", layer.depth_mm},
                {"tile_size_mm", layer.tiling_scale},
                {"rotation_deg", layer.rotation_deg},
                {"offset", {layer.offset.x(), layer.offset.y()}},
                {"invert", layer.invert},
                {"midlevel", layer.midlevel},
                {"smoothing", layer.smoothing},
                {"edge_fade", layer.edge_smoothing},
                {"edge_fade_amount", layer.edge_smoothing_amount},
                {"tile", layer.tile_enabled},
                {"tile_method", tile_method_name(layer.tile_method)},
                {"projection", projection_name(layer.projection_method)},
                {"blend", blend_name(layer.blend_mode)},
                {"color", layer.color_enabled}};
    if (layer.projection_method == TextureProjectionMethod::ViewProjected) {
        out["view"] = {{"right", {layer.view_project_right.x(), layer.view_project_right.y(), layer.view_project_right.z()}},
                       {"up", {layer.view_project_up.x(), layer.view_project_up.y(), layer.view_project_up.z()}}};
        if (layer.view_project_projective)
            out["projection_frame"] = {{"active", true}};
    }
    if (layer.projection_method == TextureProjectionMethod::LSCM)
        out["unwrap"] = {{"seam_angle_deg", layer.lscm_seam_angle_deg},
                         {"island_padding_mm", layer.island_padding_mm},
                         {"auto_connect_islands", layer.auto_connect_islands},
                         {"seams", layer.lscm_seam_edges.size()},
                         {"placed_islands", layer.islands.size()},
                         {"uv_edits", layer.lscm_uv_overrides.size()}};
    json masks   = json::array();
    bool painted = false;
    for (const MaskReport& m : paint) {
        const bool any = m.painted_facets > 0;
        painted |= any;
        masks.push_back({{"volume_id", m.volume_id},
                         {"name", m.name},
                         {"painted", any},
                         {"stale", m.stale},
                         {"painted_facets", m.painted_facets},
                         {"coverage_percent", std::round(m.area_ratio * 10000.) / 100.}});
    }
    out["paint"]             = masks;
    out["painted"]           = painted;
    out["inactive_settings"] = inactive_settings(stack, layer, options, owner_has_uv, color_capable.value_or(false));
    out["warnings"]          = layer_warnings(layer);
    return out;
}

json stack_options_json(const TextureDisplacementOptions& o)
{
    return {{"pipeline", o.pipeline_v2 ? "default" : "classic"},
            {"displace_border", o.displace_border},
            {"smooth",
             {{"enabled", o.smooth_enabled},
              {"strength", std::round(o.smooth_strength * 1000.f) / 10.f},
              {"passes", o.smooth_iterations},
              {"ignore_outer_ring", o.smooth_skip_border}}},
            {"resolution_mm", o.v2_refine_mm > 0.f ? json(o.v2_refine_mm) : json(nullptr)},
            {"budget_k", o.v2_max_triangles_k >= 0 ? json(o.v2_max_triangles_k) : json(nullptr)},
            {"color_mix", o.color_mix_enabled},
            {"color_mix_count", o.color_mix_count},
            {"color_denoise", o.color_despeckle}};
}

}}} // namespace Slic3r::GUI::OrcaMCP
