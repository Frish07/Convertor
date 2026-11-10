#include "canvas.hpp"
#include "media.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using namespace ui;
using namespace media;

constexpr UINT noticeMessage = WM_APP + 1;
constexpr int rowHeight = 68;
constexpr int rowStep = 76;
constexpr std::size_t allRows = static_cast<std::size_t>(-1);

struct Category {
    Kind kind;
    const wchar_t* tab;
    const wchar_t* title;
    const wchar_t* description;
    const wchar_t* detail;
    const wchar_t* example;
    const wchar_t* target;
    Icon icon;
};

constexpr std::array categories{
    Category{Kind::video, L"Видео", L"Конвертер видео",
             L"Меняйте формат видео и извлекайте звуковую дорожку.",
             L"MP4, MKV, WebM, MOV и аудиоформаты", L"MKV", L"mp4", Icon::video},
    Category{Kind::image, L"Изображения", L"Конвертер изображений",
             L"Конвертируйте изображения локально, сохраняя один кадр.",
             L"PNG, JPG, WebP, BMP и TIFF", L"WEBP", L"png", Icon::image},
    Category{Kind::audio, L"Аудио", L"Конвертер аудио",
             L"Меняйте формат музыки, записей и других аудиофайлов.",
             L"MP3, WAV, FLAC, M4A, OGG, Opus и AAC", L"OGG", L"mp3", Icon::audio},
};

const Category& category(Kind kind) {
    return categories[static_cast<std::size_t>(kind)];
}

std::wstring upper(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), towupper);
    return text;
}

std::wstring targetName(int target) {
    return upper(formats()[target].extension);
}

bool accepts(Kind pane, Kind output) {
    return pane == output || (pane == Kind::video && output == Kind::audio);
}

std::wstring fileSize(std::uintmax_t bytes) {
    if (bytes >= 1024 * 1024) {
        auto tenths = bytes / (1024 * 1024 / 10);
        return std::to_wstring(tenths / 10) + L"," + std::to_wstring(tenths % 10) + L" МБ";
    }
    if (bytes >= 1024) return std::to_wstring(bytes / 1024) + L" КБ";
    return std::to_wstring(bytes) + L" Б";
}

template<class T> struct Release {
    void operator()(T* ptr) const { ptr->Release(); }
};
template<class T> using Com = std::unique_ptr<T, Release<T>>;

Com<IFileOpenDialog> dialog() {
    IFileOpenDialog* picker = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker)))) {
        throw Failure{L"Не удалось открыть системный диалог Windows."};
    }
    return Com<IFileOpenDialog>(picker);
}

fs::path itemPath(IShellItem* item) {
    wchar_t* name = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) return {};
    fs::path path(name);
    CoTaskMemFree(name);
    return path;
}

enum class FileState { ready, queued, active, done, error, cancelled };
enum class Operation { idle, checking, installing, batch };
enum class Event { tools, progress, begin, result, failure, cancelled, finished };
enum class Action { tab, add, format, remove, open, folder, start, engine, option };

struct File {
    fs::path source;
    std::uintmax_t bytes;
    int target;
    FileState state = FileState::ready;
    fs::path result;
    std::wstring error;
};

struct Pane {
    std::vector<File> files;
    fs::path directory;
    int target = 0;
    int scroll = 0;
};

struct Hit {
    Box box;
    Action action;
    std::size_t index;
};

struct FormatMenu {
    std::size_t row;
    Box anchor;
    std::wstring query;
};

struct Notice {
    explicit Notice(Event type, std::size_t index = 0) : event(type), row(index) {}
    Event event;
    std::size_t row;
    std::wstring message;
    fs::path result;
    std::optional<Tools> tools;
    std::optional<double> progress;
};

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {
        for (const auto& item : categories) {
            auto it = std::ranges::find(formats(), item.target, &Format::extension);
            panes_[static_cast<std::size_t>(item.kind)].target = static_cast<int>(it - formats().begin());
        }
    }

    int show(int command) {
        icon_ = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(101), IMAGE_ICON, 0, 0,
                                              LR_DEFAULTSIZE | LR_SHARED));
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = dispatch;
        windowClass.hInstance = instance_;
        windowClass.hIcon = windowClass.hIconSm = icon_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"Convertor.Desktop";
        if (!RegisterClassExW(&windowClass)) return 1;
        constexpr DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        auto dpi = GetDpiForSystem();
        RECT frame{0, 0, MulDiv(1080, dpi, 96), MulDiv(670, dpi, 96)};
        AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_ACCEPTFILES, dpi);
        RECT work{};
        int width = frame.right - frame.left, height = frame.bottom - frame.top;
        int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            width = std::min<int>(width, work.right - work.left);
            height = std::min<int>(height, work.bottom - work.top);
            x = work.left + (work.right - work.left - width) / 2;
            y = work.top + (work.bottom - work.top - height) / 2;
        }
        if (!CreateWindowExW(WS_EX_ACCEPTFILES, windowClass.lpszClassName, L"Convertor", style,
                             x, y, width, height,
                             nullptr, nullptr, instance_, this)) return 1;
        ShowWindow(window_, command);
        UpdateWindow(window_);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    ~App() {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
        if (editBrush_) DeleteObject(editBrush_);
        if (editFont_) DeleteObject(editFont_);
    }

