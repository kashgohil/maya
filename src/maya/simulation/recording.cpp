#include "maya/simulation/recording.hpp"
#include "maya/core/build_info.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <istream>
#include <ostream>
#include <sstream>

namespace maya {
namespace {

void append_float(std::string& out, float value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    out.append(buffer, result.ptr);
}
bool parse_float(std::string_view text, float& value) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
template<class T> bool parse_integer(std::string_view text, T& value, int base = 10) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
std::string hex(uint64_t value) {
    auto out = std::ostringstream{};
    out << std::hex << value;
    return out.str();
}
std::string hex16(uint64_t value) {
    auto out = std::ostringstream{};
    out << std::hex << std::setw(16) << std::setfill('0') << value;
    return out.str();
}

template<size_t N> std::string bit_list(const std::bitset<N>& bits) {
    auto out = std::string{};
    for (size_t i = 0; i < N; ++i)
        if (bits[i]) {
            if (!out.empty()) out += ',';
            out += std::to_string(i);
        }
    return out.empty() ? "-" : out;
}
template<size_t N> bool parse_bits(std::string_view text, std::bitset<N>& bits) {
    bits.reset();
    if (text == "-") return true;
    while (!text.empty()) {
        const auto comma = text.find(',');
        auto index = size_t{};
        if (!parse_integer(text.substr(0, comma), index) || index >= N) return false;
        bits.set(index);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }
    return true;
}

/// Splits a line into words; a word starting with a quote is read as one quoted string.
std::vector<std::string> words(const std::string& line) {
    auto in = std::istringstream(line);
    auto result = std::vector<std::string>{};
    in >> std::ws;
    while (in.peek() != std::char_traits<char>::eof()) {
        auto word = std::string{};
        if (in.peek() == '"') in >> std::quoted(word);
        else in >> word;
        if (!in) break;
        result.push_back(std::move(word));
        in >> std::ws;
    }
    return result;
}

} // namespace

bool same_input(const InputFrame& a, const InputFrame& b) noexcept {
    return a.held == b.held && a.pressed == b.pressed && a.released == b.released && a.buttons_held == b.buttons_held &&
           a.buttons_pressed == b.buttons_pressed && a.buttons_released == b.buttons_released && a.look.x == b.look.x &&
           a.look.y == b.look.y && a.scroll == b.scroll;
}

void InputTrack::push(const InputFrame& frame) {
    if (m_changes.empty() || !same_input(m_changes.back().frame, frame)) m_changes.push_back({m_ticks, frame});
    ++m_ticks;
}

InputFrame InputTrack::at(uint64_t tick) const {
    const auto after = std::ranges::upper_bound(m_changes, tick, {}, &Change::tick);
    return after == m_changes.begin() ? InputFrame{} : std::prev(after)->frame;
}

bool InputTrack::add_change(uint64_t tick, InputFrame frame) {
    if (!m_changes.empty() && tick <= m_changes.back().tick) return false;
    m_changes.push_back({tick, std::move(frame)});
    return true;
}

bool InputTrack::set_size(uint64_t ticks) {
    if (!m_changes.empty() && ticks <= m_changes.back().tick) return false;
    m_ticks = ticks;
    return true;
}

std::string recording_build() {
    const auto info = build_info();
    return info.revision + " " + info.build_type + " sanitizers " + info.sanitizers + " " + info.compiler;
}

std::string recording_physics(const PhysicsSettings& settings) {
    return physics_configuration() + "; " + std::to_string(settings.collision_steps) + " collision step" +
           (settings.collision_steps == 1 ? "" : "s");
}

std::string content_hash(std::string_view bytes) {
    auto hash = uint64_t{14695981039346656037ull};
    for (const auto c : bytes) hash = (hash ^ uint8_t(c)) * 1099511628211ull;
    return hex16(hash);
}

void write_recording(std::ostream& output, const PlayRecording& recording) {
    auto out = std::ostringstream{};
    out << "maya-recording " << PlayRecording::format_version << '\n';
    out << "build " << std::quoted(recording.build) << '\n';
    out << "physics " << std::quoted(recording.physics) << '\n';
    out << "seed " << hex(recording.seed) << '\n';
    out << "ticks-per-second " << recording.ticks_per_second << '\n';
    out << "ticks " << recording.inputs.size() << '\n';
    out << "scene-name " << std::quoted(recording.scene_name) << '\n';
    for (const auto& asset : recording.assets)
        out << "asset " << asset.kind << ' ' << hex(asset.id.high) << ' ' << hex(asset.id.low) << ' ' << std::quoted(asset.path) << ' '
            << asset.hash << '\n';
    for (const auto& reload : recording.reloads)
        out << "reload " << reload.tick << ' ' << hex(reload.script.high) << ' ' << hex(reload.script.low) << ' '
            << std::quoted(reload.name) << '\n';
    for (const auto& checkpoint : recording.checkpoints) out << "checkpoint " << checkpoint.tick << ' ' << hex16(checkpoint.state) << '\n';
    out << "final " << hex16(recording.final_state) << '\n';
    for (const auto& [tick, frame] : recording.inputs.changes()) {
        auto line = std::string("input ") + std::to_string(tick) + " look ";
        append_float(line, frame.look.x);
        line += ' ';
        append_float(line, frame.look.y);
        line += " scroll ";
        append_float(line, frame.scroll);
        line += " held " + bit_list(frame.held) + " pressed " + bit_list(frame.pressed) + " released " + bit_list(frame.released) +
                " buttons " + bit_list(frame.buttons_held) + ' ' + bit_list(frame.buttons_pressed) + ' ' + bit_list(frame.buttons_released);
        out << line << '\n';
    }
    // The scene last, as a counted block of scene-file text.
    out << "scene " << recording.scene.size() << '\n' << recording.scene << "\nend\n";
    output << out.str();
}

RecordingReadResult read_recording(std::istream& input) {
    const auto fail = [](std::string message) { return RecordingReadResult{std::nullopt, std::move(message)}; };
    auto recording = PlayRecording{};
    auto line = std::string{};
    auto number = 0;
    const auto where = [&] { return "line " + std::to_string(number) + ": "; };
    if (!std::getline(input, line)) return fail("The file is empty");
    ++number;
    const auto header = words(line);
    auto version = uint32_t{};
    if (header.size() != 2 || header[0] != "maya-recording" || !parse_integer(header[1], version))
        return fail("Not a Maya play recording: it does not start with \"maya-recording\"");
    if (version != PlayRecording::format_version)
        return fail("Recording format " + std::to_string(version) + "; this build reads format " +
                    std::to_string(PlayRecording::format_version));
    auto ticks = std::optional<uint64_t>{};
    auto have_final = false;
    while (std::getline(input, line)) {
        ++number;
        const auto w = words(line);
        if (w.empty()) continue;
        const auto& key = w[0];
        const auto id = [&](size_t at, AssetId& value) {
            return w.size() > at + 1 && parse_integer(w[at], value.high, 16) && parse_integer(w[at + 1], value.low, 16);
        };
        if (key == "build" && w.size() == 2) recording.build = w[1];
        else if (key == "physics" && w.size() == 2) recording.physics = w[1];
        else if (key == "seed" && w.size() == 2 && parse_integer(w[1], recording.seed, 16)) {}
        else if (key == "ticks-per-second" && w.size() == 2 && parse_integer(w[1], recording.ticks_per_second)) {}
        else if (key == "ticks" && w.size() == 2 && parse_integer(w[1], ticks.emplace())) {}
        else if (key == "scene-name" && w.size() == 2) recording.scene_name = w[1];
        else if (key == "asset" && w.size() == 6) {
            auto asset = RecordedAsset{};
            if (!id(2, asset.id)) return fail(where() + "an asset needs two hexadecimal ID words");
            asset.kind = w[1];
            asset.path = w[4];
            asset.hash = w[5];
            recording.assets.push_back(std::move(asset));
        } else if (key == "reload" && w.size() == 5) {
            auto reload = RecordedReload{};
            if (!parse_integer(w[1], reload.tick) || !id(2, reload.script)) return fail(where() + "a reload needs a tick and a script ID");
            reload.name = w[4];
            recording.reloads.push_back(std::move(reload));
        } else if (key == "checkpoint" && w.size() == 3) {
            auto checkpoint = PlayCheckpoint{};
            if (!parse_integer(w[1], checkpoint.tick) || !parse_integer(w[2], checkpoint.state, 16))
                return fail(where() + "a checkpoint needs a tick and a hash");
            recording.checkpoints.push_back(checkpoint);
        } else if (key == "final" && w.size() == 2 && parse_integer(w[1], recording.final_state, 16)) {
            have_final = true;
        } else if (key == "input" && w.size() == 17 && w[2] == "look" && w[5] == "scroll" && w[7] == "held" && w[9] == "pressed" &&
                   w[11] == "released" && w[13] == "buttons") {
            // input <tick> look <x> <y> scroll <s> held <keys> pressed <keys> released <keys> buttons <held> <pressed> <released>
            auto tick = uint64_t{};
            auto frame = InputFrame{};
            if (!parse_integer(w[1], tick) || !parse_float(w[3], frame.look.x) || !parse_float(w[4], frame.look.y) ||
                !parse_float(w[6], frame.scroll) || !parse_bits(w[8], frame.held) || !parse_bits(w[10], frame.pressed) ||
                !parse_bits(w[12], frame.released) || !parse_bits(w[14], frame.buttons_held) || !parse_bits(w[15], frame.buttons_pressed) ||
                !parse_bits(w[16], frame.buttons_released))
                return fail(where() + "an input line is malformed");
            if (!recording.inputs.add_change(tick, frame)) return fail(where() + "input changes must come in tick order");
        } else if (key == "scene" && w.size() == 2) {
            auto size = size_t{};
            if (!parse_integer(w[1], size)) return fail(where() + "the scene needs its size in bytes");
            recording.scene.resize(size);
            if (!input.read(recording.scene.data(), std::streamsize(size))) return fail(where() + "the scene is cut short");
            input >> std::ws;
            if (!std::getline(input, line) || line != "end") return fail("The recording does not end with \"end\" after its scene");
            if (!ticks) return fail("The recording does not say how many ticks it has");
            if (!have_final) return fail("The recording has no final state");
            if (!recording.inputs.set_size(*ticks)) return fail("The recording's input runs past its ticks");
            if (recording.build.empty() || recording.physics.empty()) return fail("The recording does not name its build and physics");
            return {std::move(recording), {}};
        } else {
            return fail(where() + "unexpected \"" + key + "\"");
        }
    }
    return fail("The recording has no scene");
}

std::string replay_refusal(const PlayRecording& recording, const std::vector<RecordedAsset>& assets, const PhysicsSettings& settings) {
    if (const auto here = recording_build(); recording.build != here)
        return "It was recorded by build " + recording.build + "; this is build " + here +
               ". A replay is promised to match only on the build that recorded it";
    if (const auto here = recording_physics(settings); recording.physics != here)
        return "It was recorded with " + recording.physics + "; this session uses " + here;
    if (!recording.reloads.empty()) {
        const auto& reload = recording.reloads.front();
        return reload.name + " was reloaded at tick " + std::to_string(reload.tick) +
               " of the recorded session, so its replay is not promised to match";
    }
    for (const auto& recorded : recording.assets) {
        const auto now = std::ranges::find(assets, recorded.id, &RecordedAsset::id);
        if (now == assets.end()) return recorded.path + " is no longer in the project";
        if (now->hash != recorded.hash) return recorded.path + " has changed since the recording";
    }
    return {};
}

} // namespace maya
