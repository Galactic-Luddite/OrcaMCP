#include "FlashforgeJobProgress.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>

#include <nlohmann/json.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"

namespace Slic3r { namespace FlashforgeJobProgress {

using nlohmann::json;

const char* const kExtendedInfoKey = "slicedProgress";

std::vector<std::pair<std::uint64_t, double>> time_points(const SliceTable& table)
{
    std::vector<std::pair<std::uint64_t, double>> points;
    for (size_t i = 0; i < table.layer_start_bytes.size() && i < table.layer_start_s.size(); ++i)
        points.emplace_back(table.layer_start_bytes[i], table.layer_start_s[i]);
    for (size_t i = 0; i < table.mark_bytes.size() && i < table.mark_s.size(); ++i)
        points.emplace_back(table.mark_bytes[i], table.mark_s[i]);
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return points;
}

bool SliceTable::valid() const
{
    if (file_bytes == 0 || layer_start_bytes.empty() || layer_start_bytes.size() != layer_start_s.size() ||
        mark_bytes.size() != mark_s.size() || !(total_s > 0))
        return false;
    const auto in_order = [](const std::vector<std::uint64_t>& bytes) { return std::is_sorted(bytes.begin(), bytes.end()); };
    if (!in_order(layer_start_bytes) || !in_order(mark_bytes) || (!mark_bytes.empty() && mark_bytes.front() <= layer_start_bytes.front()))
        return false; // time points lie inside layers
    const auto points = time_points(*this);
    for (size_t i = 0; i < points.size(); ++i) {
        if (points[i].first >= file_bytes || !(points[i].second >= 0) || points[i].second > total_s)
            return false;
        if (i > 0 && (points[i].first <= points[i - 1].first || points[i].second < points[i - 1].second))
            return false;
    }
    return true;
}

SliceTable slice_table(const GCodeProcessorResult& result)
{
    constexpr size_t normal = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);

    SliceTable table;
    if (result.lines_ends.empty())
        return table;
    table.file_bytes = result.lines_ends.back();

    // A move's line starts where the line before it ends; gcode_id is 1-based, 0 for no line.
    const auto line_start = [&](unsigned int gcode_id) -> std::optional<std::uint64_t> {
        if (gcode_id == 0 || gcode_id > result.lines_ends.size())
            return std::nullopt;
        return gcode_id == 1 ? 0 : static_cast<std::uint64_t>(result.lines_ends[gcode_id - 2]);
    };

    double        elapsed    = 0;
    long          last_layer = -1;
    std::uint64_t last_byte  = 0;   // the last time point's
    double        last_s     = 0;
    bool          any_point  = false;
    for (const auto& move : result.moves) {
        const auto start  = line_start(move.gcode_id);
        const bool beyond = start && (!any_point || *start > last_byte);
        if (static_cast<long>(move.layer_id) > last_layer) {
            if (beyond) {
                table.layer_start_bytes.push_back(*start);
                table.layer_start_s.push_back(elapsed);
                last_layer = static_cast<long>(move.layer_id);
                last_byte = *start, last_s = elapsed, any_point = true;
            }
        } else if (beyond && any_point && elapsed - last_s >= kMarkEvery_s) {
            table.mark_bytes.push_back(*start);
            table.mark_s.push_back(elapsed);
            last_byte = *start, last_s = elapsed;
        }
        elapsed += move.time[normal];
    }
    table.total_s = elapsed;
    return table.valid() ? table : SliceTable{};
}

std::string to_json(const SliceTable& table)
{
    // Tenths of a second, never past the total: a point rounded up beyond it would void the table.
    const auto pairs = [&table](const std::vector<std::uint64_t>& bytes, const std::vector<double>& seconds) {
        json out = json::array();
        for (size_t i = 0; i < bytes.size() && i < seconds.size(); ++i)
            out.push_back({bytes[i], std::min(std::round(seconds[i] * 10) / 10, table.total_s)});
        return out;
    };
    json out = {{"bytes", table.file_bytes}, {"total_s", table.total_s}, {"layers", pairs(table.layer_start_bytes, table.layer_start_s)}};
    if (!table.mark_bytes.empty())
        out["marks"] = pairs(table.mark_bytes, table.mark_s);
    return out.dump();
}

std::optional<SliceTable> from_json(const std::string& text)
{
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("bytes") || !j["bytes"].is_number_unsigned() || !j.contains("total_s") ||
        !j["total_s"].is_number() || !j.contains("layers") || !j["layers"].is_array())
        return std::nullopt;

