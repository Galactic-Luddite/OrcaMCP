#include "TextureDisplacementObject.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <set>

#include "libslic3r/TriangleSelector.hpp"

namespace Slic3r::GUI {

std::vector<ModelVolume *> texture_parts(ModelObject &object)
{
    std::vector<ModelVolume *> out;
    for (ModelVolume *mv : object.volumes)
        if (mv->is_model_part())
            out.push_back(mv);
    return out;
}

std::vector<ModelVolume *> painted_texture_parts(ModelObject &object)
{
    std::vector<ModelVolume *> out;
    for (ModelVolume *mv : texture_parts(object))
        if (mv->is_texture_displacement_painted())
            out.push_back(mv);
    return out;
}

ModelVolume *texture_stack_owner(ModelObject &object)
{
    for (ModelVolume *mv : object.volumes)
        if (mv->is_model_part())
            return mv;
    return nullptr;
}

const ModelVolume *texture_stack_owner(const ModelObject &object)
{
    return texture_stack_owner(const_cast<ModelObject &>(object));
}

std::vector<TextureDisplacementLayer> layers_for_part(const ModelObject &object, const ModelVolume &part)
{
    const ModelVolume *owner = texture_stack_owner(object);
    if (owner == nullptr)
        return {};
    std::vector<TextureDisplacementLayer> out = owner->texture_displacement_layers;
    if (&part == owner)
        return out;
    for (TextureDisplacementLayer &layer : out) {
        layer.lscm_seam_edges.clear();
        layer.islands.clear();
        layer.island_groups.clear();
        layer.lscm_uv_overrides.clear();
    }
    return out;
}

Vec3d texture_anchor(const ModelObject &object)
{
    const ModelVolume *owner = texture_stack_owner(object);
    return owner != nullptr ? Vec3d(texture_displacement_volume_to_world(*owner).translation()) : Vec3d::Zero();
}

std::vector<float> resting_drops(const std::vector<std::pair<const indexed_triangle_set *, Transform3d>> &all_parts,
                                 const std::vector<std::size_t>                                          &parts)
{
    const auto bottom = [&all_parts](std::size_t i) {
        double z = std::numeric_limits<double>::max();
        for (const stl_vertex &v : all_parts[i].first->vertices)
            z = std::min(z, (all_parts[i].second * v.cast<double>()).z());
        return z;
    };
    // Each part's bottom is measured the same way as the object's, so the lowest part's drop is exactly
    // 0 and a single-part object bakes against its own bottom as it always has.
    double object_bottom = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < all_parts.size(); ++i)
        object_bottom = std::min(object_bottom, bottom(i));
    std::vector<float> out;
    out.reserve(parts.size());
    for (const std::size_t i : parts)
        out.push_back(float(bottom(i) - object_bottom));
    return out;
}

std::vector<float> resting_drops(const ModelObject &object, const std::vector<ModelVolume *> &parts)
{
    const std::vector<ModelVolume *> all = texture_parts(const_cast<ModelObject &>(object));
    std::vector<std::pair<const indexed_triangle_set *, Transform3d>> measured;
    measured.reserve(all.size());
    for (const ModelVolume *part : all)
        measured.emplace_back(&part->mesh().its, texture_displacement_volume_to_world(*part));
    // `parts` are model parts of the object, so each is one of `all`.
    std::vector<std::size_t> indices;
    indices.reserve(parts.size());
    for (const ModelVolume *part : parts)
        indices.push_back(std::size_t(std::find(all.begin(), all.end(), part) - all.begin()));
    return resting_drops(measured, indices);
}

Transform3d prepare_texture_frame(const ModelObject &object, const ModelVolume &part)
{
    if (texture_parts(const_cast<ModelObject &>(object)).size() < 2)
        return Transform3d::Identity();
    return texture_displacement_bake_frame(texture_displacement_volume_to_world(part), texture_anchor(object));
}

