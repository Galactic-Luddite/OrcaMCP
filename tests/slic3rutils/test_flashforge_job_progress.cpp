#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/Utils/FlashforgeApi.hpp"
#include "slic3r/Utils/FlashforgeJobProgress.hpp"

using namespace Slic3r;
using namespace Slic3r::FlashforgeJobProgress;

namespace {

// Three layers in a 1000-byte file: layer 1 from byte 100 (10 s of the slicer's 100 s before it,
// the start G-code's), layer 2 from byte 200 (after 70 s), layer 3 from byte 600 (after 80 s): the
// second layer is most of the time and little of the file, the third the reverse.
SliceTable three_layers()
{
    SliceTable table;
    table.file_bytes        = 1000;
    table.layer_start_bytes = {100, 200, 600};
    table.layer_start_s     = {10, 70, 80};
    table.total_s           = 100;
    return table;
}

GCodeProcessorResult::MoveVertex move(unsigned int gcode_id, unsigned int layer_id, float seconds)
{
    GCodeProcessorResult::MoveVertex m;
    m.gcode_id = gcode_id;
    m.layer_id = layer_id;
    m.time[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)] = seconds;
    return m;
}

FlashforgeApi::PrinterStatus printing(double progress, long duration_s)
{
    FlashforgeApi::PrinterStatus s;
    s.state       = "printing";
    s.print_file  = "job.gcode.3mf";
    s.progress    = progress;
    s.work_done   = progress;
    s.duration_s  = duration_s;
    s.remaining_s = FlashforgeApi::project_remaining_s(s.state, duration_s, progress);
    s.layer       = 38;
    s.layers      = 57;
    return s;
}

} // namespace

TEST_CASE("A slice table is read off a slice: each layer's first line and the time before it", "[flashforge][FlashforgeJobProgress]")
{
    GCodeProcessorResult result;
    // Five lines, ending at bytes 10, 20, 30, 40 and 50.
    result.lines_ends = {10, 20, 30, 40, 50};
    // The start G-code is layer 0 too (GCodeProcessor numbers moves before the first layer change
    // with the first layer), so the first layer starts at the first move's line.
    result.moves = {move(2, 0, 1.0f), move(3, 0, 2.0f), move(4, 1, 3.0f), move(0, 1, 0.5f), move(5, 1, 4.0f)};

    const SliceTable table = slice_table(result);
    REQUIRE(table.valid());
    CHECK(table.file_bytes == 50);
    CHECK(table.layer_start_bytes == std::vector<std::uint64_t>{10, 30}); // line 2 starts where line 1 ends
    CHECK(table.layer_start_s == std::vector<double>{0.0, 3.0});
    CHECK_THAT(table.total_s, Catch::Matchers::WithinAbs(10.5, 1e-9));
}

TEST_CASE("A layer that takes long gets time points inside it", "[flashforge][FlashforgeJobProgress]")
{
    GCodeProcessorResult result;
    result.lines_ends = {10, 20, 30, 40, 50, 60, 70};
    // Layer 1 from line 2: 10 s, then three 25 s moves (an hour of ironing, scaled down); layer 2 from line 7.
    result.moves = {move(2, 0, 10.0f), move(3, 0, 25.0f), move(4, 0, 25.0f), move(5, 0, 25.0f), move(0, 0, 1.0f),
                    move(6, 0, 1.0f), move(7, 1, 2.0f)};

    const SliceTable table = slice_table(result);
    REQUIRE(table.valid());
    CHECK(table.layer_start_bytes == std::vector<std::uint64_t>{10, 60});
    CHECK(table.layer_start_s == std::vector<double>{0.0, 87.0});
    // A point at the first move with a line once 30 s have passed since the last point: at line 4 (35 s);
    // not at line 5 (60 s, 25 s on), nor on the move with no line, but at line 6 (86 s).
    CHECK(table.mark_bytes == std::vector<std::uint64_t>{30, 50});
    CHECK(table.mark_s == std::vector<double>{35.0, 86.0});
}

