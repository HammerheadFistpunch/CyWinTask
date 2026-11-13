#include "AppUi.h"
#include "BridgeClient.h"
#include "ProcessTrust.h"
#include "UiScale.h"

#include <cyui/Core.h>
#include <cyui/modules/Plots.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <d3d11.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
struct Key {
    int id {};
    std::int64_t created {};
    bool operator==(const Key& other) const { return id == other.id && created == other.created; }
};
struct KeyHash {
    std::size_t operator()(const Key& key) const
    {
        return std::hash<std::int64_t>()((static_cast<std::int64_t>(key.id) << 32) ^ key.created);
    }
};
Key identity(const ProcessInfo& process) { return { process.id, process.created }; }
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::wstring wide(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring output(length, 0);
    if (length) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), output.data(), length);
    return output;
}
std::string format(const char* pattern, const double value)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), pattern, value);
    return buffer;
}
std::string processAge(const std::int64_t created, const std::int64_t unixMs)
{
    if (created <= 0 || unixMs <= 0) return "—";
    const auto now = 116444736000000000LL + unixMs * 10000LL;
    const auto seconds = std::max<std::int64_t>(0, (now - created) / 10000000LL);
    char buffer[48] {};
    if (seconds >= 86400) std::snprintf(buffer, sizeof(buffer), "%lld d %02lld h", seconds / 86400, seconds / 3600 % 24);
    else std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld", seconds / 3600, seconds / 60 % 60, seconds % 60);
    return buffer;
}
const char* categoryName(const std::uint8_t category)
{
    return category == 0 ? "Applications" : category == 2 ? "Windows processes" : "Background";
}
const MetricInfo* metric(const DeviceInfo& device, const std::string& id)
{
    for (const auto& item : device.metrics) if (item.id == id) return &item;
    return nullptr;
}
double metricValue(const DeviceInfo& device, const std::string& id)
{
    const auto* item = metric(device, id);
    return item ? item->value : NAN;
}
ImU32 heat(const double percent, const bool memory)
{
    const auto value = memory ? percent * 5.0 : percent;
    const auto& palette = cyui::palette();
    return value >= 50 ? palette.error : value >= 20 ? palette.warning : value >= 5 ? palette.info : palette.accent;
}
void usageBar(const double percent, const std::string& text, const bool memory, const float heightDp = 22.0f)
{
    // CyUI's rounded progress bar enforces a minimum filled width. Process loads
    // below 1% need a proportional hairline so small differences remain visible.
    const auto& palette = cyui::palette();
    const auto pos = ImGui::GetCursorScreenPos();
    const float width = std::max(cyui::dp(55.0f), ImGui::GetContentRegionAvail().x);
    const float height = cyui::dp(heightDp);
    ImGui::Dummy(ImVec2(width, height));
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 max(pos.x + width, pos.y + height);
    draw->AddRectFilled(pos, max, palette.bg3, cyui::dp(3.0f));
    if (std::isfinite(percent) && percent > 0)
        draw->AddRectFilled(ImVec2(pos.x, max.y - cyui::dp(3.0f)),
            ImVec2(pos.x + width * static_cast<float>(std::clamp(percent / 100.0, 0.0, 1.0)), max.y),
            heat(percent, memory));
    draw->AddText(ImVec2(pos.x + cyui::dp(5.0f), pos.y + (height - ImGui::GetFontSize()) * 0.5f), palette.text, text.c_str());
}
void trustBar(const TrustResult& trust, const float height)
{
    const auto& colors = cyui::palette();
    const auto position = ImGui::GetCursorScreenPos();
    const float width = std::max(cyui::dp(64.0f), ImGui::GetContentRegionAvail().x);
    ImGui::Dummy(ImVec2(width, height));
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(position, ImVec2(position.x + width, position.y + height), colors.bg3, cyui::dp(2.0f));
    if (trust.score >= 0) {
        const ImU32 colour = trust.score >= 80 ? colors.success
            : trust.score >= 55 ? colors.info : trust.score >= 30 ? colors.warning : colors.error;
        const float fill = width * static_cast<float>(trust.score) / 100.0f;
        draw->AddRectFilled(ImVec2(position.x, position.y + height - cyui::dp(3.0f)),
            ImVec2(position.x + fill, position.y + height), colour);
        const auto score = std::to_string(trust.score) + " / 100";
        draw->AddText(ImVec2(position.x + cyui::dp(5.0f),
            position.y + std::max(0.0f, (height - ImGui::GetFontSize()) * 0.5f - cyui::dp(1.0f))),
            colors.text, score.c_str());
    } else draw->AddText(ImVec2(position.x + cyui::dp(5.0f),
        position.y + (height - ImGui::GetFontSize()) * 0.5f), colors.textDim,
        trust.state == TrustState::pending ? "..." : "—");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\nLocal trust score only.",
        trust.label, trust.explanation);
}
class IconCache {
    ID3D11Device* device_ {};
    std::unordered_map<std::string, ID3D11ShaderResourceView*> cache_;
    int budget_ {};
public:
    explicit IconCache(ID3D11Device* device) : device_(device) {}
    ~IconCache() { for (auto& item : cache_) if (item.second) item.second->Release(); }
    void beginFrame() { budget_ = 3; }
    ID3D11ShaderResourceView* get(const std::string& path)
    {
        if (path.empty()) return nullptr;
        if (const auto found = cache_.find(path); found != cache_.end()) return found->second;
        if (budget_-- <= 0) return nullptr;
        if (cache_.size() >= 2048) {
            for (auto& item : cache_) if (item.second) item.second->Release();
            cache_.clear();
        }
        SHFILEINFOW info {};
        const auto source = wide(path);
        if (!SHGetFileInfoW(source.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) || !info.hIcon)
            return cache_[path] = nullptr;
        constexpr int side = 28;
        BITMAPINFO bitmap {};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = side;
        bitmap.bmiHeader.biHeight = -side;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        void* pixels {};
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP image = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
        ID3D11ShaderResourceView* view {};
        if (dc && image && pixels) {
            HGDIOBJ previous = SelectObject(dc, image);
            std::memset(pixels, 0, side * side * 4);
            DrawIconEx(dc, 0, 0, info.hIcon, side, side, 0, nullptr, DI_NORMAL);
            auto* bytes = static_cast<unsigned char*>(pixels);
            for (int i = 0; i < side * side; ++i)
                if (bytes[i * 4 + 3] == 0 && (bytes[i * 4] || bytes[i * 4 + 1] || bytes[i * 4 + 2]))
                    bytes[i * 4 + 3] = 255;
            D3D11_TEXTURE2D_DESC desc {};
            desc.Width = side; desc.Height = side; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA data { pixels, side * 4, 0 };
            ID3D11Texture2D* texture {};
            if (SUCCEEDED(device_->CreateTexture2D(&desc, &data, &texture))) {
                device_->CreateShaderResourceView(texture, nullptr, &view);
                texture->Release();
            }
            SelectObject(dc, previous);
        }
        if (image) DeleteObject(image);
        if (dc) DeleteDC(dc);
        DestroyIcon(info.hIcon);
        return cache_[path] = view;
    }
};
struct PlotPoint { double time {}, value {}; };
using History = std::deque<PlotPoint>;
struct DisplayRow {
    int index { -1 };
    int depth {};
    int descendants {};
    bool groupHeader {}, hasChildren {};
    std::string title;
};
}

struct AppUi::State {
    explicit State(ID3D11Device* device, const bool demo, const bool showPerformance)
        : icons(device), demoMode(demo), performancePage(showPerformance)
    {
        if (demoMode) { fillDemo(); return; }
        if (!bridge.start()) { error = bridge.startupError(); return; }
        bridge.send(showPerformance ? "M 1\n" : "M 0\n");
    }
    BridgeClient bridge;
    IconCache icons;
    ProcessTrust trust;
    SystemSnapshot snapshot;
    bool demoMode {};
    bool hasSnapshot {}, performancePage {}, paused {}, graphMode { true }, groupByType {}, treeMode { true };
    bool compactGraphCards { true }, graphExtraDevices { true }, compactProcesses { true }, showProcessInspector { true };
    bool showCores {}, compactCores { true }, stopModal {}, detailsOpen {}, focusSearch {};
    bool initialExpansion { true };
    int categoryFilter {}, sortColumn {}, sortDirection { 1 }, lastSelected {-1}, nextRequest {1};
    int graphColumnsMode {};
    char search[256] {};
    std::string activeDevice {"cpu"}, detailText, statusMessage, error;
    std::unordered_set<Key, KeyHash> selected, expanded;
    std::vector<Key> pendingStop;
    std::vector<DisplayRow> display;
    std::vector<int> topCpu, topMemory;
    std::vector<TrustResult> trustScores;
    std::unordered_map<std::string, History> histories;
    std::unordered_map<std::string, std::vector<cyui::Point>> plotPoints;
    std::unordered_map<std::string, double> diskRateScale;
    std::map<std::string, bool> detached;
    bool dirty { true };
    double nextTrustRefresh {};

