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
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.System.Display.h>
#include <filesystem>
#include <array>
#include <fstream>
#include <iterator>
#include <set>
#include <chrono>
#include <deque>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

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
    Grid root{nullptr}; Grid libraryView{nullptr}, diagnosticsView{nullptr};
    ListView list{nullptr}, installedList{nullptr}, pendingList{nullptr};
    TextBlock status{nullptr}, details{nullptr}, libraryStatus{nullptr};
    DispatcherTimer timer{nullptr};
    bool busy{}, refreshing{}, persistenceFailed{};
    struct LibraryItem { std::wstring name, path; bool installed{}; };
    std::vector<LibraryItem> libraryItems;
    std::vector<size_t> installedIndices, pendingIndices;
    std::wstring selectedLoaderPath;
    std::shared_ptr<Lab::InstallProgress> extraction;
    std::unique_ptr<Lab::HomebrewRuntime> homebrew;
    Lab::AngleVideo angleVideo;
    bool anglePending{}, angleActive{};
    bool homebrewLaunchPending{};
    bool homebrewCompletionShown{};
    bool importing{}, listing{};
    Windows::System::Display::DisplayRequest displayRequest{nullptr};

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
                    libraryStatus.Text(L"Chaves personalizadas importadas. O keyset FPKG embutido será substituído nesta extração.");
                } catch (...) {
                    std::error_code ignored; std::filesystem::remove(std::filesystem::path(pendingKey.Path().c_str()), ignored); throw;
                }
            }
        } catch (...) { libraryStatus.Text(L"Não foi possível importar: JSON inválido ou conjuntos RSA-2048 incompletos. As chaves anteriores foram preservadas."); }
        importing = false;
    }
    int SelectedLibraryIndex() const {
        auto index = installedList.SelectedIndex();
        if (index >= 0 && static_cast<size_t>(index) < installedIndices.size())
            return static_cast<int>(installedIndices[index]);
        index = pendingList.SelectedIndex();
        if (index >= 0 && static_cast<size_t>(index) < pendingIndices.size())
            return static_cast<int>(pendingIndices[index]);
        return -1;
    }
    void LibrarySelection() {
        auto index = SelectedLibraryIndex();
        if (index < 0 || static_cast<size_t>(index) >= libraryItems.size()) {
            selectedLoaderPath.clear();
            Find<TextBlock>(L"ContentTitle").Text(L"Selecione um conteúdo");
            Find<TextBlock>(L"ContentState").Text(L"Nenhum item selecionado.");
            Find<TextBlock>(L"ContentDetails").Text(L"Escolha um item em uma das seções acima.");
            Find<Button>(L"LaunchContent").IsEnabled(false);
            Find<Button>(L"ExtractContent").IsEnabled(false);
            Find<Button>(L"ValidateContent").IsEnabled(false);
            return;
        }
        auto const& item = libraryItems[static_cast<size_t>(index)];
        selectedLoaderPath.clear();
        if (item.installed) {
            auto eboot = std::filesystem::path(item.path) / L"eboot.bin";
            if (std::filesystem::is_regular_file(eboot)) selectedLoaderPath = eboot.wstring();
        } else {
            auto extension = std::filesystem::path(item.path).extension().wstring();
            for (auto& c : extension) c = towlower(c);
            if (extension == L".elf" || extension == L".self" || extension == L".bin")
                selectedLoaderPath = item.path;
        }
        auto extension = std::filesystem::path(item.path).extension().wstring();
        for (auto& c : extension) c = towlower(c);
        const bool package = !item.installed && extension == L".pkg";
        const bool looseExecutable = !item.installed && !selectedLoaderPath.empty();
        auto launch = Find<Button>(L"LaunchContent");
        auto extract = Find<Button>(L"ExtractContent");
        auto validate = Find<Button>(L"ValidateContent");
        launch.IsEnabled(!extraction && !importing && !selectedLoaderPath.empty() && !(homebrew && homebrew->running()));
        extract.IsEnabled(!extraction && !importing && package);
        validate.IsEnabled(!extraction && !importing && looseExecutable);
        Find<TextBlock>(L"ContentTitle").Text(item.name);
        Find<TextBlock>(L"ContentState").Text(item.installed ? L"INSTALADO · pronto para iniciar" :
            package ? L"PKG IMPORTADO · extração necessária" : L"ARQUIVO AVULSO · validação técnica");
        Find<TextBlock>(L"ContentDetails").Text(item.installed ?
            (selectedLoaderPath.empty() ? L"Instalação incompleta: eboot.bin ausente." :
             L"Conteúdo extraído e persistido. A execução PS4 ainda está em desenvolvimento.") :
            package ? L"Este pacote está na biblioteca, mas ainda não foi extraído. Selecione Extrair e instalar PKG." :
            DescribeContent(item.path));
    }
    void ValidateSelectedContent() {
        if (selectedLoaderPath.empty()) {
            libraryStatus.Text(L"Selecione um ELF/SELF importado ou um conteúdo extraído com eboot.bin.");
            return;
        }
        ShowLibrary(false);
        list.SelectedIndex(0);
        RunSelected();
    }
    void StartHomebrew() {
        if (selectedLoaderPath.empty()) {
            libraryStatus.Text(L"Selecione um conteúdo extraído com eboot.bin.");
            return;
        }
        if (homebrew && homebrew->running()) {
            libraryStatus.Text(L"O homebrew já está em execução.");
            return;
        }
        homebrewLaunchPending = true;
        anglePending = true;
        Find<Grid>(L"AngleTestView").Visibility(Visibility::Visible);
        Find<TextBlock>(L"AngleStatus").Text(L"Preparando vídeo do homebrew…");
        Find<Button>(L"CloseAngleTest").IsEnabled(false);
        Find<Button>(L"LaunchContent").IsEnabled(false);
        RecordAngle(L"running", L"Inicialização EGL do homebrew iniciada.");
    }
    void LaunchGuest() {
        try {
            homebrew = std::make_unique<Lab::HomebrewRuntime>();
            homebrewCompletionShown = false;
            homebrew->Start(std::filesystem::path(selectedLoaderPath),
                            std::filesystem::path(report->directory), &angleVideo);
            libraryStatus.Text(L"Homebrew em execução; a superfície gráfica EGL está conectada ao Xbox.");
            Find<TextBlock>(L"AngleStatus").Text(L"Homebrew em execução. A imagem aparecerá quando o programa apresentar o primeiro quadro.");
            Find<Button>(L"LaunchContent").IsEnabled(false);
        } catch (std::exception const& e) {
            homebrew.reset();
            libraryStatus.Text(L"Não foi possível iniciar o Apollo: " + std::wstring(to_hstring(e.what())));
            Find<Button>(L"CloseAngleTest").IsEnabled(true);
        } catch (hresult_error const& e) {
            homebrew.reset();
            libraryStatus.Text(L"Não foi possível iniciar o Apollo: " + std::wstring(e.message()));
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
        auto index = SelectedLibraryIndex();
        if (extraction || importing || index < 0 || static_cast<size_t>(index) >= libraryItems.size() || libraryItems[index].installed) co_return;
        auto item = libraryItems[index];
        auto state = std::make_shared<Lab::InstallProgress>();
        extraction = state;
        Find<Button>(L"ExtractContent").IsEnabled(false);
        Find<Button>(L"SelectContent").IsEnabled(false); Find<Button>(L"ImportKeys").IsEnabled(false);
        Find<Button>(L"CancelExtraction").Visibility(Visibility::Visible);
        Find<Button>(L"CancelExtraction").IsEnabled(true);
        Find<ProgressBar>(L"InstallProgress").Visibility(Visibility::Visible);
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
        libraryStatus.Text(completed ? L"Extração concluída. Relatório: LocalState/extraction-report.json." + error : L"Extração não concluída: " + error);
        extraction.reset();
        Find<Button>(L"SelectContent").IsEnabled(true); Find<Button>(L"ImportKeys").IsEnabled(true);
        Find<Button>(L"CancelExtraction").IsEnabled(false);
        Find<Button>(L"CancelExtraction").Visibility(Visibility::Collapsed);
        Find<ProgressBar>(L"InstallProgress").Visibility(Visibility::Collapsed);
        Find<ProgressBar>(L"InstallProgress").Value(completed ? 100 : 0);
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
        std::array<unsigned char, 4> header{};
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        const auto magic = (static_cast<std::uint32_t>(header[0]) << 24u) |
            (static_cast<std::uint32_t>(header[1]) << 16u) |
            (static_cast<std::uint32_t>(header[2]) << 8u) | static_cast<std::uint32_t>(header[3]);
        if (input.gcount() == static_cast<std::streamsize>(header.size()) && magic == 0x7F434E54u)
            return Lab::ProbePkg(std::filesystem::path(path)).detail;
        return error ? L"Arquivo selecionado; tamanho indisponível." :
            L"Arquivo selecionado · " + std::to_wstring(size) + L" bytes\nFormato ainda não analisado.";
    }
    void ShowLibrary(bool visible) {
        libraryView.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        diagnosticsView.Visibility(visible ? Visibility::Collapsed : Visibility::Visible);
        if (visible) PopulateLibrary();
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
            installedList.Items().Clear(); pendingList.Items().Clear();
            libraryItems.clear();
            auto add = [&](std::wstring name, std::wstring path, bool installed) {
                auto index = libraryItems.size();
                libraryItems.push_back({name, path, installed});
                auto& indices = installed ? installedIndices : pendingIndices;
                indices.push_back(index);
                StackPanel card; card.Width(142); card.Spacing(2); card.Margin({5, 2, 5, 2});
                Border art; art.Width(142); art.Height(50);
                art.Background(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 12, 75, 167)));
                TextBlock glyph; glyph.Text(installed ? L"\xE7FC" : L"\xE8B7"); glyph.FontFamily(Media::FontFamily(L"Segoe MDL2 Assets"));
                glyph.FontSize(38); glyph.HorizontalAlignment(HorizontalAlignment::Center); glyph.VerticalAlignment(VerticalAlignment::Center); art.Child(glyph);
                auto iconPath = std::filesystem::path(path) / L"sce_sys" / L"icon0.png";
                if (installed && std::filesystem::is_regular_file(iconPath) && std::filesystem::file_size(iconPath) <= 8 * 1024 * 1024) {
                    Image image;
                    auto folderName = std::filesystem::path(path).filename().wstring();
                    Media::Imaging::BitmapImage cover;
                    cover.DecodePixelWidth(284); cover.DecodePixelHeight(100);
                    cover.UriSource(Uri(L"ms-appdata:///local/Installed/" + folderName + L"/sce_sys/icon0.png"));
                    image.Source(cover);
                    image.Stretch(Media::Stretch::UniformToFill); art.Child(image);
                }
                card.Children().Append(art);
                TextBlock title; title.Text(name); title.FontSize(13); title.MaxLines(1); title.TextTrimming(TextTrimming::CharacterEllipsis); card.Children().Append(title);
                TextBlock badge; badge.Text(installed ? L"INSTALADO" :
                    (std::filesystem::path(path).extension() == L".pkg" ? L"PKG · A EXTRAIR" : L"ARQUIVO AVULSO"));
                badge.FontSize(10); badge.Foreground(Media::SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 154, 233, 255))); card.Children().Append(badge);
                (installed ? installedList : pendingList).Items().Append(card);
            };
            auto installed = co_await local.CreateFolderAsync(L"Installed", Windows::Storage::CreationCollisionOption::OpenIfExists);
            auto folders = co_await installed.GetFoldersAsync();
            std::set<std::wstring> installedPackages;
            for (auto const& game : folders) {
                auto titlePath = std::filesystem::path(game.Path().c_str()) / L"library-name.txt";
                std::ifstream titleFile(titlePath, std::ios::binary);
                std::string name{std::istreambuf_iterator<char>(titleFile), {}};
                auto title = name.empty() ? std::wstring(game.Name()) : std::wstring(to_hstring(name));
                installedPackages.insert(title);
                add(title, std::wstring(game.Path()), true);
            }
            for (auto const& file : files) {
                if (file.FileType() == L".pkg" && installedPackages.contains(std::wstring(file.Name()))) continue;
                add(std::wstring(file.Name()), std::wstring(file.Path()), false);
            }
            Find<TextBlock>(L"InstalledHeading").Text(L"INSTALADOS · " + std::to_wstring(installedIndices.size()));
            Find<TextBlock>(L"PendingHeading").Text(L"A EXTRAIR / ARQUIVOS AVULSOS · " + std::to_wstring(pendingIndices.size()));
            if (!installedIndices.empty()) installedList.SelectedIndex(0);
            else if (!pendingIndices.empty()) pendingList.SelectedIndex(0);
            else LibrarySelection();
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
            co_await file.CopyAsync(folder, file.Name(), Windows::Storage::NameCollisionOption::GenerateUniqueName);
            libraryStatus.Text(std::wstring(L"Arquivo copiado: ") + std::wstring(file.Name()) +
                L"\n" + DescribeContent(std::wstring(file.Path())));
            PopulateLibrary();
        } catch (hresult_error const& e) {
            libraryStatus.Text(L"Falha ao selecionar conteúdo: " + e.message());
        } catch (...) { libraryStatus.Text(L"Falha ao importar conteúdo."); }
        importing = false;
        LibrarySelection();
    }
    bool Save() {
        try { report->Save(); return true; }
        catch (hresult_error const& e) {
            persistenceFailed = true;
            if (status) status.Text(L"Falha ao persistir; testes bloqueados: " + e.message());
        } catch (std::exception const&) {
            persistenceFailed = true;
            if (status) status.Text(L"Falha de armazenamento; testes bloqueados.");
        }
        return false;
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
        angleVideo.Stop(); angleActive = false;
        Find<Grid>(L"AngleTestView").Visibility(Visibility::Visible);
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
        Find<Button>(L"LibraryTab").Focus(FocusState::Programmatic);
    }
    void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
        StartupLog(L"OnLaunched entered");
        if (root) { Window::Current().Activate(); return; }
        try {
            auto path = std::filesystem::path(Windows::ApplicationModel::Package::Current().InstalledLocation().Path().c_str()) / L"MainPage.xaml";
            std::ifstream file(path, std::ios::binary);
            if (!file) throw hresult_error(E_FAIL, L"MainPage.xaml ausente no pacote.");
            std::string xaml{std::istreambuf_iterator<char>(file), {}};
            StartupLog(L"Loading MainPage.xaml");
            root = Markup::XamlReader::Load(to_hstring(xaml)).as<Grid>();
            libraryView = Find<Grid>(L"LibraryView"); diagnosticsView = Find<Grid>(L"DiagnosticsView");
            list = Find<ListView>(L"Tests");
            installedList = Find<ListView>(L"InstalledList");
            pendingList = Find<ListView>(L"PendingList");
            status = Find<TextBlock>(L"Status"); details = Find<TextBlock>(L"Details");
            libraryStatus = Find<TextBlock>(L"LibraryStatus");
            StartupLog(L"XAML loaded; opening report");
            report = std::make_unique<Lab::Report>();
            Find<TextBlock>(L"DeviceInfo").Text(report->Summary());
            list.SelectionChanged([this](auto const&, auto const&) { if (!refreshing) ShowDetails(); });
            Find<Button>(L"RunAll").Click([this](auto const&, auto const&) { RunAll(); });
            Find<Button>(L"RunSelected").Click([this](auto const&, auto const&) { RunSelected(); });
            Find<Button>(L"LibraryTab").Click([this](auto const&, auto const&) { ShowLibrary(true); });
            Find<Button>(L"DiagnosticsTab").Click([this](auto const&, auto const&) { ShowLibrary(false); });
            Find<Button>(L"SelectContent").Click([this](auto const&, auto const&) { SelectContent(); });
            Find<Button>(L"ImportKeys").Click([this](auto const&, auto const&) { ImportKeys(); });
            Find<Button>(L"ExtractContent").Click([this](auto const&, auto const&) { ExtractContent(); });
            Find<Button>(L"ValidateContent").Click([this](auto const&, auto const&) { ValidateSelectedContent(); });
            Find<Button>(L"LaunchContent").Click([this](auto const&, auto const&) { StartHomebrew(); });
            Find<Button>(L"TestAngle").Click([this](auto const&, auto const&) { StartAngleTest(); });
            Find<Button>(L"CloseAngleTest").Click([this](auto const&, auto const&) { FinishAngleTest(); });
            Find<Button>(L"CancelExtraction").Click([this](auto const&, auto const&) { if (extraction) extraction->cancel.store(true); });
            installedList.SelectionChanged([this](auto const&, auto const&) {
                if (installedList.SelectedIndex() >= 0) pendingList.SelectedIndex(-1);
                LibrarySelection();
            });
            pendingList.SelectionChanged([this](auto const&, auto const&) {
                if (pendingList.SelectedIndex() >= 0) installedList.SelectedIndex(-1);
                LibrarySelection();
            });
            Find<Button>(L"Export").Click([this](auto const&, auto const&) {
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
                        if (sessionName.empty()) {
                            if (!sessionId.empty())
                                sessionName = L"homebrew-session-" + sessionId + L".jsonl";
                        }
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
                    std::string payload = to_string(exported.Stringify());
                    Lab::WriteDurable(report->directory + L"\\" + name, payload);
                    status.Text(L"Diagnóstico completo exportado para LocalState\\" + name + L". Baixe pelo Device Portal.");
                } catch (hresult_error const& e) { status.Text(L"Falha na exportação: " + e.message()); }
                  catch (std::exception const& e) { status.Text(L"Falha na exportação: " + std::wstring(to_hstring(e.what()))); }
            });
            timer = DispatcherTimer(); timer.Interval(std::chrono::milliseconds(100));
            timer.Tick([this](auto const&, auto const&) {
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
                            Find<TextBlock>(L"AngleStatus").Text(L"Não foi possível entregar o contexto EGL ao homebrew: " + detail);
                            Find<Button>(L"CloseAngleTest").IsEnabled(true);
                            libraryStatus.Text(L"Vídeo indisponível. Homebrew não iniciado.");
                        }
                    }
                }
                if (extraction) {
                    Find<ProgressBar>(L"InstallProgress").Value(extraction->percent.load());
                    libraryStatus.Text(L"Extraindo · " + std::to_wstring(extraction->files.load()) + L" arquivos · " +
                        std::to_wstring(extraction->bytes.load() / (1024 * 1024)) + L" MiB · mantenha o aplicativo aberto");
                }
                if (homebrew && !homebrew->running() && !homebrewCompletionShown) {
                    homebrewCompletionShown = true;
                    Find<Button>(L"CloseAngleTest").IsEnabled(true);
                    auto statePath = std::filesystem::path(report->directory) / L"homebrew-runtime.json";
                    try {
                        std::ifstream input(statePath, std::ios::binary);
                        std::string raw{std::istreambuf_iterator<char>(input), {}};
                        auto state = Windows::Data::Json::JsonObject::Parse(to_hstring(raw));
                        libraryStatus.Text(L"Homebrew encerrado: " + std::wstring(state.GetNamedString(L"detail")) +
                                           L" Exporte o relatório para análise.");
                        Find<TextBlock>(L"AngleStatus").Text(L"Homebrew encerrado. " +
                            std::wstring(state.GetNamedString(L"detail")) + L" Volte à biblioteca e exporte o relatório.");
                    } catch (...) {
                        libraryStatus.Text(L"Homebrew encerrado. Exporte o relatório para análise.");
                        Find<TextBlock>(L"AngleStatus").Text(L"Homebrew encerrado. Volte à biblioteca e exporte o relatório.");
                    }
                    LibrarySelection();
                }
            });
            timer.Start();
            Refresh();
            if (!report->recoveryNotice.empty()) status.Text(report->recoveryNotice);
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
                        libraryStatus.Text(L"A extração anterior foi interrompida. Relatório marcado como inconclusivo; importe ou selecione o PKG para tentar novamente.");
                    }
                } catch (...) { libraryStatus.Text(L"Relatório anterior de extração ilegível; arquivo preservado."); }
            }
            auto runtimeReport = std::filesystem::path(report->directory) / L"homebrew-runtime.json";
            if (std::filesystem::exists(runtimeReport)) {
                try {
                    std::ifstream input(runtimeReport, std::ios::binary);
                    std::string raw{std::istreambuf_iterator<char>(input), {}};
                    auto previous = Windows::Data::Json::JsonObject::Parse(to_hstring(raw));
                    auto stage = previous.GetNamedString(L"stage", L"unknown");
                    auto detail = previous.GetNamedString(L"detail", L"");
                    auto session = previous.GetNamedString(L"session_file", L"");
                    auto message = L"Última execução · " + std::wstring(stage) + L"\n" + std::wstring(detail);
                    if (!session.empty()) message += L"\nSessão: " + std::wstring(session);
                    libraryStatus.Text(message);
                } catch (...) { libraryStatus.Text(L"O registro da última execução está ilegível."); }
            }
            ShowLibrary(true);
            Find<Button>(L"LibraryTab").Focus(FocusState::Programmatic);
            StartupLog(L"Window activated; ready for explicit tests");
        } catch (hresult_error const& e) {
            StartupLog(L"OnLaunched HRESULT: " + std::to_wstring(static_cast<uint32_t>(e.code().value)) + L" " + std::wstring(e.message()));
            ShowStartupError(e.message());
        } catch (std::exception const& e) {
            StartupLog(L"OnLaunched C++ exception: " + std::wstring(to_hstring(e.what())));
            ShowStartupError(to_hstring(e.what()));
        }
    }
    void ShowStartupError(hstring const& message) {
        TextBlock error; error.Text(L"Falha ao iniciar Xbox Lab: " + message);
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
