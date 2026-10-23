#ifndef slic3r_TextureDisplacementPrepareJob_hpp_
#define slic3r_TextureDisplacementPrepareJob_hpp_

#include <cstddef>
#include <exception>
#include <functional>
#include <string>
#include <vector>

#include "Job.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"

namespace Slic3r {
class ModelObject;
class ModelVolume;
}

namespace Slic3r::GUI {

class Plater;

// Getting a mesh ready to receive displacement - the isotropic remesh, carrying the paint onto it, and
// the adaptive refinement - off the UI thread.
//
// All three used to run inline behind a wxBusyCursor, which on any part big enough to matter meant tens
// of seconds with the window not repainting and no way to stop it: CGAL's remesher is single threaded
// and its cost grows with the square of 1/target_edge, and the refinement that follows spends a budget
// of hundreds of thousands of triangles. Indistinguishable from a hang, and reported as one.
//
// The work itself is GLGizmoTextureDisplacement::prepare_mesh(), which is pure - it takes an
// indexed_triangle_set and the layers' masks and returns new ones, touching no Model and no GUI - so all
// this job adds is the worker thread, the progress notification with its Cancel button, and the commit.
enum class TextureDisplacementPrepareOutcome
{
    Committed, // the parts now carry the prepared mesh and the paint carried onto it
    Unchanged, // nothing needed doing - the meshes already met the criteria; the model was not touched
    PaintLost, // a remesh landed but no layer's paint survived it, so nothing was committed
    Failed,    // cancelled, threw, or the volumes went away while the job ran
};

// One model part's share of a prepare. As with the bake, `layers` belongs to the object rather than to
// the part (see GLGizmoTextureDisplacement::layers_for_part()).
struct TextureDisplacementPrepareVolume
{
    ObjectID                              volume_id;
    indexed_triangle_set                  base_mesh;
    TextureDisplacementFacetsData         masks;
    std::vector<TextureDisplacementLayer> layers;
    // The part's own UV channel (ModelVolume::texture_uv), empty when it has none. prepare_mesh()
    // resamples it onto the mesh it produces, and the commit puts it back - like the masks, it is
    // indexed by triangle and so cannot survive the stages on its own.
    std::vector<Vec2f>                    model_uv;
    // Where prepare_mesh() samples the texture: the bake frame shared by every part of a multibody
    // object, so each part is refined for the texture the bake will put on it. Identity, the part's own
    // coordinates, for a single-part object.
    Transform3d                           texture_frame = Transform3d::Identity();
};

struct TextureDisplacementPrepareInput
{
    // Every model part to prepare, in the object's own volume order - the same parts the bake that
    // follows will displace, so a multibody object is refined everywhere its paint reaches rather than
    // only on its first part.
    std::vector<TextureDisplacementPrepareVolume> volumes;
    TextureDisplacementPrepareParams              params;
    // Captured on the main thread. Empty when no layer is colouring, in which case the refinement
    // skips the colour criterion entirely. Only the *quantizer* is used here: refinement follows
    // perceived colour, never the interleaving that realises a mix.
    TextureColorSettings                          color;
    // The undo step the commit opens. Standard mode's Bake names it after the bake, because the
    // displacement job that follows commits into this same step rather than pushing its own.
    std::string                                   snapshot_name;
};

// Which parts a prepare acts on. A bake prepares exactly the parts that displacement will move (the
// painted ones); a plain Remesh is a mesh operation on the object and takes all of them, so painting the
// rest of a multibody object afterwards has a dense enough mesh to show relief on.
enum class TexturePrepareScope { PaintedParts, AllParts };

// The prepare in three steps, which the panel's job and OrcaMCP both run (as the bake does, see
// TextureDisplacementBakeJob.hpp): capture on the main thread, plan on any thread, apply on the main
// thread.
//
// Main thread: the job's input for `object`'s parts in `scope`. `color` is captured as given (empty when
// the refinement has no colour criterion); `snapshot_name` names the undo step the apply opens, none when
// empty (the caller has one that covers it).
TextureDisplacementPrepareInput capture_texture_prepare(const ModelObject &object, const TextureDisplacementPrepareParams &params,
                                                        TexturePrepareScope scope, const TextureColorSettings &color,
                                                        const std::string &snapshot_name);
// Main thread: the input still describes the model - what a prepare of `scope` started now would capture (the same parts
// by id, meshes, masks, UV channels, layers and texture frames). `object` is the one the input was captured from.
bool texture_prepare_applies_to(const ModelObject &object, const TextureDisplacementPrepareInput &input, TexturePrepareScope scope);
struct TextureDisplacementPreparePlan
{
    std::vector<TextureDisplacementPrepareResult> results;
    // `progress` said stop: the parts after the one it stopped in were never prepared, so nothing may be applied.
    bool                                          canceled = false;
};
// Any thread: the prepare of every part of `input`, one after another. `progress` is told the overall
// percentage and returns false to stop.
TextureDisplacementPreparePlan plan_texture_prepare(const TextureDisplacementPrepareInput &input,
                                                    const std::function<bool(int)>       &progress);
struct TextureDisplacementPrepareApplied
{
    TextureDisplacementPrepareOutcome outcome = TextureDisplacementPrepareOutcome::Failed;
    // The prepared parts' new ids (a prepare gives each a new one), in the input's order: what a bake
    // planned after it must name.
    std::vector<ObjectID>             committed;
};
// Main thread: commits the plan. Committed, Unchanged (no part needed doing, nothing touched), PaintLost
// (a remesh landed but a part kept none of its paint: nothing is committed) or Failed (a stopped plan).
TextureDisplacementPrepareApplied apply_texture_prepare(Plater &plater, TextureDisplacementPrepareInput &input,
                                                        TextureDisplacementPreparePlan &plan);

class TextureDisplacementPrepareJob : public Job
{
    TextureDisplacementPrepareInput                            m_input;
    TextureDisplacementPreparePlan                             m_plan;
    std::function<void(TextureDisplacementPrepareOutcome)>     m_on_finished;

public:
    TextureDisplacementPrepareJob(TextureDisplacementPrepareInput                        &&input,
                                  std::function<void(TextureDisplacementPrepareOutcome)>   on_finished);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;
};

// `on_finished` runs on the UI thread once the result has been committed (or found not to need
// committing), and always runs exactly once.
void queue_texture_displacement_prepare(TextureDisplacementPrepareInput                      &&input,
                                        std::function<void(TextureDisplacementPrepareOutcome)> on_finished);

} // namespace Slic3r::GUI

#endif // slic3r_TextureDisplacementPrepareJob_hpp_