    void fillDemo()
    {
        hasSnapshot = true;
        snapshot.unixMs = 1700000000000;
        snapshot.cpu = 18.4;
        snapshot.memoryUsedGb = 15.8;
        snapshot.memoryTotalGb = 32.0;
        snapshot.collectMs = 2.7;
        const auto addProcess = [this](int id, int parent, const char* name, double cpu, double gpu, int memoryMb,
                                       int threads, std::uint8_t category) {
            ProcessInfo item;
            item.id = id; item.parent = parent; item.name = name; item.cpu = cpu; item.gpu = gpu; item.gpuGroup = gpu;
            item.created = 133400000000000000LL + id * 10000000LL;
            item.workingSet = static_cast<std::int64_t>(memoryMb) * 1048576;
            item.threads = threads; item.handles = threads * 31 + id % 59; item.category = category;
            snapshot.processes.push_back(std::move(item));
        };
        addProcess(4200, 0, "Studio.exe", 6.4, 4.2, 1840, 42, 0);
        addProcess(4201, 4200, "Studio Renderer.exe", 3.1, 27.5, 920, 19, 1);
        addProcess(4202, 4200, "Studio Media.exe", 0.8, 5.1, 215, 11, 1);
        addProcess(5600, 0, "Browser.exe", 2.3, 0.3, 690, 27, 0);
        addProcess(5601, 5600, "Browser Renderer.exe", 1.7, 8.6, 412, 15, 1);
        addProcess(6800, 0, "Notes.exe", 0.2, 0.0, 155, 12, 0);
        addProcess(8900, 0, "Sync.exe", 0.6, 0.0, 188, 9, 1);
        addProcess(8901, 8900, "Sync Worker.exe", 0.4, 0.0, 120, 7, 1);
        addProcess(1100, 0, "System", 0.5, 0.0, 305, 84, 2);
        addProcess(1110, 1100, "Audio service", 0.1, 0.0, 68, 14, 2);
        for (int i = 0; i < 24; ++i)
            addProcess(12000 + i, 0, ("Demo service " + std::to_string(i + 1)).c_str(),
                i % 6 == 0 ? 0.3 : 0.0, 0.0, 12 + i * 3, 2 + i % 7, i % 3 == 0 ? 2 : 1);
        snapshot.processes[0].gpuGroup = 31.7;
        snapshot.processes[3].gpuGroup = 8.9;
        snapshot.devices = {
            { "cpu", "Processor", "Demo processor · 8 logical processors", "cpu",
                { { "cpu", "Total usage", "%", 18.4, 100 }, { "temperature", "CPU temperature", "°C", 51, NAN } } },
            { "memory", "Memory", "15.8 / 32 GiB used", "memory",
                { { "used", "Physical memory used", "GiB", 15.8, 32 },
                  { "available", "Physical memory available", "GiB", 16.2, 32 },
                  { "commit", "Committed memory", "GiB", 21.3, 48 } } },
            { "disk0", "Disk 0", "Demo SSD", "disk",
                { { "active", "Active time", "%", 24, 100 },
                  { "read", "Read", "MiB/s", 18, NAN }, { "write", "Write", "MiB/s", 7, NAN } } },
            { "network0", "Ethernet", "Demo network", "network",
                { { "receive", "Receive", "Mbps", 12.4, NAN },
                  { "send", "Send", "Mbps", 3.5, NAN } } },
            { "gpu0", "GPU 0", "Demo GPU", "gpu",
                { { "usage", "Usage", "%", 36.2, 100 },
                  { "temperature", "GPU temperature", "°C", 58, NAN },
                  { "dedicated", "Dedicated memory", "GiB", 5.2, 16 } } }
        };
        for (int i = 0; i < 8; ++i)
            snapshot.devices[0].metrics.push_back({ "core:" + std::to_string(i),
                "Logical processor " + std::to_string(i), "%", 8.0 + i * 3.1, 100 });
        selected.insert(identity(snapshot.processes.front()));
        for (const auto& process : snapshot.processes) if (process.category == 0) expanded.insert(identity(process));
        initialExpansion = false;
        refreshTopProcesses();
        refreshTrustScores();
        const double now = snapshot.unixMs / 1000.0;
        for (const auto& device : snapshot.devices)
            for (const auto& value : device.metrics) {
                auto& points = histories[device.id + "/" + value.id];
                for (int i = 0; i < 60; ++i) {
                    const double wave = 0.8 + 0.15 * std::sin(i * 0.27 + static_cast<double>(value.id.size()));
                    points.push_back({ now - 59 + i, std::max(0.0, value.value * wave) });
                }
            }
        preparePlots();
    }

