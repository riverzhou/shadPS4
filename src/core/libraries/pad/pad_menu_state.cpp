// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/libraries/pad/pad_menu_state.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "common/logging/log.h"
#include "common/path_util.h"

namespace Libraries::Pad {

namespace {

// Gran Turismo 7 writes a UI log of its own menu system (source-located lines such as
// "gt7/menu/app/startup_setting/service.swift:86: StartUpSetting.App onLoad"). That log is the
// only reliable description of which screen is on display: the title exposes no API for it, and
// the boot timing varies by tens of seconds between a cold and a warm start. Follow the log and
// track the project transitions that matter for input automation.
//
// This is deliberately fail-safe: anything unexpected (log missing, marker order uncertain)
// leaves the state at Unknown, and the caller then injects confirm-only input. A wrong direction
// press on a list screen changes the selection, which is exactly what this avoids.
constexpr std::string_view MARKER_SETTINGS_START = "StartUpSetting.App onLoad";
constexpr std::string_view MARKER_SETTINGS_END = "StartUpSetting.App onUnload";
constexpr std::string_view MARKER_SPLASH = "SplashRootEvent onInitialize";
// The title never logs "StartUpSetting.App onUnload" when the wizard completes: it jumps straight
// to the next screen, so the App stays "onLoad" forever and the settings branch kept injecting
// direction presses into the menus behind it. Each finished wizard page logs a finalize line, and
// two of them mean the wizard is over.
constexpr std::string_view MARKER_SETTINGS_PAGE = "SDRSettingRootEvent onFinalize";
constexpr std::string_view MARKER_MOVIE_START = "MovieFaceEvent onInitialize";
constexpr std::string_view MARKER_MOVIE_END = "MovieFaceEvent::onMovieFinish";
constexpr std::string_view UI_LOG_RELATIVE = "APP_DATA/logs/archived.log";
constexpr auto POLL_INTERVAL = std::chrono::milliseconds(100);

std::atomic<bool> g_started{false};
std::atomic<bool> g_settings_visible{false};
std::atomic<bool> g_movie_visible{false};

std::filesystem::path FindUiLog() {
    std::error_code ec;
    const auto download = Common::FS::GetUserPath(Common::FS::PathType::DownloadDir);
    if (download.empty()) {
        return {};
    }
    // Several title ids can live under the download dir (e.g. CUSA24767 and CUSA24769); follow
    // the log that is actually being written, i.e. the most recently modified one.
    std::filesystem::path best;
    std::filesystem::file_time_type best_time{};
    for (const auto& entry : std::filesystem::directory_iterator(download, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const auto candidate = entry.path() / UI_LOG_RELATIVE;
        if (!std::filesystem::exists(candidate, ec)) {
            continue;
        }
        const auto time = std::filesystem::last_write_time(candidate, ec);
        if (ec) {
            continue;
        }
        if (best.empty() || time > best_time) {
            best = candidate;
            best_time = time;
        }
    }
    return best;
}

void ApplyChunk(std::string& window, std::string_view chunk) {
    window.append(chunk);
    constexpr std::size_t KEEP = 64 * 1024;
    if (window.size() > KEEP) {
        window.erase(0, window.size() - KEEP);
    }
    const auto movie_start = window.rfind(MARKER_MOVIE_START);
    const auto movie_end = window.rfind(MARKER_MOVIE_END);
    if (movie_start != std::string::npos) {
        const bool movie = movie_end == std::string::npos || movie_start > movie_end;
        if (g_movie_visible.exchange(movie) != movie) {
            LOG_INFO(Lib_Pad, "menu state: movie={}", movie);
        }
    }
    const auto start = window.rfind(MARKER_SETTINGS_START);
    const auto end_unload = window.rfind(MARKER_SETTINGS_END);
    const auto end_splash = window.rfind(MARKER_SPLASH);
    if (start == std::string::npos) {
        return;
    }
    const auto end = std::max(end_unload == std::string::npos ? 0 : end_unload,
                              end_splash == std::string::npos ? 0 : end_splash);
    bool visible = end == 0 || start > end;
    if (visible) {
        std::size_t pages = 0;
        for (auto at = window.find(MARKER_SETTINGS_PAGE); at != std::string::npos;
             at = window.find(MARKER_SETTINGS_PAGE, at + 1)) {
            ++pages;
        }
        if (pages >= 2) {
            visible = false;
        }
    }
    g_settings_visible.store(visible, std::memory_order_relaxed);
}

void WatchLoop() {
    std::filesystem::path path;
    std::string window;
    std::uintmax_t offset = 0;
    while (true) {
        std::this_thread::sleep_for(POLL_INTERVAL);
        std::error_code ec;
        if (path.empty()) {
            path = FindUiLog();
            offset = 0;
            window.clear();
            if (!path.empty()) {
                LOG_INFO(Lib_Pad, "menu state: following UI log {}", path.string());
            }
            continue;
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            // The title removes or rotates its log between runs; start over on the next one.
            path.clear();
            continue;
        }
        if (size < offset) {
            offset = 0;
            window.clear();
            g_settings_visible.store(false, std::memory_order_relaxed);
        }
        if (size == offset) {
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            path.clear();
            continue;
        }
        const auto to_read = static_cast<std::size_t>(size - offset);
        std::string chunk(to_read, '\0');
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(file.gcount());
        offset += got;
        chunk.resize(got);
        ApplyChunk(window, chunk);
    }
}

} // namespace

MenuScreen GetMenuScreen() {
    if (!g_started.exchange(true)) {
        static std::thread watcher{WatchLoop};
        watcher.detach();
        LOG_INFO(Lib_Pad, "menu state: watching the title UI log for screen transitions");
    }
    if (g_settings_visible.load(std::memory_order_relaxed)) {
        return MenuScreen::StartupSettings;
    }
    if (g_movie_visible.load(std::memory_order_relaxed)) {
        return MenuScreen::IntroMovie;
    }
    return MenuScreen::Unknown;
}

} // namespace Libraries::Pad
