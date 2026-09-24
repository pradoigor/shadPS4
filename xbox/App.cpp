// SPDX-License-Identifier: GPL-2.0-or-later
#include "Probes.h"
#include "Report.h"
#include "BuildInfo.h"
#include "PkgProbe.h"
#include "PkgExtractor.h"
#include "PkgBuiltinKeys.h"
#include "HomebrewRuntime.h"
#include "AngleVideo.h"
#include <windows.h>
#include <fileapifromapp.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.System.Display.h>
#include <filesystem>
#include <array>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <chrono>
#include <deque>
#include <cwctype>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Text.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
namespace Core = Windows::UI::Core;
namespace Sys = Windows::System;

// Best-effort diagnostics must never replace the original startup failure.
static void StartupLog(std::wstring const& message) noexcept {
    try {
        auto line = std::to_wstring(Lab::Now()) + L" " + message + L"\r\n";
        OutputDebugStringW(line.c_str());
        auto path = std::wstring(Windows::Storage::ApplicationData::Current().LocalFolder().Path()) + L"\\startup.log";
        CREATEFILE2_EXTENDED_PARAMETERS params{sizeof(params)};
        params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        winrt::handle file{CreateFile2(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, &params)};
        if (!file) return;
        auto bytes = to_string(line); DWORD written{};
        if (WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
            FlushFileBuffers(file.get());
    } catch (...) {}
}

struct App : ApplicationT<App> {
    std::unique_ptr<Lab::Report> report;
    enum class Page { Home, Library, Settings, Diagnostics };
    enum class HomeCategory { Games, Applications };
    enum class HomeFilter { All, Favorites, Recent };
    enum class HomeFocusArea { Tabs, Actions, Filters, Titles, EmptyAction, Resume, End };
    Grid root{nullptr};
    Grid homeView{nullptr}, libraryView{nullptr}, settingsView{nullptr}, diagnosticsView{nullptr};
    ItemsControl homeItems{nullptr}, pendingItems{nullptr};
    ListView list{nullptr};
    TextBlock status{nullptr}, details{nullptr}, libraryStatus{nullptr};
    DispatcherTimer timer{nullptr};
    bool busy{}, refreshing{}, persistenceFailed{};
    struct LibraryItem {
        std::wstring name, path, sourceName, category;
        bool installed{};
    };
    std::vector<LibraryItem> libraryItems;
    std::vector<size_t> installedIndices, pendingIndices;
    std::vector<size_t> homeVisibleIndices;
    std::map<size_t, Button> homeButtons;
    std::map<size_t, TextBlock> favoriteMarkers;
    std::set<std::wstring> favoriteIds;
    std::deque<std::wstring> recentIds;
    Page currentPage{Page::Home}, angleReturnPage{Page::Home};
    HomeCategory homeCategory{HomeCategory::Games};
    HomeFilter homeFilter{HomeFilter::All};
    int selectedPendingIndex{-1};
    int homeFocusedIndex{-1};
    HomeFocusArea homeFocusArea{HomeFocusArea::Tabs};
    int homeFocusIndex{};
    Sys::VirtualKey lastHomeDirection{Sys::VirtualKey::None};
    std::chrono::steady_clock::time_point lastHomeMove{};
    std::wstring selectedLoaderPath;
    std::shared_ptr<Lab::InstallProgress> extraction;
    std::unique_ptr<Lab::HomebrewRuntime> homebrew;
    Lab::AngleVideo angleVideo;
    bool anglePending{}, angleActive{};
    bool homebrewLaunchPending{};
    bool homebrewCompletionShown{};
    bool guestViewShown{};
    bool guestPaused{}, pauseComboHeld{};
    std::uint64_t guestDialogGeneration{};
    bool importing{}, listing{};
    Windows::System::Display::DisplayRequest displayRequest{nullptr};

    static std::wstring Extension(std::filesystem::path const& path) {
        auto extension = path.extension().wstring();
        for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
        return extension;
    }
    static std::wstring ItemKey(LibraryItem const& item) {
        return std::wstring(item.installed ? L"installed:" : L"pending:") +
            std::filesystem::path(item.path).filename().wstring();
    }
    struct SfoInfo { std::wstring title, category; };
    static SfoInfo ReadSfo(std::filesystem::path const& path) {
        SfoInfo info;
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size < 0x14 || size > 8 * 1024 * 1024) return info;
        std::ifstream input(path, std::ios::binary);
        if (!input) return info;
        std::vector<unsigned char> bytes(static_cast<size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input || bytes[0] != 0 || bytes[1] != 'P' || bytes[2] != 'S' || bytes[3] != 'F') return info;
        auto u16 = [&bytes](size_t at) -> uint16_t {
            return static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8));
        };
        auto u32 = [&bytes](size_t at) -> uint32_t {
            return static_cast<uint32_t>(bytes[at]) |
                (static_cast<uint32_t>(bytes[at + 1]) << 8) |
                (static_cast<uint32_t>(bytes[at + 2]) << 16) |
                (static_cast<uint32_t>(bytes[at + 3]) << 24);
        };
        const auto keyTable = u32(8), dataTable = u32(12), entries = u32(16);
        const auto indexEnd = uint64_t(0x14) + uint64_t(entries) * 0x10;
        if (entries > 4096 || indexEnd > bytes.size() || keyTable < indexEnd ||
            dataTable < keyTable || dataTable > bytes.size()) return info;
        for (uint32_t i = 0; i < entries; ++i) {
            const auto at = size_t(0x14) + size_t(i) * 0x10;
            const auto keyOffset = u16(at);
            const auto format = u16(at + 2);
            const auto length = u32(at + 4);
            const auto dataOffset = u32(at + 12);
            const auto keyAt = uint64_t(keyTable) + keyOffset;
            const auto valueAt = uint64_t(dataTable) + dataOffset;
            if (keyAt >= dataTable || valueAt > bytes.size() || length > bytes.size() - valueAt || format != 0x0204)
                continue;
            auto keyEnd = keyAt;
            while (keyEnd < dataTable && bytes[static_cast<size_t>(keyEnd)] != 0) ++keyEnd;
            if (keyEnd == dataTable) continue;
            std::string key(reinterpret_cast<const char*>(bytes.data() + keyAt), static_cast<size_t>(keyEnd - keyAt));
            if (key != "TITLE" && key != "CATEGORY") continue;
            auto valueEnd = valueAt;
            const auto limit = valueAt + length;
            while (valueEnd < limit && bytes[static_cast<size_t>(valueEnd)] != 0) ++valueEnd;
            if (valueEnd == valueAt) continue;
            try {
                std::string value(reinterpret_cast<const char*>(bytes.data() + valueAt), static_cast<size_t>(valueEnd - valueAt));
                auto wide = std::wstring(to_hstring(value));
                if (key == "TITLE") info.title = std::move(wide);
                else info.category = std::move(wide);
            } catch (...) {}
        }
        return info;
    }
    bool IsFavorite(LibraryItem const& item) const { return favoriteIds.contains(ItemKey(item)); }
    bool IsRecent(LibraryItem const& item) const {
        auto key = ItemKey(item);
        return std::find(recentIds.begin(), recentIds.end(), key) != recentIds.end();
    }
    void SavePreferences() {
        Windows::Data::Json::JsonObject preferences;
        Windows::Data::Json::JsonArray favorites, recent;
        for (auto const& id : favoriteIds) favorites.Append(Windows::Data::Json::JsonValue::CreateStringValue(hstring(id)));
        for (auto const& id : recentIds) recent.Append(Windows::Data::Json::JsonValue::CreateStringValue(hstring(id)));
        preferences.Insert(L"favorites", favorites);
        preferences.Insert(L"recent", recent);
        auto path = std::filesystem::path(Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) / L"ui-preferences.json";
        Lab::WriteDurable(path.wstring(), to_string(preferences.Stringify()));
    }
    void LoadPreferences() {
        try {
            auto path = std::filesystem::path(Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) / L"ui-preferences.json";
            if (!std::filesystem::is_regular_file(path)) return;
            std::ifstream input(path, std::ios::binary);
            std::string raw{std::istreambuf_iterator<char>(input), {}};
            auto preferences = Windows::Data::Json::JsonObject::Parse(to_hstring(raw));
            for (auto const& value : preferences.GetNamedArray(L"favorites", Windows::Data::Json::JsonArray{}))
                favoriteIds.insert(std::wstring(value.GetString()));
            for (auto const& value : preferences.GetNamedArray(L"recent", Windows::Data::Json::JsonArray{}))
                recentIds.push_back(std::wstring(value.GetString()));
        } catch (...) { favoriteIds.clear(); recentIds.clear(); }
    }
    void SetNotice(std::wstring const& message) {
        if (currentPage == Page::Home) {
            Find<TextBlock>(L"HomeNoticeText").Text(message);
            Find<Border>(L"HomeNotice").Visibility(Visibility::Visible);
        } else if (currentPage == Page::Library) {
            libraryStatus.Text(message);
        } else if (currentPage == Page::Settings) {
            Find<TextBlock>(L"SettingsStatus").Text(message);
        } else if (status) {
            status.Text(message);
        }
    }
    void SetActiveButton(wchar_t const* name, bool active) {
        auto button = Find<Button>(name);
        const auto background = active ? Windows::UI::ColorHelper::FromArgb(255, 25, 96, 106) :
            Windows::UI::ColorHelper::FromArgb(255, 23, 45, 56);
        const auto border = active ? Windows::UI::ColorHelper::FromArgb(255, 38, 220, 214) :
            Windows::UI::ColorHelper::FromArgb(255, 53, 83, 94);
        button.Background(Media::SolidColorBrush(background));
        button.BorderBrush(Media::SolidColorBrush(border));
    }
    void SetHomeCategory(HomeCategory category) {
        homeFocusedIndex = -1;
        homeCategory = category;
        SetActiveButton(L"GamesTab", category == HomeCategory::Games);
        SetActiveButton(L"AppsTab", category == HomeCategory::Applications);
        Find<TextBlock>(L"HomeTitle").Text(category == HomeCategory::Games ? L"Seus games" : L"Seus aplicativos");
        Find<TextBlock>(L"HomeSubtitle").Text(category == HomeCategory::Games ?
            L"Escolha um título para iniciar" : L"Aplicativos e ferramentas instalados");
        PopulateHome();
        FocusHome(homeVisibleIndices.empty() ? HomeFocusArea::EmptyAction : HomeFocusArea::Titles, 0);
    }
    void SetHomeFilter(HomeFilter filter) {
        homeFocusedIndex = -1;
        homeFilter = filter;
        SetActiveButton(L"FilterAll", filter == HomeFilter::All);
        SetActiveButton(L"FilterFavorites", filter == HomeFilter::Favorites);
        SetActiveButton(L"FilterRecent", filter == HomeFilter::Recent);
        PopulateHome();
        Find<Grid>(L"FilterOverlay").Visibility(Visibility::Collapsed);
        FocusHome(homeVisibleIndices.empty() ? HomeFocusArea::EmptyAction : HomeFocusArea::Titles, 0);
    }
    void OpenFilters() {
        Find<Grid>(L"FilterOverlay").Visibility(Visibility::Visible);
        FocusHome(HomeFocusArea::Filters, homeFilter == HomeFilter::All ? 0 :
            homeFilter == HomeFilter::Favorites ? 1 : 2);
    }
    void CloseFilters() {
        Find<Grid>(L"FilterOverlay").Visibility(Visibility::Collapsed);
        FocusHome(HomeFocusArea::Actions, 0);
    }
    void ShowPage(Page page, bool focus = true) {
        if (page != Page::Home) {
            homeFocusedIndex = -1;
            Find<Grid>(L"FilterOverlay").Visibility(Visibility::Collapsed);
        }
        currentPage = page;
        homeView.Visibility(page == Page::Home ? Visibility::Visible : Visibility::Collapsed);
        libraryView.Visibility(page == Page::Library ? Visibility::Visible : Visibility::Collapsed);
        settingsView.Visibility(page == Page::Settings ? Visibility::Visible : Visibility::Collapsed);
        diagnosticsView.Visibility(page == Page::Diagnostics ? Visibility::Visible : Visibility::Collapsed);
        if (page == Page::Home) {
            PopulateHome();
            if (focus) FocusHome(homeVisibleIndices.empty() ? HomeFocusArea::EmptyAction : HomeFocusArea::Titles, 0);
        } else if (page == Page::Library) {
            PopulateLibrary();
            if (focus) Find<Button>(L"SelectContent").Focus(FocusState::Programmatic);
        } else if (page == Page::Settings) {
            if (focus) Find<Button>(L"SettingsBack").Focus(FocusState::Programmatic);
        } else if (focus) {
            Find<Button>(L"DiagnosticsBack").Focus(FocusState::Programmatic);
        }
    }
    void RefreshPendingSelection() {
        const bool valid = selectedPendingIndex >= 0 && static_cast<size_t>(selectedPendingIndex) < libraryItems.size();
        auto install = Find<Button>(L"ExtractContent");
        auto validate = Find<Button>(L"ValidateContent");
        if (!valid) {
            selectedLoaderPath.clear();
            Find<TextBlock>(L"SelectedContentTitle").Text(L"Selecione um arquivo");
            Find<TextBlock>(L"SelectedContentDetails").Text(L"Os PKGs só são extraídos; importar ou instalar nunca inicia o conteúdo.");
            install.IsEnabled(false); validate.IsEnabled(false);
            return;
        }
        auto const& item = libraryItems[static_cast<size_t>(selectedPendingIndex)];
        const auto extension = Extension(item.path);
        const bool package = extension == L".pkg";
        const bool loose = extension == L".elf" || extension == L".self" || extension == L".bin";
        selectedLoaderPath = loose ? item.path : L"";
        install.IsEnabled(package && !extraction && !importing);
        validate.IsEnabled(loose && !extraction && !importing);
        Find<TextBlock>(L"SelectedContentTitle").Text(item.name);
        if (package) {
            Find<TextBlock>(L"SelectedContentDetails").Text(DescribeContent(item.path) + L"\nEste pacote ainda não foi extraído. Instalar apenas extrai os arquivos para a biblioteca do emulador.");
        } else if (loose) {
            Find<TextBlock>(L"SelectedContentDetails").Text(DescribeContent(item.path) + L"\nArquivo avulso: valide o formato antes de iniciar o trabalho de compatibilidade.");
        } else {
            Find<TextBlock>(L"SelectedContentDetails").Text(L"Formato não suportado para instalação. Importe um PKG ou um arquivo ELF/SELF.");
        }
    }
    void SelectPending(size_t index) {
        if (index >= libraryItems.size() || libraryItems[index].installed) return;
        selectedPendingIndex = static_cast<int>(index);
        RefreshPendingSelection();
    }
    void UpdateHomeBackdrop(size_t index) {
        auto backdrop = Find<Image>(L"HomeBackdrop");
        if (index >= libraryItems.size()) { backdrop.Visibility(Visibility::Collapsed); return; }
        auto const& item = libraryItems[index];
        auto folder = std::filesystem::path(item.path);
        auto picture = folder / L"sce_sys" / L"pic1.png";
        std::error_code error;
        if (!std::filesystem::is_regular_file(picture, error)) picture = folder / L"sce_sys" / L"icon0.png";
        if (!std::filesystem::is_regular_file(picture, error) ||
            std::filesystem::file_size(picture, error) > 16 * 1024 * 1024) {
            backdrop.Visibility(Visibility::Collapsed); return;
        }
        try {
            Media::Imaging::BitmapImage image;
            image.DecodePixelWidth(1600);
            image.UriSource(Uri(L"ms-appdata:///local/Installed/" + folder.filename().wstring() +
                L"/sce_sys/" + picture.filename().wstring()));
            backdrop.Source(image);
            backdrop.Visibility(Visibility::Visible);
        } catch (...) { backdrop.Visibility(Visibility::Collapsed); }
    }
    void PopulateHome(bool restoreFocus = false) {
        if (!homeItems || !report) return;
        homeItems.Items().Clear();
        homeVisibleIndices.clear();
        homeButtons.clear();
        favoriteMarkers.clear();
        for (auto index : installedIndices) {
            if (index >= libraryItems.size()) continue;
            auto const& item = libraryItems[index];
            const bool game = item.category == L"gd";
            if ((homeCategory == HomeCategory::Games) != game) continue;
            if (homeFilter == HomeFilter::Favorites && !IsFavorite(item)) continue;
            if (homeFilter == HomeFilter::Recent && !IsRecent(item)) continue;

            StackPanel tile; tile.Width(246); tile.Height(246); tile.Spacing(0);
            Grid artwork; artwork.Width(246); artwork.Height(246);
            Border artworkBackground; artworkBackground.CornerRadius({12, 12, 12, 12});
            artworkBackground.Background(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 24, 55, 66)));
            artwork.Children().Append(artworkBackground);
            auto iconPath = std::filesystem::path(item.path) / L"sce_sys" / L"icon0.png";
            std::error_code iconError;
            if (std::filesystem::is_regular_file(iconPath, iconError) && std::filesystem::file_size(iconPath, iconError) <= 8 * 1024 * 1024) {
                auto folderName = std::filesystem::path(item.path).filename().wstring();
                Media::Imaging::BitmapImage cover;
                cover.DecodePixelWidth(560);
                cover.UriSource(Uri(L"ms-appdata:///local/Installed/" + folderName + L"/sce_sys/icon0.png"));
                Image image; image.Source(cover); image.Stretch(Media::Stretch::UniformToFill);
                artwork.Children().Append(image);
            }

            Border favoriteBadge; favoriteBadge.Background(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(220, 6, 11, 19)));
            favoriteBadge.CornerRadius({5, 5, 5, 5}); favoriteBadge.Padding({7, 4, 7, 4});
            TextBlock favorite; favorite.Text(IsFavorite(item) ? L"★" : L"☆"); favorite.FontSize(17);
            favorite.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 255, 215, 106)));
            favoriteBadge.Child(favorite); favoriteBadge.HorizontalAlignment(HorizontalAlignment::Right); favoriteBadge.VerticalAlignment(VerticalAlignment::Top); favoriteBadge.Margin({0, 10, 10, 0});
            artwork.Children().Append(favoriteBadge);
            tile.Children().Append(artwork);

            Button card; card.Width(264); card.Height(264); card.Padding({8, 8, 8, 8}); card.Margin({0, 0, 20, 0});
            card.BorderThickness({2, 2, 2, 2}); card.BorderBrush(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 53, 83, 94)));
            card.Background(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 10, 31, 40)));
            card.HorizontalContentAlignment(HorizontalAlignment::Center); card.VerticalContentAlignment(VerticalAlignment::Center);
            card.Content(tile); card.Tag(box_value(static_cast<int64_t>(index)));
            Windows::UI::Xaml::Automation::AutomationProperties::SetName(card, hstring(item.name));
            const auto visibleIndex = static_cast<int>(homeVisibleIndices.size());
            card.GotFocus([this, index, visibleIndex](auto const&, auto const&) {
                homeFocusedIndex = static_cast<int>(index);
                homeFocusArea = HomeFocusArea::Titles;
                homeFocusIndex = visibleIndex;
                UpdateHomeBackdrop(index);
                Find<TextBlock>(L"HomeSubtitle").Text(libraryItems[index].name);
                if (auto tile = homeButtons.find(index); tile != homeButtons.end()) {
                    tile->second.BorderBrush(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 38, 220, 214)));
                    tile->second.BorderThickness({3, 3, 3, 3});
                }
            });
            card.LostFocus([this, index](auto const&, auto const&) {
                if (auto tile = homeButtons.find(index); tile != homeButtons.end()) {
                    tile->second.BorderBrush(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 53, 83, 94)));
                    tile->second.BorderThickness({2, 2, 2, 2});
                }
            });
            card.Click([this, index](auto const&, auto const&) { LaunchInstalled(index); });
            homeItems.Items().Append(card);
            homeVisibleIndices.push_back(index);
            homeButtons[index] = card;
            favoriteMarkers[index] = favorite;
        }
        const bool empty = homeVisibleIndices.empty();
        if (empty) UpdateHomeBackdrop(libraryItems.size());
        Find<StackPanel>(L"HomeEmpty").Visibility(empty ? Visibility::Visible : Visibility::Collapsed);
        Find<TextBlock>(L"HomeEmptyTitle").Text(homeFilter == HomeFilter::All ?
            (homeCategory == HomeCategory::Games ? L"Nenhum game instalado" : L"Nenhum aplicativo instalado") :
            (homeFilter == HomeFilter::Favorites ? L"Nenhum favorito nesta categoria" : L"Ainda não há títulos recentes"));
        Find<TextBlock>(L"HomeEmptyDescription").Text(homeFilter == HomeFilter::All ?
            L"Importe um PKG pela biblioteca e instale para adicionar um card aqui." :
            L"Pressione Y em um título para favoritar ou iniciar um item para preencher esta lista.");
        Find<TextBlock>(L"HomeCount").Text(std::to_wstring(homeVisibleIndices.size()) + (homeVisibleIndices.size() == 1 ? L" TÍTULO" : L" TÍTULOS"));
        if (restoreFocus && homeFocusedIndex >= 0) {
            auto it = homeButtons.find(static_cast<size_t>(homeFocusedIndex));
            if (it != homeButtons.end()) it->second.Focus(FocusState::Programmatic);
            else homeFocusedIndex = -1;
        }
    }
    void ToggleFocusedFavorite() {
        if (homeFocusedIndex < 0 || static_cast<size_t>(homeFocusedIndex) >= libraryItems.size()) return;
        auto const& item = libraryItems[static_cast<size_t>(homeFocusedIndex)];
        auto key = ItemKey(item);
        if (favoriteIds.contains(key)) favoriteIds.erase(key);
        else favoriteIds.insert(key);
        try { SavePreferences(); } catch (...) { SetNotice(L"Não foi possível salvar os favoritos neste console."); }
        if (homeFilter == HomeFilter::Favorites && !favoriteIds.contains(key)) PopulateHome(true);
        else if (auto marker = favoriteMarkers.find(static_cast<size_t>(homeFocusedIndex)); marker != favoriteMarkers.end())
            marker->second.Text(favoriteIds.contains(key) ? L"★" : L"☆");
    }
    void RememberLaunch(LibraryItem const& item) {
        auto key = ItemKey(item);
        recentIds.erase(std::remove(recentIds.begin(), recentIds.end(), key), recentIds.end());
        recentIds.push_front(key);
        while (recentIds.size() > 20) recentIds.pop_back();
        try { SavePreferences(); } catch (...) { SetNotice(L"O título iniciou, mas não foi possível salvar o histórico recente."); }
    }
    void LaunchInstalled(size_t index) {
        if (index >= libraryItems.size() || !libraryItems[index].installed) return;
        auto const& item = libraryItems[index];
        const auto eboot = std::filesystem::path(item.path) / L"eboot.bin";
        if (!std::filesystem::is_regular_file(eboot)) {
            SetNotice(L"Este conteúdo está incompleto: não encontramos eboot.bin.");
            return;
        }
        if (homebrew && homebrew->running()) {
            SetNotice(guestPaused ? L"Há um título pausado. Retome-o antes de iniciar outro." :
                                    L"Um título já está em execução.");
            return;
        }
        selectedLoaderPath = eboot.wstring();
        RememberLaunch(item);
        StartHomebrew();
    }

    Lab::PackageKeys LoadKeys() {
        auto path = std::filesystem::path(Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) / L"keys.json";
        if (!std::filesystem::exists(path)) return Lab::BuiltinFpkgKeys();
        return ParseKeys(path);
    }
    static Lab::PackageKeys ParseKeys(std::filesystem::path const& path) {
        if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("Arquivo de chaves excede 64 KiB.");
        std::ifstream input(path, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(input), {}};
        auto json = Windows::Data::Json::JsonObject::Parse(to_hstring(text));
        Lab::PackageKeys keys;
        auto read = [&](wchar_t const* name, Lab::RsaFields& fields) {
            auto set = json.GetNamedObject(name);
            for (const auto* field : {L"PublicExponent", L"Modulus", L"Prime1", L"Prime2", L"Exponent1", L"Exponent2", L"Coefficient", L"PrivateExponent"}) {
                auto value = to_string(set.GetNamedString(field));
                if (value.size() % 2) throw std::runtime_error("Campo hexadecimal invalido.");
                Lab::Bytes bytes;
                auto digit = [](char c) -> unsigned { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; if (c >= 'A' && c <= 'F') return c - 'A' + 10; throw std::runtime_error("Campo hexadecimal invalido."); };
                for (size_t i = 0; i < value.size(); i += 2) bytes.push_back(static_cast<unsigned char>((digit(value[i]) << 4) | digit(value[i + 1])));
                fields[to_string(field)] = std::move(bytes);
            }
        };
        read(L"PkgDerivedKey3Keyset", keys.derived); read(L"FakeKeyset", keys.fake);
        Lab::ValidatePackageKeys(keys);
        return keys;
    }
    fire_and_forget ImportKeys() {
        auto lifetime = get_strong();
        if (extraction || importing) co_return;
        importing = true;
        try {
            Windows::Storage::Pickers::FileOpenPicker picker;
            picker.FileTypeFilter().Append(L".json");
            auto selected = co_await picker.PickSingleFileAsync();
            if (selected) {
                auto local = Windows::Storage::ApplicationData::Current().LocalFolder();
                auto pendingKey = co_await selected.CopyAsync(local, L"keys-import.tmp", Windows::Storage::NameCollisionOption::ReplaceExisting);
                try {
                    ParseKeys(std::filesystem::path(pendingKey.Path().c_str()));
                    co_await pendingKey.RenameAsync(L"keys.json", Windows::Storage::NameCollisionOption::ReplaceExisting);
                    SetNotice(L"Chaves personalizadas importadas. Elas serão usadas nos PKGs que precisarem delas.");
                } catch (...) {
                    std::error_code ignored; std::filesystem::remove(std::filesystem::path(pendingKey.Path().c_str()), ignored); throw;
                }
            }
        } catch (...) { SetNotice(L"Não foi possível importar: JSON inválido ou conjuntos RSA-2048 incompletos. As chaves anteriores foram preservadas."); }
        importing = false;
    }
    void ValidateSelectedContent() {
        if (selectedLoaderPath.empty()) {
            SetNotice(L"Selecione um arquivo ELF/SELF importado para validar.");
            return;
        }
        list.SelectedIndex(0);
        RunSelected();
        SetNotice(L"Validação iniciada. Exporte o relatório em Configurações para consultar o resultado.");
    }
    void StartHomebrew() {
        if (selectedLoaderPath.empty()) {
            SetNotice(L"Selecione um conteúdo instalado com eboot.bin.");
            return;
        }
        if (homebrew && homebrew->running()) {
            SetNotice(L"Um título já está em execução.");
            return;
        }
        angleReturnPage = currentPage;
        guestViewShown = false;
        guestPaused = false;
        guestDialogGeneration = 0;
        Find<Grid>(L"GuestDialogOverlay").Visibility(Visibility::Collapsed);
        Find<Button>(L"ResumeContent").Visibility(Visibility::Collapsed);
        Find<Button>(L"EndContent").Visibility(Visibility::Collapsed);
        homebrewLaunchPending = true;
        anglePending = true;
        Find<Grid>(L"AngleTestView").Visibility(Visibility::Visible);
        Find<Border>(L"AngleLoadingView").Visibility(Visibility::Visible);
        Find<Border>(L"AngleTestControls").Visibility(Visibility::Collapsed);
        Find<TextBlock>(L"AngleStatus").Text(L"Preparando vídeo e iniciando conteúdo…");
        Find<Button>(L"CloseAngleTest").IsEnabled(false);
        RecordAngle(L"running", L"Inicialização EGL do homebrew iniciada.");
    }
    void PauseContent() {
        if (guestPaused || !homebrew || !homebrew->SetPaused(true)) return;
        guestPaused = true;
        Find<Grid>(L"GuestDialogOverlay").Visibility(Visibility::Collapsed);
        Find<Grid>(L"AngleTestView").Visibility(Visibility::Collapsed);
        Find<Button>(L"ResumeContent").Visibility(Visibility::Visible);
        Find<Button>(L"EndContent").Visibility(Visibility::Visible);
        ShowPage(Page::Home, false);
        Find<Button>(L"ResumeContent").Focus(FocusState::Programmatic);
        SetNotice(L"Conteúdo pausado. Selecione Retomar ou pressione Menu + View novamente.");
    }
    void ResumeContent() {
        if (!guestPaused || !homebrew || !homebrew->running()) return;
        Find<Grid>(L"AngleTestView").Visibility(Visibility::Visible);
        Find<Button>(L"ResumeContent").Visibility(Visibility::Collapsed);
        Find<Button>(L"EndContent").Visibility(Visibility::Collapsed);
        guestPaused = false;
        homebrew->SetPaused(false);
    }
    fire_and_forget EndContent() {
        auto lifetime = get_strong();
        if (!guestPaused || !homebrew || !homebrew->running()) co_return;
        SetNotice(L"Encerrando o conteúdo e reiniciando o XS4…");
        try {
            auto failure = co_await Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(L"");
            SetNotice(L"Não foi possível reiniciar o XS4 para encerrar o conteúdo (código " +
                std::to_wstring(static_cast<int>(failure)) + L"). O conteúdo permanece pausado.");
        } catch (hresult_error const& error) {
            SetNotice(L"Não foi possível reiniciar o XS4: " + std::wstring(error.message()) +
                L". O conteúdo permanece pausado.");
        }
    }
    void LaunchGuest() {
        try {
            homebrew = std::make_unique<Lab::HomebrewRuntime>();
            homebrewCompletionShown = false;
            homebrew->Start(std::filesystem::path(selectedLoaderPath),
                            std::filesystem::path(report->directory), &angleVideo);
            Find<TextBlock>(L"AngleStatus").Text(L"Conteúdo em execução. A imagem será exibida quando o programa apresentar o primeiro quadro.");
        } catch (std::exception const& e) {
            homebrew.reset();
            Find<Border>(L"AngleLoadingView").Visibility(Visibility::Collapsed);
            Find<Border>(L"AngleTestControls").Visibility(Visibility::Visible);
            SetNotice(L"Não foi possível iniciar o conteúdo: " + std::wstring(to_hstring(e.what())));
            Find<TextBlock>(L"AngleStatus").Text(L"Falha ao iniciar: " + std::wstring(to_hstring(e.what())));
            Find<Button>(L"CloseAngleTest").IsEnabled(true);
        } catch (hresult_error const& e) {
            homebrew.reset();
            Find<Border>(L"AngleLoadingView").Visibility(Visibility::Collapsed);
            Find<Border>(L"AngleTestControls").Visibility(Visibility::Visible);
            SetNotice(L"Não foi possível iniciar o conteúdo: " + std::wstring(e.message()));
            Find<TextBlock>(L"AngleStatus").Text(L"Falha ao iniciar: " + std::wstring(e.message()));
            Find<Button>(L"CloseAngleTest").IsEnabled(true);
        }
    }
    void ExtractionRecord(std::wstring const& state, std::wstring const& message,
                          std::wstring const& name, std::wstring const& destination, double started,
                          Lab::InstallProgress const& progress) {
        auto json = report->Json();
        using Windows::Data::Json::JsonValue;
        json.Insert(L"operation", JsonValue::CreateStringValue(L"pkg_extraction"));
        json.Insert(L"status", JsonValue::CreateStringValue(state));
        json.Insert(L"detail", JsonValue::CreateStringValue(message));
        json.Insert(L"package_name", JsonValue::CreateStringValue(name));
        json.Insert(L"destination", JsonValue::CreateStringValue(destination));
        json.Insert(L"files_written", JsonValue::CreateNumberValue(static_cast<double>(progress.files.load())));
        json.Insert(L"bytes_written", JsonValue::CreateNumberValue(static_cast<double>(progress.bytes.load())));
        json.Insert(L"duration_ms", JsonValue::CreateNumberValue((Lab::Now() - started) * 1000));
        json.Insert(L"tests", Windows::Data::Json::JsonArray());
        Lab::WriteDurable(report->directory + L"\\extraction-report.json", to_string(json.Stringify()));
    }
    fire_and_forget ExtractContent() {
        auto lifetime = get_strong();
        const auto index = selectedPendingIndex;
        if (extraction || importing || index < 0 || static_cast<size_t>(index) >= libraryItems.size() ||
            libraryItems[index].installed || Extension(libraryItems[index].path) != L".pkg") co_return;
        auto item = libraryItems[static_cast<size_t>(index)];
        auto state = std::make_shared<Lab::InstallProgress>();
        extraction = state;
        Find<Button>(L"ExtractContent").IsEnabled(false);
        Find<Button>(L"SelectContent").IsEnabled(false);
        Find<Button>(L"ImportKeys").IsEnabled(false);
        Find<Button>(L"CancelExtraction").IsEnabled(true);
        Find<Button>(L"CancelExtraction").Content(box_value(L"Cancelar"));
        Find<TextBlock>(L"InstallProgressText").Text(L"Preparando pacote…");
        Find<TextBlock>(L"InstallProgressPercent").Text(L"0%");
        Find<ProgressBar>(L"InstallProgress").Value(0);
        Find<Grid>(L"InstallOverlay").Visibility(Visibility::Visible);
        auto started = Lab::Now();
        std::wstring error, destination;
        bool completed = false;
        apartment_context ui;
        try {
            displayRequest = Windows::System::Display::DisplayRequest(); displayRequest.RequestActive();
            Lab::PackageKeys keys;
            if (Lab::PackageNeedsKeys(item.path)) keys = LoadKeys();
            auto base = std::filesystem::path(report->directory);
            auto id = std::to_wstring(static_cast<uint64_t>(started * 1000000));
            auto stage = base / L"InstallStaging" / id;
            auto target = base / L"Installed" / id;
            destination = target.wstring();
            ExtractionRecord(L"running", L"Extração iniciada; destino temporário.", item.name, destination, started, *state);
            co_await resume_background();
            try {
                std::filesystem::create_directories(stage);
                Lab::ExtractPackage(item.path, stage, keys, *state);
                for (auto* set : {&keys.derived, &keys.fake}) for (auto& [field, bytes] : *set) SecureZeroMemory(bytes.data(), bytes.size());
                if (std::filesystem::exists(stage / L"library-name.txt")) throw std::runtime_error("Nome reservado aos metadados da biblioteca.");
                Lab::WriteDurable((stage / L"library-name.txt").wstring(), to_string(item.name));
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::rename(stage, target);
                completed = true; state->percent.store(100);
            } catch (std::exception const& e) { error = to_hstring(e.what()); }
              catch (...) { error = L"Falha de armazenamento ou da plataforma durante a extração."; }
            for (auto* set : {&keys.derived, &keys.fake}) for (auto& [field, bytes] : *set) SecureZeroMemory(bytes.data(), bytes.size());
            if (!completed) { std::error_code ignored; std::filesystem::remove_all(stage, ignored); }
            co_await ui;
        } catch (std::exception const& e) { error = to_hstring(e.what()); }
          catch (...) { error = L"Não foi possível iniciar a extração. Verifique chaves e armazenamento."; }
        co_await ui;
        try {
            ExtractionRecord(completed ? L"completed" : (state->cancel ? L"cancelled" : L"failed"),
                completed ? L"Conteúdo extraído; execução PS4 não validada." : error, item.name, destination, started, *state);
        } catch (...) { error += L" Não foi possível salvar extraction-report.json."; }
        try { if (displayRequest) displayRequest.RequestRelease(); } catch (...) {}
        displayRequest = nullptr;
        Find<Grid>(L"InstallOverlay").Visibility(Visibility::Collapsed);
        extraction.reset();
        Find<Button>(L"SelectContent").IsEnabled(true);
        Find<Button>(L"ImportKeys").IsEnabled(true);
        Find<Button>(L"CancelExtraction").IsEnabled(false);
        Find<ProgressBar>(L"InstallProgress").Value(completed ? 100 : 0);
        SetNotice(completed ? L"Instalação concluída. O título já aparece na tela Games ou Aplicativos." :
            (state->cancel.load() ? L"Instalação cancelada. Os arquivos temporários foram descartados." : L"Instalação não concluída: " + error));
        PopulateLibrary();
    }

    App() {
        StartupLog(L"Application constructed");
        UnhandledException([](auto const&, UnhandledExceptionEventArgs const& e) {
            StartupLog(L"Unhandled XAML error: " + std::to_wstring(static_cast<uint32_t>(e.Exception().value)) + L" " + std::wstring(e.Message()));
        });
    }
    template<typename T> T Find(wchar_t const* name) { return root.FindName(name).as<T>(); }
    static std::wstring DescribeContent(std::wstring const& path) {
        std::error_code error;
        const auto size = std::filesystem::file_size(std::filesystem::path(path), error);
        const auto extension = Extension(path);
        const auto sizeText = error ? std::wstring(L"tamanho indisponível") :
            (size >= 1024 * 1024 ? std::to_wstring(size / (1024 * 1024)) + L" MB" :
             std::to_wstring(size / 1024) + L" KB");
        if (extension == L".pkg") {
            const auto probe = Lab::ProbePkg(std::filesystem::path(path));
            return (probe.recognized ? std::wstring(L"Pacote PS4 reconhecido") : std::wstring(L"Não reconhecido como PKG PS4")) +
                L" · " + sizeText + L"\nA instalação extrai os arquivos e não inicia o conteúdo.";
        }
        if (extension == L".elf") return L"Executável ELF · " + sizeText + L"\nA validação técnica não executa este arquivo.";
        if (extension == L".self") return L"Executável SELF · " + sizeText + L"\nA validação técnica não executa este arquivo.";
        if (extension == L".bin") return L"Arquivo BIN · " + sizeText + L"\nO formato será validado antes de qualquer execução.";
        return L"Arquivo selecionado · " + sizeText;
    }
    fire_and_forget PopulateLibrary() {
        auto lifetime = get_strong();
        if (listing) co_return;
        listing = true;
        try {
            auto local = Windows::Storage::ApplicationData::Current().LocalFolder();
            auto folder = co_await local.CreateFolderAsync(L"Library", Windows::Storage::CreationCollisionOption::OpenIfExists);
            auto files = co_await folder.GetFilesAsync();
            installedIndices.clear(); pendingIndices.clear();
            homeItems.Items().Clear(); pendingItems.Items().Clear();
            libraryItems.clear();
            auto installed = co_await local.CreateFolderAsync(L"Installed", Windows::Storage::CreationCollisionOption::OpenIfExists);
            auto folders = co_await installed.GetFoldersAsync();
            std::set<std::wstring> installedSources;
            for (auto const& game : folders) {
                const auto gamePath = std::filesystem::path(game.Path().c_str());
                auto titlePath = gamePath / L"library-name.txt";
                std::ifstream titleFile(titlePath, std::ios::binary);
                std::string source{std::istreambuf_iterator<char>(titleFile), {}};
                auto sourceName = source.empty() ? std::wstring(game.Name()) : std::wstring(to_hstring(source));
                if (Extension(std::filesystem::path(sourceName)) == L".pkg") installedSources.insert(sourceName);
                const auto metadata = ReadSfo(gamePath / L"sce_sys" / L"param.sfo");
                const auto displayName = metadata.title.empty() ? sourceName : metadata.title;
                const auto index = libraryItems.size();
                libraryItems.push_back({displayName, gamePath.wstring(), sourceName, metadata.category, true});
                installedIndices.push_back(index);
            }
            for (auto const& file : files) {
                const auto path = std::filesystem::path(file.Path().c_str());
                const auto extension = Extension(path);
                if (extension == L".pkg" && installedSources.contains(std::wstring(file.Name()))) continue;
                const auto index = libraryItems.size();
                libraryItems.push_back({std::wstring(file.Name()), path.wstring(), std::wstring(file.Name()), L"", false});
                pendingIndices.push_back(index);

                StackPanel tile; tile.Width(220); tile.Height(220); tile.Spacing(8);
                Grid artwork; artwork.Width(220); artwork.Height(158);
                Border artworkBackground; artworkBackground.CornerRadius({9, 9, 9, 9});
                const auto package = extension == L".pkg";
                artworkBackground.Background(Media::SolidColorBrush(package ?
                    Windows::UI::ColorHelper::FromArgb(255, 18, 64, 102) :
                    Windows::UI::ColorHelper::FromArgb(255, 39, 48, 69)));
                artwork.Children().Append(artworkBackground);
                TextBlock glyph; glyph.Text(package ? L"\xE8B7" : L"\xE8A5");
                glyph.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets")); glyph.FontSize(56);
                glyph.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 129, 190, 242)));
                glyph.HorizontalAlignment(HorizontalAlignment::Center); glyph.VerticalAlignment(VerticalAlignment::Center);
                artwork.Children().Append(glyph);
                Border badge; badge.Background(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(225, 6, 11, 19)));
                badge.CornerRadius({5, 5, 5, 5}); badge.Padding({8, 5, 8, 5});
                TextBlock badgeText; badgeText.Text(package ? L"PKG · A INSTALAR" : L"ARQUIVO · A VALIDAR"); badgeText.FontSize(10); badgeText.CharacterSpacing(70);
                badge.Child(badgeText); badge.HorizontalAlignment(HorizontalAlignment::Left); badge.VerticalAlignment(VerticalAlignment::Top); badge.Margin({10, 10, 0, 0});
                artwork.Children().Append(badge);
                tile.Children().Append(artwork);
                TextBlock title; title.Text(std::wstring(file.Name())); title.FontSize(15); title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                title.MaxLines(1); title.TextTrimming(TextTrimming::CharacterEllipsis); tile.Children().Append(title);
                TextBlock kind; kind.Text(package ? L"Pacote PS4" : (extension == L".elf" ? L"Executável ELF" : (extension == L".self" ? L"Executável SELF" : L"Arquivo avulso")));
                kind.FontSize(12); kind.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 137, 152, 174))); tile.Children().Append(kind);
                Button card; card.Width(234); card.Height(234); card.Padding({6, 6, 6, 6}); card.BorderThickness({1, 1, 1, 1});
                card.BorderBrush(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 33, 41, 56)));
                card.Background(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 14, 18, 27)));
                card.Content(tile); card.Tag(box_value(static_cast<int64_t>(index)));
                card.Click([this, index](auto const&, auto const&) { SelectPending(index); });
                pendingItems.Items().Append(card);
            }
            selectedPendingIndex = -1;
            RefreshPendingSelection();
            Find<TextBlock>(L"PendingCount").Text(std::to_wstring(pendingIndices.size()) +
                (pendingIndices.size() == 1 ? L" arquivo" : L" arquivos"));
            Find<StackPanel>(L"PendingEmpty").Visibility(pendingIndices.empty() ? Visibility::Visible : Visibility::Collapsed);
            PopulateHome();
        } catch (hresult_error const& e) {
            libraryStatus.Text(L"Falha ao listar a biblioteca: " + e.message());
        } catch (...) { libraryStatus.Text(L"Não foi possível ler a biblioteca local."); }
        listing = false;
    }
    fire_and_forget SelectContent() {
        auto lifetime = get_strong();
        if (extraction || importing) co_return;
        importing = true;
        try {
            Windows::Storage::Pickers::FileOpenPicker picker;
            picker.ViewMode(Windows::Storage::Pickers::PickerViewMode::List);
            picker.FileTypeFilter().Append(L".elf");
            picker.FileTypeFilter().Append(L".self");
            picker.FileTypeFilter().Append(L".bin");
            picker.FileTypeFilter().Append(L".pkg");
            auto file = co_await picker.PickSingleFileAsync();
            if (!file) { importing = false; co_return; }
            auto local = Windows::Storage::ApplicationData::Current().LocalFolder();
            auto folder = co_await local.CreateFolderAsync(L"Library", Windows::Storage::CreationCollisionOption::OpenIfExists);
            auto copied = co_await file.CopyAsync(folder, file.Name(), Windows::Storage::NameCollisionOption::GenerateUniqueName);
            PopulateLibrary();
            SetNotice(L"Arquivo importado para a biblioteca: " + std::wstring(copied.Name()));
        } catch (hresult_error const& e) {
            SetNotice(L"Falha ao importar conteúdo: " + std::wstring(e.message()));
        } catch (...) { SetNotice(L"Falha ao importar conteúdo."); }
        importing = false;
        RefreshPendingSelection();
    }
    bool Save() {
        try { report->Save(); return true; }
        catch (hresult_error const& e) {
            persistenceFailed = true;
            SetNotice(L"Falha ao persistir; testes bloqueados: " + std::wstring(e.message()));
        } catch (std::exception const&) {
            persistenceFailed = true;
            SetNotice(L"Falha de armazenamento; testes bloqueados.");
        }
        return false;
    }
    void ExportReport() {
        if (!Save()) return;
        try {
            auto name = L"report-export-" + std::to_wstring(static_cast<uint64_t>(Lab::Now())) + L".json";
            auto exported = report->Json();
            Windows::Data::Json::JsonObject debug;
            auto directory = std::filesystem::path(report->directory);
            auto runtimePath = directory / L"homebrew-runtime.json";
            std::wstring sessionName;
            std::wstring sessionId;
            if (std::filesystem::is_regular_file(runtimePath)) {
                std::ifstream input(runtimePath, std::ios::binary);
                std::string raw{std::istreambuf_iterator<char>(input), {}};
                auto runtime = Windows::Data::Json::JsonObject::Parse(to_hstring(raw));
                sessionName = std::wstring(runtime.GetNamedString(L"session_file", L""));
                sessionId = std::wstring(runtime.GetNamedString(L"session_id", L""));
                if (sessionName.empty() && !sessionId.empty())
                    sessionName = L"homebrew-session-" + sessionId + L".jsonl";
                debug.SetNamedValue(L"runtime", runtime);
            }
            auto includeLines = [&](wchar_t const* field, std::filesystem::path const& path) {
                Windows::Data::Json::JsonArray events;
                if (std::filesystem::is_regular_file(path)) {
                    std::ifstream input(path, std::ios::binary);
                    std::string line;
                    std::deque<Windows::Data::Json::JsonObject> recent;
                    while (std::getline(input, line)) {
                        try {
                            recent.push_back(Windows::Data::Json::JsonObject::Parse(to_hstring(line)));
                            if (recent.size() > 8192) recent.pop_front();
                        } catch (...) {}
                    }
                    for (auto const& event : recent) events.Append(event);
                }
                debug.SetNamedValue(field, events);
            };
            includeLines(L"hle_trace", directory / L"homebrew-hle-trace.jsonl");
            if (!sessionName.empty())
                includeLines(L"session_events", directory / std::filesystem::path(sessionName).filename());
            auto lastHle = directory / L"homebrew-last-hle.json";
            auto anglePath = directory / L"angle-video.json";
            if (std::filesystem::is_regular_file(anglePath)) {
                std::ifstream input(anglePath, std::ios::binary);
                std::string raw{std::istreambuf_iterator<char>(input), {}};
                exported.SetNamedValue(L"angle_video", Windows::Data::Json::JsonObject::Parse(to_hstring(raw)));
            }
            if (std::filesystem::is_regular_file(lastHle)) {
                try {
                    std::ifstream input(lastHle, std::ios::binary);
                    std::string raw{std::istreambuf_iterator<char>(input), {}};
                    debug.SetNamedValue(L"last_hle", Windows::Data::Json::JsonObject::Parse(to_hstring(raw)));
                } catch (...) {}
            }
            if (!sessionId.empty()) {
                auto console = directory / (L"homebrew-console-" + sessionId + L".log");
                if (std::filesystem::is_regular_file(console)) {
                    std::ifstream input(console, std::ios::binary);
                    std::string raw{std::istreambuf_iterator<char>(input), {}};
                    try {
                        debug.SetNamedValue(L"console_log",
                            Windows::Data::Json::JsonValue::CreateStringValue(to_hstring(raw)));
                    } catch (...) {}
                }
            }
            exported.SetNamedValue(L"homebrew_debug", debug);
            Lab::WriteDurable(report->directory + L"\\" + name, to_string(exported.Stringify()));
            SetNotice(L"Relatório exportado: LocalState\\" + name + L". Baixe pelo Device Portal.");
        } catch (hresult_error const& e) {
            SetNotice(L"Falha na exportação: " + std::wstring(e.message()));
        } catch (std::exception const& e) {
            SetNotice(L"Falha na exportação: " + std::wstring(to_hstring(e.what())));
        }
    }
    void GoBack() {
        if (currentPage == Page::Home && Find<Grid>(L"FilterOverlay").Visibility() == Visibility::Visible) {
            CloseFilters(); return;
        }
        switch (currentPage) {
        case Page::Home: break;
        case Page::Library: ShowPage(Page::Home); break;
        case Page::Settings: ShowPage(Page::Home); break;
        case Page::Diagnostics: ShowPage(Page::Settings); break;
        }
    }
    void FocusHome(HomeFocusArea area, int index = 0) {
        switch (area) {
        case HomeFocusArea::Tabs: {
            constexpr wchar_t const* names[]{L"GamesTab", L"AppsTab"};
            index = std::clamp(index, 0, 1);
            Find<Button>(names[index]).Focus(FocusState::Programmatic);
            break;
        }
        case HomeFocusArea::Actions: {
            constexpr wchar_t const* names[]{L"OpenFilters", L"OpenLibrary", L"OpenSettings"};
            index = std::clamp(index, 0, 2);
            Find<Button>(names[index]).Focus(FocusState::Programmatic);
            break;
        }
        case HomeFocusArea::Filters: {
            constexpr wchar_t const* names[]{L"FilterAll", L"FilterFavorites", L"FilterRecent"};
            index = std::clamp(index, 0, 2);
            Find<Button>(names[index]).Focus(FocusState::Programmatic);
            break;
        }
        case HomeFocusArea::Titles:
            if (homeVisibleIndices.empty()) {
                FocusHome(HomeFocusArea::EmptyAction);
                return;
            }
            index = std::clamp(index, 0, static_cast<int>(homeVisibleIndices.size()) - 1);
            if (auto found = homeButtons.find(homeVisibleIndices[index]); found != homeButtons.end())
                found->second.Focus(FocusState::Programmatic);
            break;
        case HomeFocusArea::EmptyAction:
            Find<Button>(L"EmptyImport").Focus(FocusState::Programmatic);
            break;
        case HomeFocusArea::Resume:
            if (guestPaused) Find<Button>(L"ResumeContent").Focus(FocusState::Programmatic);
            break;
        case HomeFocusArea::End:
            if (guestPaused) Find<Button>(L"EndContent").Focus(FocusState::Programmatic);
            break;
        }
    }
    void NavigateHome(int dx, int dy) {
        if (dx == 0 && dy == 0) return;
        if (Find<Grid>(L"FilterOverlay").Visibility() == Visibility::Visible) {
            if (dy) FocusHome(HomeFocusArea::Filters, std::clamp(homeFocusIndex + dy, 0, 2));
            return;
        }
        switch (homeFocusArea) {
        case HomeFocusArea::Tabs:
            if (dy > 0) FocusHome(HomeFocusArea::Titles, 0);
            else if (dy < 0) FocusHome(guestPaused ? HomeFocusArea::Resume : HomeFocusArea::Actions, 0);
            else FocusHome(HomeFocusArea::Tabs, homeFocusIndex + dx);
            break;
        case HomeFocusArea::Actions:
            if (dy > 0) FocusHome(HomeFocusArea::Titles, 0);
            else if (dx < 0 && homeFocusIndex == 0)
                FocusHome(guestPaused ? HomeFocusArea::End : HomeFocusArea::Tabs, 1);
            else FocusHome(HomeFocusArea::Actions, homeFocusIndex + dx);
            break;
        case HomeFocusArea::Filters:
            FocusHome(HomeFocusArea::Filters, homeFocusIndex + dy);
            break;
        case HomeFocusArea::Titles:
            if (dy < 0) FocusHome(HomeFocusArea::Tabs, homeCategory == HomeCategory::Games ? 0 : 1);
            else {
                const auto next = homeFocusIndex + dx;
                if (next >= 0 && next < static_cast<int>(homeVisibleIndices.size()))
                    FocusHome(HomeFocusArea::Titles, next);
            }
            break;
        case HomeFocusArea::EmptyAction:
            if (dy < 0) FocusHome(HomeFocusArea::Tabs, homeCategory == HomeCategory::Games ? 0 : 1);
            break;
        case HomeFocusArea::Resume:
            if (dy > 0) FocusHome(HomeFocusArea::Tabs, 1);
            else if (dx > 0) FocusHome(HomeFocusArea::End);
            break;
        case HomeFocusArea::End:
            if (dy > 0) FocusHome(HomeFocusArea::Tabs, 1);
            else if (dx < 0) FocusHome(HomeFocusArea::Resume);
            else if (dx > 0) FocusHome(HomeFocusArea::Actions, 0);
            break;
        }
    }
    void DismissGuestDialog(bool canceled) {
        if (homebrew) homebrew->CompleteMessageDialog(canceled);
        Find<Grid>(L"GuestDialogOverlay").Visibility(Visibility::Collapsed);
    }
    void OnGamepadKey(Core::CoreWindow const&, Core::KeyEventArgs const& args) {
        // The guest polls the physical pad directly. Do not also navigate XS4
        // when B/Menu are pressed inside a running title.
        if (homebrew && homebrew->running() && !guestPaused) {
            if (Find<Grid>(L"GuestDialogOverlay").Visibility() == Visibility::Visible) {
                if (args.VirtualKey() == Sys::VirtualKey::GamepadB) {
                    DismissGuestDialog(true);
                    args.Handled(true);
                } else if (args.VirtualKey() != Sys::VirtualKey::GamepadA) {
                    args.Handled(true);
                }
                // A is handled by the focused dialog button.
                return;
            }
            // The guest reads the physical pad through scePadReadState. Consume
            // the parallel UWP key event so Xbox does not treat B as Back and
            // close the host while the guest is still displaying a menu.
            args.Handled(true);
            return;
        }
        const auto key = args.VirtualKey();
        bool handled = true;
        int dx{}, dy{};
        if (key == Sys::VirtualKey::GamepadDPadLeft ||
            key == Sys::VirtualKey::GamepadLeftThumbstickLeft ||
            key == Sys::VirtualKey::Left) dx = -1;
        else if (key == Sys::VirtualKey::GamepadDPadRight ||
                 key == Sys::VirtualKey::GamepadLeftThumbstickRight ||
                 key == Sys::VirtualKey::Right) dx = 1;
        else if (key == Sys::VirtualKey::GamepadDPadUp ||
                 key == Sys::VirtualKey::GamepadLeftThumbstickUp ||
                 key == Sys::VirtualKey::Up) dy = -1;
        else if (key == Sys::VirtualKey::GamepadDPadDown ||
                 key == Sys::VirtualKey::GamepadLeftThumbstickDown ||
                 key == Sys::VirtualKey::Down) dy = 1;
        if (currentPage == Page::Home && (dx || dy) &&
            Find<Grid>(L"AngleTestView").Visibility() != Visibility::Visible) {
            const auto now = std::chrono::steady_clock::now();
            if (key != lastHomeDirection || now - lastHomeMove >= std::chrono::milliseconds(150)) {
                lastHomeDirection = key;
                lastHomeMove = now;
                NavigateHome(dx, dy);
            }
        } else if (key == Sys::VirtualKey::GamepadB) {
            const bool angleVisible = Find<Grid>(L"AngleTestView").Visibility() == Visibility::Visible;
            if (angleVisible && !(homebrew && homebrew->running()) && !homebrewLaunchPending)
                FinishAngleTest();
            else
                GoBack();
        } else if (key == Sys::VirtualKey::GamepadMenu && currentPage != Page::Settings && currentPage != Page::Diagnostics) {
            ShowPage(Page::Settings);
        } else if (currentPage == Page::Home && key == Sys::VirtualKey::GamepadY) {
            ToggleFocusedFavorite();
        } else if (currentPage == Page::Home && key == Sys::VirtualKey::GamepadLeftShoulder) {
            SetHomeCategory(HomeCategory::Games);
        } else if (currentPage == Page::Home && key == Sys::VirtualKey::GamepadRightShoulder) {
            SetHomeCategory(HomeCategory::Applications);
        } else if (currentPage == Page::Library && key == Sys::VirtualKey::GamepadX) {
            ExtractContent();
        } else {
            handled = false;
        }
        if (handled) args.Handled(true);
    }
    void RecordAngle(std::wstring const& state, std::wstring const& detail) {
        try {
            Windows::Data::Json::JsonObject result;
            using Windows::Data::Json::JsonValue;
            result.Insert(L"test", JsonValue::CreateStringValue(L"angle_egl_swapchainpanel"));
            result.Insert(L"status", JsonValue::CreateStringValue(state));
            result.Insert(L"detail", JsonValue::CreateStringValue(detail));
            result.Insert(L"commit", JsonValue::CreateStringValue(XBOX_BUILD_COMMIT));
            result.Insert(L"timestamp", JsonValue::CreateNumberValue(Lab::Now()));
            Lab::WriteDurable(report->directory + L"\\angle-video.json", to_string(result.Stringify()));
        } catch (...) { Find<TextBlock>(L"AngleStatus").Text(detail + L" · falha ao gravar o resultado"); }
    }
    void StartAngleTest() {
        if (homebrewLaunchPending || (homebrew && homebrew->running())) return;
        angleReturnPage = currentPage;
        angleVideo.Stop(); angleActive = false;
        Find<Grid>(L"AngleTestView").Visibility(Visibility::Visible);
        Find<Border>(L"AngleLoadingView").Visibility(Visibility::Collapsed);
        Find<Border>(L"AngleTestControls").Visibility(Visibility::Visible);
        Find<TextBlock>(L"AngleStatus").Text(L"Preparando superfície EGL…");
        Find<Button>(L"CloseAngleTest").IsEnabled(true);
        anglePending = true;
        RecordAngle(L"running", L"Teste iniciado; ausência de resultado final indica interrupção do aplicativo.");
        Find<Button>(L"CloseAngleTest").Focus(FocusState::Programmatic);
    }
    void FinishAngleTest() {
        if (homebrewLaunchPending || (homebrew && homebrew->running())) return;
        anglePending = false; angleActive = false;
        angleVideo.Stop();
        Find<Grid>(L"AngleTestView").Visibility(Visibility::Collapsed);
        Find<Border>(L"AngleLoadingView").Visibility(Visibility::Collapsed);
        Find<Border>(L"AngleTestControls").Visibility(Visibility::Visible);
        ShowPage(angleReturnPage);
    }
    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
        StartupLog(L"OnLaunched entered");
        // Xbox defaults UWP apps to gamepad-driven mouse mode. Hiding its
        // cursor does not switch to D-pad/left-stick focus navigation.
        RequiresPointerMode(ApplicationRequiresPointerMode::WhenRequested);
        if (root) { Window::Current().Activate(); return; }
        try {
            auto path = std::filesystem::path(Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()) / L"MainPage.xaml";
            std::ifstream file(path, std::ios::binary);
            if (!file) throw hresult_error(E_FAIL, L"MainPage.xaml ausente no pacote.");
            std::string xaml{std::istreambuf_iterator<char>(file), {}};
            StartupLog(L"Loading MainPage.xaml");
            root = Markup::XamlReader::Load(to_hstring(xaml)).as<Grid>();
            homeView = Find<Grid>(L"HomeView");
            libraryView = Find<Grid>(L"LibraryView");
            settingsView = Find<Grid>(L"SettingsView");
            diagnosticsView = Find<Grid>(L"DiagnosticsView");
            homeItems = Find<ItemsControl>(L"HomeItems");
            pendingItems = Find<ItemsControl>(L"PendingItems");
            list = Find<ListView>(L"Tests");
            status = Find<TextBlock>(L"Status"); details = Find<TextBlock>(L"Details");
            libraryStatus = Find<TextBlock>(L"LibraryStatus");
            StartupLog(L"XAML loaded; opening report");
            report = std::make_unique<Lab::Report>();
            Find<TextBlock>(L"DeviceInfo").Text(report->Summary());
            auto version = Windows::ApplicationModel::Package::Current().Id().Version();
            Find<TextBlock>(L"CurrentVersion").Text(std::to_wstring(version.Major) + L"." +
                std::to_wstring(version.Minor) + L"." + std::to_wstring(version.Build) + L"." +
                std::to_wstring(version.Revision));
            LoadPreferences();
            list.SelectionChanged([this](auto const&, auto const&) { if (!refreshing) ShowDetails(); });
            Find<Button>(L"RunSelected").Click([this](auto const&, auto const&) { RunSelected(); });
            Find<Button>(L"GamesTab").Click([this](auto const&, auto const&) { SetHomeCategory(HomeCategory::Games); });
            Find<Button>(L"AppsTab").Click([this](auto const&, auto const&) { SetHomeCategory(HomeCategory::Applications); });
            Find<Button>(L"FilterAll").Click([this](auto const&, auto const&) { SetHomeFilter(HomeFilter::All); });
            Find<Button>(L"FilterFavorites").Click([this](auto const&, auto const&) { SetHomeFilter(HomeFilter::Favorites); });
            Find<Button>(L"FilterRecent").Click([this](auto const&, auto const&) { SetHomeFilter(HomeFilter::Recent); });
            Find<Button>(L"OpenFilters").Click([this](auto const&, auto const&) { OpenFilters(); });
            Find<Button>(L"CloseFilters").Click([this](auto const&, auto const&) { CloseFilters(); });
            auto trackHomeFocus = [this](wchar_t const* name, HomeFocusArea area, int index) {
                Find<Button>(name).GotFocus([this, area, index](auto const&, auto const&) {
                    homeFocusArea = area;
                    homeFocusIndex = index;
                    if (area != HomeFocusArea::Titles) homeFocusedIndex = -1;
                });
            };
            trackHomeFocus(L"GamesTab", HomeFocusArea::Tabs, 0);
            trackHomeFocus(L"AppsTab", HomeFocusArea::Tabs, 1);
            trackHomeFocus(L"OpenFilters", HomeFocusArea::Actions, 0);
            trackHomeFocus(L"OpenLibrary", HomeFocusArea::Actions, 1);
            trackHomeFocus(L"OpenSettings", HomeFocusArea::Actions, 2);
            trackHomeFocus(L"FilterAll", HomeFocusArea::Filters, 0);
            trackHomeFocus(L"FilterFavorites", HomeFocusArea::Filters, 1);
            trackHomeFocus(L"FilterRecent", HomeFocusArea::Filters, 2);
            trackHomeFocus(L"EmptyImport", HomeFocusArea::EmptyAction, 0);
            trackHomeFocus(L"ResumeContent", HomeFocusArea::Resume, 0);
            trackHomeFocus(L"EndContent", HomeFocusArea::End, 0);
            Find<Button>(L"ResumeContent").Click([this](auto const&, auto const&) { ResumeContent(); });
            Find<Button>(L"EndContent").Click([this](auto const&, auto const&) { EndContent(); });
            Find<Button>(L"GuestDialogAccept").Click([this](auto const&, auto const&) { DismissGuestDialog(false); });
            Find<Button>(L"GuestDialogCancel").Click([this](auto const&, auto const&) { DismissGuestDialog(true); });
            Find<Button>(L"OpenLibrary").Click([this](auto const&, auto const&) { ShowPage(Page::Library); });
            Find<Button>(L"EmptyImport").Click([this](auto const&, auto const&) { ShowPage(Page::Library); SelectContent(); });
            Find<Button>(L"PendingEmptyImport").Click([this](auto const&, auto const&) { SelectContent(); });
            Find<Button>(L"LibraryBack").Click([this](auto const&, auto const&) { ShowPage(Page::Home); });
            Find<Button>(L"OpenSettings").Click([this](auto const&, auto const&) { ShowPage(Page::Settings); });
            Find<Button>(L"SettingsBack").Click([this](auto const&, auto const&) { ShowPage(Page::Home); });
            Find<Button>(L"DiagnosticsBack").Click([this](auto const&, auto const&) { ShowPage(Page::Settings); });
            Find<Button>(L"SelectContent").Click([this](auto const&, auto const&) { SelectContent(); });
            Find<Button>(L"ImportKeys").Click([this](auto const&, auto const&) { ImportKeys(); });
            Find<Button>(L"ExtractContent").Click([this](auto const&, auto const&) { ExtractContent(); });
            Find<Button>(L"ValidateContent").Click([this](auto const&, auto const&) { ValidateSelectedContent(); });
            Find<Button>(L"TestAngle").Click([this](auto const&, auto const&) { StartAngleTest(); });
            Find<Button>(L"CloseAngleTest").Click([this](auto const&, auto const&) { FinishAngleTest(); });
            Find<Button>(L"CancelExtraction").Click([this](auto const&, auto const&) {
                if (extraction) {
                    extraction->cancel.store(true);
                    Find<Button>(L"CancelExtraction").IsEnabled(false);
                    Find<TextBlock>(L"InstallProgressText").Text(L"Cancelando e limpando os arquivos temporários…");
                }
            });
            Find<Button>(L"Export").Click([this](auto const&, auto const&) { ExportReport(); });
            Find<Button>(L"DiagnosticsExport").Click([this](auto const&, auto const&) { ExportReport(); });
            Window::Current().CoreWindow().KeyDown([this](Core::CoreWindow const& sender, Core::KeyEventArgs const& args) {
                OnGamepadKey(sender, args);
            });
            // XS4 and the guest use a gamepad-first UI; hide the system mouse
            // cursor while this window is active.
            Window::Current().CoreWindow().PointerCursor(nullptr);
            timer = DispatcherTimer(); timer.Interval(std::chrono::milliseconds(100));
            timer.Tick([this](auto const&, auto const&) {
                if (homebrew && homebrew->running() && !guestPaused) {
                    try {
                        auto dialog = homebrew->GetMessageDialog();
                        if (dialog.status == 2) {
                            if (dialog.generation != guestDialogGeneration ||
                                Find<Grid>(L"GuestDialogOverlay").Visibility() != Visibility::Visible) {
                                guestDialogGeneration = dialog.generation;
                                try { Find<TextBlock>(L"GuestDialogMessage").Text(to_hstring(dialog.message)); }
                                catch (...) { Find<TextBlock>(L"GuestDialogMessage").Text(L"O conteúdo abriu uma caixa de diálogo."); }
                                Find<Grid>(L"GuestDialogOverlay").Visibility(Visibility::Visible);
                                Find<Button>(L"GuestDialogAccept").Focus(FocusState::Programmatic);
                            }
                        } else Find<Grid>(L"GuestDialogOverlay").Visibility(Visibility::Collapsed);
                    } catch (...) {}
                } else Find<Grid>(L"GuestDialogOverlay").Visibility(Visibility::Collapsed);
                bool shortcutDown = false;
                try {
                    using namespace Windows::Gaming::Input;
                    auto pads = Gamepad::Gamepads();
                    if (pads.Size()) {
                        const auto buttons = pads.GetAt(0).GetCurrentReading().Buttons;
                        shortcutDown = (buttons & GamepadButtons::Menu) != GamepadButtons::None &&
                                       (buttons & GamepadButtons::View) != GamepadButtons::None;
                    }
                } catch (...) {}
                if (shortcutDown && !pauseComboHeld && homebrew && homebrew->running()) {
                    if (guestPaused) ResumeContent();
                    else PauseContent();
                }
                pauseComboHeld = shortcutDown;
                if (anglePending) {
                    anglePending = false;
                    std::wstring detail;
                    try {
                        angleActive = angleVideo.Start(Find<SwapChainPanel>(L"AnglePanel"), detail);
                    } catch (winrt::hresult_error const& e) { detail = std::wstring(e.message()); angleActive = false; }
                      catch (std::exception const& e) { detail = std::wstring(to_hstring(e.what())); angleActive = false; }
                    Find<TextBlock>(L"AngleStatus").Text((angleActive ? L"Aprovado: " : L"Falhou: ") + detail +
                        L". Confira visualmente o fundo azul e exporte o JSON.");
                    RecordAngle(angleActive ? L"approved" : L"failed", detail);
                    if (homebrewLaunchPending) {
                        homebrewLaunchPending = false;
                        if (angleActive && angleVideo.ReleaseForGuest()) LaunchGuest();
                        else {
                            Find<Border>(L"AngleLoadingView").Visibility(Visibility::Collapsed);
                            Find<Border>(L"AngleTestControls").Visibility(Visibility::Visible);
                            Find<TextBlock>(L"AngleStatus").Text(L"Não foi possível entregar o contexto EGL ao homebrew: " + detail);
                            Find<Button>(L"CloseAngleTest").IsEnabled(true);
                            SetNotice(L"O vídeo não está disponível. O conteúdo não foi iniciado.");
                        }
                    }
                }
                if (homebrew && homebrew->running() && !guestViewShown && angleVideo.GuestFrames() > 0) {
                    guestViewShown = true;
                    Find<Border>(L"AngleLoadingView").Visibility(Visibility::Collapsed);
                }
                if (extraction) {
                    const auto percent = extraction->percent.load();
                    Find<ProgressBar>(L"InstallProgress").Value(percent);
                    Find<TextBlock>(L"InstallProgressPercent").Text(std::to_wstring(percent) + L"%");
                    if (!extraction->cancel.load()) {
                        Find<TextBlock>(L"InstallProgressText").Text(L"Instalando · " +
                            std::to_wstring(extraction->files.load()) + L" arquivos · " +
                            std::to_wstring(extraction->bytes.load() / (1024 * 1024)) + L" MB processados");
                    }
                }
                if (homebrew && !homebrew->running() && !homebrewCompletionShown) {
                    homebrewCompletionShown = true;
                    guestPaused = false;
                    Find<Grid>(L"GuestDialogOverlay").Visibility(Visibility::Collapsed);
                    Find<Button>(L"ResumeContent").Visibility(Visibility::Collapsed);
                    Find<Button>(L"EndContent").Visibility(Visibility::Collapsed);
                    Find<Border>(L"AngleLoadingView").Visibility(Visibility::Collapsed);
                    Find<Border>(L"AngleTestControls").Visibility(Visibility::Visible);
                    Find<Button>(L"CloseAngleTest").IsEnabled(true);
                    auto statePath = std::filesystem::path(report->directory) / L"homebrew-runtime.json";
                    try {
                        std::ifstream input(statePath, std::ios::binary);
                        std::string raw{std::istreambuf_iterator<char>(input), {}};
                        auto state = Windows::Data::Json::JsonObject::Parse(to_hstring(raw));
                        auto detail = std::wstring(state.GetNamedString(L"detail", L"O processo terminou."));
                        SetNotice(L"O conteúdo foi encerrado. Consulte Configurações para exportar o relatório, se necessário.");
                        Find<TextBlock>(L"AngleStatus").Text(L"Conteúdo encerrado. " + detail +
                            L" Pressione B para voltar à tela anterior.");
                    } catch (...) {
                        SetNotice(L"O conteúdo foi encerrado. Abra Configurações para exportar o relatório, se necessário.");
                        Find<TextBlock>(L"AngleStatus").Text(L"Conteúdo encerrado. Pressione B para voltar à tela anterior.");
                    }
                }
            });
            timer.Start();
            Refresh();
            if (!report->recoveryNotice.empty()) Find<TextBlock>(L"SettingsStatus").Text(report->recoveryNotice);
            Save();
            Window::Current().Content(root); Window::Current().Activate();
            auto extractionReport = std::filesystem::path(report->directory) / L"extraction-report.json";
            if (std::filesystem::exists(extractionReport)) {
                try {
                    std::ifstream input(extractionReport, std::ios::binary); std::string raw{std::istreambuf_iterator<char>(input), {}};
                    auto previous = Windows::Data::Json::JsonObject::Parse(to_hstring(raw));
                    if (previous.GetNamedString(L"status", L"") == L"running") {
                        previous.SetNamedValue(L"status", Windows::Data::Json::JsonValue::CreateStringValue(L"inconclusive"));
                        Lab::WriteDurable(extractionReport.wstring(), to_string(previous.Stringify()));
                        Find<TextBlock>(L"SettingsStatus").Text(L"A instalação anterior foi interrompida e o relatório foi marcado como inconclusivo. Selecione o PKG na Biblioteca para tentar novamente.");
                    }
                } catch (...) { Find<TextBlock>(L"SettingsStatus").Text(L"O relatório da instalação anterior está ilegível; o arquivo foi preservado."); }
            }
            SetHomeCategory(HomeCategory::Games);
            SetHomeFilter(HomeFilter::All);
            ShowPage(Page::Home);
            PopulateLibrary();
            StartupLog(L"Window activated; home screen ready");
        } catch (hresult_error const& e) {
            StartupLog(L"OnLaunched HRESULT: " + std::to_wstring(static_cast<uint32_t>(e.code().value)) + L" " + std::wstring(e.message()));
            ShowStartupError(e.message());
        } catch (std::exception const& e) {
            StartupLog(L"OnLaunched C++ exception: " + std::wstring(to_hstring(e.what())));
            ShowStartupError(to_hstring(e.what()));
        }
    }
    void ShowStartupError(hstring const& message) {
        TextBlock error; error.Text(L"Falha ao iniciar XS4: " + message);
        error.TextWrapping(TextWrapping::Wrap); error.Margin({48,48,48,48});
        Window::Current().Content(error); Window::Current().Activate();
    }
    void Refresh() {
        int index = list.SelectedIndex(); refreshing = true;
        list.Items().Clear();
        for (auto const& t : report->tests) {
            TextBlock text; text.Text(Lab::StatusLabel(t.status) + L"\n" + t.title);
            text.TextWrapping(TextWrapping::Wrap); text.FontSize(16); text.Margin({0, 6, 0, 6});
            list.Items().Append(text);
        }
        list.SelectedIndex(report->tests.empty() ? -1 : (index < 0 ? 0 : index)); refreshing = false; ShowDetails();
    }
    void ShowDetails() {
        auto i = list.SelectedIndex(); if (i < 0) return;
        auto const& t = report->tests[i];
        details.Text(t.title + L"\n" + Lab::StatusLabel(t.status) + L"\n\n" +
            (t.detail.empty() ? L"Nenhuma evidência coletada." : t.detail) + L"\n" + t.error +
            L"\n\n" + std::wstring(t.measurements.Stringify()));
    }
    bool CanRun() {
        if (persistenceFailed) { status.Text(L"Persistência indisponível. Feche e reabra após corrigir o armazenamento."); return false; }
        if (busy) { status.Text(L"Um teste está em execução."); return false; }
        return true;
    }
    IAsyncAction Run(int index) {
        auto lifetime = get_strong();
        auto& original = report->tests[index];
        original.status = L"running"; original.detail = L"Teste iniciado; registro persistido antes da execução.";
        original.error.clear(); original.measurements = Windows::Data::Json::JsonObject();
        original.startedAt = Lab::Now(); original.durationMs = 0;
        // Never run a potentially terminating probe if its start record cannot be persisted.
        if (!Save()) co_return;
        Refresh(); status.Text(L"Executando: " + original.title);
        auto start = std::chrono::steady_clock::now();
        try {
            apartment_context ui;
            // Measurements are collected off the UI thread and transferred as JSON.
            auto id = original.id; auto executablePath = selectedLoaderPath;
            std::wstring serialized, probeStatus, probeDetail, probeError;
            co_await resume_background();
            try {
                Lab::Test result; result.id = id;
                Lab::RunProbe(result, executablePath, report->directory);
                serialized = result.measurements.Stringify(); probeStatus = result.status; probeDetail = result.detail;
            } catch (hresult_error const& e) {
                auto hr = e.code();
                probeStatus = (hr == E_ACCESSDENIED || hr == E_NOTIMPL ||
                    hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)) ? L"unavailable" : L"failed";
                probeError = std::to_wstring(static_cast<uint32_t>(hr.value)) + L": " + std::wstring(e.message());
                probeDetail = L"O probe não concluiu. O arquivo selecionado não foi executado.";
            } catch (std::exception const& e) {
                probeStatus = L"failed"; probeError = to_hstring(e.what());
            }
            co_await ui;
            original.status = probeStatus; original.detail = probeDetail; original.error = probeError;
            if (!serialized.empty()) original.measurements = Windows::Data::Json::JsonObject::Parse(serialized);
        } catch (hresult_error const& e) {
            original.status = L"failed"; original.error = e.message();
        }
        original.durationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        bool saved = Save(); Refresh();
        if (saved) status.Text(Lab::StatusLabel(original.status) + L" · " + original.title);
    }
    fire_and_forget RunSelected() {
        auto lifetime = get_strong(); if (!CanRun()) co_return;
        auto index = list.SelectedIndex(); if (index < 0) co_return;
        busy = true;
        try { co_await Run(index); } catch (hresult_error const& e) { status.Text(L"Erro: " + e.message()); }
        busy = false;
    }
    fire_and_forget RunAll() {
        auto lifetime = get_strong(); if (!CanRun()) co_return;
        busy = true;
        try {
            for (size_t i = 0; i < report->tests.size() && !persistenceFailed; ++i)
                if (!report->tests[i].isolated) co_await Run(static_cast<int>(i));
            if (!persistenceFailed) status.Text(L"Probe concluído. O arquivo selecionado nunca foi executado.");
        } catch (hresult_error const& e) { status.Text(L"Erro: " + e.message()); }
        busy = false;
    }
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        // UWP starts from an MTA; Application::Start creates the XAML view thread.
        // The desktop WinUI/STA bootstrap is not the UWP activation model.
        init_apartment(apartment_type::multi_threaded);
        StartupLog(L"Process entered; commit " + std::wstring(XBOX_BUILD_COMMIT));
        Application::Start([](auto&&) {
            StartupLog(L"Application::Start callback");
            make<App>();
        });
        StartupLog(L"Application::Start returned");
        return 0;
    } catch (hresult_error const& e) {
        StartupLog(L"Bootstrap HRESULT: " + std::to_wstring(static_cast<uint32_t>(e.code().value)) + L" " + std::wstring(e.message()));
        return static_cast<int>(e.code().value);
    } catch (std::exception const& e) {
        StartupLog(L"Bootstrap C++ exception: " + std::wstring(to_hstring(e.what())));
        return 1;
    }
}