    void switchPage(const bool performance)
    {
        performancePage = performance;
        if (!demoMode) bridge.send(performance || !detached.empty() ? "M 1\n" : "M 0\n");
    }
    void receive()
    {
        if (demoMode) return;
        SystemSnapshot next;
        std::vector<BridgeReply> replies;
        std::string nextError;
        if (bridge.poll(next, replies, nextError)) {
            snapshot = std::move(next);
            hasSnapshot = true;
            dirty = true;
            refreshTopProcesses();
            refreshTrustScores();
            error.clear();
            const double now = snapshot.unixMs / 1000.0;
            for (const auto& device : snapshot.devices)
                for (const auto& value : device.metrics) {
                    auto& points = histories[device.id + "/" + value.id];
                    points.push_back({ now, value.value });
                    while (!points.empty() && (points.size() > 120 || now - points.front().time > 60.0))
                        points.pop_front();
                }
            preparePlots();
            std::unordered_set<Key, KeyHash> alive;
            for (const auto& process : snapshot.processes) alive.insert(identity(process));
            if (initialExpansion) {
                for (const auto& process : snapshot.processes)
                    if (process.category == 0) expanded.insert(identity(process));
                initialExpansion = false;
            }
            for (auto it = selected.begin(); it != selected.end();)
                it = alive.count(*it) ? std::next(it) : selected.erase(it);
            for (auto it = expanded.begin(); it != expanded.end();)
                it = alive.count(*it) ? std::next(it) : expanded.erase(it);
        }
        if (!nextError.empty()) error = std::move(nextError);
        for (const auto& reply : replies) {
            if (reply.success) {
                statusMessage = reply.text;
                cyui::notify(reply.text.c_str(), cyui::Tone::success);
                if (reply.text != "Stop requested.") { detailText = reply.text; detailsOpen = true; }
            } else {
                statusMessage = reply.text;
                cyui::notify(reply.text.c_str(), cyui::Tone::error);
            }
        }
    }
    void refreshTopProcesses()
    {
        topCpu.resize(snapshot.processes.size());
        std::iota(topCpu.begin(), topCpu.end(), 0);
        topMemory = topCpu;
        const auto keep = std::min<std::size_t>(5, snapshot.processes.size());
        const auto cpuValue = [this](const int index) {
            const double value = snapshot.processes[index].cpu;
            return std::isfinite(value) ? value : 0.0;
        };
        std::partial_sort(topCpu.begin(), topCpu.begin() + keep, topCpu.end(),
            [&](int a, int b) { return cpuValue(a) == cpuValue(b)
                ? snapshot.processes[a].workingSet > snapshot.processes[b].workingSet : cpuValue(a) > cpuValue(b); });
        std::partial_sort(topMemory.begin(), topMemory.begin() + keep, topMemory.end(),
            [&](int a, int b) { return snapshot.processes[a].workingSet == snapshot.processes[b].workingSet
                ? cpuValue(a) > cpuValue(b) : snapshot.processes[a].workingSet > snapshot.processes[b].workingSet; });
        topCpu.resize(keep);
        topMemory.resize(keep);
    }
    bool refreshTrustScores()
    {
        std::vector<TrustResult> updated;
        updated.reserve(snapshot.processes.size());
        for (const auto& process : snapshot.processes) {
            if (demoMode) {
                const int score = process.id == 12009 ? 12 : process.id == 6800 ? 40
                    : process.category == 2 ? 94 : process.category == 0 ? 88 : 63;
                updated.push_back({ TrustState::verified, score, "Simulation", "Simulated trust score." });
            } else updated.push_back(trust.get(process.path));
        }
        bool changed = updated.size() != trustScores.size();
        if (!changed)
            for (std::size_t i = 0; i < updated.size(); ++i)
                if (updated[i].state != trustScores[i].state || updated[i].score != trustScores[i].score) {
                    changed = true;
                    break;
                }
        trustScores = std::move(updated);
        return changed;
    }
    void preparePlots()
    {
        plotPoints.clear();
        diskRateScale.clear();
        const double now = snapshot.unixMs / 1000.0;
        for (const auto& device : snapshot.devices) {
            double rateMaximum = 1.0;
            if (device.kind == "disk") {
                for (const auto& value : device.metrics) {
                    if (value.id != "read" && value.id != "write") continue;
                    if (std::isfinite(value.value)) rateMaximum = std::max(rateMaximum, value.value);
                    if (const auto found = histories.find(device.id + "/" + value.id); found != histories.end())
                        for (const auto& point : found->second)
                            if (std::isfinite(point.value) && now - point.time <= 60.0)
                                rateMaximum = std::max(rateMaximum, point.value);
                }
                rateMaximum = std::max(1.0, rateMaximum * 1.15);
                diskRateScale[device.id] = rateMaximum;
            }
            for (const auto& value : device.metrics) {
                const auto key = device.id + "/" + value.id;
                const auto found = histories.find(key);
                if (found == histories.end()) continue;
                auto& prepared = plotPoints[key];
                prepared.reserve(found->second.size() * 2);
                double previousTime = NAN;
                for (const auto& point : found->second) {
                    if (now - point.time > 60.0) continue;
                    if (std::isfinite(previousTime) && point.time - previousTime > 3.5)
                        prepared.push_back({(previousTime + point.time) * 0.5, NAN});
                    // Plots has one Y axis; map disk throughput onto the activity
                    // percentage axis while retaining raw values for the tooltip.
                    const double y = device.kind == "disk" && (value.id == "read" || value.id == "write")
                        ? point.value / rateMaximum * 100.0 : point.value;
                    prepared.push_back({point.time, y});
                    previousTime = point.time;
                }
            }
        }
    }
    const ProcessInfo* find(const Key& key) const
    {
        for (const auto& process : snapshot.processes) if (identity(process) == key) return &process;
        return nullptr;
    }
    bool queryMatches(const ProcessInfo& process) const
    {
        const auto query = lower(search);
        if (query.rfind("pid:", 0) == 0) return std::to_string(process.id) == query.substr(4);
        return query.empty() || lower(process.name).find(query) != std::string::npos
            || std::to_string(process.id).find(query) != std::string::npos;
    }
    bool matches(const ProcessInfo& process) const
    {
        return (!categoryFilter || process.category == categoryFilter - 1) && queryMatches(process);
    }
    bool less(const int left, const int right) const
    {
        const auto& a = snapshot.processes[left];
        const auto& b = snapshot.processes[right];
        int comparison {};
        switch (sortColumn) {
        case 1: comparison = (trustScores[left].score > trustScores[right].score)
            - (trustScores[left].score < trustScores[right].score); break;
        case 2: comparison = (a.id > b.id) - (a.id < b.id); break;
        case 3: comparison = (a.cpu > b.cpu) - (a.cpu < b.cpu); break;
        case 4: { const double av = treeMode ? a.gpuGroup : a.gpu; const double bv = treeMode ? b.gpuGroup : b.gpu;
            if (std::isfinite(av) != std::isfinite(bv)) return std::isfinite(av);
            comparison = (av > bv) - (av < bv); break; }
        case 5: comparison = (a.workingSet > b.workingSet) - (a.workingSet < b.workingSet); break;
        case 6: comparison = (a.threads > b.threads) - (a.threads < b.threads); break;
        case 7: comparison = (a.handles > b.handles) - (a.handles < b.handles); break;
        case 8: comparison = (b.created > a.created) - (b.created < a.created); break;
        default: comparison = lower(a.name).compare(lower(b.name)); break;
        }
        if (comparison == 0) comparison = (a.id > b.id) - (a.id < b.id);
        return sortDirection >= 0 ? comparison < 0 : comparison > 0;
    }
    void buildRows()
    {
        if (!dirty) return;
        dirty = false;
        display.clear();
        std::vector<int> all(snapshot.processes.size());
        std::iota(all.begin(), all.end(), 0);
        if (!treeMode) {
            for (int group = 0; group < (groupByType ? 3 : 1); ++group) {
                std::vector<int> indices;
                for (int index : all)
                    if (matches(snapshot.processes[index]) && (!groupByType || snapshot.processes[index].category == group))
                        indices.push_back(index);
                std::sort(indices.begin(), indices.end(), [this](int a, int b) { return less(a, b); });
                if (groupByType && !indices.empty()) display.push_back({ -1, 0, 0, true, false,
                    std::string(categoryName(static_cast<std::uint8_t>(group))) + " (" + std::to_string(indices.size()) + ")" });
                for (int index : indices) display.push_back({ index });
            }
            return;
        }
        std::unordered_map<int, int> byId;
        for (int index : all) byId[snapshot.processes[index].id] = index;
        std::vector<std::vector<int>> children(all.size());
        std::vector<int> roots;
        for (int index : all) {
            const auto& child = snapshot.processes[index];
            const auto found = byId.find(child.parent);
            if (found != byId.end() && found->second != index
                && snapshot.processes[found->second].created <= child.created)
                children[found->second].push_back(index);
            else roots.push_back(index);
        }
        const std::string query = lower(search);
        std::vector<bool> retained(all.size(), query.empty());
        if (!query.empty()) {
            std::vector<bool> visitedDescendants(all.size(), false);
            std::function<void(int, int)> keepDescendants = [&](int index, int depth) {
                if (depth > 24 || visitedDescendants[index]) return;
                visitedDescendants[index] = true;
                retained[index] = true;
                for (int child : children[index]) keepDescendants(child, depth + 1);
            };
            for (int index : all) {
                const auto& process = snapshot.processes[index];
                if (!queryMatches(process)) continue;
                keepDescendants(index, 0);
                int current = index;
                for (std::size_t guard = 0; guard < all.size(); ++guard) {
                    retained[current] = true;
                    const auto parent = byId.find(snapshot.processes[current].parent);
                    if (parent == byId.end() || parent->second == current
                        || snapshot.processes[parent->second].created > snapshot.processes[current].created) break;
                    current = parent->second;
                }
            }
        }
        std::vector<int> descendantCounts(all.size(), -1);
        std::function<int(int, int)> countDescendants = [&](int index, int depth) -> int {
            if (depth > 24) return 0;
            if (descendantCounts[index] == -2) return 0;
            if (descendantCounts[index] >= 0) return descendantCounts[index];
            descendantCounts[index] = -2;
            int total {};
            for (int child : children[index]) if (retained[child]) total += 1 + countDescendants(child, depth + 1);
            return descendantCounts[index] = total;
        };
        std::sort(roots.begin(), roots.end(), [this](int a, int b) { return less(a, b); });
        for (auto& branch : children) std::sort(branch.begin(), branch.end(), [this](int a, int b) { return less(a, b); });
        std::function<void(int, int)> add = [&](int index, int depth) {
            if (depth > 24 || !retained[index]) return;
            const int descendants = countDescendants(index, 0);
            const bool hasChildren = descendants > 0;
            display.push_back({ index, depth, descendants, false, hasChildren, {} });
            const bool open = expanded.count(identity(snapshot.processes[index])) > 0 || (!query.empty() && hasChildren);
            if (open) for (int child : children[index]) add(child, depth + 1);
        };
        for (int group = 0; group < (groupByType ? 3 : 1); ++group) {
            std::vector<int> selectedRoots;
            for (int root : roots)
                if (retained[root] && (!categoryFilter || snapshot.processes[root].category == categoryFilter - 1)
                    && (!groupByType || snapshot.processes[root].category == group))
                    selectedRoots.push_back(root);
            if (groupByType && !selectedRoots.empty()) display.push_back({ -1, 0, 0, true, false,
                std::string(categoryName(static_cast<std::uint8_t>(group))) + " (" + std::to_string(selectedRoots.size()) + ")" });
            for (int root : selectedRoots) add(root, 0);
        }
    }
    void select(const Key key, const int displayIndex)
    {
        const auto& io = ImGui::GetIO();
        if (io.KeyShift && lastSelected >= 0) {
            if (!io.KeyCtrl) selected.clear();
            const int begin = std::max(0, std::min(lastSelected, displayIndex));
            const int end = std::min(static_cast<int>(display.size()) - 1, std::max(lastSelected, displayIndex));
            for (int index = begin; index <= end; ++index)
                if (!display[index].groupHeader) selected.insert(identity(snapshot.processes[display[index].index]));
        } else if (io.KeyCtrl) {
            if (!selected.erase(key)) selected.insert(key);
        } else { selected.clear(); selected.insert(key); }
        lastSelected = displayIndex;
    }
    void focusProcess(const ProcessInfo& process)
    {
        categoryFilter = 0;
        groupByType = false;
        treeMode = false;
        std::snprintf(search, sizeof(search), "pid:%d", process.id);
        selected.clear();
        selected.insert(identity(process));
        lastSelected = -1;
        dirty = true;
        switchPage(false);
    }
    void requestDetails(const ProcessInfo& process)
    {
        if (demoMode) {
            detailText = process.name + " · PID " + std::to_string(process.id) + "\nSimulated demo data.";
            detailsOpen = true;
            return;
        }
        detailText = "Loading details...";
        detailsOpen = true;
        bridge.send("D " + std::to_string(nextRequest++) + " " + std::to_string(process.id)
            + " " + std::to_string(process.created) + "\n");
    }
    void askStop()
    {
        if (demoMode) return;
        pendingStop.clear();
        for (const auto& key : selected) if (find(key)) pendingStop.push_back(key);
        stopModal = !pendingStop.empty();
    }
    void processActions(const ProcessInfo& process)
    {
        if (ImGui::MenuItem(selected.size() > 1 ? "End selected tasks..." : "End process...",
                            nullptr, false, !demoMode)) askStop();
        if (ImGui::MenuItem("Advanced details...")) requestDetails(process);
        ImGui::Separator();
        const bool hasPath = !process.path.empty();
        if (ImGui::MenuItem("Open file location", nullptr, false, hasPath)) {
            const auto path = wide(process.path);
            const auto parameter = L"/select,\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", parameter.c_str(), nullptr, SW_SHOWNORMAL);
        }
        if (ImGui::MenuItem("Copy file path", nullptr, false, hasPath)) ImGui::SetClipboardText(process.path.c_str());
        if (ImGui::MenuItem("Copy name and PID")) {
            const auto label = process.name + " · PID " + std::to_string(process.id);
            ImGui::SetClipboardText(label.c_str());
        }
    }
    void statusBar()
    {
        if (cyui::beginStatusBar("##status")) {
            if (demoMode) cyui::statusText(cyui::Tone::info, "Simulated data · Demo");
            else if (paused) cyui::statusText(cyui::Tone::warning, "Updates paused");
            else if (!error.empty()) cyui::statusText(cyui::Tone::error, "%s", error.c_str());
            else if (!statusMessage.empty()) cyui::statusText(cyui::Tone::success, "%s", statusMessage.c_str());
            else if (hasSnapshot) cyui::statusText(cyui::Tone::neutral, "1 s · Sampling %.1f ms · %zu processes", snapshot.collectMs, snapshot.processes.size());
            else cyui::statusText(cyui::Tone::neutral, "Connecting to collector...");
            ImGui::SameLine();
            cyui::alignRight(cyui::dp(235.0f));
            cyui::textFaint("Ctrl+1/2 · Ctrl+F · Ctrl/Shift");
        }
        cyui::endStatusBar();
    }
    void topBar()
    {
        if (cyui::beginTopBar("##top")) {
            const bool compact = ImGui::GetWindowWidth() < cyui::dp(850.0f);
            cyui::heading("CyWinTask", 3);
            if (demoMode && !compact) { ImGui::SameLine(); cyui::badge("DEMO", cyui::Tone::warning); }
            ImGui::SameLine(0.0f, cyui::dp(compact ? 8.0f : 18.0f));
            const char* pages[] { "Processes", "Performance" };
            int page = performancePage ? 1 : 0;
            if (cyui::segmented("##page", pages, 2, &page, cyui::dp(compact ? 190.0f : 250.0f))) switchPage(page == 1);
            ImGui::SameLine();
            cyui::alignRight(cyui::dp(compact ? 175.0f : 245.0f));
            if (compact) {
                if (cyui::button("Text", cyui::Variant::secondary)) ImGui::OpenPopup("##textScale");
                if (ImGui::BeginPopup("##textScale")) {
                    if (cyui::button("A−##smaller", cyui::Variant::secondary)) ui_scale::adjust(-10);
                    ImGui::SameLine();
                    ImGui::Text("%d %%", ui_scale::percent());
                    ImGui::SameLine();
                    if (cyui::button("A+##larger", cyui::Variant::secondary)) ui_scale::adjust(10);
                    ImGui::EndPopup();
                }
            } else {
                if (cyui::button("A−##smaller", cyui::Variant::secondary)) ui_scale::adjust(-10);
                ImGui::SameLine();
                ImGui::Text("%d %%", ui_scale::percent());
                ImGui::SameLine();
                if (cyui::button("A+##larger", cyui::Variant::secondary)) ui_scale::adjust(10);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(demoMode);
            if (cyui::button(paused ? "Resume" : "Pause", cyui::Variant::secondary)) {
                paused = !paused;
                bridge.send(paused ? "P 1\n" : "P 0\n");
            }
            ImGui::EndDisabled();
            cyui::tooltip("Pause or resume sampling");
        }
        cyui::endTopBar();
    }
    void processOverview(const bool withTitle)
    {
        if (cyui::beginToolbar("##processOverview")) {
            const auto& colors = cyui::palette();
            if (withTitle) {
                cyui::heading("Processes", 3);
                cyui::toolbarSeparator();
            }
            cyui::textDim("CPU"); ImGui::SameLine();
            if (std::isfinite(snapshot.cpu)) cyui::textColoured(colors.info, "%.0f %%", snapshot.cpu);
            else cyui::textFaint("—");
            cyui::toolbarSeparator();
            cyui::textDim("RAM"); ImGui::SameLine();
            if (snapshot.memoryTotalGb > 0)
                cyui::textColoured(colors.text, "%.0f %%", snapshot.memoryUsedGb / snapshot.memoryTotalGb * 100.0);
            else cyui::textFaint("—");
            cyui::toolbarSeparator();
            cyui::textDim("Processes"); ImGui::SameLine();
            cyui::textColoured(colors.text, "%zu", snapshot.processes.size());
            if (!selected.empty()) {
                ImGui::SameLine();
                cyui::alignRight(cyui::dp(150.0f));
                cyui::textColoured(colors.accentText, "%zu selected", selected.size());
            }
        }
        cyui::endToolbar();
    }
    void processTable(const float height)
    {
        buildRows();
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter
            | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable
            | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingStretchProp;
        const float innerWidth = std::max(ImGui::GetContentRegionAvail().x, cyui::dp(1210.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
            ImVec2(cyui::dp(5.0f), cyui::dp(compactProcesses ? 1.0f : 3.0f)));
        if (!ImGui::BeginTable("##processes", 9, flags, ImVec2(0, height), innerWidth)) {
            ImGui::PopStyleVar();
            return;
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Process name", ImGuiTableColumnFlags_DefaultSort, 0.31f, 0);
        ImGui::TableSetupColumn("Trust", ImGuiTableColumnFlags_PreferSortDescending, 0.09f, 1);
        ImGui::TableSetupColumn("PID", ImGuiTableColumnFlags_PreferSortAscending, 0.06f, 2);
        ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_PreferSortDescending, 0.09f, 3);
        ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_PreferSortDescending, 0.09f, 4);
        ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_PreferSortDescending, 0.16f, 5);
        ImGui::TableSetupColumn("Threads", ImGuiTableColumnFlags_PreferSortDescending, 0.06f, 6);
        ImGui::TableSetupColumn("Handles", ImGuiTableColumnFlags_PreferSortDescending, 0.07f, 7);
        ImGui::TableSetupColumn("Uptime", ImGuiTableColumnFlags_PreferSortDescending, 0.07f, 8);
        ImGui::TableHeadersRow();
        if (auto* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty && specs->SpecsCount > 0) {
            sortColumn = static_cast<int>(specs->Specs[0].ColumnUserID);
            sortDirection = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending ? -1 : 1;
            specs->SpecsDirty = false;
            dirty = true;
            buildRows();
        }
        const auto& palette = cyui::palette();
        const float rowHeight = cyui::dp(compactProcesses ? 23.0f : 30.0f);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(display.size()), rowHeight);
        while (clipper.Step()) for (int displayIndex = clipper.DisplayStart; displayIndex < clipper.DisplayEnd; ++displayIndex) {
            const auto& row = display[displayIndex];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
            ImGui::TableSetColumnIndex(0);
            if (row.groupHeader) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, palette.bg4);
                cyui::textColoured(palette.accentHover, "%s", row.title.c_str());
                continue;
            }
            const auto& process = snapshot.processes[row.index];
            const Key key = identity(process);
            const bool active = selected.count(key) != 0;
            const auto position = ImGui::GetCursorScreenPos();
            const std::string id = "##row" + std::to_string(process.id) + "_" + std::to_string(process.created);
            if (active)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, palette.selection);
            else if (row.depth > 0 || row.hasChildren)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, row.depth ? palette.bg3 : palette.bg4);
            const bool clicked = ImGui::Selectable(id.c_str(), active,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                ImVec2(0, rowHeight - cyui::dp(compactProcesses ? 2.0f : 6.0f)));
            const auto bounds = ImGui::GetItemRectMax();
            const bool arrowClick = row.hasChildren && ImGui::IsItemHovered()
                && ImGui::GetMousePos().x < position.x + cyui::dp(18.0f + row.depth * 15.0f);
            if (clicked) {
                if (arrowClick) { if (!expanded.erase(key)) expanded.insert(key); dirty = true; }
                else { select(key, displayIndex); if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) requestDetails(process); }
            }
            auto* draw = ImGui::GetWindowDrawList();
            const float textY = position.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
            if (treeMode && (row.depth > 0 || row.hasChildren)) {
                const float rail = position.x + cyui::dp(2.0f);
                draw->AddRectFilled(ImVec2(rail, position.y), ImVec2(rail + cyui::dp(2.0f), position.y + rowHeight),
                    palette.lineStrong);
                if (row.depth == 0) draw->AddLine(position, ImVec2(bounds.x, position.y), palette.lineStrong);
            }
            if (active) {
                draw->AddRect(position, ImVec2(bounds.x, position.y + rowHeight), palette.accent, 0, 0, cyui::dp(2.0f));
                draw->AddRectFilled(position, ImVec2(position.x + cyui::dp(4.0f), position.y + rowHeight), palette.accent);
            }
            float x = position.x + cyui::dp(5.0f + row.depth * 15.0f);
            if (row.hasChildren) {
                draw->AddText(ImVec2(x, textY), palette.accentHover,
                    expanded.count(key) ? "▼" : "▶");
            }
            x += cyui::dp(17.0f);
            if (auto* icon = icons.get(process.path)) {
                const float iconSize = cyui::dp(compactProcesses ? 15.0f : 18.0f);
                const float iconY = position.y + (rowHeight - iconSize) * 0.5f;
                draw->AddImage(ImTextureRef(icon), ImVec2(x, iconY),
                    ImVec2(x + iconSize, iconY + iconSize));
            } else draw->AddText(ImVec2(x, textY), palette.textFaint, cyui::icon::code);
            x += cyui::dp(28.0f);
            const auto available = std::max(0.0f, ImGui::GetCurrentTable()->Columns[0].WidthGiven - (x - position.x) - cyui::dp(8.0f));
            const std::string title = row.hasChildren
                ? process.name + " (" + std::to_string(row.descendants) + ")" : process.name;
            cyui::clippedText(draw, ImVec2(x, textY), available,
                active ? palette.accentText : palette.text, title.c_str());
            if (ImGui::BeginPopupContextItem(id.c_str())) {
                if (!selected.count(key)) { selected.clear(); selected.insert(key); }
                processActions(process);
                ImGui::EndPopup();
            }
            ImGui::TableSetColumnIndex(1); trustBar(trustScores[row.index], cyui::dp(compactProcesses ? 17.0f : 22.0f));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", process.id);
            ImGui::TableSetColumnIndex(3); usageBar(process.cpu, std::isfinite(process.cpu) ? format("%.1f %%", process.cpu) : "—", false,
                compactProcesses ? 18.0f : 22.0f);
            const double gpu = treeMode && row.hasChildren ? process.gpuGroup : process.gpu;
            ImGui::TableSetColumnIndex(4);
            usageBar(gpu, std::isfinite(gpu) ? format("%.1f %%", gpu) : "—", false,
                compactProcesses ? 18.0f : 22.0f);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s · busiest GPU engine%s",
                treeMode && row.hasChildren ? "Group including related processes" : "Single process",
                std::isfinite(gpu) ? "" : " · counter unavailable");
            const double memoryMb = process.workingSet / 1048576.0;
            const double memoryPercent = snapshot.memoryTotalGb > 0 ? memoryMb / (snapshot.memoryTotalGb * 1024.0) * 100.0 : 0.0;
            ImGui::TableSetColumnIndex(5);
            usageBar(memoryPercent, format("%.1f MiB", memoryMb) + " · " + format("%.1f %%", memoryPercent), true,
                compactProcesses ? 18.0f : 22.0f);
            ImGui::TableSetColumnIndex(6); ImGui::Text("%d", process.threads);
            ImGui::TableSetColumnIndex(7); ImGui::Text("%d", process.handles);
            ImGui::TableSetColumnIndex(8); ImGui::TextUnformatted(processAge(process.created, snapshot.unixMs).c_str());
        }
        ImGui::EndTable();
        ImGui::PopStyleVar();
    }
    void processInspector()
    {
        if (cyui::beginCard("##processInspector", nullptr, compactProcesses ? 120.0f : 145.0f)) {
            if (selected.empty()) {
                cyui::heading("Quick details", 3);
                cyui::textDim("Select a process to see its identity, path, and trust score.");
                cyui::textFaint("Local trust score based on the Authenticode signature and file location.");
            } else if (selected.size() > 1) {
                cyui::heading("Multiple selection", 3);
                cyui::textDim("%zu processes selected · right-click for actions.", selected.size());
            } else if (const auto* process = find(*selected.begin())) {
                const auto index = static_cast<std::size_t>(process - snapshot.processes.data());
                const auto& result = trustScores[index];
                cyui::heading(process->name.c_str(), 3);
                ImGui::SameLine(0.0f, cyui::dp(16.0f));
                if (result.score >= 0) cyui::textColoured(result.score >= 80 ? cyui::palette().success
                    : result.score >= 55 ? cyui::palette().info
                    : result.score >= 30 ? cyui::palette().warning : cyui::palette().error,
                    "Trust %d/100 · %s", result.score, result.label);
                else cyui::textFaint("Trust — · %s", result.label);
                cyui::textDim("PID %d · Parent PID %d · %s · %s · CPU %.1f %% · GPU %s · %.1f MiB",
                    process->id, process->parent, categoryName(process->category),
                    processAge(process->created, snapshot.unixMs).c_str(),
                    process->cpu, std::isfinite(process->gpu) ? format("%.1f %%", process->gpu).c_str() : "—",
                    process->workingSet / 1048576.0);
                cyui::textFaint("%s", result.explanation);
                if (!process->path.empty()) {
                    cyui::textDim("File: %s", process->path.c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", process->path.c_str());
                } else cyui::textFaint("File path unavailable.");
            }
        }
        cyui::endCard();
    }
    void processes()
    {
        if (hasSnapshot && !demoMode && ImGui::GetTime() >= nextTrustRefresh) {
            if (refreshTrustScores()) dirty = true;
            nextTrustRefresh = ImGui::GetTime() + 0.75;
        }
        if (compactProcesses) processOverview(true);
        else {
            cyui::panelHeader("Processes", "Search, sort, and inspect running tasks");
            cyui::space(8);
            processOverview(false);
        }
        cyui::space(compactProcesses ? 4.0f : 6.0f);
        if (cyui::beginToolbar("##processToolbar")) {
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) focusSearch = true;
            if (focusSearch) { ImGui::SetKeyboardFocusHere(); focusSearch = false; }
            const float actions = cyui::dp(search[0] ? 325.0f : 255.0f);
            const float width = std::max(cyui::dp(180.0f), ImGui::GetContentRegionAvail().x - actions);
            if (cyui::searchField("##search", search, sizeof(search), "Name, PID, or pid:1234...", width))
                dirty = true;
            cyui::tooltip("Enter pid:1234 to show a specific process.");
            if (search[0]) {
                ImGui::SameLine();
                if (cyui::button("Clear", cyui::Variant::ghost)) { search[0] = 0; dirty = true; }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(selected.size() != 1);
            if (cyui::button("Inspect", cyui::Variant::secondary) && selected.size() == 1)
                if (const auto* process = find(*selected.begin())) requestDetails(*process);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(demoMode || selected.empty());
            if (cyui::dangerButton(selected.size() > 1 ? ("End " + std::to_string(selected.size()) + " tasks").c_str()
                                                       : "End task"))
                askStop();
            ImGui::EndDisabled();
        }
        cyui::endToolbar();
        cyui::space(4);
        const bool narrowFilters = ImGui::GetContentRegionAvail().x < cyui::dp(830.0f);
        const auto filterOptions = [this] {
            if (ImGui::Checkbox("Group by type", &groupByType)) dirty = true;
            ImGui::SameLine(0.0f, cyui::dp(16.0f));
            if (ImGui::Checkbox("Related processes", &treeMode)) dirty = true;
            ImGui::SameLine(0.0f, cyui::dp(16.0f));
            ImGui::Checkbox("Compact", &compactProcesses);
            ImGui::SameLine(0.0f, cyui::dp(16.0f));
            ImGui::Checkbox("Inspection", &showProcessInspector);
        };
        if (cyui::beginToolbar("##processFilters")) {
            cyui::textDim("Show"); ImGui::SameLine();
            ImGui::SetNextItemWidth(cyui::dp(220.0f));
            const char* filters[] { "All processes", "Applications", "Background", "Windows processes" };
            if (ImGui::Combo("##categoryFilter", &categoryFilter, filters, 4)) dirty = true;
            if (!narrowFilters) { ImGui::SameLine(0.0f, cyui::dp(16.0f)); filterOptions(); }
        }
        cyui::endToolbar();
        if (narrowFilters) {
            if (cyui::beginToolbar("##processFilterOptions")) filterOptions();
            cyui::endToolbar();
        }
        cyui::space(4);
        if (hasSnapshot) buildRows();
        const float available = ImGui::GetContentRegionAvail().y;
        const bool inspectorVisible = showProcessInspector && available >= cyui::dp(340.0f);
        const float inspectorHeight = cyui::dp(compactProcesses ? 120.0f : 145.0f);
        const float tableHeight = inspectorVisible ? std::max(cyui::dp(140.0f), available - inspectorHeight - cyui::dp(8.0f)) : available;
        if (!hasSnapshot) cyui::emptyState("Waiting for process data", "Starting the collector.", cyui::icon::refresh);
        else if (display.empty()) cyui::emptyState("No processes found", "Change the search or filter.", cyui::icon::search);
        else processTable(tableHeight);
        if (inspectorVisible) { cyui::space(4); processInspector(); }
    }
    ImU32 seriesColor(const DeviceInfo& device, const MetricInfo& value) const
    {
        return cyui::categorical(seriesIndex(device, value));
    }
    int seriesIndex(const DeviceInfo& device, const MetricInfo& value) const
    {
        if (device.kind == "memory") return value.id == "used" ? 3 : value.id == "available" ? 4 : 5;
        if (device.kind == "disk") return value.id == "active" ? 6 : value.id == "read" ? 7 : 8;
        if (device.kind == "network") return value.id == "receive" ? 9 : 10;
        if (device.kind == "gpu") return 11;
        return 0;
    }
    void chart(const DeviceInfo& device, const std::vector<const MetricInfo*>& series, const float height)
    {
        const bool disk = device.kind == "disk" && series.size() > 1;
        const double now = snapshot.unixMs / 1000.0;
        const auto maximumFor = [&](const MetricInfo* value) {
            if (!value) return 1.0;
            if (std::isfinite(value->maximum) && value->maximum > 0) return value->maximum;
            double maximum = std::isfinite(value->value) ? value->value : 0.0;
            if (const auto found = histories.find(device.id + "/" + value->id); found != histories.end())
                for (const auto& point : found->second)
                    if (std::isfinite(point.value) && now - point.time <= 60.0)
                        maximum = std::max(maximum, point.value);
            return std::max(1.0, maximum * 1.15);
        };
        double leftMaximum = disk ? 100.0 : maximumFor(series.empty() ? nullptr : series.front());
        if (!disk)
            for (std::size_t i = 1; i < series.size(); ++i)
                leftMaximum = std::max(leftMaximum, maximumFor(series[i]));
        std::vector<cyui::PlotSeries> lines;
        lines.reserve(series.size());
        for (const auto* value : series) {
            if (!value) continue;
            const auto found = plotPoints.find(device.id + "/" + value->id);
            if (found == plotPoints.end()) continue;
            cyui::PlotSeries line;
            line.id = static_cast<cyui::ItemId>(lines.size() + 1);
            line.label = value->label.c_str();
            line.points = cyui::ArrayView<cyui::Point>(found->second);
            line.colorIndex = seriesIndex(device, *value);
            lines.push_back(line);
        }
        cyui::PlotState state;
        state.x.range = {now - 60.0, now};
        state.y.range = {0.0, leftMaximum};
        cyui::PlotOptions options;
        options.navigation = false;
        options.legend = false;
        options.crosshair = false;
        options.axes = false;
        const std::string unit = series.empty() || !series.front() ? "" : series.front()->unit;
        const auto axisLabel = [&unit](double value) {
            return format(value < 10.0 ? "%.1f" : "%.0f", value) + (unit.empty() ? "" : " " + unit);
        };
        const float fontSize = std::max(cyui::dp(10.0f), ImGui::GetFontSize() * 0.78f);
        auto* font = ImGui::GetFont();
        const auto labelWidth = [font, fontSize](const std::string& text) {
            return font->CalcTextSizeA(fontSize, 10000.0f, 0.0f, text.c_str()).x;
        };
        const float leftMargin = std::max(cyui::dp(37.0f), labelWidth(axisLabel(leftMaximum)) + cyui::dp(7.0f));
        const double rateMaximum = disk && diskRateScale.count(device.id) ? diskRateScale.at(device.id) : 1.0;
        const float rightMargin = disk ? labelWidth(format("%.0f", rateMaximum) + " MiB/s") + cyui::dp(9.0f) : 0.0f;
        const float available = ImGui::GetContentRegionAvail().x;
        const float width = std::max(cyui::dp(50.0f), available - leftMargin - rightMargin);
        const float bottomMargin = std::max(cyui::dp(17.0f), fontSize + cyui::dp(4.0f));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(start.x + leftMargin, start.y));
        const std::string id = "##plot_" + device.id + "_" + (series.empty() || !series.front() ? "empty" : series.front()->id);
        const auto result = cyui::plot(id.c_str(), state, cyui::ArrayView<cyui::PlotSeries>(lines),
            ImVec2(width, std::max(cyui::dp(42.0f), cyui::dp(height) - bottomMargin)), options);
        const ImVec2 plotMin = ImGui::GetItemRectMin();
        const ImVec2 plotMax = ImGui::GetItemRectMax();
        auto* draw = ImGui::GetWindowDrawList();
        for (int step = 0; step <= 4; ++step) {
            const float y = plotMax.y - (plotMax.y - plotMin.y) * step / 4.0f;
            const auto left = axisLabel(leftMaximum * step / 4.0);
            draw->AddText(font, fontSize, ImVec2(plotMin.x - cyui::dp(5.0f) - labelWidth(left),
                std::clamp(y - fontSize * 0.5f, plotMin.y, plotMax.y - fontSize)),
                cyui::palette().textDim, left.c_str());
            if (disk) {
                const auto label = format("%.0f", rateMaximum * step / 4.0) + " MiB/s";
                draw->AddText(font, fontSize,
                    ImVec2(plotMax.x + cyui::dp(3.0f),
                        std::clamp(y - fontSize * 0.5f, plotMin.y, plotMax.y - fontSize)),
                    cyui::palette().textDim, label.c_str());
            }
        }
        for (int step = 0; step <= 2; ++step) {
            const auto label = std::to_string(-60 + step * 30) + " s";
            const float x = plotMin.x + (plotMax.x - plotMin.x) * step / 2.0f;
            draw->AddText(font, fontSize,
                ImVec2(std::clamp(x - labelWidth(label) * 0.5f, plotMin.x,
                    std::max(plotMin.x, plotMax.x - labelWidth(label))), plotMax.y + cyui::dp(2.0f)),
                cyui::palette().textDim, label.c_str());
        }
        if (result.hovered) {
            const double secondsAgo = std::clamp(now - result.mouse.x, 0.0, 60.0);
            draw->AddLine(ImVec2(ImGui::GetMousePos().x, plotMin.y),
                ImVec2(ImGui::GetMousePos().x, plotMax.y), cyui::palette().textDim, cyui::dp(1.0f));
            ImGui::BeginTooltip();
            ImGui::Text("Il y a %.0f s", secondsAgo);
            for (const auto* value : series) {
                if (!value) continue;
                const auto found = histories.find(device.id + "/" + value->id);
                if (found == histories.end()) continue;
                const PlotPoint* closest = nullptr;
                double distance = 2.5;
                for (const auto& point : found->second)
                    if (std::isfinite(point.value) && std::abs(point.time - result.mouse.x) < distance) {
                        closest = &point;
                        distance = std::abs(point.time - result.mouse.x);
                    }
                if (closest) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(cyui::categorical(seriesIndex(device, *value))),
                    "%s : %.1f %s", value->label.c_str(), closest->value, value->unit.c_str());
            }
            ImGui::EndTooltip();
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, plotMax.y));
        ImGui::Dummy(ImVec2(available, bottomMargin));
    }
    void chartMetric(const DeviceInfo& device, const MetricInfo& value, const float height)
    {
        ImGui::TextUnformatted(value.label.c_str());
        ImGui::SameLine();
        if (std::isfinite(value.value)) cyui::textColoured(seriesColor(device, value), "%.1f %s", value.value, value.unit.c_str());
        else cyui::textFaint("Unavailable");
        chart(device, { &value }, height);
    }
    void resourceDetails(const DeviceInfo& device)
    {
        cyui::heading(device.title.c_str(), 2);
        cyui::textDim("%s", device.description.c_str());
        if (const auto* temperature = metric(device, "temperature")) {
            if (std::isfinite(temperature->value)) cyui::badge((format("%.0f", temperature->value) + " °C").c_str(), cyui::Tone::neutral);
            else cyui::textFaint("Temperature unavailable");
        }
        cyui::space(12);
        if (device.kind == "cpu") {
            if (ImGui::Checkbox("Show logical processors", &showCores)) {}
            if (showCores) {
                const char* modes[] { "Compact (Windows)", "Detailed" };
                int mode = compactCores ? 0 : 1;
                if (cyui::segmented("##cpuMode", modes, 2, &mode, cyui::dp(340.0f))) compactCores = mode == 0;
            }
            if (const auto* total = metric(device, "cpu")) {
                if (!showCores || !compactCores) chartMetric(device, *total, 165);
                else cyui::textDim("Total usage : %.1f %%", total->value);
            }
            if (showCores) {
                std::vector<const MetricInfo*> cores;
                for (const auto& value : device.metrics) if (value.id.rfind("core:", 0) == 0) cores.push_back(&value);
                const int columns = compactCores ? 4 : 2;
                if (ImGui::BeginTable("##cores", columns, ImGuiTableFlags_SizingStretchSame))
                {
                    for (const auto* core : cores) {
                        ImGui::TableNextColumn();
                        chartMetric(device, *core, compactCores ? 70.0f : 110.0f);
                    }
                    ImGui::EndTable();
                }
            }
        } else if (device.kind == "disk") {
            const std::vector<const MetricInfo*> series { metric(device, "active"), metric(device, "read"), metric(device, "write") };
            for (const auto* value : series)
                if (value) { cyui::textColoured(seriesColor(device, *value), "● %s : %.1f %s", value->label.c_str(), value->value, value->unit.c_str()); ImGui::SameLine(); }
            ImGui::NewLine();
            chart(device, series, 340.0f);
        } else {
            for (const auto& value : device.metrics) {
                if (value.id == "temperature") continue;
                chartMetric(device, value, device.kind == "gpu" ? 110.0f : 155.0f);
                cyui::space(8);
            }
        }
    }
    void dashboardCard(const DeviceInfo& device, const bool single)
    {
        const auto* primary = metric(device, device.kind == "cpu" ? "cpu" : device.kind == "memory" ? "used"
            : device.kind == "disk" ? "active" : device.kind == "network" ? "receive" : "usage");
        if (!primary) return;
        std::vector<const MetricInfo*> series { primary };
        if (device.kind == "memory") series.push_back(metric(device, "commit"));
        else if (device.kind == "disk") {
            series.push_back(metric(device, "read"));
            series.push_back(metric(device, "write"));
        } else if (device.kind == "network") series.push_back(metric(device, "send"));
        const float chartHeight = single ? (compactGraphCards ? 155.0f : 225.0f)
            : (compactGraphCards ? 86.0f : 170.0f);
        const float cardHeight = single ? (compactGraphCards ? 265.0f : 335.0f)
            : (compactGraphCards ? 195.0f : 280.0f);
        const std::string id = "##graphCard_" + device.id;
        if (cyui::beginCard(id.c_str(), nullptr, cardHeight)) {
            cyui::heading(device.kind == "memory" ? "Memory" : device.title.c_str(), 3);
            if (!single) {
                ImGui::SameLine();
                cyui::alignRight(cyui::dp(28.0f));
                if (cyui::iconButton(cyui::icon::newWindow, "Open this graph in a separate window",
                    false, 0, cyui::dp(26.0f, 24.0f))) {
                    detached[device.id] = true;
                    if (!demoMode) bridge.send("M 1\n");
                }
            }
            {
                cyui::ScopedFont font(cyui::boldFont(), 1.08f);
                if (std::isfinite(primary->value))
                    cyui::textColoured(seriesColor(device, *primary), "%.1f %s", primary->value, primary->unit.c_str());
                else cyui::textFaint("Measurement unavailable");
            }
            {
                cyui::ScopedFont font(nullptr, 0.88f);
                if (series.size() > 1) {
                    for (std::size_t i = 0; i < series.size(); ++i) {
                        const auto* value = series[i];
                        if (!value) continue;
                        const char* shortName = device.kind == "memory" ? (i == 0 ? "RAM" : "Commit")
                            : device.kind == "disk" ? (i == 0 ? "Active" : i == 1 ? "L" : "E")
                            : (i == 0 ? "R" : "E");
                        if (i > 0) ImGui::SameLine(0.0f, cyui::dp(8.0f));
                        if (std::isfinite(value->value))
                            cyui::textColoured(seriesColor(device, *value),
                                device.kind == "disk" ? "%s %.0f" : "%s %.1f", shortName, value->value);
                        else cyui::textFaint("%s —", shortName);
                    }
                } else {
                    std::string detail;
                    if (const auto* temperature = metric(device, "temperature"); temperature && std::isfinite(temperature->value))
                        detail = "Temperature " + format("%.0f", temperature->value) + " °C";
                    if (const auto* dedicated = metric(device, "dedicated"); dedicated && std::isfinite(dedicated->value))
                        detail += (detail.empty() ? "" : "  ·  ") + std::string("VRAM ") + format("%.1f", dedicated->value) + " GiB";
                    if (detail.empty()) detail = "Last 60 seconds";
                    cyui::textDim("%s", detail.c_str());
                }
            }
            cyui::space(3.0f);
            chart(device, series, chartHeight);
        }
        cyui::endCard();
    }
    void topProcessCard(const char* id, const char* title, const std::vector<int>& ranking, const bool byCpu)
    {
        if (cyui::beginCard(id, nullptr, 230.0f)) {
            cyui::heading(title, 3);
            if (ranking.empty()) cyui::textFaint("No process data");
            else if (ImGui::BeginTable((std::string("##ranking_") + id).c_str(), 2,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
                ImGui::TableSetupColumn("Processes", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, cyui::dp(105.0f));
                for (const int index : ranking) {
                    const auto& process = snapshot.processes[index];
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, cyui::dp(27.0f));
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushID(process.id);
                    if (ImGui::Selectable(process.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns,
                        ImVec2(0.0f, cyui::dp(25.0f)))) focusProcess(process);
                    if (ImGui::IsItemHovered()) cyui::tooltip("Show this process in the list");
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(1);
                    if (byCpu && std::isfinite(process.cpu))
                        cyui::textColoured(heat(process.cpu, false), "%.1f %%", process.cpu);
                    else if (byCpu) cyui::textFaint("—");
                    else cyui::textColoured(cyui::palette().text, "%.0f MiB", process.workingSet / 1048576.0);
                }
                ImGui::EndTable();
            }
        }
        cyui::endCard();
    }
    void topProcesses()
    {
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = available >= cyui::dp(690.0f) ? 2 : 1;
        if (ImGui::BeginTable("##topProcesses", columns,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableNextColumn();
            topProcessCard("##topCpu", "Processes · CPU", topCpu, true);
            ImGui::TableNextColumn();
            topProcessCard("##topMemory", "Processes · Memory", topMemory, false);
            ImGui::EndTable();
        }
    }
    void dashboard(const std::string& only, const bool withTopProcesses = false)
    {
        std::vector<const DeviceInfo*> visible;
        for (const char* kind : { "cpu", "memory", "gpu", "disk", "network" }) {
            if (only.empty() && !graphExtraDevices && std::strcmp(kind, "disk") == 0) continue;
            if (only.empty() && !graphExtraDevices && std::strcmp(kind, "network") == 0) continue;
            for (const auto& device : snapshot.devices)
                if (device.kind == kind && (only.empty() || device.id == only)) visible.push_back(&device);
        }
        if (visible.empty()) {
            cyui::emptyState("Waiting for measurements", "Graphs appear after sampling starts.", cyui::icon::refresh);
            return;
        }
        const float available = ImGui::GetContentRegionAvail().x;
        const float gap = ImGui::GetStyle().CellPadding.x * 2.0f;
        const int automatic = std::max(1, static_cast<int>((available + gap)
            / (cyui::dp(compactGraphCards ? 240.0f : 380.0f) + gap)));
        const int safe = std::max(1, static_cast<int>((available + gap) / (cyui::dp(225.0f) + gap)));
        const int requested = only.empty() ? (graphColumnsMode ? graphColumnsMode : automatic) : 1;
        const int columns = std::clamp(std::min(requested, safe), 1, std::min(5, static_cast<int>(visible.size())));
        if (ImGui::BeginTable(("##dashboardGrid" + only).c_str(), columns,
            ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_NoSavedSettings)) {
            for (const auto* device : visible) {
                ImGui::TableNextColumn();
                dashboardCard(*device, !only.empty());
            }
            ImGui::EndTable();
        }
        if (withTopProcesses) {
            cyui::space(10.0f);
            topProcesses();
        }
    }
    std::string resourceSummary(const DeviceInfo& device) const
    {
        const auto value = [&](const char* id) {
            const auto* current = metric(device, id);
            return current && std::isfinite(current->value) ? current->value : NAN;
        };
        const double temperature = value("temperature");
        if (device.kind == "cpu" || device.kind == "gpu") {
            const double usage = value(device.kind == "cpu" ? "cpu" : "usage");
            if (!std::isfinite(usage)) return "Measurement unavailable";
            std::string summary = format("%.1f", usage) + " %";
            if (std::isfinite(temperature)) summary += "  ·  " + format("%.0f", temperature) + " °C";
            return summary;
        }
        if (device.kind == "memory") {
            const auto* used = metric(device, "used");
            if (!used || !std::isfinite(used->value)) return "Measurement unavailable";
            std::string summary = format("%.1f", used->value) + " GiB";
            if (std::isfinite(used->maximum) && used->maximum > 0)
                summary += " / " + format("%.1f", used->maximum) + " GiB";
            return summary;
        }
        if (device.kind == "disk") {
            const double active = value("active"), read = value("read"), write = value("write");
            std::string summary = std::isfinite(active) ? format("%.0f", active) + " %" : "Activity unavailable";
            if (std::isfinite(read) && std::isfinite(write))
                summary += "  ·  L " + format("%.1f", read) + " / E " + format("%.1f", write) + " MiB/s";
            return summary;
        }
        if (device.kind == "network") {
            const double receive = value("receive"), send = value("send");
            if (std::isfinite(receive) && std::isfinite(send))
                return "R " + format("%.1f", receive) + " / E " + format("%.1f", send) + " Mbps";
        }
        return "Measurement unavailable";
    }
    void performance()
    {
        cyui::panelHeader("Performance", "Live readings · 60 seconds of history · hover over a graph for values");
        cyui::space(5);
        if (cyui::beginToolbar("##performanceToolbar")) {
            const char* views[] { "Graph view", "Detailed view" };
            int view = graphMode ? 0 : 1;
            if (cyui::segmented("##performanceView", views, 2, &view, cyui::dp(275.0f))) graphMode = view == 0;
            ImGui::SameLine();
            if (graphMode) {
                if (cyui::button("Layout", cyui::Variant::secondary)) ImGui::OpenPopup("##graphLayout");
                if (ImGui::BeginPopup("##graphLayout")) {
                    if (ImGui::MenuItem("Automatic columns", nullptr, graphColumnsMode == 0)) graphColumnsMode = 0;
                    if (ImGui::MenuItem("2 columns", nullptr, graphColumnsMode == 2)) graphColumnsMode = 2;
                    if (ImGui::MenuItem("3 columns", nullptr, graphColumnsMode == 3)) graphColumnsMode = 3;
                    if (ImGui::MenuItem("4 columns", nullptr, graphColumnsMode == 4)) graphColumnsMode = 4;
                    if (ImGui::MenuItem("5 columns", nullptr, graphColumnsMode == 5)) graphColumnsMode = 5;
                    ImGui::Separator();
                    ImGui::MenuItem("Compact cards", nullptr, &compactGraphCards);
                    ImGui::MenuItem("Show disks and network", nullptr, &graphExtraDevices);
                    ImGui::EndPopup();
                }
            } else {
                cyui::textDim("Select a resource from the list on the left");
            }
            ImGui::SameLine();
            cyui::alignRight(cyui::dp(145.0f));
            if (cyui::button("Separate window", cyui::Variant::secondary)) {
                detached["all"] = true;
                if (!demoMode) bridge.send("M 1\n");
            }
        }
        cyui::endToolbar();
        cyui::space(6);
        if (!hasSnapshot || snapshot.devices.empty()) {
            cyui::emptyState("Waiting for measurements", "Waiting for the first sample.", cyui::icon::refresh);
            return;
        }
        if (graphMode) {
            ImGui::BeginChild("##dashboardScroll", ImVec2(0, 0), ImGuiChildFlags_None);
            dashboard("", true);
            ImGui::EndChild();
            return;
        }
        ImGui::BeginChild("##resources", ImVec2(cyui::dp(245.0f), 0), ImGuiChildFlags_Borders);
        for (const auto& device : snapshot.devices) {
            const std::string label = "##resource_" + device.id;
            const bool active = activeDevice == device.id;
            if (ImGui::Selectable(label.c_str(), active, 0, ImVec2(0, cyui::dp(47.0f))))
                activeDevice = device.id;
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const auto& colors = cyui::palette();
            auto* draw = ImGui::GetWindowDrawList();
            if (active) draw->AddRectFilled(itemMin, ImVec2(itemMin.x + cyui::dp(3.0f), itemMax.y), colors.accent);
            const float left = itemMin.x + cyui::dp(10.0f);
            const float available = std::max(1.0f, itemMax.x - left - cyui::dp(6.0f));
            cyui::clippedText(draw, ImVec2(left, itemMin.y + cyui::dp(5.0f)), available,
                active ? colors.accentText : colors.text, device.title.c_str());
            const auto summary = resourceSummary(device);
            const float summarySize = ImGui::GetFontSize() * 0.82f;
            draw->PushClipRect(ImVec2(left, itemMin.y), ImVec2(itemMax.x - cyui::dp(5.0f), itemMax.y), true);
            draw->AddText(ImGui::GetFont(), summarySize, ImVec2(left, itemMin.y + cyui::dp(26.0f)),
                active ? colors.accentText : colors.textDim, summary.c_str());
            draw->PopClipRect();
            if (ImGui::IsItemHovered()) cyui::tooltip(device.description.c_str());
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##resourceDetails", ImVec2(0, 0), ImGuiChildFlags_None);
        const DeviceInfo* chosen {};
        for (const auto& device : snapshot.devices) if (device.id == activeDevice) { chosen = &device; break; }
        if (!chosen && !snapshot.devices.empty()) { chosen = &snapshot.devices.front(); activeDevice = chosen->id; }
        if (chosen) resourceDetails(*chosen);
        ImGui::EndChild();
    }
    void dialogs()
    {
        if (cyui::beginModal("End tasks?###stopModal", &stopModal, 430.0f)) {
            cyui::callout(cyui::Tone::warning, "Unsaved changes may be lost.");
            cyui::space(8);
            cyui::textDim("%zu processes selected", pendingStop.size());
            for (std::size_t i = 0; i < std::min<std::size_t>(8, pendingStop.size()); ++i)
                if (const auto* process = find(pendingStop[i])) ImGui::BulletText("%s · PID %d", process->name.c_str(), process->id);
            if (pendingStop.size() > 8) cyui::textFaint("... and %zu more", pendingStop.size() - 8);
            const int action = cyui::modalButtons("End", "Cancel", true);
            if (action == 1) {
                for (const auto& key : pendingStop)
                    bridge.send("E " + std::to_string(nextRequest++) + " " + std::to_string(key.id)
                        + " " + std::to_string(key.created) + "\n");
                stopModal = false;
            } else if (action == 2) stopModal = false;
            cyui::endModal();
        }
        if (detailsOpen) {
            ImGui::SetNextWindowSize(cyui::dp(780.0f, 580.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Advanced details###advanced", &detailsOpen)) {
                if (cyui::button("Copy all", cyui::Variant::secondary)) ImGui::SetClipboardText(detailText.c_str());
                ImGui::Separator();
                ImGui::BeginChild("##report");
                ImGui::TextUnformatted(detailText.c_str());
                ImGui::EndChild();
            }
            ImGui::End();
        }
    }
    void detachedWindows()
    {
        bool anyOpen {};
        for (auto& [id, open] : detached) {
            if (!open) continue;
            const auto title = id == "all" ? "CPU, RAM, and GPU graphs###detached_all" : "Graph " + id + "###detached_" + id;
            const auto* main = ImGui::GetMainViewport();
            ImGui::SetNextWindowSize(cyui::dp(id == "all" ? 970.0f : 650.0f, id == "all" ? 650.0f : 420.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x - cyui::dp(60.0f), main->Pos.y + cyui::dp(55.0f)), ImGuiCond_FirstUseEver);
            if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoDocking)) {
                ImGui::BeginChild(("##popoutScroll" + id).c_str(), ImVec2(0, 0));
                dashboard(id == "all" ? "" : id);
                ImGui::EndChild();
            }
            ImGui::End();
            anyOpen |= open;
        }
        if (!anyOpen && !performancePage && !detached.empty()) bridge.send("M 0\n");
        for (auto it = detached.begin(); it != detached.end();)
            it = it->second ? std::next(it) : detached.erase(it);
    }
};

AppUi::AppUi(ID3D11Device* device, const bool demoMode, const bool performanceAtStart)
    : state_(new State(device, demoMode, performanceAtStart)) {}
AppUi::~AppUi() { delete state_; }
void AppUi::frame()
{
    auto& state = *state_;
    const auto& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) ui_scale::adjust(10);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) ui_scale::adjust(-10);
        if (ImGui::IsKeyPressed(ImGuiKey_0)) ui_scale::adjust(115 - ui_scale::percent());
        if (ImGui::IsKeyPressed(ImGuiKey_1)) state.switchPage(false);
        if (ImGui::IsKeyPressed(ImGuiKey_2)) state.switchPage(true);
    }
    state.icons.beginFrame();
    state.receive();
    state.topBar();
    state.statusBar();
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##main", nullptr, flags))
        state.performancePage ? state.performance() : state.processes();
    ImGui::End();
    state.dialogs();
    state.detachedWindows();
}
