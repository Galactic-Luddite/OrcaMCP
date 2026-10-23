#ifndef slic3r_TextureDisplacementObject_hpp_
#define slic3r_TextureDisplacementObject_hpp_

#include <cstddef>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"

// The texture displacement panel's logic over an object, as functions of the model alone: which parts a
// stack acts on, the stack as it applies to each, the frame it is baked in, the layer moves and the
// seam and island arithmetic. The gizmo's members call these with the object it has selected and so
// does OrcaMCP; nothing here touches wx, the app or the selection.
namespace Slic3r::GUI {

// Every model part of `object`, in the object's own volume order.
std::vector<ModelVolume *> texture_parts(ModelObject &object);
// The parts a bake or a preview acts on: those of texture_parts() carrying paint in at least one slot.
std::vector<ModelVolume *> painted_texture_parts(ModelObject &object);
// The model part that owns the object's layer stack, its options and its unwrap: the first model part.
// nullptr when the object has none.
ModelVolume       *texture_stack_owner(ModelObject &object);
const ModelVolume *texture_stack_owner(const ModelObject &object);
// The object's layer stack as it applies to `part`: the owner's, with the fields that index the owner's
// mesh (seams, island placements and groups, uv overrides) dropped for every other part.
std::vector<TextureDisplacementLayer> layers_for_part(const ModelObject &object, const ModelVolume &part);
// The world point the object's texture is anchored at: the owner's origin. Zero without an owner.
Vec3d texture_anchor(const ModelObject &object);
// For each of `parts` (indices into `all_parts`), how far the lowest of `all_parts` lies below that
// part's own bottom, in mm: what its bake lowers its resting plane by. 0 for the lowest part. Takes
// meshes and world transforms rather than volumes so a prepared mesh the model does not hold yet can be
// measured.
std::vector<float> resting_drops(const std::vector<std::pair<const indexed_triangle_set *, Transform3d>> &all_parts,
                                 const std::vector<std::size_t>                                          &parts);
// The same for `parts` of `object` as they are in the model now.
std::vector<float> resting_drops(const ModelObject &object, const std::vector<ModelVolume *> &parts);
// Where the prepare samples `part`'s texture: the bake frame when the object has several model parts,
// the identity for one.
Transform3d prepare_texture_frame(const ModelObject &object, const ModelVolume &part);

// Removes the layer in `slot` and resets that slot's mask on every model part. False when there was
// neither a layer nor a mask in it (nothing changed then).
bool remove_texture_layer(ModelObject &object, int slot);
// Exchanges two slots: the layer definitions and the masks of every model part. No undo step.
// False when nothing was exchanged (equal or out-of-range slots, no model part).
bool swap_texture_layer_slots(ModelObject &object, int slot_a, int slot_b);
// The neighbour swaps that put the layer in `slot` at insertion index `to_index` of the slot-ordered
// stack (the layer count counts as an index), in the order they run; empty when nothing moves.
std::vector<std::pair<int, int>> texture_layer_move_swaps(const ModelObject &object, int slot, int to_index);
// Moves the layer in `slot` there, its mask with it, by running those swaps; `on_swap` is told of each
// after it was made. False when nothing moves.
bool move_texture_layer(ModelObject &object, int slot, int to_index,
                        const std::function<void(int, int)> &on_swap = {});

// Shortest path over the mesh's edge graph (Dijkstra, edge length) as sorted vertex pairs, from `v_to`
// back to `v_from`. Empty when either vertex is out of range, they are the same or unreachable.
std::vector<std::pair<int, int>> seam_path_edges(const indexed_triangle_set &its, int v_from, int v_to);
// `seams` with the edges of `chart` that cross the plane through its middle, across its longest axis,
// added (sorted, no duplicates). `seams` unchanged when the chart has no triangle.
std::vector<std::pair<int, int>> cut_island_seams(const indexed_triangle_set &its, const PatchUnwrap &unwrap, int chart,
                                                  const std::vector<std::pair<int, int>> &seams);
// What Join does for `chart`: the neighbour (one it shares an edge with) placed nearest, and the
// placement that joins `chart` to it. nullopt when it has no neighbour.
std::optional<std::pair<int, TextureIsland>> join_target(const PatchUnwrap &unwrap, const std::vector<TextureIsland> &islands,
                                                         int chart);

// The join-group id of chart `c`: its explicit entry in `groups`, or `c` itself (its own singleton) when unset.
// Two charts move together iff this matches.
int island_group_of(const std::vector<int> &groups, int c);
// Merges chart `b`'s join group into chart `a`'s (materialising `groups` to `chart_count` first).
void join_island_groups(std::vector<int> &groups, int a, int b, int chart_count);

// Why smoothed_baked_mesh() gave nothing, for the panel's two messages.
enum class TextureSmoothSkip { None, NothingPainted, NoInteriorVertices };
// The owner's mesh with the displaced surface relaxed over the vertices of its painted triangles (the
// rim held with "Ignore outer ring"). nullopt for no change; `why`, when given, says which.
std::optional<indexed_triangle_set> smoothed_baked_mesh(const ModelVolume &owner, const TextureDisplacementOptions &options,
                                                        TextureSmoothSkip *why = nullptr);

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementObject_hpp_