TEST_CASE("The work done inside a layer follows its time points", "[flashforge][FlashforgeJobProgress]")
{
    // three_layers' layer 2 runs from byte 200 (70 s) to 600 (80 s). Its first 40 bytes take 9 of its 10 s.
    SliceTable table   = three_layers();
    table.mark_bytes   = {240};
    table.mark_s       = {79};
    REQUIRE(table.valid());
    CHECK_THAT(read(table, 0.22).time_fraction, Catch::Matchers::WithinAbs(0.745, 1e-9)); // halfway to the mark
    CHECK_THAT(read(table, 0.42).time_fraction, Catch::Matchers::WithinAbs(0.795, 1e-9)); // halfway from it to layer 3
    CHECK(read(table, 0.42).layer == 2); // a time point is no layer
    CHECK(read(table, 0.42).layers == 3);

    // A point that sits on a layer start, goes back in time or past the end voids the table.
    table.mark_bytes = {200};
    CHECK_FALSE(table.valid());
    table.mark_bytes = {240}, table.mark_s = {60};
    CHECK_FALSE(table.valid());
    table.mark_bytes = {1200}, table.mark_s = {90};
    CHECK_FALSE(table.valid());
}

TEST_CASE("A slice with no lines or no moves gives no table", "[flashforge][FlashforgeJobProgress]")
{
    GCodeProcessorResult empty;
    CHECK_FALSE(slice_table(empty).valid());

    GCodeProcessorResult no_moves;
    no_moves.lines_ends = {10, 20};
    CHECK_FALSE(slice_table(no_moves).valid());
}

TEST_CASE("The layer printing is the last one started at the printer's byte progress", "[flashforge][FlashforgeJobProgress]")
{
    const SliceTable table = three_layers();
    CHECK(read(table, 0.0).layer == 1);  // the start G-code counts as the first layer's
    CHECK(read(table, 0.15).layer == 1);
    CHECK(read(table, 0.2).layer == 2);  // byte 200 is where layer 2 starts
    CHECK(read(table, 0.59).layer == 2);
    CHECK(read(table, 0.6).layer == 3);
    CHECK(read(table, 1.0).layer == 3);
    CHECK(read(table, 0.5).layers == 3);
}