    SliceTable table;
    table.file_bytes = j["bytes"].get<std::uint64_t>();
    table.total_s    = j["total_s"].get<double>();
    const auto read_pairs = [](const json& list, std::vector<std::uint64_t>& bytes, std::vector<double>& seconds) {
        for (const auto& pair : list) {
            if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number_unsigned() || !pair[1].is_number())
                return false;
            bytes.push_back(pair[0].get<std::uint64_t>());
            seconds.push_back(pair[1].get<double>());
        }
        return true;
    };
    if (!read_pairs(j["layers"], table.layer_start_bytes, table.layer_start_s))
        return std::nullopt;
    // Optional: a table from before 2026-10-23 has none.
    if (j.contains("marks") && (!j["marks"].is_array() || !read_pairs(j["marks"], table.mark_bytes, table.mark_s)))
        return std::nullopt;
    if (!table.valid())
        return std::nullopt;
    return table;
}

std::optional<SliceTable> from_extended_info(const std::map<std::string, std::string>& extended_info)
{
    const auto it = extended_info.find(kExtendedInfoKey);
    return it == extended_info.end() ? std::nullopt : from_json(it->second);
}

std::optional<SliceTable> for_upload(const std::map<std::string, std::string>& extended_info,
                                     bool                                      gcode_in_3mf,
                                     std::uint64_t                             uploaded_bytes)
{
    auto table = from_extended_info(extended_info);
    if (table && !gcode_in_3mf && uploaded_bytes != table->file_bytes)
        return std::nullopt;
    return table;
}

Reading read(const SliceTable& table, double byte_progress)
{
    Reading reading;
    if (!table.valid())
        return reading;
    reading.layers     = static_cast<int>(table.layer_start_bytes.size());
    const double bytes = std::clamp(byte_progress, 0.0, 1.0) * static_cast<double>(table.file_bytes);
    const auto   at_or_before = [bytes](const auto& begin, const auto& end, const auto& byte_of) {
        return std::upper_bound(begin, end, bytes, [&](double b, const auto& item) { return b < static_cast<double>(byte_of(item)); });
    };

    // The layer whose start is the last one at or before `bytes`; before the first, the start G-code,
    // which counts as the first layer's.
    const auto layer_after = at_or_before(table.layer_start_bytes.begin(), table.layer_start_bytes.end(),
                                          [](std::uint64_t start) { return start; });
    reading.layer = std::max<int>(1, static_cast<int>(layer_after - table.layer_start_bytes.begin()));

    // The time: the slicer's seconds at the last time point at or before `bytes`, then spread over the
    // bytes to the next point. Before the first point (the first layer's start), from 0.
    const auto points = time_points(table);
    const auto after  = at_or_before(points.begin(), points.end(), [](const auto& point) { return point.first; });
    const double from_b = after == points.begin() ? 0.0 : static_cast<double>(std::prev(after)->first);
    const double from_s = after == points.begin() ? 0.0 : std::prev(after)->second;
    const double to_b   = after != points.end() ? static_cast<double>(after->first) : static_cast<double>(table.file_bytes);
    const double to_s   = after != points.end() ? after->second : table.total_s;
    const double within = to_b > from_b ? std::clamp((bytes - from_b) / (to_b - from_b), 0.0, 1.0) : 0.0;
    reading.time_fraction = std::clamp((from_s + within * (to_s - from_s)) / table.total_s, 0.0, 1.0);
    return reading;
}

void apply(FlashforgeApi::PrinterStatus& status, const SliceTable& table)
{
    const bool active = status.state == "printing" || status.state == "paused";
    if (!active || !status.implausible_telemetry.empty() || !table.valid())
        return;
    const Reading reading  = read(table, status.progress);
    status.layer           = reading.layer;
    status.layers          = reading.layers;
    status.work_done       = reading.time_fraction;
    status.remaining_s     = FlashforgeApi::project_remaining_s(status.state, status.duration_s, reading.time_fraction);
    status.progress_source = "slice";
}

}} // namespace Slic3r::FlashforgeJobProgress