bool remove_texture_layer(ModelObject &object, int slot)
{
    ModelVolume *owner = texture_stack_owner(object);
    if (owner == nullptr)
        return false;
    auto      &layers  = owner->texture_displacement_layers;
    const auto removed = std::remove_if(layers.begin(), layers.end(),
                                        [slot](const TextureDisplacementLayer &l) { return l.slot == slot; });
    bool changed = removed != layers.end();
    layers.erase(removed, layers.end());
    if (slot >= 0 && slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS)) {
        // Every model part: paint left in the slot on another part would still bake, and the next layer
        // added reuses the slot.
        for (ModelVolume *part : texture_parts(object)) {
            changed = changed || !part->texture_displacement_facet(slot).empty();
            part->texture_displacement_facet(slot).reset();
        }
    }
    return changed;
}

bool swap_texture_layer_slots(ModelObject &object, int slot_a, int slot_b)
{
    ModelVolume *owner = texture_stack_owner(object);
    const int    n     = int(TEXTURE_DISPLACEMENT_MAX_LAYERS);
    if (owner == nullptr || slot_a == slot_b || slot_a < 0 || slot_b < 0 || slot_a >= n || slot_b >= n)
        return false;

    // Every model part, not only the owner: the selectors paint the active slot on all of them.
    for (ModelVolume *v : texture_parts(object)) {
        TriangleSelector::TriangleSplittingData data_a = v->texture_displacement_facet(slot_a).get_data();
        TriangleSelector::TriangleSplittingData data_b = v->texture_displacement_facet(slot_b).get_data();
        v->texture_displacement_facet(slot_a).set_data(std::move(data_b));
        v->texture_displacement_facet(slot_b).set_data(std::move(data_a));
    }
    for (TextureDisplacementLayer &l : owner->texture_displacement_layers) {
        if (l.slot == slot_a)
            l.slot = slot_b;
        else if (l.slot == slot_b)
            l.slot = slot_a;
    }
    return true;
}

std::vector<std::pair<int, int>> texture_layer_move_swaps(const ModelObject &object, int slot, int to_index)
{
    const ModelVolume *owner = texture_stack_owner(object);
    if (owner == nullptr)
        return {};

    std::vector<int> slots;
    for (const TextureDisplacementLayer &l : owner->texture_displacement_layers)
        slots.push_back(l.slot);
    std::sort(slots.begin(), slots.end());
    const auto it = std::find(slots.begin(), slots.end(), slot);
    if (it == slots.end())
        return {};
    const int from = int(it - slots.begin());
    // An insertion index counts the moved layer's own position, which is gone once it is lifted out.
    const int to = std::clamp(to_index > from ? to_index - 1 : to_index, 0, int(slots.size()) - 1);
    if (to == from)
        return {};

    // Walked one neighbour at a time, so every layer in between shifts by one and keeps its order.
    std::vector<std::pair<int, int>> swaps;
    const int                        step = to > from ? 1 : -1;
    for (int i = from; i != to; i += step)
        swaps.emplace_back(slots[size_t(i)], slots[size_t(i + step)]);
    return swaps;
}

bool move_texture_layer(ModelObject &object, int slot, int to_index, const std::function<void(int, int)> &on_swap)
{
    const std::vector<std::pair<int, int>> swaps = texture_layer_move_swaps(object, slot, to_index);
    for (const auto &[a, b] : swaps) {
        swap_texture_layer_slots(object, a, b);
        if (on_swap)
            on_swap(a, b);
    }
    return !swaps.empty();
}