private:
    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            auto* app = static_cast<App*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!app) return DefWindowProcW(window, message, wparam, lparam);
        try {
            return app->handle(message, wparam, lparam);
        } catch (const Failure& error) {
            MessageBoxW(window, error.message.c_str(), L"Convertor", MB_ICONERROR);
        } catch (const std::exception& error) {
            MessageBoxW(window, fromUtf8(error.what()).c_str(), L"Convertor", MB_ICONERROR);
        }
        return 0;
    }

    int px(int value) const { return MulDiv(value, dpi_, 96); }
    int dip(int value) const { return MulDiv(value, 96, dpi_); }
    int footerY() const { return height_ - 70; }
    int visibleRows() const { return std::max(1, (footerY() - 253) / rowStep); }
    bool busy() const { return operation_ != Operation::idle; }
    bool canCancel() const { return busy() && operation_ != Operation::checking && !worker_.get_stop_token().stop_requested(); }
    Pane& pane() { return panes_[static_cast<std::size_t>(active_)]; }
    const Pane& pane() const { return panes_[static_cast<std::size_t>(active_)]; }

    void redraw() { InvalidateRect(window_, nullptr, FALSE); }

    void resized() {
        RECT size{};
        GetClientRect(window_, &size);
        width_ = dip(size.right);
        height_ = dip(size.bottom);
        if (menu_) dismissMenu();
        scroll(0);
    }

    void post(Notice notice) {
        {
            std::lock_guard lock(noticeMutex_);
            notices_.push_back(std::move(notice));
        }
        PostMessageW(window_, noticeMessage, 0, 0);
    }

    Progress progressCallback() {
        return [this](std::optional<double> value) {
            Notice update(Event::progress);
            update.progress = value;
            post(std::move(update));
        };
    }

    void startTools(bool install) {
        operation_ = install ? Operation::installing : Operation::checking;
        status_ = install ? L"Загрузка FFmpeg с GitHub (~110 МБ)…" : L"Проверка FFmpeg…";
        progress_.reset();
        SetTimer(window_, 1, 90, nullptr);
        worker_ = std::jthread([this, install](std::stop_token stop) {
            Notice notice(Event::tools);
            try {
                if (install) notice.tools = installTools(stop, progressCallback());
                else notice.tools = findTools(stop);
            } catch (const Cancelled&) {
            } catch (const Failure& error) {
                notice.message = error.message;
            } catch (const std::exception& error) {
                notice.message = fromUtf8(error.what());
            }
            post(std::move(notice));
        });
        redraw();
    }

    void startBatch() {
        if (busy() || pane().files.empty() || !tools_) return;
        dismissMenu();
        auto jobs = pane().files;
        auto output = pane().directory;
        auto tools = *tools_;
        for (auto& file : pane().files) {
            file.state = FileState::queued;
            file.result.clear();
            file.error.clear();
        }
        batchPane_ = active_;
        operation_ = Operation::batch;
        progress_.reset();
        status_ = L"Подготовка файлов…";
        SetTimer(window_, 1, 90, nullptr);
        worker_ = std::jthread([this, jobs = std::move(jobs), output = std::move(output), tools](std::stop_token stop) {
            std::size_t completed = 0, failed = 0;
            for (std::size_t row = 0; row < jobs.size() && !stop.stop_requested(); ++row) {
                const auto& file = jobs[row];
                Notice begin(Event::begin, row);
                begin.message = std::to_wstring(row + 1) + L"/" + std::to_wstring(jobs.size()) + L"  ·  " +
                                file.source.filename().native();
                post(std::move(begin));
                Notice result(Event::result, row);
                try {
                    result.result = convert(tools, file.source, output.empty() ? file.source.parent_path() : output,
                                            formats()[file.target], stop, progressCallback());
                    ++completed;
                } catch (const Cancelled&) {
                    post(Notice(Event::cancelled, row));
                    break;
                } catch (const Failure& error) {
                    result.event = Event::failure;
                    result.message = error.message;
                    ++failed;
                } catch (const std::exception& error) {
                    result.event = Event::failure;
                    result.message = fromUtf8(error.what());
                    ++failed;
                }
                post(std::move(result));
            }
            Notice finish(Event::finished);
            finish.message = L"Готово: " + std::to_wstring(completed) + L"    ·    Ошибок: " + std::to_wstring(failed);
            auto skipped = jobs.size() - completed - failed;
            if (skipped) finish.message += L"    ·    Прервано: " + std::to_wstring(skipped);
            post(std::move(finish));
        });
        redraw();
    }

    void finish() {
        operation_ = Operation::idle;
        progress_.reset();
        KillTimer(window_, 1);
    }

    void receive() {
        std::deque<Notice> updates;
        {
            std::lock_guard lock(noticeMutex_);
            updates.swap(notices_);
        }
        for (auto& notice : updates) {
            auto& files = panes_[static_cast<std::size_t>(batchPane_)].files;
            switch (notice.event) {
                case Event::tools:
                    tools_ = std::move(notice.tools);
                    status_ = tools_ ? L"FFmpeg готов. Добавьте файлы для конвертации."
                                     : L"Подключите FFmpeg для конвертации.";
                    finish();
                    if (!notice.message.empty() && !closing_) {
                        MessageBoxW(window_, notice.message.c_str(), L"FFmpeg", MB_ICONERROR);
                    }
                    break;
                case Event::progress:
                    if (canCancel()) progress_ = notice.progress;
                    break;
                case Event::begin:
                    files[notice.row].state = FileState::active;
                    if (canCancel()) status_ = std::move(notice.message);
                    progress_.reset();
                    if (batchPane_ == active_) {
                        auto row = static_cast<int>(notice.row);
                        if (row < pane().scroll || row >= pane().scroll + visibleRows()) pane().scroll = row;
                        scroll(0);
                    }
                    break;
                case Event::result:
                    files[notice.row].state = FileState::done;
                    files[notice.row].result = std::move(notice.result);
                    break;
                case Event::failure:
                    files[notice.row].state = FileState::error;
                    files[notice.row].error = std::move(notice.message);
                    break;
                case Event::cancelled:
                    files[notice.row].state = FileState::cancelled;
                    break;
                case Event::finished:
                    for (auto& file : files) {
                        if (file.state == FileState::queued) file.state = FileState::cancelled;
                    }
                    status_ = std::move(notice.message);
                    finish();
                    break;
            }
        }
        if (closing_ && !busy()) DestroyWindow(window_);
        else redraw();
    }

    void cancel() {
        worker_.request_stop();
        status_ = L"Остановка операции…";
        redraw();
    }

    void addPaths(const std::vector<fs::path>& paths) {
        const auto& extensions = inputExtensions(active_);
        std::size_t added = 0, rejected = 0;
        for (const auto& path : paths) {
            std::error_code error;
            if (!fs::is_regular_file(path, error) || std::ranges::find(extensions, extensionOf(path)) == extensions.end()) {
                ++rejected;
                continue;
            }
            if (std::ranges::any_of(pane().files, [&](const File& file) {
                    return CompareStringOrdinal(file.source.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL;
                })) continue;
            auto bytes = fs::file_size(path, error);
            pane().files.push_back({path, error ? 0 : bytes, pane().target, FileState::ready, {}, {}});
            ++added;
        }
        status_ = L"Добавлено файлов: " + std::to_wstring(added);
        if (rejected) status_ += L"    ·    Пропущено для этой вкладки: " + std::to_wstring(rejected);
        redraw();
    }

    void chooseFiles() {
        auto picker = dialog();
        picker->SetTitle(L"Выбрать файлы");
        picker->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
        std::wstring pattern;
        for (const auto& ext : inputExtensions(active_)) {
            if (!pattern.empty()) pattern += L";";
            pattern += L"*." + ext;
        }
        COMDLG_FILTERSPEC filters[]{{category(active_).tab, pattern.c_str()}, {L"Все файлы", L"*.*"}};
        picker->SetFileTypes(2, filters);
        if (FAILED(picker->Show(window_))) return;
        IShellItemArray* array = nullptr;
        if (FAILED(picker->GetResults(&array))) return;
        Com<IShellItemArray> results(array);
        DWORD count = 0;
        results->GetCount(&count);
        std::vector<fs::path> paths;
        for (DWORD index = 0; index < count; ++index) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(results->GetItemAt(index, &item))) {
                Com<IShellItem> selected(item);
                paths.push_back(itemPath(selected.get()));
            }
        }
        addPaths(paths);
    }

    void dropFiles(HDROP drop) {
        std::vector<fs::path> paths;
        if (!busy()) {
            UINT count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
            for (UINT index = 0; index < count; ++index) {
                auto length = DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(length + 1, L'\0');
                DragQueryFileW(drop, index, path.data(), length + 1);
                path.resize(length);
                paths.emplace_back(path);
            }
        }
        DragFinish(drop);
        if (!paths.empty()) {
            dismissMenu();
            addPaths(paths);
        }
    }

    void chooseFolder(Box button) {
        auto popup = CreatePopupMenu();
        if (!popup) return;
        AppendMenuW(popup, MF_STRING | (pane().directory.empty() ? MF_CHECKED : 0), 1, L"Рядом с исходными файлами");
        AppendMenuW(popup, MF_STRING, 2, L"Выбрать папку…");
        POINT position{px(button.x), px(button.y)};
        ClientToScreen(window_, &position);
        int selection = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN,
                                         position.x, position.y, window_, nullptr);
        DestroyMenu(popup);
        if (selection == 1) {
            pane().directory.clear();
            status_ = L"Результаты сохраняются рядом с исходными файлами.";
        } else if (selection == 2) {
            auto picker = dialog();
            picker->SetTitle(L"Куда сохранять результаты");
            picker->SetOptions(FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST);
            if (SUCCEEDED(picker->Show(window_))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(picker->GetResult(&item))) {
                    Com<IShellItem> folder(item);
                    pane().directory = itemPath(folder.get());
                    status_ = L"Папка результата: " + pane().directory.native();
                }
            }
        }
        redraw();
    }

    void openFile(std::size_t index) {
        const auto& file = pane().files[index];
        if (file.state == FileState::error) {
            MessageBoxW(window_, file.error.c_str(), file.source.filename().c_str(), MB_ICONERROR);
        } else if (file.state == FileState::done) {
            auto opened = reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", file.result.c_str(),
                                                                   nullptr, nullptr, SW_SHOWNORMAL));
            if (opened <= 32) MessageBoxW(window_, file.result.c_str(), L"Файл сохранён", MB_ICONINFORMATION);
        } else {
            MessageBoxW(window_, file.source.c_str(), L"Исходный файл", MB_ICONINFORMATION);
        }
    }

    void scroll(int by) {
        auto limit = std::max(0, static_cast<int>(pane().files.size()) - visibleRows());
        pane().scroll = std::clamp(pane().scroll + by, 0, limit);
        redraw();
    }

    Box menuBox() const {
        constexpr int width = 316;
        int height = active_ == Kind::video ? 204 : 141;
        const auto& anchor = menu_->anchor;
        int y = anchor.y + anchor.h + 8;
        if (y + height > footerY() - 8) y = std::max(69, anchor.y - height - 8);
        return {std::clamp(anchor.x + anchor.w - width, 24, width_ - width - 24), y, width, height};
    }

    void showMenu(const Hit& hit) {
        menu_ = FormatMenu{hit.index, hit.box, {}};
        focus_ = -1;
        auto box = menuBox();
        SetWindowTextW(search_, L"");
        SetWindowPos(search_, HWND_TOP, px(box.x + 40), px(box.y + 11), px(box.w - 52), px(23), SWP_SHOWWINDOW);
        SetFocus(search_);
        redraw();
    }

    void dismissMenu() {
        if (!menu_) return;
        menu_.reset();
        ShowWindow(search_, SW_HIDE);
        SetFocus(window_);
        focus_ = -1;
        redraw();
    }

    std::vector<int> menuFormats(Kind kind) const {
        std::vector<int> visible;
        const auto& choices = formats();
        if (!accepts(active_, kind)) return visible;
        for (std::size_t index = 0; index < choices.size(); ++index) {
            if (choices[index].kind == kind && targetName(static_cast<int>(index)).find(menu_->query) != std::wstring::npos) {
                visible.push_back(static_cast<int>(index));
            }
        }
        return visible;
    }

    void selectFormat(int target) {
        auto change = [target](File& file) {
            if (file.target == target) return;
            file.target = target;
            file.state = FileState::ready;
            file.result.clear();
            file.error.clear();
        };
        if (menu_->row == allRows) {
            pane().target = target;
            for (auto& file : pane().files) change(file);
            status_ = L"Формат для всех файлов: " + targetName(target);
        } else {
            change(pane().files[menu_->row]);
            status_ = L"Выбран формат " + targetName(target) + L".";
        }
        dismissMenu();
    }

    void activate(const Hit& hit) {
        if (menu_ && hit.action != Action::option) {
            dismissMenu();
            return;
        }
        switch (hit.action) {
            case Action::tab:
                active_ = static_cast<Kind>(hit.index);
                focus_ = -1;
                scroll(0);
                if (!busy()) status_ = pane().directory.empty() ? L"Сохранение рядом с исходными файлами"
                                                               : L"Папка результата: " + pane().directory.native();
                break;
            case Action::add: chooseFiles(); break;
            case Action::folder: chooseFolder(hit.box); break;
            case Action::format: showMenu(hit); break;
            case Action::option: selectFormat(static_cast<int>(hit.index)); break;
            case Action::open: openFile(hit.index); break;
            case Action::remove:
                pane().files.erase(pane().files.begin() + hit.index);
                focus_ = -1;
                scroll(0);
                status_ = L"Файл удалён из очереди.";
                break;
            case Action::start:
                if (canCancel()) cancel();
                else startBatch();
                break;
            case Action::engine:
                startTools(true);
                break;
        }
        redraw();
    }

    int hitAt(POINT point) const {
        for (auto index = hits_.size(); index > 0; --index) {
            if (hits_[index - 1].box.contains(point)) return static_cast<int>(index - 1);
        }
        return -1;
    }

    bool control(Box box, Action action, std::size_t index = 0, bool enabled = true) {
        if (!enabled) return false;
        hits_.push_back({box, action, index});
        return box.contains(mouse_);
    }

    void button(Canvas& canvas, Box box, std::wstring_view label, Icon icon, Action action,
                bool enabled = true, bool primary = false, std::size_t index = 0, bool dropdown = false) {
        bool hover = control(box, action, index, enabled);
        auto fill = primary ? (enabled ? (hover ? redHover : red) : wine) : (hover ? border : elevated);
        auto ink = enabled ? text : faint;
        canvas.round(box, fill, primary ? fill : border, 4);
        canvas.icon(icon, box.x + 12, box.y + (box.h - 16) / 2, 16, ink);
        canvas.label({box.x + 36, box.y, box.w - (dropdown ? 62 : 43), box.h}, label, ink, Font::strong);
        if (dropdown) canvas.icon(Icon::chevron, box.x + box.w - 23, box.y + (box.h - 14) / 2, 14, ink);
    }

    void header(Canvas& canvas) {
        canvas.gradient({0, 0, width_ + 1, 65}, background, RGB(33, 25, 27), true);
        canvas.logo(icon_, {30, 15, 35, 35});
        canvas.label({77, 0, 116, 64}, L"Convertor", text, Font::brand);
        int x = 204;
        constexpr std::array widths{71, 114, 76};
        for (std::size_t i = 0; i < categories.size(); ++i) {
            Box tab{x, 14, widths[i], 36};
            bool hover = control(tab, Action::tab, i);
            bool current = categories[i].kind == active_;
            if (hover || current) canvas.round(tab, current ? wine : panel, current ? wine : panel, 4);
            canvas.label(tab, categories[i].tab, current ? blush : muted, Font::strong, DT_CENTER);
            x += widths[i] + 4;
        }
        canvas.round({width_ - 75, 19, 47, 26}, panel, border, 4);
        canvas.label({width_ - 75, 19, 47, 26}, L"v2.0.0", muted, Font::small, DT_CENTER);
        if (tools_ || busy()) {
            auto dot = tools_ ? green : faint;
            canvas.circle({width_ - 210, 29, 7, 7}, dot, dot);
            canvas.label({width_ - 195, 15, 112, 35}, tools_ ? L"FFmpeg готов" : L"FFmpeg", muted, Font::small);
        } else {
            button(canvas, {width_ - 266, 14, 178, 36}, L"Скачать FFmpeg", Icon::add, Action::engine);
        }
    }

    void hero(Canvas& canvas) {
        const auto& item = category(active_);
        canvas.gradient({0, 65, width_ + 1, 171}, background, RGB(38, 28, 31), true);
        for (int x = 32; x < width_; x += 62) canvas.line(x, 65, x, 236, RGB(31, 32, 33));
        for (int y = 101; y < 236; y += 46) canvas.line(0, y, width_, y, RGB(30, 32, 33));
        auto heading = width_ < 850 ? Font::compactHeadline : Font::headline;
        canvas.label({30, 96, width_ - 365, 52}, item.title, text, heading);
        canvas.label({32, 154, width_ - 369, 23}, item.description, muted);
        canvas.label({32, 180, width_ - 369, 23}, item.detail, faint, Font::small);

        const auto& file = pane().files;
        auto from = file.empty() ? item.example : upper(extensionOf(file.front().source));
        Box input{width_ - 307, 88, 92, 114};
        Box output{width_ - 127, 88, 97, 114};
        canvas.round(input, panel, border, 10);
        canvas.icon(item.icon, input.x + 32, input.y + 17, 28, muted);
        canvas.label({input.x, input.y + 53, input.w, 25}, from, text, Font::strong, DT_CENTER);
        canvas.label({input.x, input.y + 80, input.w, 22}, L"ИСХОДНЫЙ", faint, Font::small, DT_CENTER);
        canvas.circle({width_ - 191, 119, 39, 39}, wine, RGB(78, 44, 48));
        canvas.icon(Icon::swap, width_ - 182, 128, 21, blush);
        canvas.label({width_ - 211, 163, 78, 25}, L"В ФОРМАТ", muted, Font::small, DT_CENTER);
        bool hover = control(output, Action::format, allRows, !busy());
        canvas.round(output, hover ? wine : RGB(28, 24, 27), RGB(91, 49, 54), 10);
        canvas.icon(category(formats()[pane().target].kind).icon, output.x + 34, output.y + 17, 28, blush);
        canvas.label({output.x, output.y + 53, output.w, 25}, targetName(pane().target), blush, Font::strong, DT_CENTER);
        canvas.label({output.x + 2, output.y + 81, output.w - 13, 22}, L"ДЛЯ ВСЕХ", muted, Font::small, DT_CENTER);
        canvas.icon(Icon::chevron, output.x + output.w - 20, output.y + 87, 12, blush);
    }

    std::pair<std::wstring, COLORREF> fileStatus(const File& file) const {
        switch (file.state) {
            case FileState::queued: return {L"В очереди", muted};
            case FileState::active: return {L"Конвертация…", blush};
            case FileState::done: return {L"Готово · открыть", green};
            case FileState::error: return {L"Ошибка · подробнее", blush};
            case FileState::cancelled: return {L"Прервано", muted};
            default: return {fileSize(file.bytes) + L"  ·  " + upper(extensionOf(file.source)), muted};
        }
    }

    void queue(Canvas& canvas) {
        const auto& files = pane().files;
        if (files.empty()) {
            int center = std::max(265, (236 + footerY()) / 2 - 57);
            canvas.label({30, center, width_ - 60, 29}, L"Добавьте файлы для конвертации",
                         text, Font::strong, DT_CENTER);
            button(canvas, {width_ / 2 - 91, center + 42, 182, 45}, L"Выбрать файлы",
                   Icon::add, Action::add, !busy(), true);
            canvas.label({30, center + 95, width_ - 60, 25}, L"или перетащите их в окно",
                         faint, Font::small, DT_CENTER);
            return;
        }
        auto last = std::min(static_cast<int>(files.size()), pane().scroll + visibleRows());
        for (int row = pane().scroll; row < last; ++row) {
            const auto& file = files[row];
            int y = 252 + (row - pane().scroll) * rowStep;
            canvas.round({24, y, width_ - 48, rowHeight}, panel, RGB(36, 41, 42), 5);
            canvas.round({40, y + 13, 42, 42}, elevated, elevated, 4);
            canvas.icon(category(active_).icon, 50, y + 23, 22, muted);
            bool open = control({92, y + 7, width_ - 437, 54}, Action::open, row, !busy());
            canvas.label({94, y + 9, width_ - 441, 25}, file.source.filename().native(),
                         open ? blush : text, Font::strong);
            const auto [status, ink] = fileStatus(file);
            canvas.label({94, y + 34, width_ - 441, 23}, status, ink, Font::small);
            canvas.round({width_ - 307, y + 18, 73, 32}, elevated, elevated, 4);
            canvas.label({width_ - 307, y + 18, 73, 32}, upper(extensionOf(file.source)), muted,
                         Font::strong, DT_CENTER);
            canvas.icon(Icon::arrow, width_ - 220, y + 24, 20, faint);
            Box target{width_ - 185, y + 15, 111, 38};
            auto hover = control(target, Action::format, row, !busy());
            canvas.round(target, hover ? RGB(72, 35, 40) : wine, wine, 4);
            canvas.label({target.x + 12, target.y, target.w - 38, target.h}, targetName(file.target),
                         blush, Font::strong);
            canvas.icon(Icon::chevron, target.x + target.w - 24, target.y + 13, 14, blush);
            auto remove = control({width_ - 64, y + 15, 32, 38}, Action::remove, row, !busy());
            canvas.icon(Icon::close, width_ - 57, y + 25, 16, remove ? blush : faint);
        }
        if (static_cast<int>(files.size()) > visibleRows()) {
            int track = visibleRows() * rowStep - 8;
            int thumb = std::max(20, track * visibleRows() / static_cast<int>(files.size()));
            int offset = (track - thumb) * pane().scroll / (static_cast<int>(files.size()) - visibleRows());
            canvas.round({width_ - 14, 252 + offset, 4, thumb}, border, border, 2);
        }
    }

    void footer(Canvas& canvas) {
        int y = footerY();
        canvas.rect({0, y, width_ + 1, 71}, elevated);
        canvas.line(0, y, width_, y, border);
        canvas.label({26, y + 9, width_ - 470, 27}, status_, muted);
        std::wstring caption;
        if (busy() && progress_) caption = std::to_wstring(static_cast<int>(*progress_ * 100)) + L"%";
        else if (!busy() && !pane().files.empty()) caption = L"Файлов в очереди: " + std::to_wstring(pane().files.size());
        else if (!busy()) caption = L"Локальная обработка";
        if (!caption.empty()) canvas.label({26, y + 37, width_ - 472, 20}, caption, faint, Font::small);
        if (busy() && operation_ != Operation::checking) {
            int width = width_ - 482;
            canvas.round({26, y + 60, width, 3}, border, border, 1);
            if (progress_) {
                canvas.rect({26, y + 60, static_cast<int>(*progress_ * width), 3}, redHover);
            } else {
                int segment = std::min(width / 3, 96);
                int offset = static_cast<int>((GetTickCount64() / 10) % (width - segment + 1));
                canvas.rect({26 + offset, y + 60, segment, 3}, redHover);
            }
        }
        button(canvas, {width_ - 439, y + 15, 101, 40}, L"Папка",
               Icon::folder, Action::folder, !busy());
        button(canvas, {width_ - 328, y + 15, 131, 40}, L"Добавить",
               Icon::add, Action::add, !busy());
        button(canvas, {width_ - 187, y + 15, 163, 40}, busy() ? L"Отмена" : L"Конвертировать",
               busy() ? Icon::close : Icon::swap, Action::start,
               canCancel() || (!busy() && tools_ && !pane().files.empty()), true);
    }

    void drawMenu(Canvas& canvas) {
        auto box = menuBox();
        canvas.round({box.x + 3, box.y + 5, box.w, box.h}, RGB(12, 14, 15), RGB(12, 14, 15), 7);
        canvas.round(box, panel, border, 7);
        canvas.round({box.x + 9, box.y + 7, box.w - 18, 31}, elevated, border, 4);
        canvas.icon(Icon::search, box.x + 16, box.y + 14, 16, muted);
        auto first = menuFormats(active_);
        auto second = active_ == Kind::video ? menuFormats(Kind::audio) : std::vector<int>{};
        auto group = [&](const std::vector<int>& options, std::wstring_view name, int y) {
            if (options.empty()) return;
            canvas.label({box.x + 13, y, box.w - 26, 21}, name, muted, Font::small);
            for (std::size_t index = 0; index < options.size(); ++index) {
                Box tile{box.x + 12 + static_cast<int>(index % 4) * 74,
                         y + 25 + static_cast<int>(index / 4) * 30, 68, 25};
                bool hover = control(tile, Action::option, options[index]);
                canvas.round(tile, hover ? wine : elevated, hover ? wine : elevated, 4);
                canvas.label(tile, targetName(options[index]), hover ? blush : text, Font::strong, DT_CENTER);
            }
        };
        group(first, category(active_).tab, box.y + 43);
        group(second, L"Аудио из видео", box.y + (first.empty() ? 43 : 107));
        if (first.empty() && second.empty()) {
            canvas.label({box.x + 12, box.y + 62, box.w - 24, 28}, L"Формат не найден", muted, Font::normal, DT_CENTER);
        }
    }

    void render(Canvas& canvas) {
        hits_.clear();
        canvas.rect({0, 0, width_ + 1, height_ + 1}, background);
        header(canvas);
        hero(canvas);
        queue(canvas);
        footer(canvas);
        if (menu_) {
            hits_.clear();
            drawMenu(canvas);
        }
        if (focus_ >= 0 && static_cast<std::size_t>(focus_) < hits_.size() && GetFocus() == window_) {
            auto box = hits_[focus_].box;
            canvas.line(box.x, box.y + box.h - 1, box.x + box.w, box.y + box.h - 1, blush, 2);
        }
    }

    void paint() {
        PAINTSTRUCT paint{};
        auto device = BeginPaint(window_, &paint);
        RECT size{};
        GetClientRect(window_, &size);
        if (size.right && size.bottom) {
            Canvas canvas(device, dpi_, size.right, size.bottom);
            render(canvas);
        }
        EndPaint(window_, &paint);
    }

    void refreshEditFont() {
        if (editFont_) DeleteObject(editFont_);
        editFont_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    }

    static LRESULT CALLBACK searchKeys(HWND edit, UINT message, WPARAM wparam, LPARAM lparam,
                                       UINT_PTR, DWORD_PTR reference) {
        auto* app = reinterpret_cast<App*>(reference);
        if (message == WM_KEYDOWN && app->menu_) {
            if (wparam == VK_ESCAPE) {
                app->dismissMenu();
                return 0;
            }
            if (wparam == VK_TAB || wparam == VK_DOWN) {
                SetFocus(app->window_);
                app->focus_ = 0;
                app->redraw();
                return 0;
            }
            if (wparam == VK_RETURN) {
                auto options = app->menuFormats(app->active_);
                if (options.empty()) options = app->menuFormats(Kind::audio);
                if (!options.empty()) app->selectFormat(options.front());
                return 0;
            }
        }
        if (message == WM_CHAR && (wparam == VK_RETURN || wparam == VK_TAB || wparam == VK_ESCAPE)) return 0;
        return DefSubclassProc(edit, message, wparam, lparam);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_CREATE: {
                dpi_ = GetDpiForWindow(window_);
                BOOL dark = TRUE;
                if (FAILED(DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark)))) {
                    DwmSetWindowAttribute(window_, 19, &dark, sizeof(dark));
                }
                search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                          0, 0, 0, 0, window_, nullptr, instance_, nullptr);
                SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Поиск формата"));
                SendMessageW(search_, EM_SETLIMITTEXT, 24, 0);
                editBrush_ = CreateSolidBrush(elevated);
                refreshEditFont();
                SetWindowSubclass(search_, searchKeys, 1, reinterpret_cast<DWORD_PTR>(this));
                startTools(false);
                return 0;
            }
            case WM_SIZE: resized(); return 0;
            case WM_DPICHANGED: {
                dpi_ = LOWORD(wparam);
                auto area = reinterpret_cast<RECT*>(lparam);
                SetWindowPos(window_, nullptr, area->left, area->top, area->right - area->left,
                             area->bottom - area->top, SWP_NOZORDER | SWP_NOACTIVATE);
                refreshEditFont();
                return 0;
            }
            case WM_GETMINMAXINFO: {
                auto info = reinterpret_cast<MINMAXINFO*>(lparam);
                info->ptMinTrackSize = {px(790), px(530)};
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: paint(); return 0;
            case WM_MOUSEMOVE: {
                int old = hitAt(mouse_);
                mouse_ = {dip(GET_X_LPARAM(lparam)), dip(GET_Y_LPARAM(lparam))};
                if (old != hitAt(mouse_)) redraw();
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window_, 0};
                TrackMouseEvent(&tracking);
                return 0;
            }
            case WM_MOUSELEAVE:
                mouse_ = {-1, -1};
                redraw();
                return 0;
            case WM_SETCURSOR:
                if (LOWORD(lparam) == HTCLIENT && hitAt(mouse_) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                break;
            case WM_LBUTTONUP: {
                POINT point{dip(GET_X_LPARAM(lparam)), dip(GET_Y_LPARAM(lparam))};
                int index = hitAt(point);
                if (index >= 0) {
                    focus_ = -1;
                    activate(hits_[index]);
                } else if (menu_ && !menuBox().contains(point)) dismissMenu();
                return 0;
            }
            case WM_MOUSEWHEEL:
                if (!menu_) scroll(GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? -2 : 2);
                return 0;
            case WM_DROPFILES: dropFiles(reinterpret_cast<HDROP>(wparam)); return 0;
            case WM_CTLCOLOREDIT:
                SetTextColor(reinterpret_cast<HDC>(wparam), text);
                SetBkColor(reinterpret_cast<HDC>(wparam), elevated);
                return reinterpret_cast<LRESULT>(editBrush_);
            case WM_COMMAND:
                if (reinterpret_cast<HWND>(lparam) == search_ && HIWORD(wparam) == EN_CHANGE && menu_) {
                    wchar_t query[25]{};
                    GetWindowTextW(search_, query, 25);
                    menu_->query = upper(query);
                    redraw();
                }
                return 0;
            case WM_KEYDOWN:
                if (wparam == VK_ESCAPE && menu_) dismissMenu();
                else if (wparam == VK_ESCAPE && canCancel()) cancel();
                else if (wparam == 'O' && GetKeyState(VK_CONTROL) < 0 && !busy()) {
                    dismissMenu();
                    chooseFiles();
                } else if ((wparam == VK_TAB || (menu_ && (wparam == VK_DOWN || wparam == VK_UP))) && !hits_.empty()) {
                    int direction = (wparam == VK_UP || GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
                    auto count = static_cast<int>(hits_.size());
                    focus_ = focus_ < 0 ? (direction > 0 ? 0 : count - 1) : (focus_ + direction + count) % count;
                    redraw();
                } else if ((wparam == VK_RETURN || wparam == VK_SPACE) && focus_ >= 0 &&
                           static_cast<std::size_t>(focus_) < hits_.size()) {
                    activate(hits_[focus_]);
                } else if (!menu_ && (wparam == VK_NEXT || wparam == VK_PRIOR)) {
                    scroll(wparam == VK_NEXT ? visibleRows() : -visibleRows());
                }
                return 0;
            case WM_TIMER:
                if (busy() && !progress_) redraw();
                return 0;
            case noticeMessage: receive(); return 0;
            case WM_CLOSE:
                if (busy()) {
                    if (canCancel() && MessageBoxW(window_, L"Прервать операцию и закрыть Convertor?",
                                                   L"Convertor", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
                    closing_ = true;
                    cancel();
                } else DestroyWindow(window_);
                return 0;
            case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(window_, message, wparam, lparam);
    }

    HINSTANCE instance_;
    HWND window_{}, search_{};
    HICON icon_{};
    HBRUSH editBrush_{};
    HFONT editFont_{};
    UINT dpi_ = 96;
    int width_ = 1080, height_ = 670;
    int focus_ = -1;
    POINT mouse_{-1, -1};
    std::vector<Hit> hits_;
    std::array<Pane, 3> panes_;
    Kind active_ = Kind::video, batchPane_ = Kind::video;
    std::optional<FormatMenu> menu_;
    std::optional<Tools> tools_;
    std::optional<double> progress_;
    std::wstring status_;
    Operation operation_ = Operation::checking;
    bool closing_ = false;
    std::mutex noticeMutex_;
    std::deque<Notice> notices_;
    std::jthread worker_;
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int command) {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(initialized)) return 1;
    int code;
    {
        App app(instance);
        code = app.show(command);
    }
    CoUninitialize();
    return code;
}
