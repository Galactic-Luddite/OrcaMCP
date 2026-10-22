#ifndef slic3r_GUI_Utils_FixModelByCgal_hpp_
#define slic3r_GUI_Utils_FixModelByCgal_hpp_

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/MeshRepair.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

class wxString;

namespace Slic3r::GUI { class ProgressDialog; }

namespace Slic3r {

class Model;
class ModelObject;
class ModelVolume;

// OrcaMCP: the repair in steps, so that only the main thread ever changes the model.
//
// Upstream repairs each volume in place (ModelVolume::repair, over Slic3r::repair_mesh) on a worker
// thread against the live ModelObject while the progress dialog pumps events: it replaces meshes and
// gives volumes new ids there, which only the main thread may do (ObjectID.hpp), while the dialog's
// yields run other main-thread work -- an MCP call's among it -- on the same object. Here the worker
// only plans: repair_mesh on copies of the meshes captured on the main thread, with each repaired
// mesh's convex hull and its kept painting remapped. The model changes afterwards, on the main thread,
// as the end of ModelVolume::repair does, taking each mesh from the plan. The object list's Repair
// (ObjectList::fix_through_cgal), the cut gizmo (fix_model_with_cgal_gui) and MCP's repair_mesh all run
// these steps: capture, plan, apply.

// A volume's painting of every kind, as ModelVolume::save_painting keeps it, without the mesh.
struct CgalRepairPainting
{
    TriangleSelector::TriangleSplittingData supported, seam, mmu, fuzzy;
};

// One volume the repair reads, as captured on the main thread.
struct CgalRepairVolume
{
    size_t                              index = 0; // in the object's volumes when captured
    ObjectID                            id;
    std::string                         name;
    std::shared_ptr<const TriangleMesh> mesh;      // immutable once shared: safe to read on any thread
    std::optional<CgalRepairPainting>   painting;  // captured to keep: what ModelVolume::save_painting keeps
    // Its painting's timestamps (supports, seams, colour, fuzzy skin) when captured: painting written
    // since (a paint tool, the gizmo) makes the plan's remap of it stale.
    std::array<ObjectWithTimestamp::Timestamp, 4> painting_stamps{};
};

// What repairing an object reads: every volume, or the one volume asked for.
struct CgalRepairInput
{
    const Model*                  model  = nullptr; // the model whose list held the object, or nullptr
    const ModelObject*            object = nullptr;
    ObjectID                      object_id;
    bool                          whole_object = true;
    size_t                        volume_count = 0;
    std::vector<CgalRepairVolume> targets;               // the volumes repaired, in volume order
    bool                          keep_painting = false; // the painting is captured, and the plan remaps it
};

// Main thread. With `keep_painting`, each painted model part's painting is captured too, so the plan
// can remap it (TriangleSelector::remap_painting, which takes minutes on a large painted mesh) off
// the main thread.
CgalRepairInput capture_cgal_repair(const ModelObject& object, int volume_idx, bool keep_painting = false);

// The Repair dialog's progress: a message (a volume's name, or a msgid translated where it is shown)
// and a percent.
using CgalRepairProgress = std::function<void(const std::string& message, unsigned percent)>;

// What the repair will do to the captured volumes, worked out on the meshes alone: repair_mesh on each,
// and for each it changed, the repaired mesh, its convex hull and, when it is kept, its painting
// remapped onto it. Applying it gives each changed volume those, rather than working them out on the
// main thread.
class CgalRepairPlan
{
public:
    CgalRepairPlan() = default;
    explicit CgalRepairPlan(CgalRepairInput input) : m_input(std::move(input)) {}

    const CgalRepairInput& input() const { return m_input; }

    // One captured volume, as the plan worked it out.
    struct PlannedVolume
    {
        ObjectID                          target;
        MeshRepairStats                   stats;
        std::string                       error;  // repair_mesh's: no solid could be built, the volume stays as it is
        TriangleMesh                      repaired;
        TriangleMesh                      repaired_hull;
        std::optional<CgalRepairPainting> repaired_painting; // planned with painting kept: what it gets
        bool                              taken = false;
    };
    std::vector<PlannedVolume> volumes; // the captured volumes planned, in order; fewer when stopped early

    bool        canceled = false; // stopped before the end: nothing may be applied
    std::string error;            // every volume that could not be repaired, one per line, as upstream lists them
    int         error_volume = -1; // the first of them, as captured

    // The planned volumes the repair changes, and their stats summed.
    size_t          volumes_changed() const;
    MeshRepairStats totals() const;
    // Changes nothing: every volume was sound, or could not be repaired. Known only for a plan planned
    // to its end.
    bool changes_nothing() const { return volumes_changed() == 0; }

    // Whether `object` is still what was captured: the same object, with every volume the plan read
    // still there, holding the same mesh (and, for a whole-object repair, no volume added or removed).
    bool applies_to(const ModelObject& object) const;
    // The index in `object` of the one volume planned for; -1 for a whole-object plan.
    int volume_index_in(const ModelObject& object) const;
    // The captured volume `volume` is, by its id; nullptr for one the plan did not read.
    const CgalRepairVolume* target_of(const ModelVolume& volume) const;
    // Whether `volume` is one the plan read and its painting is still as captured, so the painting the
    // plan remapped can be given to it. Painting written since is remapped in place instead.
    bool painting_unchanged(const ModelVolume& volume) const;

private:
    CgalRepairInput m_input;
};

// Any thread: the expensive half. Reads only `input`'s meshes, never a Model, a ModelVolume or wx.
// `canceled` is asked as repair_mesh reports its progress, so it stops within a volume too.
CgalRepairPlan plan_cgal_repair(CgalRepairInput input, const CgalRepairProgress& progress, const std::function<bool()>& canceled);

// What applying a plan did.
struct CgalRepairResult
{
    std::string error;             // why nothing was applied (a msgid when its own)
    size_t      volumes_changed = 0;
};

// Main thread: why `plan` must not be applied to `object`, or "" when it can be: it was canceled, or the
// object is gone (nullptr) or no longer what the plan read. A msgid, translated where it is shown.
std::string cgal_repair_refusal(const ModelObject* object, const CgalRepairPlan& plan);

// Main thread: the plan's repaired meshes given to their volumes, as the end of ModelVolume::repair
// gives a volume its repaired mesh, unless cgal_repair_refusal refuses it. A volume the plan could not
// repair is left as it is, as upstream leaves it.
CgalRepairResult apply_cgal_repair(ModelObject& object, bool keep_painting, CgalRepairPlan& plan);

// Main thread: plans on a worker thread while `progress_dialog` shows the progress and offers Cancel.
CgalRepairPlan plan_cgal_repair_with_dialog(CgalRepairInput input, GUI::ProgressDialog& progress_dialog, const wxString& msg_header);

// Main thread: true while a Repair's progress dialog runs, when other main-thread work can be served.
bool cgal_repair_dialog_running();

// Main thread: the index of the captured object in `model`'s list, or -1 when that list no longer
// holds it. The dialog lets other work run, which can delete the object or move it in the list.
int cgal_repair_object_index(const Model& model, const CgalRepairPlan& plan);

// Return false if fixing was canceled. fix_result is empty on success; modified tells whether any mesh changed.
extern bool fix_model_with_cgal_gui(ModelObject &model_object, int volume_idx, GUI::ProgressDialog &progress_dlg, const wxString &msg_header, std::string &fix_result, bool keep_painting,
                                    bool *modified = nullptr);

} // namespace Slic3r

#endif /* slic3r_GUI_Utils_FixModelByCgal_hpp_ */