std::vector<std::pair<int, int>> seam_path_edges(const indexed_triangle_set &its, int v_from, int v_to)
{
    const size_t n = its.vertices.size();
    if (v_from < 0 || v_to < 0 || v_from == v_to || size_t(v_from) >= n || size_t(v_to) >= n)
        return {};

    // Shortest path over the mesh's edge graph (Dijkstra, edge weight = length). Built on demand; one
    // pass per click is fine even on a dense mesh.
    std::vector<std::vector<std::pair<int, float>>> adj(n);
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = tri[i], b = tri[(i + 1) % 3];
            const float w = (its.vertices[size_t(a)] - its.vertices[size_t(b)]).norm();
            adj[size_t(a)].push_back({ b, w });
            adj[size_t(b)].push_back({ a, w });
        }

    std::vector<float> dist(n, std::numeric_limits<float>::infinity());
    std::vector<int>   prev(n, -1);
    using QN = std::pair<float, int>;
    std::priority_queue<QN, std::vector<QN>, std::greater<QN>> pq;
    dist[size_t(v_from)] = 0.f;
    pq.push({ 0.f, v_from });
    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[size_t(u)])
            continue;
        if (u == v_to)
            break;
        for (const auto &[w, ew] : adj[size_t(u)]) {
            const float nd = d + ew;
            if (nd < dist[size_t(w)]) {
                dist[size_t(w)] = nd;
                prev[size_t(w)] = u;
                pq.push({ nd, w });
            }
        }
    }
    if (prev[size_t(v_to)] < 0)
        return {}; // unreachable (disconnected components)

    std::vector<std::pair<int, int>> edges;
    for (int v = v_to; v != v_from && v >= 0; v = prev[size_t(v)]) {
        const int p = prev[size_t(v)];
        if (p < 0)
            break;
        edges.emplace_back(std::min(v, p), std::max(v, p));
    }
    return edges;
}