TEST_CASE("The work done is the slicer's time before the byte, spread evenly within its layer", "[flashforge][FlashforgeJobProgress]")
{
    const SliceTable table = three_layers();
    CHECK_THAT(read(table, 0.0).time_fraction, Catch::Matchers::WithinAbs(0.0, 1e-9));
    // Halfway through the start G-code's 100 bytes is halfway through its 10 s.
    CHECK_THAT(read(table, 0.05).time_fraction, Catch::Matchers::WithinAbs(0.05, 1e-9));
    CHECK_THAT(read(table, 0.2).time_fraction, Catch::Matchers::WithinAbs(0.7, 1e-9));
    // A tenth of the way through layer 2's 400 bytes is a tenth of its 10 s.
    CHECK_THAT(read(table, 0.24).time_fraction, Catch::Matchers::WithinAbs(0.71, 1e-9));
    // Halfway through layer 3's 400 bytes is halfway through its 20 s.
    CHECK_THAT(read(table, 0.8).time_fraction, Catch::Matchers::WithinAbs(0.9, 1e-9));
    CHECK_THAT(read(table, 1.0).time_fraction, Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("A print this app sent is read by its slice: layer, work done and remaining time", "[flashforge][FlashforgeJobProgress]")
{
    // The 2026-10-09 Creator 5 Pro job in small: the printer had read a fifth of the file and said
    // layer 38, while the layer printing (3 here) had most of the time behind it.
    FlashforgeApi::PrinterStatus s = printing(0.8, 9000);
    CHECK(s.remaining_s == 2250); // from bytes: 9000 * 0.2 / 0.8

    apply(s, three_layers());
    CHECK(s.progress_source == "slice");
    CHECK(s.layer == 3);
    CHECK(s.layers == 3);
    CHECK_THAT(s.work_done, Catch::Matchers::WithinAbs(0.9, 1e-9));
    CHECK(s.remaining_s == 1000); // from time: 9000 * 0.1 / 0.9
    CHECK_THAT(s.progress, Catch::Matchers::WithinAbs(0.8, 1e-9)); // the printer's own figure stays as sent
}

TEST_CASE("A status that is not a reading of an active job keeps the printer's numbers", "[flashforge][FlashforgeJobProgress]")
{
    SECTION("not printing")
    {
        FlashforgeApi::PrinterStatus s = printing(0.8, 9000);
        s.state = "completed";
        apply(s, three_layers());
        CHECK(s.progress_source == "printer");
        CHECK(s.layer == 38);
    }
    SECTION("numbers that are memory")
    {
        FlashforgeApi::PrinterStatus s = printing(0.8, 9000);
        s.implausible_telemetry = {"printLayer 5111810"};
        apply(s, three_layers());
        CHECK(s.progress_source == "printer");
    }
    SECTION("no table")
    {
        FlashforgeApi::PrinterStatus s = printing(0.8, 9000);
        apply(s, SliceTable{});
        CHECK(s.progress_source == "printer");
        CHECK(s.remaining_s == 2250);
    }
    SECTION("too little done to project from")
    {
        SliceTable table       = three_layers();
        table.layer_start_s[0] = 0; // no start G-code time
        FlashforgeApi::PrinterStatus s = printing(0.101, 60);
        apply(s, table); // a hundredth of layer 1's bytes is 0.7 % of the time: nothing to project yet
        CHECK(s.progress_source == "slice");
        CHECK(s.layer == 1);
        CHECK(s.remaining_s == -1);
    }
}

TEST_CASE("A slice table travels with an upload as JSON and comes back the same", "[flashforge][FlashforgeJobProgress]")
{
    // The flashforge-obico agent reads this very text (its tests/test_slice_table.py, ORCAMCP_JSON).
    CHECK(to_json(three_layers()) == R"({"bytes":1000,"layers":[[100,10.0],[200,70.0],[600,80.0]],"total_s":100.0})");

    const SliceTable table = three_layers();
    const auto       back  = from_json(to_json(table));
    REQUIRE(back.has_value());
    CHECK(back->file_bytes == table.file_bytes);
    CHECK(back->layer_start_bytes == table.layer_start_bytes);
    CHECK(back->layer_start_s == table.layer_start_s);
    CHECK(back->total_s == table.total_s);

    CHECK_FALSE(from_json("").has_value());
    CHECK_FALSE(from_json("[]").has_value());
    CHECK_FALSE(from_json(R"({"bytes":1000,"total_s":100,"layers":[[600,80],[200,70]]})").has_value()); // not ascending
    CHECK_FALSE(from_json(R"({"bytes":100,"total_s":100,"layers":[[200,70]]})").has_value());            // past the end
    CHECK_FALSE(from_json(R"({"bytes":1000,"total_s":100,"layers":[["a",70]]})").has_value());

    // Time points travel as "marks", only when there are any.
    SliceTable marked = three_layers();
    marked.mark_bytes = {240}, marked.mark_s = {79.04};
    CHECK(to_json(marked) == R"({"bytes":1000,"layers":[[100,10.0],[200,70.0],[600,80.0]],"marks":[[240,79.0]],"total_s":100.0})");
    const auto marked_back = from_json(to_json(marked));
    REQUIRE(marked_back.has_value());
    CHECK(marked_back->mark_bytes == std::vector<std::uint64_t>{240});
    CHECK_FALSE(from_json(R"({"bytes":1000,"total_s":100,"layers":[[100,10]],"marks":[[50,5]]})").has_value()); // before layer 1
    CHECK_FALSE(from_json(R"({"bytes":1000,"total_s":100,"layers":[[100,10]],"marks":{}})").has_value());

    // Rounded to tenths, a point never passes the total.
    SliceTable late = three_layers();
    late.total_s     = 80.04;
    late.mark_bytes  = {900}, late.mark_s = {80.04};
    REQUIRE(late.valid());
    CHECK(from_json(to_json(late)).has_value());
}

TEST_CASE("An upload's table is kept only when it describes the file sent", "[flashforge][FlashforgeJobProgress]")
{
    const std::map<std::string, std::string> info = {{kExtendedInfoKey, to_json(three_layers())}};
    CHECK(for_upload(info, false, 1000).has_value());
    // A plain G-code copy that post-processing changed is not the sliced file.
    CHECK_FALSE(for_upload(info, false, 1200).has_value());
    // The G-code inside a .gcode.3mf is the sliced file itself; the 3MF's own size says nothing.
    CHECK(for_upload(info, true, 5555).has_value());
    CHECK_FALSE(for_upload({}, false, 1000).has_value());
}

TEST_CASE("parse_detail takes the layer, layer count and work done from the printer", "[flashforge][FlashforgeJobProgress]")
{
    FlashforgeApi::PrinterStatus s;
    std::string                  err;
    REQUIRE(FlashforgeApi::parse_detail(
        R"({"code":0,"detail":{"status":"printing","printFileName":"job.gcode.3mf","printProgress":0.206,"printDuration":14537,
             "printLayer":38,"targetPrintLayer":57}})",
        s, err));
    CHECK(s.layer == 38);
    CHECK(s.layers == 57);
    CHECK_THAT(s.work_done, Catch::Matchers::WithinAbs(0.206, 1e-9));
    CHECK(s.progress_source == "printer");

    FlashforgeApi::PrinterStatus idle;
    REQUIRE(FlashforgeApi::parse_detail(R"({"code":0,"detail":{"status":"ready"}})", idle, err));
    CHECK(idle.layer == -1);
    CHECK(idle.layers == -1);
}
