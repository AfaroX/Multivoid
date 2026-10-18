// ui/media_panel.cpp -- see ui/media_panel.h.

#include "ui/media_panel.h"

#include "coop/media/media_pool.h"
#include "coop/text/utf8_codec.h"
#include "l10n/l10n.h"

#include <cstdio>
#include <string>

#include "imgui.h"

namespace ui::media_panel {
namespace {

namespace MP = coop::media_pool;

std::string U8(const std::wstring& w) { return coop::text::ToUtf8(w); }

std::string Size(uint64_t b) {
    char buf[32];
    if (b >= (1ull << 30)) std::snprintf(buf, sizeof(buf), "%.1f GB", static_cast<double>(b) / (1ull << 30));
    else if (b >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(b) / (1ull << 20));
    else std::snprintf(buf, sizeof(buf), "%.0f KB", static_cast<double>(b) / 1024.0);
    return buf;
}

int g_kind = 0;   // the kind the "your files" list shows

}  // namespace

void Render() {
    MP::NotePanelOpen();
    const MP::Snapshot s = MP::GetSnapshot();

    ImGui::TextWrapped("%s", l10n::T("Share your own pictures, posters, radio tracks and TV videos with the other "
                                     "players. A shared file is copied into every player's game folder under the "
                                     "same name, so picture frames, posters, the radio and the TVs show it for "
                                     "everyone."));
    if (s.folder.empty()) {
        ImGui::TextDisabled("%s", l10n::T("Waiting for the game's media folder (it is known once a world is loaded)..."));
        return;
    }
    ImGui::TextDisabled("%s", U8(s.folder).c_str());
    if (s.importing) {
        ImGui::BeginDisabled();
        ImGui::Button(l10n::Label(l10n::T("Choosing files..."), "media_add"));
        ImGui::EndDisabled();
    } else if (ImGui::Button(l10n::Label(l10n::T("Add files..."), "media_add"))) {
        MP::RequestImport(static_cast<MP::Kind>(g_kind));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", l10n::T("Pick pictures, tracks or videos anywhere on this PC. They are copied into your "
                                        "media folder and, in a session, shared. A picture goes to the kind selected "
                                        "below."));
    ImGui::SameLine();
    if (ImGui::Button(l10n::Label(l10n::T("Refresh"), "media_refresh"))) MP::RequestRescan();
    if (!s.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", U8(s.status).c_str()); }
    if (!s.session) ImGui::TextDisabled("%s", l10n::T("Host or join a session to share your files."));
    ImGui::Separator();

    // ---- this player's own files ----
    ImGui::TextUnformatted(l10n::T("Your files"));
    for (int k = 0; k < static_cast<int>(MP::Kind::Count); ++k) {
        if (k) ImGui::SameLine();
        ImGui::PushID(k);
        if (ImGui::RadioButton(l10n::T(MP::KindLabel(static_cast<MP::Kind>(k))), g_kind == k)) g_kind = k;
        ImGui::PopID();
    }
    if (s.session) {
        if (ImGui::SmallButton(l10n::Label(l10n::T("Share all of this kind"), "share_all")))
            MP::RequestShareAll(static_cast<MP::Kind>(g_kind));
    }
    if (ImGui::BeginChild("media_local", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 8), true)) {
        int shown = 0;
        for (const MP::LocalFile& f : s.local) {
            if (static_cast<int>(f.kind) != g_kind) continue;
            ++shown;
            ImGui::PushID(U8(f.name).c_str());
            if (f.pooled) {
                ImGui::TextDisabled("%s", l10n::T("shared"));
            } else if (!s.session) {
                ImGui::TextDisabled("%s", l10n::T("yours"));
            } else if (ImGui::SmallButton(l10n::Label(l10n::T("Share"), "share"))) {
                MP::RequestShare(f.kind, f.name);
            }
            ImGui::SameLine();
            ImGui::Text("%s  (%s)", U8(f.name).c_str(), Size(f.size).c_str());
            ImGui::PopID();
        }
        if (!shown) {
            char line[512];
            l10n::Fmt(line, sizeof(line), l10n::T("No files here yet. Put them in %s"),
                      U8(s.folder + L"\\" + MP::KindFolder(static_cast<MP::Kind>(g_kind))).c_str());
            ImGui::TextDisabled("%s", line);
        }
    }
    ImGui::EndChild();

    if (!s.session) return;

    // ---- the pool ----
    char head[128];
    l10n::Fmt(head, sizeof(head), l10n::T("Shared with everyone (%d)"), static_cast<int>(s.pool.size()));
    ImGui::TextUnformatted(head);
    if (ImGui::BeginTable("media_pool", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 10))) {
        ImGui::TableSetupColumn(l10n::T("File"));
        ImGui::TableSetupColumn(l10n::T("Kind"));
        ImGui::TableSetupColumn(l10n::T("Size"));
        ImGui::TableSetupColumn(l10n::T("Here"));
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const MP::PoolRow& r : s.pool) {
            ImGui::PushID(static_cast<int>(r.hash & 0x7FFFFFFF));
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(U8(r.name).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(l10n::T(MP::KindLabel(r.kind)));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Size(r.size).c_str());
            ImGui::TableNextColumn();
            switch (r.here) {
            case MP::PoolRow::Here::Have:     ImGui::TextUnformatted(l10n::T("ready")); break;
            case MP::PoolRow::Here::Fetching: ImGui::ProgressBar(r.progress, ImVec2(-1, 0)); break;
            case MP::PoolRow::Here::Conflict: ImGui::TextUnformatted(l10n::T("name taken by another file")); break;
            case MP::PoolRow::Here::Failed:   ImGui::TextUnformatted(l10n::T("failed")); break;
            case MP::PoolRow::Here::Missing:
                ImGui::TextUnformatted(r.ready ? l10n::T("waiting") : l10n::T("uploading to the host")); break;
            }
            ImGui::TableNextColumn();
            if ((s.host || r.owner != 0) && ImGui::SmallButton(l10n::Label(l10n::T("Remove"), "unshare")))
                MP::RequestUnshare(r.hash);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", l10n::T("A file you play on a TV or the radio, or hang in a frame, is shared by itself. "
                                      "Removing a file from the pool keeps every copy already made."));
}

}  // namespace ui::media_panel