std::vector<std::pair<int, int>> cut_island_seams(const indexed_triangle_set &its, const PatchUnwrap &u, int chart,
                                                  const std::vector<std::pair<int, int>> &seams)
{
    const std::vector<Vec3f> &verts = its.vertices;

    // Collect the chart's triangles back in mesh-vertex space, and its 3D bounding box.
    std::vector<std::array<int, 3>> tris;
    Vec3f lo(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    Vec3f hi(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());
    for (const stl_triangle_vertex_indices &t : u.indices) {
        if (t[0] < 0 || size_t(t[0]) >= u.vertex_chart.size() || u.vertex_chart[size_t(t[0])] != chart)
            continue;
        std::array<int, 3> bt{};
        bool               ok = true;
        for (int k = 0; k < 3; ++k) {
            const int uvv = t[k];
            if (uvv < 0 || size_t(uvv) >= u.source_vertex.size()) { ok = false; break; }
            const int base = u.source_vertex[size_t(uvv)];
            if (base < 0 || size_t(base) >= verts.size()) { ok = false; break; }
            bt[k] = base;
            lo    = lo.cwiseMin(verts[size_t(base)]);
            hi    = hi.cwiseMax(verts[size_t(base)]);
        }
        if (ok)
            tris.push_back(bt);
    }
    if (tris.empty())
        return seams;

    // Cut perpendicular to the longest axis, through the centroid - a long thin island is split
    // across its narrow middle, which is exactly the "islands might be very long" case.
    const Vec3f  ext  = hi - lo;
    const int    axis = (ext.x() >= ext.y() && ext.x() >= ext.z()) ? 0 : (ext.y() >= ext.z() ? 1 : 2);
    const float  mid  = 0.5f * (lo[axis] + hi[axis]);

    std::set<std::pair<int, int>> out(seams.begin(), seams.end());
    for (const std::array<int, 3> &bt : tris)
        for (int i = 0; i < 3; ++i) {
            const int a = bt[i], b = bt[(i + 1) % 3];
            // Endpoints on opposite sides of the plane -> this edge crosses it -> make it a seam.
            if ((verts[size_t(a)][axis] < mid) != (verts[size_t(b)][axis] < mid))
                out.insert({ std::min(a, b), std::max(a, b) });
        }
    return { out.begin(), out.end() };
}

std::optional<std::pair<int, TextureIsland>> join_target(const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands,
                                                         int chart)
{
    // Join to whichever neighbouring island (one it shares an edge with) is currently placed
    // nearest - i.e. the one it was dragged up against.
    const Eigen::Matrix<float, 2, 3> sel_m = island_transform_matrix(chart, unwrap, islands);
    const Vec2f sel_c = sel_m.block<2, 2>(0, 0) * unwrap.chart_centroid[size_t(chart)] + sel_m.col(2);
    int   best_parent = -1;
    float best_d2     = std::numeric_limits<float>::max();
    TextureIsland best_place, cand;
    for (int p = 0; p < unwrap.chart_count; ++p) {
        if (p == chart)
            continue;
        if (!join_chart_placement(unwrap, islands, chart, p, cand))
            continue; // not a neighbour
        const Eigen::Matrix<float, 2, 3> pm = island_transform_matrix(p, unwrap, islands);
        const Vec2f pc = pm.block<2, 2>(0, 0) * unwrap.chart_centroid[size_t(p)] + pm.col(2);
        const float d2 = (pc - sel_c).squaredNorm();
        if (d2 < best_d2) { best_d2 = d2; best_parent = p; best_place = cand; }
    }
    if (best_parent < 0)
        return std::nullopt;
    return std::make_pair(best_parent, best_place);
}

std::optional<indexed_triangle_set> smoothed_baked_mesh(const ModelVolume &owner, const TextureDisplacementOptions &opts,
                                                        TextureSmoothSkip *why)
{
    if (why != nullptr)
        *why = TextureSmoothSkip::None;
    if (opts.smooth_strength <= 0.f || opts.smooth_iterations <= 0)
        return std::nullopt;

    // Movable = the vertices of the painted triangles, so the first ring of purely unpainted vertices
    // outside them stays put and anchors the result. Restricted rather than whole-model on purpose:
    // this runs on geometry that has already been baked, and relaxing the whole thing would quietly
    // round off every unrelated feature on the part. With "Ignore outer ring" the patch's own rim is
    // held as well - see TextureDisplacementOptions::smooth_skip_border.
    const indexed_triangle_set &its = owner.mesh().its;
    std::vector<uint8_t>        painted_face(its.indices.size(), 0);
    bool                        any = false;
    for (int slot = 0; slot < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++slot)
        for (const TriangleSelector::TriangleBitStreamMapping &m : owner.texture_displacement_facet(slot).get_data().triangles_to_split)
            if (size_t(m.triangle_idx) < its.indices.size()) {
                painted_face[size_t(m.triangle_idx)] = 1;
                any                                  = true;
            }
    if (!any) {
        if (why != nullptr)
            *why = TextureSmoothSkip::NothingPainted;
        return std::nullopt;
    }
    std::vector<uint8_t> movable(its.vertices.size(), 0);
    for (size_t i = 0; i < its.indices.size(); ++i)
        if (painted_face[i])
            for (int k = 0; k < 3; ++k)
                movable[size_t(its.indices[i][k])] = 1;
    if (opts.smooth_skip_border)
        for (size_t i = 0; i < its.indices.size(); ++i)
            if (!painted_face[i])
                for (int k = 0; k < 3; ++k)
                    movable[size_t(its.indices[i][k])] = 0; // also used by unpainted geometry -> the rim
    if (std::none_of(movable.begin(), movable.end(), [](uint8_t m) { return m != 0; })) {
        // Every painted vertex is on the patch's rim, so "Ignore outer ring" leaves nothing to move.
        if (why != nullptr)
            *why = TextureSmoothSkip::NoInteriorVertices;
        return std::nullopt;
    }

    indexed_triangle_set smoothed = its;
    smooth_mesh_vertices(smoothed, movable, opts.smooth_strength, opts.smooth_iterations);
    return smoothed;
}

} // namespace Slic3r::GUI
